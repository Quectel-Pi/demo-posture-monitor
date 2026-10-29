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

  // 电动对焦 UVC 的焦距锁定。连续自动对焦会让镜头每帧微调，
  // 画面持续轻微失焦（实测清晰度只有锁死时的 1/8），必须关掉。
  // focus_absolute 的取值范围依设备而定（0..1023），用
  //   bash scripts/focus.sh --scan
  // 现场测出最佳值再填进来；-1 表示「不干预，保持设备默认」。
  bool focus_autofocus = false;
  int focus_absolute = -1;
  // 曝光同理：自动曝光在人在/人走时会来回调，导致亮度跳变影响关键点。
  // -1 表示不干预。
  int exposure_time_absolute = -1;
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
  // 「鼻子离肩线太近」的下限。俯视机位（摄像头装在显示器上方往下拍）会让
  // 头肩纵向距离天然变小，人一低头更是直接压到肩线上 —— 这时不能判"几何不可用"，
  // 那恰恰就是要检测的低头。所以这里只留一个极低的兜底门槛，
  // 用来挡"模型把鼻子检到肩膀上"这种明显错误，而不是挡正常低头。
  float min_head_gap = 0.08f;

  // 低头阈值：相对「基准头肩距离」的比例。
  //
  // 为什么不能用固定绝对值：头肩纵向距离本身随摄像头俯仰角变化很大
  // （直视机位基准约 -0.9 肩宽，显示器上方俯拍只有 -0.20）。固定的 0.35
  // 在俯视机位下永远达不到（鼻子压到肩线上最多也就降 0.20），低头检测直接失效。
  // 用比例就与机位无关：鼻子朝肩线下降了「自己基准距离的 55%」即判明显低头。
  float head_down_drop_ratio = 0.25f;
  float head_down_drop_min = 0.10f;  // 比例算出的阈值下限（防噪声）
  float head_down_drop_max = 0.35f;  // 上限（防直视机位下过于迟钝）
  float head_down_drop_legacy = 0.35f;   // 兼容旧配置键，仅在没有基准时参考
  float head_down_hold_s = 5.0f;
  float head_tilt_deg = 18.0f;       // 鼻-肩中点连线相对基准的偏角
  float head_tilt_hold_s = 5.0f;
  // 离屏过近：改为「相对校准基准的倍数」，与机位/FOV 无关。
  // 绝对值（too_close_ratio）在不同摄像头 FOV 下完全不可比 ——
  // 实测 icSpring 机位坐近是 0.78，UGREEN 俯视机位正常距离就已经 0.42。
  // 用倍数：1.35 表示「比校准时近了 1/1.35 = 26%」。
  float too_close_ratio_rel = 1.35f;
  float too_close_ratio = 0.75f;     // 绝对值兜底（没有基准时用）
  float too_close_hold_s = 5.0f;
  // 托腮门控：手腕必须比「校准时的手腕高度」抬高多少肩宽。
  // 不能用「手腕高于肩线」—— 俯拍机位下下巴/手腕投影都在肩线以下，
  // 实测托腮时 wrist_y_rel=0.38（仍低于肩线 0.38），而正常坐 1.19。
  float hand_face_wrist_rise = 0.35f;
  float hand_face_ratio = 0.95f;

  // 人体尺寸门：主用户的肩宽占比不得低于「校准基准 × 该比例」。
  // 用来挡「人走了以后椅子/桌角被当成人体」—— 实测假阳性框只有 95x118px
  // （宽占比 0.12），而正常坐着的用户是 0.35。同时也能挡掉这类小框
  // 造成的「多人」误判。
  float min_size_ratio = 0.55f;     // 手腕-鼻子距离 / 肩宽
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

  // ---- 界面视频流 ----
  // 后台服务把采集画面编码成 JPEG 沿本地 unix socket 发给界面显示。
  // 只在本机内存/本地 socket 里流转，不落盘、不出网（privacy 那几条仍强制为 false）。
  // 关掉后界面只剩骨架（隐私更强的模式）。
  bool ui_stream_video = true;
  int ui_video_width = 640;      // 下采样宽度；0 = 原始宽度
  int ui_video_fps = 10;         // 视频流帧率（与推理帧率无关）
  int ui_jpeg_quality = 70;      // 1-100

  // ---- runtime（本版新增，部署路径）----
  std::string status_socket = "/run/posture-guard/status.sock";
  std::string state_dir = "/var/lib/posture-guard";
  std::string stats_dir = "/var/lib/posture-guard/stats";
  bool calibrate_on_start = false;   // 首启无基准时自动进入校准
};

}  // namespace posture
