#include "posture/Config.hpp"

#include <sstream>

#include "posture/Yaml.hpp"

namespace posture {

Config Config::Defaults() { return Config{}; }

bool Config::Load(const std::string& path, Config* out, std::string* err) {
  Yaml yaml;
  if (!yaml.Load(path, err)) return false;

  Config c = Defaults();

  c.camera_device = yaml.Str("camera.device", c.camera_device);
  c.camera_width = yaml.Int("camera.width", c.camera_width);
  c.camera_height = yaml.Int("camera.height", c.camera_height);
  c.capture_fps = yaml.Int("camera.capture_fps", c.capture_fps);
  c.infer_fps = yaml.Int("camera.infer_fps", c.infer_fps);
  c.preferred_pixel_format =
      yaml.Str("camera.preferred_pixel_format", c.preferred_pixel_format);
  c.camera_reconnect_backoff_s =
      static_cast<float>(yaml.Num("camera.reconnect_backoff_s", c.camera_reconnect_backoff_s));
  c.camera_reconnect_backoff_max_s = static_cast<float>(
      yaml.Num("camera.reconnect_backoff_max_s", c.camera_reconnect_backoff_max_s));

  c.focus_autofocus = yaml.Bool("camera.focus_autofocus", c.focus_autofocus);
  c.focus_absolute = yaml.Int("camera.focus_absolute", c.focus_absolute);
  c.exposure_time_absolute =
      yaml.Int("camera.exposure_time_absolute", c.exposure_time_absolute);

  c.model_path = yaml.Str("models.pose", c.model_path);
  c.pose_confidence = static_cast<float>(yaml.Num("models.pose_confidence", c.pose_confidence));
  c.joint_confidence =
      static_cast<float>(yaml.Num("models.joint_confidence", c.joint_confidence));
  c.nms_iou = static_cast<float>(yaml.Num("models.nms_iou", c.nms_iou));

  const auto roi = yaml.Nums("user.roi");
  if (roi.size() == 4) {
    c.roi_x = static_cast<float>(roi[0]);
    c.roi_y = static_cast<float>(roi[1]);
    c.roi_w = static_cast<float>(roi[2]);
    c.roi_h = static_cast<float>(roi[3]);
  }
  c.calibration_duration_s =
      static_cast<float>(yaml.Num("user.calibration_duration_s", c.calibration_duration_s));
  c.baseline_file = yaml.Str("user.baseline_file", c.baseline_file);
  c.calibrate_on_start = yaml.Bool("user.calibrate_on_start", c.calibrate_on_start);

  c.shoulder_trigger_deg =
      static_cast<float>(yaml.Num("posture.shoulder_trigger_deg", c.shoulder_trigger_deg));
  c.shoulder_recover_deg =
      static_cast<float>(yaml.Num("posture.shoulder_recover_deg", c.shoulder_recover_deg));
  c.shoulder_hold_s =
      static_cast<float>(yaml.Num("posture.shoulder_hold_s", c.shoulder_hold_s));
  c.recovery_hold_s =
      static_cast<float>(yaml.Num("posture.recovery_hold_s", c.recovery_hold_s));
  c.missing_data_timeout_s = static_cast<float>(
      yaml.Num("posture.missing_data_timeout_s", c.missing_data_timeout_s));

  c.head_down_drop = static_cast<float>(yaml.Num("posture.head_down_drop", c.head_down_drop));
  c.head_down_hold_s =
      static_cast<float>(yaml.Num("posture.head_down_hold_s", c.head_down_hold_s));
  c.head_tilt_deg = static_cast<float>(yaml.Num("posture.head_tilt_deg", c.head_tilt_deg));
  c.head_tilt_hold_s =
      static_cast<float>(yaml.Num("posture.head_tilt_hold_s", c.head_tilt_hold_s));
  c.too_close_ratio =
      static_cast<float>(yaml.Num("posture.too_close_ratio", c.too_close_ratio));
  c.too_close_hold_s =
      static_cast<float>(yaml.Num("posture.too_close_hold_s", c.too_close_hold_s));
  c.hand_face_ratio = static_cast<float>(yaml.Num("posture.hand_face_ratio", c.hand_face_ratio));
  c.hand_face_hold_s =
      static_cast<float>(yaml.Num("posture.hand_face_hold_s", c.hand_face_hold_s));

  c.continuous_use_s =
      static_cast<float>(yaml.Num("eye_care.continuous_use_s", c.continuous_use_s));
  c.far_look_s = static_cast<float>(yaml.Num("eye_care.far_look_s", c.far_look_s));
  c.presence_confirm_s =
      static_cast<float>(yaml.Num("eye_care.presence_confirm_s", c.presence_confirm_s));
  c.absent_confirm_s =
      static_cast<float>(yaml.Num("eye_care.absent_confirm_s", c.absent_confirm_s));
  c.reminder_policy = yaml.Str("eye_care.reminder_policy", c.reminder_policy);

  c.cooldown_s = static_cast<float>(yaml.Num("notification.cooldown_s", c.cooldown_s));
  c.headless_log = yaml.Bool("notification.headless_log", c.headless_log);
  c.desktop_popup = yaml.Bool("notification.desktop_popup", c.desktop_popup);
  c.voice = yaml.Bool("notification.voice", c.voice);

  c.save_raw_video = yaml.Bool("privacy.save_raw_video", c.save_raw_video);
  c.save_face_images = yaml.Bool("privacy.save_face_images", c.save_face_images);
  c.log_keypoints = yaml.Bool("privacy.log_keypoints", c.log_keypoints);
  c.retain_statistics_days =
      yaml.Int("privacy.retain_statistics_days", c.retain_statistics_days);

  c.ui_stream_video = yaml.Bool("ui.stream_video", c.ui_stream_video);
  c.ui_video_width = yaml.Int("ui.video_width", c.ui_video_width);
  c.ui_video_fps = yaml.Int("ui.video_fps", c.ui_video_fps);
  c.ui_jpeg_quality = yaml.Int("ui.jpeg_quality", c.ui_jpeg_quality);
  if (c.ui_jpeg_quality < 30) c.ui_jpeg_quality = 30;
  if (c.ui_jpeg_quality > 95) c.ui_jpeg_quality = 95;
  if (c.ui_video_fps < 1) c.ui_video_fps = 1;
  if (c.ui_video_fps > 30) c.ui_video_fps = 30;

  c.status_socket = yaml.Str("runtime.status_socket", c.status_socket);
  c.state_dir = yaml.Str("runtime.state_dir", c.state_dir);
  c.stats_dir = yaml.Str("runtime.stats_dir", c.stats_dir);

  // 隐私红线：这两项一旦打开，等于把原始画面落盘，直接拒绝启动。
  if (c.save_raw_video || c.save_face_images) {
    if (err) {
      *err =
          "配置里 privacy.save_raw_video / save_face_images 为 true，"
          "本系统按设计不落盘原始画面（实施方案 §18.2），请改成 false";
    }
    return false;
  }

  *out = c;
  return true;
}

std::string Config::Describe() const {
  std::ostringstream os;
  os << "camera=" << camera_device << " " << camera_width << "x" << camera_height
     << "@" << capture_fps << "fps infer=" << infer_fps << "fps fmt="
     << preferred_pixel_format << "\n";
  os << "focus: autofocus=" << (focus_autofocus ? "on" : "off")
     << " absolute=" << focus_absolute
     << " exposure=" << exposure_time_absolute << "\n";
  os << "model=" << model_path << " pose_conf=" << pose_confidence
     << " joint_conf=" << joint_confidence << "\n";
  os << "roi=" << roi_x << "," << roi_y << "," << roi_w << "," << roi_h << "\n";
  os << "baseline=" << baseline_file << " calibration=" << calibration_duration_s << "s\n";
  os << "shoulder: trigger=" << shoulder_trigger_deg << "deg recover="
     << shoulder_recover_deg << "deg hold=" << shoulder_hold_s << "s rec_hold="
     << recovery_hold_s << "s missing_timeout=" << missing_data_timeout_s << "s\n";
  os << "approx: head_down_drop=" << head_down_drop << " head_tilt_deg=" << head_tilt_deg
     << " too_close_ratio=" << too_close_ratio << " hand_face_ratio=" << hand_face_ratio
     << "\n";
  os << "eye_care: use=" << continuous_use_s << "s far_look=" << far_look_s
     << "s presence=" << presence_confirm_s << "s absent=" << absent_confirm_s
     << "s policy=" << reminder_policy << "\n";
  os << "notify: cooldown=" << cooldown_s << "s log=" << (headless_log ? "on" : "off")
     << " popup=" << (desktop_popup ? "on" : "off") << "\n";
  os << "ui_video: stream=" << (ui_stream_video ? "on" : "off") << " w=" << ui_video_width
     << " fps=" << ui_video_fps << " q=" << ui_jpeg_quality << "\n";
  os << "runtime: socket=" << status_socket << " state=" << state_dir
     << " stats=" << stats_dir << "\n";
  return os.str();
}

}  // namespace posture
