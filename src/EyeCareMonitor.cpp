#include "posture/EyeCareMonitor.hpp"

#include <algorithm>
#include <cmath>

#include "posture/Logger.hpp"

namespace posture {

const char* ToString(SessionState state) {
  switch (state) {
    case SessionState::kAbsent: return "ABSENT";
    case SessionState::kActive: return "ACTIVE";
    case SessionState::kBreakDue: return "BREAK_DUE";
  }
  return "?";
}

EyeCareMonitor::EyeCareMonitor(const Config& cfg, double continuous_use_s_override)
    : cfg_(cfg) {
  continuous_use_s_ = continuous_use_s_override > 0 ? continuous_use_s_override
                                                    : cfg.continuous_use_s;
  policy_ = ParsePolicy(cfg.reminder_policy);
}

EyeCareMonitor::Policy EyeCareMonitor::ParsePolicy(const std::string& text) {
  if (text == "auto_restart") return Policy::kAutoRestart;
  return Policy::kManualAckOrSessionReset;
}

void EyeCareMonitor::Reset() {
  state_ = SessionState::kAbsent;
  active_accum_ = 0.0;
  present_since_.reset();
  absent_since_.reset();
  break_due_at_.reset();
  has_tick_ = false;
}

void EyeCareMonitor::Acknowledge() {
  if (state_ != SessionState::kBreakDue) return;
  state_ = SessionState::kActive;
  active_accum_ = 0.0;
  break_due_at_.reset();
  present_since_.reset();
  absent_since_.reset();
  LOG_INFO("用户确认已休息，开始新一轮 ", static_cast<int>(continuous_use_s_), "s 计时");
}

void EyeCareMonitor::SetPaused(bool paused) {
  if (paused_ == paused) return;
  paused_ = paused;
  LOG_INFO(paused ? "护眼计时已暂停" : "护眼计时已恢复");
}

double EyeCareMonitor::active_s(SteadyTime now) const {
  if (state_ == SessionState::kAbsent) return 0.0;
  if (state_ == SessionState::kBreakDue) return active_accum_;
  return active_accum_;
}

EyeCareMonitor::Output EyeCareMonitor::Update(bool present, SteadyTime now) {
  Output out;
  out.present = present;

  if (!has_tick_) {
    has_tick_ = true;
    last_tick_ = now;
  }
  const double dt = std::max(0.0, std::chrono::duration<double>(now - last_tick_).count());
  last_tick_ = now;
  // 防止系统休眠/暂停后一次性灌入巨大 dt
  const double dt_clamped = std::min(dt, 2.0);

  switch (state_) {
    case SessionState::kAbsent: {
      if (!present) {
        absent_since_.reset();
        present_since_.reset();
        out.reason = "无人";
        break;
      }
      if (!present_since_) present_since_ = now;
      const double held = std::chrono::duration<double>(now - *present_since_).count();
      if (held >= cfg_.presence_confirm_s) {
        state_ = SessionState::kActive;
        active_accum_ = 0.0;
        present_since_.reset();
        absent_since_.reset();
        LOG_INFO("检测到用户已在座，开始用眼计时（阈值 ",
                 static_cast<int>(continuous_use_s_), "s）");
      } else {
        out.reason = "确认在座中";
      }
      break;
    }
    case SessionState::kActive: {
      if (present) {
        absent_since_.reset();
        if (!paused_) active_accum_ += dt_clamped;
      } else {
        if (!absent_since_) absent_since_ = now;
        const double gone = std::chrono::duration<double>(now - *absent_since_).count();
        if (gone >= cfg_.absent_confirm_s) {
          state_ = SessionState::kAbsent;
          active_accum_ = 0.0;
          absent_since_.reset();
          present_since_.reset();
          out.session_reset = true;
          out.reason = "离开超时，会话结束";
          LOG_INFO("用户离开超过 ", static_cast<int>(cfg_.absent_confirm_s),
                   "s，本轮用眼会话结束并清零");
        } else {
          out.reason = "短暂离开，宽限中";
        }
      }
      if (state_ == SessionState::kActive && active_accum_ >= continuous_use_s_) {
        state_ = SessionState::kBreakDue;
        break_due_at_ = now;
        out.rest_due_now = true;
        out.reason = "达到连续用眼阈值";
        LOG_INFO("连续用眼 ", static_cast<int>(active_accum_),
                 "s 达到阈值，触发护眼休息提醒（远眺 ",
                 static_cast<int>(cfg_.far_look_s), "s）");
      }
      break;
    }
    case SessionState::kBreakDue: {
      const double since_due =
          break_due_at_ ? std::chrono::duration<double>(now - *break_due_at_).count() : 0.0;
      if (!present) {
        if (!absent_since_) absent_since_ = now;
        const double gone = std::chrono::duration<double>(now - *absent_since_).count();
        if (gone >= cfg_.absent_confirm_s) {
          state_ = SessionState::kAbsent;
          active_accum_ = 0.0;
          absent_since_.reset();
          break_due_at_.reset();
          out.session_reset = true;
          out.reason = "休息期间离开，会话结束";
        }
      } else {
        absent_since_.reset();
      }
      // 自动策略：休息够 far_look_s 就自动开始新一轮（默认需要用户确认）
      if (state_ == SessionState::kBreakDue && policy_ == Policy::kAutoRestart &&
          since_due >= cfg_.far_look_s) {
        state_ = SessionState::kActive;
        active_accum_ = 0.0;
        break_due_at_.reset();
        LOG_INFO("自动策略：休息 ", static_cast<int>(cfg_.far_look_s), "s 已到，开始新一轮计时");
      }
      break;
    }
  }

  out.state = state_;
  out.active_s = active_accum_;
  out.remaining_s = std::max(0.0, continuous_use_s_ - active_accum_);
  return out;
}

}  // namespace posture
