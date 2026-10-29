// 个人校准（对应实施方案 §4.6）
//
// 流程：用户保持正常坐姿 -> 采集 calibration_duration_s 秒的有效帧 ->
// 取每个特征的中位数作为基准 -> 连同「摄像头 ID / 分辨率 / 模型 SHA256」一起落盘。
// 任何一项变了（换机位、换模型、换分辨率）都要求重新校准。
#pragma once

#include <string>
#include <vector>

#include "posture/Config.hpp"
#include "posture/Types.hpp"

namespace posture {

class UserCalibration {
 public:
  enum class FeedResult { kIgnored, kSampled, kDone, kFailed };

  explicit UserCalibration(const Config& cfg);

  static bool LoadBaseline(const std::string& path, Baseline* out);
  static bool SaveBaseline(const std::string& path, const Baseline& baseline,
                           std::string* err);

  void Start(SteadyTime now, const std::string& camera_id, int frame_width,
             int frame_height, const std::string& model_sha256,
             double reference_distance_cm = 0.0);
  void Cancel();

  bool active() const { return active_; }
  double progress(SteadyTime now) const;
  int samples() const { return static_cast<int>(samples_.size()); }
  // 已采样的秒数（日志用）
  double elapsed_s(SteadyTime now) const;

  // 喂一个可信的几何样本。kDone 时把结果写进 out。
  FeedResult Feed(const PostureMetrics& metrics, SteadyTime now, Baseline* out,
                  std::string* message, int min_samples = 15);

  // 采样门槛：默认 15 帧，但如果设备/场景给不出那么多（推理帧率低、几何闸门
  // 拒掉一部分帧），死磕 15 帧会让校准永远失败。这里按「实际拿到多少」放宽到 10。
  int min_samples() const { return 10; }

  // 基准是否与当前设备/模型匹配；不匹配返回原因
  static bool BaselineMatches(const Baseline& baseline, const std::string& camera_id,
                             int frame_width, int frame_height,
                             const std::string& model_sha256, std::string* why);

 private:
  struct Sample {
    float shoulder_angle_deg;
    float head_offset_x;
    float head_offset_y;
    float head_axis_deg;
    float width_ratio;
    float ear_angle_deg;
    bool ear_angle_valid;
  };

  Config cfg_;
  bool active_ = false;
  SteadyTime started_{};
  std::vector<Sample> samples_;
  std::string camera_id_;
  int frame_width_ = 0;
  int frame_height_ = 0;
  std::string model_sha256_;
  double reference_distance_cm_ = 0.0;
};

}  // namespace posture
