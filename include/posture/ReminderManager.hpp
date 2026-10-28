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

  // 免打扰：忽略若干分钟
  void Snooze(double seconds, SteadyTime now);
  void ClearSnooze();
  bool snoozed(SteadyTime now) const;

  // 休息提醒单独走一条通道（它不受 cooldown_s 约束，但同样受免打扰影响）
  bool ConsiderRestReminder(SteadyTime now, Notification* out);

  const std::map<std::string, SteadyTime>& last_notified() const { return last_notified_; }

 private:
  Config cfg_;
  std::map<std::string, SteadyTime> last_notified_;
  std::optional<SteadyTime> snooze_until_;
  std::optional<SteadyTime> last_rest_;
};

}  // namespace posture
