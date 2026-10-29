// 坐姿分析（对应实施方案 §4 + §11.2/§11.3）
//
// 职责边界（§6.2 明确要求）：只做几何特征与状态机，不弹窗、不写日志通知。
//   PostureAnalyzer  -> PostureObservation（含 metrics + 本帧新激活的规则）
//   ReminderManager  -> 冷却/去重/优先级后发给 Notifier
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "posture/Config.hpp"
#include "posture/Types.hpp"

namespace posture {

// 「持续成立 N 秒才激活、恢复 M 秒才清除」的迟滞规则（§11.3）
struct HoldRuleCfg {
  std::string key;
  std::string label_zh;
  float trigger = 0.f;      // 触发阈值
  float recover = 0.f;      // 恢复阈值（迟滞带）
  double hold_s = 0.0;      // 需要持续多久才激活
  double recover_hold_s = 0.0;  // 恢复后需要持续多久才解除
  bool higher_is_bad = true;
  int priority = 0;         // 通知优先级（越大越先显示）
};

class HoldRule {
 public:
  struct Result {
    bool valid = false;         // 本帧有没有有效输入
    bool latched = false;       // 当前是否处于激活态
    bool just_activated = false;
    bool just_cleared = false;
    double active_s = 0.0;      // 已激活多久
  };

  explicit HoldRule(HoldRuleCfg cfg) : cfg_(std::move(cfg)) {}

  Result Update(std::optional<float> value, SteadyTime now);
  void Reset();
  // 运行期调整阈值（低头阈值依赖校准基准，基准变了要跟着变）
  void SetTrigger(float trigger, float recover) {
    cfg_.trigger = trigger;
    cfg_.recover = recover;
  }
  float trigger() const { return cfg_.trigger; }

  bool latched() const { return latched_; }
  const HoldRuleCfg& cfg() const { return cfg_; }

 private:
  HoldRuleCfg cfg_;
  bool latched_ = false;
  std::optional<SteadyTime> abnormal_since_;
  std::optional<SteadyTime> recovery_since_;
  std::optional<SteadyTime> latched_at_;
};

// 主用户选择结果
struct PrimarySelection {
  const PersonPose* person = nullptr;
  bool ambiguous = false;     // 多人尺寸接近 -> 不做个体判定（§11.4 / TC-11）
  int plausible_count = 0;
  std::string reason;
  // 有检出但全被几何闸门拒掉时，这里放「最像人那个」被拒的原因，
  // 用于把「无人」和「有人但机位不对」区分开（否则用户只会看到"无人"）。
  std::string reject_reason;
  bool saw_detection = false;
  BBox reject_person_box;
};

class PostureAnalyzer {
 public:
  PostureAnalyzer(const Config& cfg, const Baseline& baseline);

  void SetBaseline(const Baseline& baseline);
  // 当前生效的低头阈值（随校准基准自动缩放，供自检/日志展示）
  float HeadDownThreshold() const;
  bool HasBaseline() const { return baseline_.valid; }
  const Baseline& baseline() const { return baseline_; }

  // 主循环调用：每帧一次。persons 为引擎输出（已按置信度排序）。
  PostureObservation Update(const std::vector<PersonPose>& persons, SteadyTime now,
                            int frame_width, int frame_height);

  // 校准采样：返回是否拿到有效样本
  bool CalibrateFeed(const std::vector<PersonPose>& persons, int frame_width,
                     int frame_height, PostureMetrics* out_metrics);

  // 单帧几何特征（也供校准与自检直接调用）
  PostureMetrics ComputeMetrics(const PersonPose& person, int frame_width,
                                int frame_height) const;

  PrimarySelection SelectPrimary(const std::vector<PersonPose>& persons, int frame_width,
                                 int frame_height) const;

  const PostureMetrics& last_metrics() const { return last_metrics_; }
  void ResetRules();

  // 检测「当前画面」和「校准基准」是否还对应同一个机位/距离。
  //
  // 为什么需要：BaselineMatches() 只能比对 camera_id / 分辨率 / 模型 SHA256，
  // 这些都不变时（典型情况：人把摄像头挪了位置，摄像头还是同一个），
  // 它会认为基准有效，于是继续拿旧基准判定 —— 所有阈值都算错。
  // width_ratio（肩宽占画面宽）对「人-摄像头距离」最敏感，是这里的主力判据。
  //
  // 返回空字符串 = 一致；非空 = 漂移原因（可直接给用户看）。
  std::string CheckBaselineDrift(const PostureMetrics& m) const;

  // 归一化直线角到 [-90, 90)，两条线的方向差再折叠一次（§4.2 强调的周期折叠）
  static float NormalizeLineAngle(float deg);
  static float LineAngleDelta(float current, float baseline);
  static float NormalizeAngle180(float deg);

 private:
  bool Plausible(const PostureMetrics& m, const PersonPose& p, int frame_width,
                 int frame_height) const;

  Config cfg_;
  Baseline baseline_;
  PostureMetrics last_metrics_;

  HoldRule shoulder_rule_;
  HoldRule head_down_rule_;
  HoldRule head_tilt_rule_;
  HoldRule too_close_rule_;
  HoldRule hand_face_rule_;

  std::optional<SteadyTime> last_valid_at_;

  // width_ratio 的滑动中值窗口。
  // 为什么要平滑：肩宽是「左肩-右肩」两点之差，w8a8 量化下任一肩点抖动
  // 几十像素会直接放大成 ±39% 的 width_ratio 波动（实测 0.377→0.524），
  // 而人实际距离没变。距离是慢变量，中值滤波几乎不引入延迟但能压掉离群帧。
  std::array<float, 7> wr_window_{};
  int wr_count_ = 0;
  int wr_next_ = 0;

  float SmoothWidthRatio(float raw);
  void ResetSmoother();
};

}  // namespace posture
