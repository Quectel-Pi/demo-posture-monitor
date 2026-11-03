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
  cfg.posture_reminder_frames = 0;
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
  cfg.posture_reminder_frames = 0;
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
  cfg.posture_reminder_frames = 0;
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
  cfg.posture_reminder_frames = 0;
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
  cfg.posture_reminder_frames = 0;
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

void TestRestRetryAndCountdown() {
  Config cfg;
  cfg.posture_reminder_frames = 0;
  cfg.cooldown_s = 60;
  cfg.presence_confirm_s = 0;
  cfg.continuous_use_s = 3;
  cfg.far_look_s = 7;
  EyeCareMonitor eye(cfg);
  eye.Update(true, At(0));
  eye.Update(true, At(1));
  eye.Update(true, At(2));
  auto out = eye.Update(true, At(3));
  Check(out.rest_due_now && out.rest_remaining_s == 7, "休息开始使用配置中的倒计时");
  out = eye.Update(true, At(8));
  Check(out.rest_remaining_s == 2, "倒计时按服务时钟推进，重新连接无需重计");
  ReminderManager reminders(cfg);
  Notification n;
  reminders.Snooze(10, At(0));
  Check(!reminders.ConsiderRestReminder(At(3), &n), "免打扰中暂缓护眼语音");
  Check(reminders.ConsiderRestReminder(At(10), &n), "免打扰结束后能补发护眼提醒");
  Check(!reminders.ConsiderRestReminder(At(15), &n), "持续到期状态不每 5 秒重复播报");
  Check(reminders.ConsiderRestReminder(At(70), &n), "未休息时 60 秒后再次提示");
}

void TestFastFeedbackStableSpeech() {
  Config cfg;
  cfg.posture_reminder_frames = 0;
  cfg.posture_reminder_hold_s = 2.0;
  ReminderManager reminders(cfg);
  PostureObservation obs;
  obs.state = PostureState::kHeadDown;
  obs.metrics.valid = true;
  Notification notify;
  Check(!reminders.ConsiderPosture(obs, At(0.5), &notify),
        "界面已识别低头时，语音不立即打断");
  Check(!reminders.ConsiderPosture(obs, At(2.4), &notify), "不足 2 秒稳定异常不播报");
  Check(reminders.ConsiderPosture(obs, At(2.5), &notify), "稳定异常 2 秒后播报");
  Check(notify.key == "head_down", "语音对应当前姿势");
  Check(!reminders.ConsiderPosture(obs, At(5), &notify), "稳定异常仍受冷却约束");

  obs.state = PostureState::kHeadTilt;
  Check(!reminders.ConsiderPosture(obs, At(6), &notify), "切换姿势重新等待语音稳定");
  obs.state = PostureState::kNormal;
  reminders.ConsiderPosture(obs, At(7), &notify);
  obs.state = PostureState::kHeadTilt;
  Check(!reminders.ConsiderPosture(obs, At(8), &notify), "恢复后短动作不沿用旧语音计时");
  obs.metrics.valid = false;
  reminders.ConsiderPosture(obs, At(9), &notify);
  obs.metrics.valid = true;
  Check(!reminders.ConsiderPosture(obs, At(10), &notify), "丢点后语音重新防抖");
  Check(reminders.ConsiderPosture(obs, At(12), &notify), "恢复有效输入后可正常播报新姿势");
}

void TestNoiseDoesNotDelayReminder() {
  std::printf("-- 偶发噪声不该拖慢播报\n");
  // 实测踩过：稳定低头中偶尔一帧被判「正常」，旧逻辑把计时清零，
  // 同一次低头被拖到 4.9 秒才播报。
  Config cfg;
  cfg.posture_reminder_frames = 0;
  cfg.posture_reminder_hold_s = 0.5;
  ReminderManager r(cfg);
  PostureObservation obs;
  obs.metrics.valid = true;
  Notification n;
  obs.state = PostureState::kHeadDown;
  Check(!r.ConsiderPosture(obs, At(0.0), &n), "起计，未满 0.5s");
  Check(!r.ConsiderPosture(obs, At(0.2), &n), "0.2s 未满");
  // 单帧噪声：判成正常（无提醒键）
  obs.state = PostureState::kNormal;
  Check(!r.ConsiderPosture(obs, At(0.25), &n), "噪声帧本身不播报");
  obs.state = PostureState::kHeadDown;
  // 噪声后应当在原计时基础上很快播报，而不是重新数 0.5s
  Check(r.ConsiderPosture(obs, At(0.6), &n),
        "噪声后 0.6s 就播报（按原计时，不被单帧噪声清零）");
  Check(n.key == "head_down", "播报的是低头");
}

void TestRealPoseSwitchStillWaits() {
  std::printf("-- 真换姿势必须重新计时（不能蹭用上一个姿势的时长）\n");
  Config cfg;
  cfg.posture_reminder_frames = 0;
  cfg.posture_reminder_hold_s = 0.5;
  ReminderManager r(cfg);
  PostureObservation obs;
  obs.metrics.valid = true;
  Notification n;
  // 先让低头攒够时长但不播报（用大冷却前先播一次占用冷却）
  obs.state = PostureState::kHeadDown;
  r.ConsiderPosture(obs, At(0.0), &n);
  r.ConsiderPosture(obs, At(1.0), &n);      // 这一帧应播报低头
  // 换成歪头：必须重新等 0.5s，不能立刻播
  obs.state = PostureState::kHeadTilt;
  Check(!r.ConsiderPosture(obs, At(1.1), &n), "刚换歪头时立即播报 = 蹭用旧计时（不该发生）");
  Check(!r.ConsiderPosture(obs, At(1.4), &n), "歪头 0.3s 未满门限");
  Check(r.ConsiderPosture(obs, At(1.8), &n), "歪头稳定 0.7s 后播报");
  Check(n.key == "head_tilt", "播报的是歪头");
}

void TestTenBadFrames() {
  for (const auto state : {PostureState::kHeadDown, PostureState::kHeadTilt,
       PostureState::kShoulderTilt, PostureState::kTooClose, PostureState::kHandOnFace}) {
    Config cfg;
    cfg.posture_reminder_frames = 10;
    ReminderManager r(cfg);
    PostureObservation o;
    o.state = state; o.metrics.valid = true;
    Notification n;
    for (int i=1; i<10; ++i)
      Check(!r.ConsiderPosture(o, At(i*.04), &n, i), "前 9 个不良帧不播报");
    Check(!r.ConsiderPosture(o, At(.39), &n, 9), "重复 frame_id 不参与帧数累计");
    Check(r.ConsiderPosture(o, At(.40), &n, 10), "第 10 个不良帧立即产生提醒，无额外秒数等待");
    Check(!r.ConsiderPosture(o, At(.44), &n, 11), "持续异常仍受冷却约束");
    Check(std::fabs(r.posture_cooldown_remaining_s(At(.44)) - (cfg.cooldown_s-.04)) < .001, "第10帧播报后报告真实剩余冷却时间");
    Check(r.posture_cooldown_remaining_s(At(cfg.cooldown_s+1))==0, "冷却到期显示0而非负数");
  }
  Config cfg; cfg.posture_reminder_frames = 10;
  ReminderManager r(cfg);
  PostureObservation o; o.metrics.valid=true; o.state=PostureState::kHeadDown;
  Notification n;
  for(int i=1;i<=9;++i) r.ConsiderPosture(o,At(i*.04),&n,i);
  o.state=PostureState::kNormal;r.ConsiderPosture(o,At(.4),&n,10);
  Check(r.bad_frames()==0,"正常坐姿清空不良帧计数");
  o.state=PostureState::kHeadDown;
  for(int i=11;i<20;++i) Check(!r.ConsiderPosture(o,At(i*.04),&n,i),"恢复异常需重新累计");
  o.metrics.valid=false;r.ConsiderPosture(o,At(.8),&n,20);
  Check(r.bad_frames()==0,"无效/多人帧不能累计语音提醒");
  o.metrics.valid=true;
  for(int i=21;i<30;++i) {
    o.state=(i%2) ? PostureState::kHeadTilt : PostureState::kHeadDown;
    Check(!r.ConsiderPosture(o,At(i*.04),&n,i),"不良类型切换仍累计连续不良帧");
  }
  Check(r.ConsiderPosture(o,At(1.2),&n,30),"持续不良不因类型互跳而无限重计");
  o.state=PostureState::kNormal;
  for(int i=31;i<=40;++i) r.ConsiderPosture(o,At(i*.04),&n,i);
  o.state=PostureState::kHeadDown;
  for(int i=41;i<50;++i) Check(!r.ConsiderPosture(o,At(i*.04),&n,i),"新一轮异常也不能早于第10帧");
  Check(r.ConsiderPosture(o,At(2),&n,50),"确实恢复正常后，新一轮不良10帧可重新播报，不被上一轮60秒冷却吞掉");
}

}  // namespace

int main() {
  std::printf("=== eye care / reminder 单测 ===\n");
  TestPresenceConfirm();
  TestTenBadFrames();
  TestRestRetryAndCountdown();
  TestTwentyMinutes();
  TestLeaveAndReset();
  TestPaused();
  TestReminderCooldown();
  TestFastFeedbackStableSpeech();
  TestNoiseDoesNotDelayReminder();
  TestRealPoseSwitchStillWaits();
  if (g_failures == 0) {
    std::printf("=== 全部通过 ===\n");
    return 0;
  }
  std::printf("=== %d 项失败 ===\n", g_failures);
  return 1;
}
