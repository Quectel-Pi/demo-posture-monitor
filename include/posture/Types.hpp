// 统一数据契约（对应实施方案 §6.3 / §11.1）
//
// 约定：
//   * 时间：业务计时一律用 steady_clock（不受系统改时间影响）；
//           日志与统计用 system_clock（墙上时钟）。
//   * 坐标：PersonPose 里的关键点是「原图坐标系」像素值（已从 letterbox 还原）。
//   * 无效数据：用 confidence < 门槛 或 PostureState::UNKNOWN 表达，
//               绝不用 0 值冒充「姿态正常」。
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace posture {

using SteadyClock = std::chrono::steady_clock;
using SteadyTime = SteadyClock::time_point;
using WallClock = std::chrono::system_clock;
using WallTime = WallClock::time_point;

constexpr int kNumJoints = 17;

// COCO-17 下标（与 YOLOv8-Pose 一致）
enum Joint : int {
  kNose = 0,
  kEyeL = 1,
  kEyeR = 2,
  kEarL = 3,
  kEarR = 4,
  kShoulderL = 5,
  kShoulderR = 6,
  kElbowL = 7,
  kElbowR = 8,
  kWristL = 9,
  kWristR = 10,
  kHipL = 11,
  kHipR = 12,
  kKneeL = 13,
  kKneeR = 14,
  kAnkleL = 15,
  kAnkleR = 16,
};

const char* JointName(int idx);

struct Keypoint {
  float x = 0.f;
  float y = 0.f;
  float confidence = 0.f;
};

struct BBox {
  float x = 0.f;  // 左上角
  float y = 0.f;
  float w = 0.f;
  float h = 0.f;
  float CenterX() const { return x + w * 0.5f; }
  float CenterY() const { return y + h * 0.5f; }
  float Area() const { return w * h; }
};

// 一帧图像（采集线程 -> 推理线程）
struct Frame {
  cv::Mat bgr;                       // 原图，BGR8
  int width = 0;
  int height = 0;
  std::uint64_t frame_id = 0;
  SteadyTime captured_at{};          // 单调时间戳（业务计时用这个）
  WallTime wall_at{};                // 墙上时间（日志用）
};

// 一个人体姿态
struct PersonPose {
  std::array<Keypoint, kNumJoints> joints{};
  BBox box;
  float person_confidence = 0.f;
  std::uint64_t frame_id = 0;
  SteadyTime captured_at{};
  bool JointValid(int idx, float min_conf) const {
    return joints[idx].confidence >= min_conf;
  }
};

// 判定状态。UNKNOWN / USER_AWAY 与 NORMAL 是三件不同的事（§6.3 明确要求）。
enum class PostureState {
  kNormal,
  kShoulderTilt,   // 高低肩
  kHeadDown,       // 低头 / 趴桌（头肩纵向距离相对基准的下降比例）
  kHeadTilt,       // 歪头（用左右耳连线的角度，俯视机位下也稳定）
  kTooClose,       // 离屏幕过近（肩宽占比相对基准的倍数；趋势判据，不测距）
  kHandOnFace,     // 托腮
  kUserAway,
  kUnknown,
};

const char* ToString(PostureState state);
const char* ToChinese(PostureState state);

// 单帧几何特征。所有「归一化」量除的都是肩宽（像素），
// 因此人走近走远时数值基本稳定（§4.3）。
struct PostureMetrics {
  bool valid = false;                    // 关键点是否足够可信
  float shoulder_angle_deg = 0.f;        // 肩线方向角，已折叠到 [-90, 90)
  float shoulder_dev_deg = 0.f;          // 与个人基准的偏差（无基准时为 0）
  float shoulder_width_px = 0.f;
  float head_offset_x = 0.f;             // (鼻子X - 肩中点X) / 肩宽
  float head_offset_y = 0.f;             // (鼻子Y - 肩中点Y) / 肩宽
  // 备用头部参考点：耳朵中点。俯拍机位下鼻子几乎不可见（正脸被头顶遮住），
  // 实测鼻子 x 方向抖动 ±69px，而耳线只 ±5°。用它替代鼻子可显著降噪。
  float ear_mid_offset_y = 0.f;          // (双耳中点Y - 肩中点Y) / 肩宽
  bool ear_mid_valid = false;
  float eye_mid_offset_y = 0.f;          // (双眼中点Y - 肩中点Y) / 肩宽（对照）
  bool eye_mid_valid = false;
  float width_ratio = 0.f;               // 肩宽 / 画面宽（已做滑动中值平滑）
  float width_ratio_raw = 0.f;           // 未平滑值（诊断用）
  float wrist_to_nose = 0.f;             // 手腕到鼻子距离 / 肩宽（托腮）；99 = 门控未通过
  float wrist_to_nose_raw = 99.f;        // 未经门控的原始值（诊断用）
  float ear_ratio = 0.f;                 // 两耳间距 / 肩宽（诊断：0.43~0.68 为可信）
  bool wrist_above_shoulder = false;     // 是否有手腕抬到肩线之上（托腮门控）
  float wrist_y_rel = 0.f;               // 手腕相对肩线的高度（负=更高，按肩宽归一化）
  bool wrist_valid = false;              // 手腕关键点是否可信
  // 歪头判据：左右耳连线相对水平的角度。
  // 不用「肩中点->鼻子」的偏角 —— 俯视机位下 sy-nose.y 很小（分母趋零），
  // 横向一点点噪声就把角度放大几十度，实测波动 ±50°，完全不可用。
  // 耳线是一条横跨头部的基线，和肩线同样稳定（实测波动 6°）。
  float ear_angle_deg = 0.f;
  bool ear_angle_valid = false;
  float head_axis_deg = 0.f;             // 仅诊断用（俯视下不稳定，不参与判定）
  float person_confidence = 0.f;
  std::string invalid_reason;
};

// 一条规则的触发信息（供 ReminderManager 去重/冷却）
// 规则诊断快照（进 status JSON，供现场调参时看清「当前值 vs 阈值 vs 已持续」）
struct RuleDiag {
  std::string key;
  bool has_value = false;
  float value = 0.f;
  float trigger = 0.f;
  bool latched = false;
  double held_s = 0.0;
  double hold_s = 0.0;
};

struct RuleTrigger {
  std::string key;          // shoulder_tilt / head_down / head_tilt / too_close / hand_on_face
  std::string label_zh;     // 给界面看的中文名
  double active_s = 0.0;    // 已持续多少秒
  int priority = 0;         // 越大越优先
};

struct PostureObservation {
  PostureState state = PostureState::kUnknown;
  PostureMetrics metrics;
  std::string explanation;
  std::vector<RuleTrigger> triggers;   // 本帧新「激活」的规则（已去重，不会每帧重复）
  std::vector<std::string> active_keys;  // 当前仍处于激活态的规则
  std::vector<RuleDiag> rule_diags;      // 每条规则的「当前值 / 阈值 / 已持续」
  // 逐姿势画像分类结果（profile 模式启用时有效）
  bool profile_used = false;
  std::string profile_label;             // 命中标签；空 = 不在已采集样本范围内
  double profile_distance = 0.0;         // 兼容：= 1/(strength+1)
  double profile_strength = 0.0;         // 命中规则越阈值的倍数，越大越确定
  std::string profile_rule;              // 命中的规则描述
};

// 个人校准基准（§4.6）
struct Baseline {
  bool valid = false;
  double shoulder_angle_deg = 0.0;
  double head_offset_x = 0.0;
  double head_offset_y = 0.0;
  // 校准时的手腕高度（相对肩线，按肩宽归一化）。托腮判据用它作参考：
  // 手腕比这个高度抬高一定量 = 手抬起来了。
  double wrist_y_rel = 0.0;
  bool wrist_valid = false;
  // 耳中点相对肩线的高度。俯拍机位下比鼻子可靠得多：
  // 实测鼻子驱动的 head_offset_y 校准值在不同次校准间从 -0.34 跳到 -0.086，
  // 而耳中点稳定在 -0.60 左右。低头判据优先用它。
  double ear_mid_offset_y = 0.0;
  bool ear_mid_valid = false;
  double head_axis_deg = 0.0;
  double ear_angle_deg = 0.0;
  bool ear_angle_valid = false;   // 校准时耳朵是否可信（不可信则不用耳线判歪头）
  double width_ratio = 0.0;
  int samples = 0;
  int frame_width = 0;
  int frame_height = 0;
  std::string camera_id;
  std::string model_sha256;
  std::string created_at;   // ISO8601 墙上时间
};

}  // namespace posture
