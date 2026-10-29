#include "posture/UserCalibration.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "posture/Json.hpp"
#include "posture/Logger.hpp"

namespace posture {
namespace {

// 极简 JSON 取值（只解析本系统自己写出去的 baseline.json，不是通用解析器）
bool ExtractNumber(const std::string& text, const std::string& key, double* out) {
  const std::string needle = "\"" + key + "\":";
  size_t pos = text.find(needle);
  if (pos == std::string::npos) return false;
  pos += needle.size();
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
  size_t end = pos;
  while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) ||
                               text[end] == '-' || text[end] == '+' || text[end] == '.' ||
                               text[end] == 'e' || text[end] == 'E')) {
    ++end;
  }
  if (end == pos) return false;
  try {
    *out = std::stod(text.substr(pos, end - pos));
    return true;
  } catch (...) {
    return false;
  }
}

bool ExtractString(const std::string& text, const std::string& key, std::string* out) {
  const std::string needle = "\"" + key + "\":";
  size_t pos = text.find(needle);
  if (pos == std::string::npos) return false;
  pos += needle.size();
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
  if (pos >= text.size() || text[pos] != '"') return false;
  ++pos;
  std::string value;
  while (pos < text.size() && text[pos] != '"') {
    if (text[pos] == '\\' && pos + 1 < text.size()) {
      ++pos;
      switch (text[pos]) {
        case 'n': value += '\n'; break;
        case 't': value += '\t'; break;
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        default: value += text[pos];
      }
    } else {
      value += text[pos];
    }
    ++pos;
  }
  *out = value;
  return true;
}

bool ExtractBool(const std::string& text, const std::string& key, bool* out) {
  const std::string needle = "\"" + key + "\":";
  size_t pos = text.find(needle);
  if (pos == std::string::npos) return false;
  pos += needle.size();
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
  if (text.compare(pos, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (text.compare(pos, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

double Median(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  if (n % 2 == 1) return values[n / 2];
  return (values[n / 2 - 1] + values[n / 2]) * 0.5;
}

void EnsureParentDir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return;
  const std::string dir = path.substr(0, slash);
  std::string cur;
  std::istringstream is(dir);
  std::string part;
  if (!dir.empty() && dir[0] == '/') cur = "/";
  while (std::getline(is, part, '/')) {
    if (part.empty()) continue;
    cur += part;
    ::mkdir(cur.c_str(), 0755);
    cur += "/";
  }
}

}  // namespace

UserCalibration::UserCalibration(const Config& cfg) : cfg_(cfg) {}

bool UserCalibration::LoadBaseline(const std::string& path, Baseline* out) {
  std::ifstream ifs(path);
  if (!ifs) return false;
  std::ostringstream os;
  os << ifs.rdbuf();
  const std::string text = os.str();
  if (text.empty()) return false;

  Baseline b;
  double v = 0;
  if (ExtractBool(text, "valid", &b.valid)) {
  } else {
    b.valid = true;  // 老文件没有 valid 字段时按有效处理
  }
  if (!ExtractNumber(text, "shoulder_angle_deg", &v)) return false;
  b.shoulder_angle_deg = v;
  if (ExtractNumber(text, "head_offset_x", &v)) b.head_offset_x = v;
  if (ExtractNumber(text, "head_offset_y", &v)) b.head_offset_y = v;
  if (ExtractNumber(text, "head_axis_deg", &v)) b.head_axis_deg = v;
  if (ExtractNumber(text, "width_ratio", &v)) b.width_ratio = v;
  if (ExtractNumber(text, "reference_distance_cm", &v)) b.reference_distance_cm = v;
  double n = 0;
  if (ExtractNumber(text, "samples", &n)) b.samples = static_cast<int>(n);
  if (ExtractNumber(text, "frame_width", &n)) b.frame_width = static_cast<int>(n);
  if (ExtractNumber(text, "frame_height", &n)) b.frame_height = static_cast<int>(n);
  bool eav = false;
  if (ExtractBool(text, "ear_angle_valid", &eav)) b.ear_angle_valid = eav;
  if (ExtractNumber(text, "ear_angle_deg", &v)) b.ear_angle_deg = v;
  ExtractString(text, "camera_id", &b.camera_id);
  ExtractString(text, "model_sha256", &b.model_sha256);
  ExtractString(text, "created_at", &b.created_at);

  *out = b;
  return true;
}

bool UserCalibration::SaveBaseline(const std::string& path, const Baseline& baseline,
                                   std::string* err) {
  EnsureParentDir(path);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream ofs(tmp, std::ios::trunc);
    if (!ofs) {
      if (err) *err = "无法写入 " + tmp;
      return false;
    }
    Json j;
    j.Bool("valid", true)
        .Bool("ear_angle_valid", baseline.ear_angle_valid)
        .Num("ear_angle_deg", baseline.ear_angle_deg, 3)
        .Num("shoulder_angle_deg", baseline.shoulder_angle_deg, 3)
        .Num("head_offset_x", baseline.head_offset_x, 4)
        .Num("head_offset_y", baseline.head_offset_y, 4)
        .Num("head_axis_deg", baseline.head_axis_deg, 3)
        .Num("width_ratio", baseline.width_ratio, 4)
        .Num("reference_distance_cm", baseline.reference_distance_cm, 1)
        .Int("samples", baseline.samples)
        .Int("frame_width", baseline.frame_width)
        .Int("frame_height", baseline.frame_height)
        .Str("camera_id", baseline.camera_id)
        .Str("model_sha256", baseline.model_sha256)
        .Str("created_at", baseline.created_at);
    ofs << j.Build() << "\n";
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = "rename 失败 " + tmp + " -> " + path;
    return false;
  }
  return true;
}

void UserCalibration::Start(SteadyTime now, const std::string& camera_id, int frame_width,
                            int frame_height, const std::string& model_sha256,
                            double reference_distance_cm) {
  reference_distance_cm_ = reference_distance_cm;
  active_ = true;
  started_ = now;
  samples_.clear();
  camera_id_ = camera_id;
  frame_width_ = frame_width;
  frame_height_ = frame_height;
  model_sha256_ = model_sha256;
}

void UserCalibration::Cancel() {
  active_ = false;
  samples_.clear();
}

double UserCalibration::elapsed_s(SteadyTime now) const {
  if (!active_) return 0.0;
  return std::chrono::duration<double>(now - started_).count();
}

double UserCalibration::progress(SteadyTime now) const {
  if (!active_ || cfg_.calibration_duration_s <= 0.f) return 0.0;
  const double p = elapsed_s(now) / cfg_.calibration_duration_s;
  return std::max(0.0, std::min(1.0, p));
}

UserCalibration::FeedResult UserCalibration::Feed(const PostureMetrics& metrics, SteadyTime now,
                                                  Baseline* out, std::string* message,
                                                  int min_samples) {
  if (!active_) return FeedResult::kIgnored;
  if (!metrics.valid) {
    if (message) *message = "无效样本：" + metrics.invalid_reason;
    return FeedResult::kIgnored;
  }
  // 明显离群（例如说话、挠头）不入样，避免把异常姿态当基准
  if (std::fabs(metrics.shoulder_angle_deg) > 45.f) {
    if (message) *message = "肩线倾角过大，已丢弃该样本";
    return FeedResult::kIgnored;
  }
  samples_.push_back({metrics.shoulder_angle_deg, metrics.head_offset_x,
                      metrics.head_offset_y, metrics.head_axis_deg, metrics.width_ratio,
                      metrics.ear_angle_deg, metrics.ear_angle_valid});

  if (elapsed_s(now) < cfg_.calibration_duration_s) {
    if (message) {
      *message = "校准中 " + std::to_string(samples_.size()) + " 帧 / " +
                 std::to_string(static_cast<int>(elapsed_s(now))) + "s";
    }
    return FeedResult::kSampled;
  }

  // 质量门限：样本太少说明画面不稳定，拒绝保存（§4.6 第 4 条）
  const size_t need = static_cast<size_t>(std::max(6, min_samples));
  if (samples_.size() < need) {
    if (message) {
      *message = "校准样本不足（" + std::to_string(samples_.size()) +
                 " 帧），请保持坐姿并确认摄像头画面清晰";
    }
    active_ = false;
    samples_.clear();
    return FeedResult::kFailed;
  }

  // §4.6 第 2 条「过滤明显离群值」：
  // 取全样本中位数，再丢掉偏离中位数过多的帧（说话、拿东西、转身的瞬间），
  // 用剩下的帧重新取中位数。否则一次伸手就把基准带歪，之后一直误报。
  std::vector<double> angles, ox, oy, axis, ratio;
  std::vector<double> ears;      // 只有耳朵可信的样本才进这里
  for (const auto& s : samples_) {
    angles.push_back(s.shoulder_angle_deg);
    ox.push_back(s.head_offset_x);
    oy.push_back(s.head_offset_y);
    axis.push_back(s.head_axis_deg);
    ratio.push_back(s.width_ratio);
    if (s.ear_angle_valid) ears.push_back(s.ear_angle_deg);
  }
  const double med_angle = Median(angles);
  const double med_oy = Median(oy);
  const double med_ox = Median(ox);
  const double med_axis = Median(axis);
  std::vector<Sample> kept;
  for (const auto& s : samples_) {
    if (std::fabs(s.shoulder_angle_deg - med_angle) > 4.0) continue;
    if (std::fabs(s.head_offset_y - med_oy) > 0.35) continue;
    if (std::fabs(s.head_offset_x - med_ox) > 0.30) continue;
    if (std::fabs(s.head_axis_deg - med_axis) > 20.0) continue;
    kept.push_back(s);
  }
  if (kept.size() * 100 < samples_.size() * 50 ||
      kept.size() < std::max<size_t>(5, need * 2 / 3)) {
    if (message) {
      *message = "校准样本不稳定（" + std::to_string(kept.size()) + "/" +
                 std::to_string(samples_.size()) +
                 " 帧一致），请坐正并保持不动后重试";
    }
    active_ = false;
    samples_.clear();
    return FeedResult::kFailed;
  }
  angles.clear(); ox.clear(); oy.clear(); axis.clear(); ratio.clear();
  for (const auto& s : kept) {
    angles.push_back(s.shoulder_angle_deg);
    ox.push_back(s.head_offset_x);
    oy.push_back(s.head_offset_y);
    axis.push_back(s.head_axis_deg);
    ratio.push_back(s.width_ratio);
  }
  Baseline b;
  b.valid = true;
  b.shoulder_angle_deg = Median(angles);
  b.head_offset_x = Median(ox);
  b.head_offset_y = Median(oy);
  b.head_axis_deg = Median(axis);
  b.width_ratio = Median(ratio);
  b.samples = static_cast<int>(kept.size());
  b.frame_width = frame_width_;
  b.frame_height = frame_height_;
  b.camera_id = camera_id_;
  b.model_sha256 = model_sha256_;
  // 耳线基准：至少一半样本可信才用；否则标记无效，规则层会自动退回头轴角
  b.ear_angle_valid = !ears.empty() && ears.size() * 2 >= samples_.size();
  b.ear_angle_deg = b.ear_angle_valid ? Median(ears) : 0.0;
  b.reference_distance_cm = reference_distance_cm_;
  b.created_at = WallNowString();

  active_ = false;
  samples_.clear();
  if (out) *out = b;
  if (message) {
    *message = "校准完成：肩线基准 " + std::to_string(b.shoulder_angle_deg) +
               "°，头肩纵向基准 " + std::to_string(b.head_offset_y) + "，样本 " +
               std::to_string(b.samples) + " 帧";
  }
  return FeedResult::kDone;
}

bool UserCalibration::BaselineMatches(const Baseline& baseline, const std::string& camera_id,
                                      int frame_width, int frame_height,
                                      const std::string& model_sha256, std::string* why) {
  if (!baseline.valid) {
    if (why) *why = "没有校准基准";
    return false;
  }
  if (!baseline.camera_id.empty() && !camera_id.empty() &&
      baseline.camera_id != camera_id) {
    if (why) *why = "摄像头变了（" + baseline.camera_id + " -> " + camera_id + "）";
    return false;
  }
  if (baseline.frame_width > 0 &&
      (baseline.frame_width != frame_width || baseline.frame_height != frame_height)) {
    if (why) {
      *why = "分辨率变了（" + std::to_string(baseline.frame_width) + "x" +
             std::to_string(baseline.frame_height) + " -> " + std::to_string(frame_width) +
             "x" + std::to_string(frame_height) + "）";
    }
    return false;
  }
  if (!baseline.model_sha256.empty() && !model_sha256.empty() &&
      baseline.model_sha256 != model_sha256) {
    if (why) *why = "模型变了（SHA256 不一致）";
    return false;
  }
  return true;
}

}  // namespace posture
