// 提醒去重 / 冷却 / 优先级（对应实施方案 §5.3 末段与 §11.3）
//
// 规则层已经做了「持续 N 秒才触发 + 恢复迟滞」，这里再加一层：
//   * 同类提醒 cooldown_s 内不重复（默认 60s）
//   * 「忽略 15 分钟」免打扰
//   * 同一时刻多条规则成立 -> 合并成一条通知，按 priority 取标题
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "posture/Config.hpp"
#include "posture/Types.hpp"

namespace posture {

struct Notification {
  std::string key;          // 主规则的 key
  std::string title;        // 中文标题，如「高低肩」
  std::string detail;       // 「持续 12s」
  int priority = 0;
  std::vector<std::string> merged_keys;   // 同时成立被合并进来的其它规则
  double active_s = 0.0;
};

class ReminderManager {
 public:
  explicit ReminderManager(const Config& cfg);

  // 输入本帧新激活的规则（PostureObservation::triggers），输出要不要发通知。
  bool Consider(const std::vector<RuleTrigger>& triggers, SteadyTime now,
                Notification* out);

  // UI classification is fast; notifications require this same posture to remain stable.
  bool ConsiderPosture(const PostureObservation& obs, SteadyTime now, Notification* out, std::uint64_t frame_id = 0);
  int bad_frames() const { return bad_frames_; }
  double posture_cooldown_remaining_s(SteadyTime now) const;
  const std::string& posture_reason() const { return posture_reason_; }

  // 免打扰：忽略若干分钟
  void Snooze(double seconds, SteadyTime now);
  void ClearSnooze();
  bool snoozed(SteadyTime now) const;

  // 休息提醒单独走一条通道（它不受 cooldown_s 约束，但同样受免打扰影响）
  bool ConsiderRestReminder(SteadyTime now, Notification* out);

  const std::map<std::string, SteadyTime>& last_notified() const { return last_notified_; }

 private:
  Config cfg_;
  int bad_frames_ = 0;
  int normal_frames_ = 0;
  std::uint64_t last_frame_id_ = 0;
  std::optional<SteadyTime> last_observation_at_;
  std::string current_posture_key_;
  std::string posture_reason_ = "waiting_frames";
  std::map<std::string, SteadyTime> last_notified_;
  std::optional<SteadyTime> snooze_until_;
  std::optional<SteadyTime> last_rest_;
  std::string pending_posture_key_;
  std::optional<SteadyTime> pending_posture_since_;
  // ★ 毛刺容忍。
  //   旧逻辑：状态一变就重置计时，于是稳定低头中偶尔一帧被判成「正常」
  //   就要重新数满门限 —— 实测同一姿势被拖到 4.9 秒才播报。
  //   新模型：候选键（candidate）单独计时，短时间内回到主键即视为抖动、
  //   主键计时不受影响；候选键持续超过容忍时长则扶正为新的主键。
  //   ★ 播报永远按【当前帧的键】用【它自己的】起始时间计，所以刚切换的
  //   姿势绝不会蹭用上一个姿势攒下的时长（踩过：会立即误播）。
  std::string candidate_key_;
  std::optional<SteadyTime> candidate_since_;
  static constexpr double kGlitchMaxS = 0.20;   // 约 5 帧 @ 25fps
};

}  // namespace posture
