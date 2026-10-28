#include "posture/Notifier.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "posture/Json.hpp"
#include "posture/Logger.hpp"

namespace posture {
namespace {

void EnsureDirs(const std::string& path) {
  std::string cur;
  if (!path.empty() && path[0] == '/') cur = "/";
  std::stringstream ss(path);
  std::string part;
  while (std::getline(ss, part, '/')) {
    if (part.empty()) continue;
    cur += part;
    ::mkdir(cur.c_str(), 0755);
    cur += "/";
  }
}

std::string TodayDate() {
  const std::string now = WallNowString();  // 2026-10-28T16:30:00.123
  return now.substr(0, 10);
}

// 从 jsonl 行里抽 "key":number（只服务本系统自己写的格式）
double ExtractNumber(const std::string& text, const std::string& key, double def) {
  const std::string needle = "\"" + key + "\":";
  const size_t pos = text.find(needle);
  if (pos == std::string::npos) return def;
  size_t b = pos + needle.size();
  size_t e = b;
  while (e < text.size() && (std::isdigit(static_cast<unsigned char>(text[e])) ||
                             text[e] == '-' || text[e] == '.' || text[e] == 'e')) {
    ++e;
  }
  try {
    return std::stod(text.substr(b, e - b));
  } catch (...) {
    return def;
  }
}

bool ExtractString(const std::string& text, const std::string& key, std::string* out) {
  const std::string needle = "\"" + key + "\":\"";
  const size_t pos = text.find(needle);
  if (pos == std::string::npos) return false;
  const size_t b = pos + needle.size();
  const size_t e = text.find('"', b);
  if (e == std::string::npos) return false;
  *out = text.substr(b, e - b);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
StatsStore::StatsStore(const Config& cfg) : cfg_(cfg), date_(TodayDate()) {}

std::string StatsStore::today_file() const {
  return cfg_.stats_dir + "/stats-" + date_ + ".jsonl";
}

bool StatsStore::Init(std::string* err) {
  EnsureDirs(cfg_.stats_dir);
  today_ = Totals{};
  std::ifstream ifs(today_file());
  if (ifs) {
    std::string line;
    while (std::getline(ifs, line)) {
      if (line.empty()) continue;
      std::string kind;
      if (!ExtractString(line, "kind", &kind)) continue;
      if (kind == "tick") {
        today_.use_s += ExtractNumber(line, "use_s", 0.0);
        today_.bad_s += ExtractNumber(line, "bad_s", 0.0);
      } else if (kind == "alert") {
        today_.alerts += 1;
        // 违规时长按事件记；tick 里已包含，避免重复累加
      } else if (kind == "violation") {
        // 时长在 alert 与 tick 里统计算，这里只保留记录供审计
      }
    }
    LOG_INFO("已恢复今日统计：在座 ", static_cast<int>(today_.use_s), "s，不良 ",
             static_cast<int>(today_.bad_s), "s，提醒 ", today_.alerts, " 次");
  }
  // 写一条启动标记，便于人肉审计
  Append(Json().Str("kind", "start").Str("at", WallNowString()).Build());
  (void)err;
  return true;
}

void StatsStore::Append(const std::string& line) {
  const std::string date_now = TodayDate();
  if (date_now != date_) {
    date_ = date_now;      // 跨天：内存计数清零，文件自动换新
    today_ = Totals{};
  }
  std::ofstream ofs(today_file(), std::ios::app);
  if (!ofs) {
    LOG_WARN("统计文件写入失败: ", today_file());
    return;
  }
  ofs << line << "\n";
}

void StatsStore::AddViolation(const std::string& key, double duration_s) {
  Append(Json().Str("kind", "violation").Str("rule", key)
             .Num("duration_s", duration_s, 1).Str("at", WallNowString()).Build());
}

void StatsStore::AddUseTime(double use_delta_s, double bad_delta_s) {
  today_.use_s += use_delta_s;
  today_.bad_s += bad_delta_s;
  Append(Json().Str("kind", "tick").Num("use_s", use_delta_s, 1)
             .Num("bad_s", bad_delta_s, 1).Str("at", WallNowString()).Build());
}

void StatsStore::AddAlert(const std::string& key, double active_s) {
  today_.alerts += 1;
  Append(Json().Str("kind", "alert").Str("rule", key).Num("active_s", active_s, 1)
             .Str("at", WallNowString()).Build());
}

// ---------------------------------------------------------------------------
Notifier::Notifier(const Config& cfg) : cfg_(cfg) {}

Notifier::~Notifier() { Close(); }

bool Notifier::Init(std::string* err) {
  socket_path_ = cfg_.status_socket;
  const size_t slash = socket_path_.find_last_of('/');
  if (slash != std::string::npos) EnsureDirs(socket_path_.substr(0, slash));

  ::unlink(socket_path_.c_str());
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    if (err) *err = std::string("socket() 失败: ") + std::strerror(errno);
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    if (err) *err = "socket 路径过长: " + socket_path_;
    return false;
  }
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err) *err = "bind 失败 " + socket_path_ + ": " + std::strerror(errno);
    return false;
  }
  // 允许同用户的 UI 进程连接（目录按 0755 建，socket 给 0666 免去 uid 差异）
  ::chmod(socket_path_.c_str(), 0666);
  if (::listen(listen_fd_, 8) != 0) {
    if (err) *err = std::string("listen 失败: ") + std::strerror(errno);
    return false;
  }
  const int flags = ::fcntl(listen_fd_, F_GETFL, 0);
  ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
  LOG_INFO("状态 socket 已监听 ", socket_path_);
  return true;
}

void Notifier::Close() {
  for (size_t i = 0; i < clients_.size(); ++i) {
    if (clients_[i] >= 0) ::close(clients_[i]);
  }
  clients_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    ::unlink(socket_path_.c_str());
  }
}

void Notifier::AcceptNew() {
  for (;;) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    clients_.push_back(fd);
    LOG_INFO("UI 客户端已连接（当前 ", clients_.size(), " 个）");
  }
}

void Notifier::DropClient(size_t index) {
  if (index >= clients_.size()) return;
  if (clients_[index] >= 0) ::close(clients_[index]);
  clients_.erase(clients_.begin() + static_cast<long>(index));
  LOG_INFO("UI 客户端已断开（剩余 ", clients_.size(), " 个）");
}

void Notifier::Broadcast(const std::string& line) {
  const std::string payload = line + "\n";
  for (size_t i = 0; i < clients_.size();) {
    const ssize_t n = ::send(clients_[i], payload.data(), payload.size(), MSG_NOSIGNAL);
    if (n <= 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        ++i;   // 客户端读得慢：丢这一帧，不阻塞推理（§14.3）
        continue;
      }
      DropClient(i);
      continue;
    }
    ++i;
  }
}

std::string Notifier::SerializeStatus(const StatusSnapshot& s) const {
  Json j;
  j.Str("type", "status")
      .Str("posture_state", s.posture_state)
      .Str("posture_state_zh", s.posture_state_zh)
      .Str("explanation", s.explanation)
      .Bool("present", s.present)
      .Int("persons", s.persons)
      .Int("persons_primary", s.persons_primary)
      .Num("latency_ms", s.latency_ms, 1)
      .Num("fps", s.fps, 1)
      .Int("frame_id", static_cast<long long>(s.frame_id))
      .Bool("has_baseline", s.has_baseline)
      .Bool("calibrating", s.calibrating)
      .Num("calibration_progress", s.calibration_progress * 100.0, 1)
      .Str("baseline_created_at", s.baseline_created_at)
      .Bool("metrics_valid", s.metrics.valid)
      .Num("shoulder_angle_deg", s.metrics.shoulder_angle_deg, 1)
      .Num("shoulder_tilt_dev", s.metrics.shoulder_dev_deg, 1)
      .Num("head_offset_y", s.metrics.head_offset_y, 3)
      .Num("head_axis_deg", s.metrics.head_axis_deg, 1)
      .Num("width_ratio", s.metrics.width_ratio, 3)
      .Num("wrist_to_nose", s.metrics.wrist_to_nose, 2)
      .Str("session_state", s.session_state)
      .Num("use_s", s.use_s, 1)
      .Num("remaining_s", s.remaining_s, 0)
      .Strings("active_rules", s.active_rules)
      .Str("last_notification", s.last_notification_title)
      .Num("last_notification_ago_s", s.last_notification_ago_s, 0)
      .Num("today_bad_s", s.today_bad_s, 1)
      .Num("today_use_s", s.today_use_s, 1)
      .Int("today_alerts", s.today_alerts)
      .Int("frame_width", s.frame_width)
      .Int("frame_height", s.frame_height)
      .Str("backend", s.backend)
      .Str("runtime_version", s.runtime_version)
      .Str("model_sha256", s.model_sha256)
      .Bool("joint_conf_reliable", s.joint_confidence_reliable)
      .Bool("snoozed", s.snoozed)
      .Str("device_id", s.device_id);
  std::vector<double> roi{s.roi_x, s.roi_y, s.roi_w, s.roi_h};
  j.Array("roi", roi, 2);
  j.Bool("keypoints_valid", s.keypoints_valid);
  std::vector<double> kp;
  kp.reserve(s.keypoints_norm.size());
  for (float v : s.keypoints_norm) kp.push_back(static_cast<double>(v));
  j.Array("keypoints", kp, 4);
  return j.Build();
}

void Notifier::Publish(const StatusSnapshot& snapshot) {
  AcceptNew();
  const WallTime now = WallClock::now();
  if (has_published_ &&
      std::chrono::duration<double>(now - last_publish_).count() < 0.5) {
    return;   // 最多 2Hz，UI 侧够用
  }
  last_publish_ = now;
  has_published_ = true;
  if (clients_.empty()) return;
  Broadcast(SerializeStatus(snapshot));
}

void Notifier::PublishVideo(const std::vector<unsigned char>& jpeg, int width, int height,
                            std::uint64_t seq) {
  if (clients_.empty() || jpeg.empty()) return;
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  b64.reserve((jpeg.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < jpeg.size(); i += 3) {
    const std::uint32_t v = (static_cast<std::uint32_t>(jpeg[i]) << 16) |
                            (static_cast<std::uint32_t>(jpeg[i + 1]) << 8) |
                            static_cast<std::uint32_t>(jpeg[i + 2]);
    b64.push_back(kTable[(v >> 18) & 0x3F]);
    b64.push_back(kTable[(v >> 12) & 0x3F]);
    b64.push_back(kTable[(v >> 6) & 0x3F]);
    b64.push_back(kTable[v & 0x3F]);
  }
  if (i < jpeg.size()) {
    std::uint32_t v = static_cast<std::uint32_t>(jpeg[i]) << 16;
    const bool two = (i + 1 < jpeg.size());
    if (two) v |= static_cast<std::uint32_t>(jpeg[i + 1]) << 8;
    b64.push_back(kTable[(v >> 18) & 0x3F]);
    b64.push_back(kTable[(v >> 12) & 0x3F]);
    b64.push_back(two ? kTable[(v >> 6) & 0x3F] : '=');
    b64.push_back('=');
  }

  Json j;
  j.Str("type", "video")
      .Int("seq", static_cast<long long>(seq))
      .Int("w", width)
      .Int("h", height)
      .Str("jpeg", b64);
  Broadcast(j.Build());
}

void Notifier::EmitEvent(const std::string& key, const std::string& title,
                         const std::string& detail, double active_s) {
  Json j;
  j.Str("type", "notification")
      .Str("key", key)
      .Str("title", title)
      .Str("detail", detail)
      .Num("active_s", active_s, 1)
      .Str("at", WallNowString());
  const std::string line = j.Build();
  Broadcast(line);
  if (cfg_.headless_log) {
    LOG_INFO("[提醒] ", title, " — ", detail);
  }
}

std::vector<Notifier::Command> Notifier::PollCommands() {
  std::vector<Command> out;
  AcceptNew();
  for (size_t i = 0; i < clients_.size();) {
    char buf[512];
    const ssize_t n = ::recv(clients_[i], buf, sizeof(buf), 0);
    if (n == 0) {
      DropClient(i);
      continue;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        ++i;
        continue;
      }
      DropClient(i);
      continue;
    }
    pending_.insert(pending_.end(), buf, buf + n);
    ++i;
  }

  // 按行解析
  for (;;) {
    auto it = std::find(pending_.begin(), pending_.end(), '\n');
    if (it == pending_.end()) break;
    std::string line(pending_.begin(), it);
    pending_.erase(pending_.begin(), it + 1);
    if (line.empty()) continue;
    Command cmd;
    if (ExtractString(line, "cmd", &cmd.name)) {
      cmd.minutes = ExtractNumber(line, "minutes", 15.0);
      out.push_back(cmd);
      LOG_DEBUG("收到 UI 命令: ", cmd.name);
    }
  }
  if (pending_.size() > 65536) pending_.clear();   // 防御异常客户端
  return out;
}

}  // namespace posture
