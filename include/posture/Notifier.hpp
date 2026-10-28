// 输出层（对应实施方案 §13.1 / §13.3）
//
// 本进程是 systemd 系统服务，跑在无图形会话里，因此：
//   * 日志 -> stdout（journald 收集）                 <- V0 能力
//   * 事件/状态 -> AF_UNIX 流式 socket（JSON 行）      <- UI 进程从这里读
//   * 统计 -> /var/lib/posture-guard/stats/*.jsonl    <- 追加写，崩溃安全
//
// 桌面 UI（ui/posture_guard_ui.py，用户会话里的 GTK 进程）通过 socket 订阅，
// 这样后台服务不需要 DISPLAY/WAYLAND_DISPLAY 权限（§13.3 明确要求）。
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "posture/Config.hpp"
#include "posture/Types.hpp"

namespace posture {

// 运行态快照，序列化成一行 JSON 推给 UI
struct StatusSnapshot {
  std::string posture_state = "UNKNOWN";
  std::string posture_state_zh = "无法判定";
  std::string explanation;
  bool present = false;
  int persons = 0;          // 引擎原始检出（w8a8 会有多尺度重复框）
  int persons_primary = 0;  // 去重后的人数（真正用来判定的）
  double latency_ms = 0.0;
  double fps = 0.0;
  std::uint64_t frame_id = 0;

  bool has_baseline = false;
  bool calibrating = false;
  double calibration_progress = 0.0;
  std::string baseline_created_at;

  PostureMetrics metrics;

  std::string session_state = "ABSENT";
  double use_s = 0.0;
  double remaining_s = 0.0;

  std::vector<std::string> active_rules;
  std::string last_notification_title;
  double last_notification_ago_s = -1.0;

  double today_bad_s = 0.0;
  double today_use_s = 0.0;
  int today_alerts = 0;

  int frame_width = 0;
  int frame_height = 0;
  float roi_x = 0.f, roi_y = 0.f, roi_w = 0.f, roi_h = 0.f;
  std::string model_path;
  std::string model_sha256;
  std::string backend;
  std::string runtime_version;
  bool joint_confidence_reliable = true;
  bool snoozed = false;
  std::string device_id;

  // 归一化到 0..1 的 17 个关键点（x,y 交替）。只通过本地 socket 发给 UI 画骨架，
  // 不写日志、不落盘、不出网（实施方案 §18.2）。空表示本帧没有可信人体。
  std::vector<float> keypoints_norm;
  bool keypoints_valid = false;
};

// 追加式统计（不依赖 sqlite/外部库）
class StatsStore {
 public:
  struct Totals {
    double bad_s = 0.0;    // 今日不良姿势累计秒
    double use_s = 0.0;    // 今日在座累计秒
    int alerts = 0;        // 今日提醒次数
  };

  explicit StatsStore(const Config& cfg);

  bool Init(std::string* err);
  void AddViolation(const std::string& key, double duration_s);
  void AddUseTime(double use_delta_s, double bad_delta_s);
  void AddAlert(const std::string& key, double active_s);
  const Totals& today() const { return today_; }
  std::string today_file() const;

 private:
  void Append(const std::string& line);

  Config cfg_;
  Totals today_;
  std::string date_;
};

class Notifier {
 public:
  struct Command {
    std::string name;      // ack / snooze / clear_snooze / calibrate / quit / status
    double minutes = 0.0;
  };

  explicit Notifier(const Config& cfg);
  ~Notifier();

  bool Init(std::string* err);
  void Close();

  // 推送一帧状态给所有客户端（内部做 1Hz 节流，避免刷爆 UI）
  void Publish(const StatusSnapshot& snapshot);
  // 推送一帧视频（JPEG 已编码 + base64）。返回是否真的发出去（没有客户端时 false）
  void PublishVideo(const std::vector<unsigned char>& jpeg, int width, int height,
                    std::uint64_t seq);

  // 推送一条通知事件
  void EmitEvent(const std::string& key, const std::string& title,
                 const std::string& detail, double active_s);

  // 取出客户端发来的命令（非阻塞）
  std::vector<Command> PollCommands();

  int client_count() const { return static_cast<int>(clients_.size()); }

 private:
  void AcceptNew();
  void DropClient(size_t index);
  void Broadcast(const std::string& line);
  std::string SerializeStatus(const StatusSnapshot& s) const;

  Config cfg_;
  int listen_fd_ = -1;
  std::vector<int> clients_;
  std::deque<char> pending_;   // 客户端上行缓冲
  WallTime last_publish_{};
  bool has_published_ = false;
  std::string socket_path_;
};

}  // namespace posture
