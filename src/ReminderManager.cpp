#include "posture/ReminderManager.hpp"

#include <algorithm>
#include <cmath>

#include "posture/Logger.hpp"

namespace posture {

ReminderManager::ReminderManager(const Config& cfg) : cfg_(cfg) {}

bool ReminderManager::snoozed(SteadyTime now) const {
  if (!snooze_until_) return false;
  return now < *snooze_until_;
}

void ReminderManager::Snooze(double seconds, SteadyTime now) {
  snooze_until_ = now + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
  LOG_INFO("已进入免打扰 ", static_cast<int>(seconds), "s（期间不显示坐姿提醒）");
}

void ReminderManager::ClearSnooze() {
  snooze_until_.reset();
  LOG_INFO("免打扰已解除");
}

bool ReminderManager::Consider(const std::vector<RuleTrigger>& triggers, SteadyTime now,
                               Notification* out) {
  if (triggers.empty()) return false;
  if (snoozed(now)) {
    LOG_DEBUG("免打扰中，跳过 ", triggers.size(), " 条提醒");
    return false;
  }
  // 合并：取优先级最高的一条做主通知，其余作为 merged_keys
  const RuleTrigger* best = &triggers.front();
  for (const auto& t : triggers) {
    if (t.priority > best->priority) best = &t;
  }
  const auto it = last_notified_.find(best->key);
  if (it != last_notified_.end()) {
    const double since = std::chrono::duration<double>(now - it->second).count();
    if (since < cfg_.cooldown_s) {
      LOG_DEBUG("规则 ", best->key, " 冷却中（", static_cast<int>(since), "/",
                static_cast<int>(cfg_.cooldown_s), "s）");
      return false;
    }
  }

  Notification n;
  n.key = best->key;
  n.title = best->label_zh;
  n.priority = best->priority;
  n.active_s = best->active_s;
  n.detail = "持续 " + std::to_string(static_cast<int>(best->active_s)) + "s";
  for (const auto& t : triggers) {
    if (t.key != best->key) n.merged_keys.push_back(t.label_zh);
  }
  if (!n.merged_keys.empty()) {
    std::string extra;
    for (size_t i = 0; i < n.merged_keys.size(); ++i) {
      if (i) extra += "、";
      extra += n.merged_keys[i];
    }
    n.detail += "；同时：" + extra;
  }

  last_notified_[best->key] = now;
  if (out) *out = n;
  return true;
}

bool ReminderManager::ConsiderRestReminder(SteadyTime now, Notification* out) {
  if (snoozed(now)) {
    LOG_DEBUG("免打扰中，跳过护眼休息提醒");
    return false;
  }
  // 休息提醒的节流只受 continuous_use_s 约束（由 EyeCareMonitor 保证），
  // 这里只防「同一秒重复发」。
  if (last_rest_) {
    const double since = std::chrono::duration<double>(now - *last_rest_).count();
    if (since < 5.0) return false;
  }
  last_rest_ = now;
  Notification n;
  n.key = "rest";
  n.title = "该远眺休息了";
  n.detail = "连续用眼已达 " + std::to_string(static_cast<int>(cfg_.continuous_use_s)) +
             "s，请看向 6 米外至少 " + std::to_string(static_cast<int>(cfg_.far_look_s)) + "s";
  n.priority = 100;
  n.active_s = cfg_.continuous_use_s;
  if (out) *out = n;
  return true;
}

}  // namespace posture
