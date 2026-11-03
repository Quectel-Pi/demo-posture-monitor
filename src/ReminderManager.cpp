#include "posture/ReminderManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
  // 文案不再写「持续 Ns」：门限降到 0.5s 后这个值就是 0~1 秒，
  // 对用户毫无意义（实测输出过「持续 0s」）。语音里只播姿势名，
  // 文字只给一句可操作的建议。
  if (best->active_s >= 3.0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "已持续 %.0f 秒，请调整姿势", best->active_s);
    n.detail = buf;
  } else {
    n.detail = "请调整成正确姿势";
  }
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

double ReminderManager::posture_cooldown_remaining_s(SteadyTime now) const {
  const auto it = last_notified_.find(current_posture_key_);
  if (it == last_notified_.end()) return 0.0;
  return std::max(0.0, cfg_.cooldown_s -
      std::chrono::duration<double>(now - it->second).count());
}

bool ReminderManager::ConsiderPosture(const PostureObservation& obs, SteadyTime now,
                                       Notification* out, std::uint64_t frame_id) {
  std::string key;
  switch (obs.state) {
    case PostureState::kHandOnFace: key = "hand_on_face"; break;
    case PostureState::kHeadDown: key = "head_down"; break;
    case PostureState::kShoulderTilt: key = "shoulder_tilt"; break;
    case PostureState::kHeadTilt: key = "head_tilt"; break;
    case PostureState::kTooClose: key = "too_close"; break;
    default: break;
  }
  current_posture_key_ = key;
  if (cfg_.posture_reminder_frames > 0) {
    if (frame_id != 0 && frame_id == last_frame_id_) return false;
    last_frame_id_ = frame_id;
    if (last_observation_at_ &&
        std::chrono::duration<double>(now - *last_observation_at_).count() > cfg_.missing_data_timeout_s) {
      bad_frames_ = 0;
      normal_frames_ = 0;
      pending_posture_since_.reset();
    }
    last_observation_at_ = now;
    if (!obs.metrics.valid || key.empty()) {
      if (obs.metrics.valid && obs.state == PostureState::kNormal) {
        normal_frames_ = std::min(normal_frames_ + 1, cfg_.posture_reminder_frames);
        // After a genuine recovery a new bad-posture episode deserves a fresh
        // warning; cooldown only suppresses repeats within the same episode.
        if (normal_frames_ == cfg_.posture_reminder_frames) last_notified_.clear();
      } else normal_frames_ = 0;
      bad_frames_ = 0;
      pending_posture_since_.reset();
      posture_reason_ = obs.metrics.valid && obs.state == PostureState::kNormal ? "normal" : "invalid_pose";
      return false;
    }
    normal_frames_ = 0;
    if (bad_frames_ == 0) pending_posture_since_ = now;
    bad_frames_ = std::min(bad_frames_ + 1, cfg_.posture_reminder_frames);
    if (bad_frames_ < cfg_.posture_reminder_frames) {
      posture_reason_ = "waiting_frames";
      return false;
    }
    // Switching between bad labels does not erase an uninterrupted bad-posture streak.
    if (snoozed(now)) { posture_reason_ = "snoozed"; return false; }
    const double held = std::chrono::duration<double>(now - *pending_posture_since_).count();
    const bool emitted = Consider({{key, ToChinese(obs.state), held, 50}}, now, out);
    posture_reason_ = emitted ? "notified" : "cooldown";
    return emitted;
  }
  // ★ 必须区分「姿势正常」和「数据无效」：
  //   正常姿势没有提醒 key（空），旧代码把它和「指标无效」一起处理成
  //   “清空计时”，于是稳定低头中偶尔一帧判成正常就把计时清零 ——
  //   实测同一姿势被拖到 4.9 秒才播报。这里给正常姿势一个占位键，
  //   让它走毛刺容忍路径，而不是无条件清零。
  if (!obs.metrics.valid) {
    // 真的拿不到指标（丢点/多人/出框）：全部重置，不沿用旧计时
    pending_posture_key_.clear();
    pending_posture_since_.reset();
    candidate_key_.clear();
    candidate_since_.reset();
    return false;
  }
  // ★ 区分「姿势正常」和「数据无效」：正常姿势没有提醒 key（空），
  //   旧代码把它和「指标无效」一起当成清空计时，于是稳定低头中偶尔一帧
  //   判成正常就把计时清零 —— 实测同一姿势被拖到 4.9 秒才播报。
  if (key.empty()) key = "__ok__";

  if (key == pending_posture_key_) {
    // 回到主键：若候选是抖动，丢弃即可，主键计时一直保留
    candidate_key_.clear();
    candidate_since_.reset();
  } else {
    // 与主键不同：开始/继续跟踪候选键
    if (key != candidate_key_) {
      candidate_key_ = key;
      candidate_since_ = now;
    }
    const double cand_s =
        std::chrono::duration<double>(now - *candidate_since_).count();
    if (pending_posture_key_.empty() || cand_s >= kGlitchMaxS) {
      // 没有主键（首次）或候选已站稳：扶正为新主键，从这里重新计时
      pending_posture_key_ = key;
      pending_posture_since_ = *candidate_since_;
      candidate_key_.clear();
      candidate_since_.reset();
    } else {
      // 仍在容忍窗口内：本帧不参与播报判定，等它稳定或回弹
      return false;
    }
  }

  if (!pending_posture_since_) pending_posture_since_ = now;
  const double held = std::chrono::duration<double>(now - *pending_posture_since_).count();
  if (key == "__ok__") return false;          // 正常姿势不产生提醒
  if (held < cfg_.posture_reminder_hold_s) return false;
  return Consider({{key, ToChinese(obs.state), held, 50}}, now, out);
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
    if (since < std::max(60.0, static_cast<double>(cfg_.cooldown_s))) return false;
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
