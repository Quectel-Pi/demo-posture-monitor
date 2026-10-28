// 运行配置（对应实施方案 §12 的 posture.yaml）
//
// 所有阈值都是「工程初始值」，不是人体工学规范；现场必须用
// scripts/calibrate.sh + docs/calibration.md 的流程重新标定。
#pragma once

#include <string>

namespace posture {

struct Config {
  static Config Defaults();
  // 从 YAML 读取；缺省键用默认值补齐，文件不存在直接报错
  static bool Load(const std::string& path, Config* out, std::string* err);
  // 把实际生效的值序列化成可读文本（自检/审计用）
  std::string Describe() const;

  // ---- camera ----
  std::string camera_device = "/dev/video0";
  int camera_width = 640;
  int camera_height = 480;
  int capture_fps = 20;
  int infer_fps = 8;
  std::string preferred_pixel_format = "MJPEG";
  float camera_reconnect_backoff_s = 1.0f;
  float camera_reconnect_backoff_max_s = 15.0f;

  // ---- models ----
  std::string model_path = "/opt/posture-guard/models/yolov8n-pose.rknn";
  float pose_confidence = 0.5f;
  float joint_confidence = 0.5f;
  float nms_iou = 0.45f;

  // ---- user ----
  float roi_x = 0.10f, roi_y = 0.05f, roi_w = 0.80f, roi_h = 0.90f;
  float calibration_duration_s = 5.0f;
  std::string baseline_file = "/var/lib/posture-guard/baseline.json";

  // ---- posture（核心规则，对应 §4.2 / §4.3 / §12.2）----
  float shoulder_trigger_deg = 12.0f;
  float shoulder_recover_deg = 8.0f;
  float shoulder_hold_s = 8.0f;
  float recovery_hold_s = 3.0f;
  float missing_data_timeout_s = 1.5f;

  // 演示需求（DEMO-036）额外要求的三类姿态。实施方案把它们列为 V2
  // （需要人脸 Landmark / ToF），这里用「肩宽归一化的近似量」实现，
  // 界面与日志都会明确标注 approx，不当成真实角度。
  float head_down_drop = 0.35f;      // head_offset_y 相对校准基准的下降量
  float head_down_hold_s = 5.0f;
  float head_tilt_deg = 18.0f;       // 鼻-肩中点连线相对基准的偏角
  float head_tilt_hold_s = 5.0f;
  float too_close_ratio = 0.75f;     // 肩宽 / 画面宽
  float too_close_hold_s = 5.0f;
  float hand_face_ratio = 0.95f;     // 手腕-鼻子距离 / 肩宽
  float hand_face_hold_s = 5.0f;

  // ---- eye care（§5.1 / §5.2）----
  float continuous_use_s = 1200.0f;
  float far_look_s = 20.0f;
  float presence_confirm_s = 2.0f;
  float absent_confirm_s = 10.0f;
  std::string reminder_policy = "manual_ack_or_session_reset";

  // ---- notification（§13.1）----
  float cooldown_s = 60.0f;
  bool headless_log = true;
  bool desktop_popup = false;
  bool voice = false;

  // ---- privacy（§18.2）----
  bool save_raw_video = false;
  bool save_face_images = false;
  bool log_keypoints = false;
  int retain_statistics_days = 30;

  // ---- runtime（本版新增，部署路径）----
  std::string status_socket = "/run/posture-guard/status.sock";
  std::string state_dir = "/var/lib/posture-guard";
  std::string stats_dir = "/var/lib/posture-guard/stats";
  bool calibrate_on_start = false;   // 首启无基准时自动进入校准
};

}  // namespace posture
