// 用眼会话状态机单测（对准实施方案 §5.2 的状态图）
//
// 覆盖 TC-08（20min 触发）、TC-09（离开 10s 结束会话）、
// 以及「计时不依赖 FPS」这一点。
#include <cmath>
#include <cstdio>
#include <string>

#include "posture/EyeCareMonitor.hpp"
#include "posture/ReminderManager.hpp"

using namespace posture;

namespace {

int g_failures = 0;

void Check(bool cond, const std::string& what) {
  std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", what.c_str());
  if (!cond) ++g_failures;
}

SteadyTime At(double seconds) {
  return SteadyTime{} + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
}

void TestPresenceConfirm() {
  std::printf("-- 在座确认 2s 后进入 ACTIVE（§5.2）\n");
  Config cfg;
  cfg.presence_confirm_s = 2.f;
  cfg.absent_confirm_s = 10.f;
  EyeCareMonitor m(cfg);

  auto out = m.Update(true, At(0.0));
  Check(out.state == SessionState::kAbsent, "0s：还在 ABSENT");
  out = m.Update(true, At(1.0));
  Check(out.state == SessionState::kAbsent, "1s：仍未确认");
  out = m.Update(true, At(2.1));
  Check(out.state == SessionState::kActive, "2.1s：进入 ACTIVE");
}

void TestTwentyMinutes() {
  std::printf("-- 20 分钟触发一次休息提醒（TC-08）\n");
  Config cfg;
  cfg.presence_confirm_s = 2.f;
  cfg.absent_confirm_s = 10.f;
  cfg.continuous_use_s = 1200.f;
  EyeCareMonitor m(cfg);

  m.Update(true, At(0.0));
  m.Update(true, At(2.5));   // 进入 ACTIVE
  int rest_count = 0;
  // 用 1s 步长推进，模拟低帧率也要正确计时
  for (int i = 0; i <= 1210; ++i) {
    const auto out = m.Update(true, At(3.0 + i));
    if (out.rest_due_now) ++rest_count;
    if (out.rest_due_now) {
      Check(out.state == SessionState::kBreakDue, "触发时状态为 BREAK_DUE");
      Check(out.active_s >= 1199.0, "触发时累计用眼 >= 1200s");
    }
  }
  Check(rest_count == 1, "20 分钟内只触发一次（不会每帧重复）");
  Check(m.state() == SessionState::kBreakDue, "保持 BREAK_DUE 直到用户确认");

  m.Acknowledge();
  Check(m.state() == SessionState::kActive, "确认后回到 ACTIVE");
  const auto after = m.Update(true, At(1300.0));
  Check(after.active_s < 10.0, "确认后重新计时（清零）");
}

void TestLeaveAndReset() {
  std::printf("-- 离开 10s 结束会话（TC-09）\n");
  Config cfg;
  cfg.presence_confirm_s = 2.f;
  cfg.absent_confirm_s = 10.f;
  cfg.continuous_use_s = 1200.f;
  EyeCareMonitor m(cfg);

  m.Update(true, At(0.0));
  m.Update(true, At(2.5));
  for (int i = 1; i <= 30; ++i) m.Update(true, At(3.0 + i));
  const auto before = m.Update(true, At(34.0));
  Check(before.active_s > 30.0, "在座时累计用眼增长");

  // 短暂离开 5s：宽限，不清零
  m.Update(false, At(35.0));
  auto out = m.Update(false, At(40.0));
  Check(out.state == SessionState::kActive, "离开 5s 仍在 ACTIVE（宽限内）");
  Check(!out.session_reset, "未结束会话");

  // 离开超过 10s：会话结束并清零
  bool reset = false;
  for (int i = 1; i <= 12; ++i) {
    out = m.Update(false, At(40.0 + i));
    if (out.session_reset) reset = true;
  }
  Check(reset, "离开超过 10s 后 session_reset");
  Check(out.state == SessionState::kAbsent, "回到 ABSENT");
  const auto zero = m.Update(true, At(60.0));
  Check(zero.active_s == 0.0, "新会话从 0 开始计时");
}

void TestPaused() {
  std::printf("-- 暂停时不计时\n");
  Config cfg;
  cfg.presence_confirm_s = 1.f;
  cfg.continuous_use_s = 100.f;
  EyeCareMonitor m(cfg);
  m.Update(true, At(0.0));
  m.Update(true, At(1.5));
  m.SetPaused(true);
  for (int i = 1; i <= 20; ++i) m.Update(true, At(1.5 + i));
  const auto out = m.Update(true, At(22.0));
  Check(out.active_s < 2.0, "暂停 20s 后累计用眼几乎没涨");
}

void TestReminderCooldown() {
  std::printf("-- 同类提醒 60s 冷却 + 多规则合并（§5.3）\n");
  Config cfg;
  cfg.cooldown_s = 60.f;
  ReminderManager rm(cfg);

  std::vector<RuleTrigger> one{{"shoulder_tilt", "高低肩", 12.0, 40}};
  Notification n;
  Check(rm.Consider(one, At(0.0), &n), "首次触发可发通知");
  Check(n.title == "高低肩", "标题正确");
  Check(!rm.Consider(one, At(30.0), &n), "30s 内被冷却拦住");
  Check(rm.Consider(one, At(61.0), &n), "61s 后可以再发");

  // 两条同时成立 -> 合并，取高优先级标题
  std::vector<RuleTrigger> two{{"shoulder_tilt", "高低肩", 9.0, 40},
                               {"hand_on_face", "托腮", 6.0, 60}};
  ReminderManager rm2(cfg);
  Check(rm2.Consider(two, At(0.0), &n), "合并通知发出");
  Check(n.key == "hand_on_face", "取优先级最高的规则作主通知");
  Check(n.merged_keys.size() == 1 && n.merged_keys[0] == "高低肩", "另一条被合并进 detail");

  // 免打扰
  rm2.Snooze(900.0, At(100.0));
  Check(rm2.snoozed(At(200.0)), "免打扰生效中");
  Check(!rm2.Consider(two, At(200.0), &n), "免打扰期间不发提醒");
  Check(!rm2.snoozed(At(1100.0)), "15 分钟后自动解除");
}

}  // namespace

int main() {
  std::printf("=== eye care / reminder 单测 ===\n");
  TestPresenceConfirm();
  TestTwentyMinutes();
  TestLeaveAndReset();
  TestPaused();
  TestReminderCooldown();
  if (g_failures == 0) {
    std::printf("=== 全部通过 ===\n");
    return 0;
  }
  std::printf("=== %d 项失败 ===\n", g_failures);
  return 1;
}
