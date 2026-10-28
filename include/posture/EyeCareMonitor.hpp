// 用眼会话 + 20-20-20 护眼提醒（对应实施方案 §5.1 / §5.2）
//
// 状态机（严格按 §5.2 的文字实现）：
//   ABSENT  --用户连续 presence_confirm_s 秒在座-->  ACTIVE
//   ACTIVE  --连续用眼 >= continuous_use_s-->      BREAK_DUE（发一次休息提醒）
//   ACTIVE  --无人连续 absent_confirm_s 秒-->      ABSENT（结束会话、清零）
//   BREAK_DUE --用户确认已休息 / 自动策略到期-->     ACTIVE（新一轮计时）
//   BREAK_DUE --无人连续 absent_confirm_s 秒-->    ABSENT
//
// 计时全部用 steady_clock，与摄像头 FPS 完全解耦（§3.2 关键原则）。
#pragma once

#include <optional>
#include <string>

#include "posture/Config.hpp"
#include "posture/Types.hpp"

namespace posture {

enum class SessionState { kAbsent, kActive, kBreakDue };

const char* ToString(SessionState state);

class EyeCareMonitor {
 public:
  enum class Policy { kManualAckOrSessionReset, kAutoRestart };

  struct Output {
    SessionState state = SessionState::kAbsent;
    double active_s = 0.0;        // 本会话已连续用眼秒数
    double remaining_s = 0.0;     // 距下次休息提醒还剩多少秒
    bool rest_due_now = false;    // 本帧刚触发休息提醒（只 true 一次）
    bool session_reset = false;   // 本帧因离开而结束了会话
    bool present = false;
    std::string reason;
  };

  EyeCareMonitor(const Config& cfg, double continuous_use_s_override = 0.0);

  Output Update(bool present, SteadyTime now);

  // 用户点「已休息」/ 界面确认
  void Acknowledge();
  // 暂停计时（例如夜间免打扰）
  void SetPaused(bool paused);
  bool paused() const { return paused_; }

  SessionState state() const { return state_; }
  double active_s(SteadyTime now) const;
  void Reset();

 private:
  static Policy ParsePolicy(const std::string& text);

  Config cfg_;
  double continuous_use_s_ = 1200.0;
  Policy policy_ = Policy::kManualAckOrSessionReset;
  SessionState state_ = SessionState::kAbsent;

  SteadyTime last_tick_{};
  bool has_tick_ = false;
  double active_accum_ = 0.0;          // 已累计用眼秒数（只在 present 时增长）
  std::optional<SteadyTime> present_since_;
  std::optional<SteadyTime> absent_since_;
  std::optional<SteadyTime> break_due_at_;
  bool paused_ = false;
};

}  // namespace posture
