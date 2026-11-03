// 坐姿分析（对应实施方案 §4 + §11.2/§11.3）
//
// 职责边界（§6.2 明确要求）：只做几何特征与状态机，不弹窗、不写日志通知。
//   PostureAnalyzer  -> PostureObservation（含 metrics + 本帧新激活的规则）
//   ReminderManager  -> 冷却/去重/优先级后发给 Notifier
#pragma once

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "posture/Config.hpp"
#include "posture/PostureProfile.hpp"
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
    // ---- 现场诊断用（不影响判定逻辑）----
    float value = 0.f;          // 本帧输入到规则的量
    double held_s = 0.0;        // 越线已持续多久（未激活时=计时中；激活后=激活时长）
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
  // 拒因分类：现场需要用同一类原因做稳定反馈，不能每帧换一种说法
  enum RejectKind { kRejectNone = 0, kRejectHeadcut, kRejectTooBig, kRejectGeometry };
  RejectKind reject_kind = kRejectNone;
  bool saw_detection = false;
  BBox reject_person_box;
};

class PostureAnalyzer {
 public:
  PostureAnalyzer(const Config& cfg, const Baseline& baseline);

  void SetBaseline(const Baseline& baseline);

  // ---- 逐姿势校准画像（per-posture profile）----
  // 启用后，姿态状态由「最近原型分类 + 标签保持 N 秒」决定，
  // 不再使用手调阈值。规则仍会继续计算，仅用于诊断对照。
  void SetProfile(const PostureProfile* profile, double hold_s, double accept_radius,
                  double release_ratio = 0.5);
  // 逐类覆盖保持门限（秒，<=0 = 用统一值）
  void SetHoldOverrides(double hand_face_s, double head_tilt_s, double head_down_s,
                        double too_close_s) {
    profile_hold_hand_face_s_ = hand_face_s;
    profile_hold_head_tilt_s_ = head_tilt_s;
    profile_hold_head_down_s_ = head_down_s;
    profile_hold_too_close_s_ = too_close_s;
  }
  void ClearProfile();
  bool profile_active() const { return profile_ != nullptr && !profile_->empty(); }
  const std::vector<std::string>& profile_labels() const { return profile_label_names_; }
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

  // 摄像头绕光轴转动（roll）的提示 —— 只告警，不自动重新校准。
  //
  // 为什么和 CheckBaselineDrift 分开：CheckBaselineDrift 命中会作废基准并自动重校准，
  // 但「肩线偏 29°」有两种可能：
  //   a) 摄像头被转动/位置歪了  → 应该重校准
  //   b) 用户本人正歪着坐        → 绝不能重校准，否则等于把坏姿势当成正常，之后永远不报警
  // 系统分不清 a 和 b，所以只提示，由人来判断。
  std::string CheckRollHint(const PostureMetrics& m) const;

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

  // ---- 画像分类状态 ----
  const PostureProfile* profile_ = nullptr;
  double profile_hold_s_ = 2.0;
  // 逐类保持门限（0 = 用 profile_hold_s_）
  double profile_hold_hand_face_s_ = 0.0;
  double profile_hold_head_tilt_s_ = 0.0;
  double profile_hold_head_down_s_ = 0.0;
  double profile_hold_too_close_s_ = 0.0;
  // 某标签实际使用的保持门限
  double HoldForLabel(const std::string& label) const;
  double profile_accept_radius_ = 3.0;
  double profile_release_ratio_ = 0.5;  // 迟滞比例，0 = 关闭
  std::string pending_label_;
  SteadyTime pending_since_{};
  std::string active_label_;              // 已采纳的标签（空 = normal/未定）

  // ---- 时间窗投票（替代「连续保持 N 秒」）----
  // 旧做法：同一标签必须无间隔地连续保持 N 秒才采纳。问题是一个噪声帧
  // （关键点跳动、瞬时遮挡）就会把 pending_since_ 重置，于是 N 必须设得很大
  // 才能稳（实测只得 1.5~2.5s）—— 延迟和精度被绑死。
  // 新做法：只看最近 kLabelWindowS 秒内各标签占了多少帧，"占多数"就采纳。
  // 偶尔一帧噪声不会把票数清零，所以窗口可以设得又短又稳。
  //   · 进入快速：窗口 0.6s，80% 帧（约 12 帧）支持就切
  //   · 退出良好：窗口 1.2s，70% 帧支持就切回正常（防抖仍强）
  struct LabelVote {
    std::string label;
    SteadyTime at{};
  };
  std::deque<LabelVote> votes_;
  static constexpr double kEnterWindowS = 0.60;
  static constexpr double kExitWindowS = 1.20;
  static constexpr double kEnterRatio = 0.80;
  static constexpr double kExitRatio = 0.70;
  // 在窗口内某标签的帧数与总帧数
  void TallyVotes(SteadyTime now, double window_s, const std::string& label,
                  int* hit, int* total) const;
  bool VoteAccepts(SteadyTime now, const std::string& label, bool is_exit) const;

  std::vector<std::string> profile_label_names_;
  bool ProfileUpdate(const PostureMetrics& m, SteadyTime now, PostureObservation* obs);
  void MarkProfileUncertain(SteadyTime now);
  std::optional<SteadyTime> profile_uncertain_since_;

  // width_ratio 的滑动中值窗口。
  // 为什么要平滑：肩宽是「左肩-右肩」两点之差，w8a8 量化下任一肩点抖动
  // 几十像素会直接放大成 ±39% 的 width_ratio 波动（实测 0.377→0.524），
  // 而人实际距离没变。3 帧中值保留单帧离群抑制，阶跃响应约一帧，避免大窗口拖慢反馈。
  static constexpr int kSmoothWindow = 3;
  std::array<float, kSmoothWindow> wr_window_{};
  int wr_count_ = 0;
  int wr_next_ = 0;
  // 耳线角同样做中值平滑：实测单帧会从 -9.5° 跳到 +15.9°（耳点检歪），
  // 不平滑则歪头判据只能靠运气连上 5 秒。
  std::array<float, kSmoothWindow> ear_window_{};
  int ear_count_ = 0;
  int ear_next_ = 0;

  float SmoothWidthRatio(float raw);
  float SmoothEarAngle(float raw, bool valid);
  void ResetSmoother();
};

}  // namespace posture
