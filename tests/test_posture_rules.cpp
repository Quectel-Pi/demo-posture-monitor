// 坐姿规则单测（不需要摄像头/NPU）：对准实施方案 §11.2/§11.3 的行为
//
// 覆盖 TC-05（瞬间倾斜不报警）、TC-06（恢复迟滞）、TC-07（关键点缺失 -> UNKNOWN）
#include <cassert>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "posture/PostureAnalyzer.hpp"

using namespace posture;

namespace {

int g_failures = 0;

void Check(bool cond, const std::string& what) {
  if (!cond) {
    std::printf("  [FAIL] %s\n", what.c_str());
    ++g_failures;
  } else {
    std::printf("  [ ok ] %s\n", what.c_str());
  }
}

SteadyTime At(double seconds) {
  return SteadyTime{} + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
}

void TestNormalizeLineAngle() {
  std::printf("-- 肩线角度周期折叠（§4.2）\n");
  Check(std::fabs(PostureAnalyzer::NormalizeLineAngle(95.f) + 85.f) < 1e-3,
        "95° 折叠成 -85°");
  Check(std::fabs(PostureAnalyzer::NormalizeLineAngle(-95.f) - 85.f) < 1e-3,
        "-95° 折叠成 85°");
  Check(std::fabs(PostureAnalyzer::NormalizeLineAngle(190.f) - 10.f) < 1e-3,
        "190° 折叠成 10°");
  // 左右肩顺序反过来时，角度差算出来是同一根线
  const float a = PostureAnalyzer::NormalizeLineAngle(5.f);
  const float b = PostureAnalyzer::NormalizeLineAngle(5.f + 180.f);
  Check(std::fabs(PostureAnalyzer::LineAngleDelta(a, b)) < 1e-3,
        "180° 反向的同一根线差值 = 0");
}

void TestHoldRule() {
  std::printf("-- 持续异常 + 恢复迟滞（§11.3）\n");
  HoldRuleCfg cfg;
  cfg.key = "shoulder_tilt";
  cfg.label_zh = "高低肩";
  cfg.trigger = 12.f;
  cfg.recover = 8.f;
  cfg.hold_s = 8.0;
  cfg.recover_hold_s = 3.0;
  cfg.higher_is_bad = true;
  HoldRule rule(cfg);

  // 瞬时倾斜 1s：不报警（TC-05）
  auto r = rule.Update(20.f, At(0.0));
  Check(!r.latched, "0s 20° 未激活");
  r = rule.Update(20.f, At(1.0));
  Check(!r.latched && !r.just_activated, "1s 20° 仍未激活（未达 8s 门限）");

  // 回到正常 -> 计时清零
  r = rule.Update(3.f, At(2.0));
  Check(!r.latched, "回到 3° 后未激活");

  // 持续满 8s 才激活（计时从第一次越线的样本开始）
  rule.Reset();
  for (int i = 0; i <= 7; ++i) r = rule.Update(20.f, At(10.0 + i));
  Check(!r.latched, "持续 7s 时仍未激活（未达 8s 门限）");
  r = rule.Update(20.f, At(18.0));
  Check(r.just_activated && r.latched, "持续满 8s 时激活并 just_activated");
  r = rule.Update(20.f, At(19.0));
  Check(!r.just_activated, "激活后不会每帧重复 just_activated");

  // 迟滞：降到 10°（低于 trigger 但高于 recover）保持激活
  r = rule.Update(10.f, At(19.0));
  Check(r.latched, "降到 10°（迟滞带内）仍保持激活");

  // 恢复到 8° 以下需持续 3s 才解除（TC-06）
  r = rule.Update(5.f, At(20.0));
  Check(r.latched, "首次回到 5° 仍保持（恢复计时中）");
  r = rule.Update(5.f, At(22.5));
  Check(r.latched, "恢复 2.5s 仍保持");
  r = rule.Update(5.f, At(23.5));
  Check(!r.latched && r.just_cleared, "恢复满 3s 后解除");

  // 无效输入不新增异常（TC-07）
  rule.Reset();
  for (int i = 0; i <= 20; ++i) r = rule.Update(std::nullopt, At(30.0 + i));
  Check(!r.latched, "全程无有效数据时不会激活");
}

void TestMetricsAndPlausibility() {
  std::printf("-- 几何特征与主用户选择（§4.1/§4.3）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  Baseline baseline;
  baseline.valid = true;
  baseline.shoulder_angle_deg = 2.0;
  baseline.head_offset_y = -0.80;
  baseline.head_axis_deg = 1.0;
  PostureAnalyzer analyzer(cfg, baseline);

  PersonPose p{};
  p.person_confidence = 0.9f;
  p.box = BBox{200, 150, 240, 320};
  p.joints[kShoulderL] = Keypoint{260, 300, 0.9f};
  p.joints[kShoulderR] = Keypoint{380, 306, 0.9f};   // 略微倾斜
  p.joints[kNose] = Keypoint{318, 230, 0.9f};
  p.joints[kWristL] = Keypoint{240, 390, 0.9f};
  p.joints[kWristR] = Keypoint{400, 395, 0.9f};

  const PostureMetrics m = analyzer.ComputeMetrics(p, 1280, 800);
  Check(m.valid, "关键点齐全时指标有效");
  const float sw = std::hypot(380.f - 260.f, 306.f - 300.f);
  Check(std::fabs(m.shoulder_width_px - sw) < 0.5f, "肩宽计算正确");
  Check(std::fabs(m.width_ratio - sw / 1280.f) < 1e-4, "宽度比 = 肩宽/画面宽");
  const float expect_oy = (230.f - 303.f) / sw;
  Check(std::fabs(m.head_offset_y - expect_oy) < 1e-4, "头肩纵向偏移归一化正确");

  // 缺肩点 -> 指标无效（不能当成正常）
  PersonPose no_shoulder = p;
  no_shoulder.joints[kShoulderL].confidence = 0.0f;
  const PostureMetrics bad = analyzer.ComputeMetrics(no_shoulder, 1280, 800);
  Check(!bad.valid, "肩点不可信时指标无效且带原因");
  Check(!bad.invalid_reason.empty(), "无效原因非空");

  // ROI 之外的人不算主用户
  PersonPose away = p;
  away.box = BBox{5, 5, 60, 70};
  std::vector<PersonPose> persons{away};
  const auto sel = analyzer.SelectPrimary(persons, 1280, 800);
  Check(sel.person == nullptr && sel.plausible_count == 0, "ROI 外的框被排除");

  // 同一个人被多尺度重复检出（框中心抖动 20px）-> 不算多人，仍能判定
  PersonPose dup = p;
  dup.box = BBox{215, 158, 236, 316};
  dup.person_confidence = 0.88f;
  std::vector<PersonPose> dup_persons{p, dup};
  const auto sel_dup = analyzer.SelectPrimary(dup_persons, 1280, 800);
  Check(sel_dup.person != nullptr && !sel_dup.ambiguous && sel_dup.plausible_count == 1,
        "同一人的重复框被合并，不误判为多人");

  // 真的有两个人尺寸接近 -> ambiguous，不做个体判定（TC-11）
  PersonPose p2 = p;
  p2.box = BBox{520, 150, 240, 320};
  p2.joints[kShoulderL] = Keypoint{580, 300, 0.9f};
  p2.joints[kShoulderR] = Keypoint{700, 300, 0.9f};
  p2.joints[kNose] = Keypoint{640, 230, 0.9f};
  std::vector<PersonPose> two{p, p2};
  const auto sel2 = analyzer.SelectPrimary(two, 1280, 800);
  Check(sel2.ambiguous, "两人尺寸接近时标记 ambiguous");
}

void TestAnalyzerObservation() {
  std::printf("-- 端到端判定（含未校准行为）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.too_close_ratio = 0.9f;      // 本测试不触发距离规则
  cfg.too_close_hold_s = 5;
  cfg.shoulder_hold_s = 8;
  cfg.recovery_hold_s = 3;

  Baseline baseline;
  baseline.valid = true;
  baseline.shoulder_angle_deg = 0.0;
  baseline.head_offset_y = -0.80;
  baseline.head_axis_deg = 0.0;
  PostureAnalyzer analyzer(cfg, baseline);

  auto make_person = [](float shoulder_dy) {
    PersonPose p{};
    p.person_confidence = 0.9f;
    p.box = BBox{300, 150, 300, 360};
    p.joints[kShoulderL] = Keypoint{340, 300, 0.9f};
    p.joints[kShoulderR] = Keypoint{460, 300 + shoulder_dy, 0.9f};
    p.joints[kNose] = Keypoint{400, 220, 0.9f};
    p.joints[kWristL] = Keypoint{330, 400, 0.9f};
    p.joints[kWristR] = Keypoint{470, 400, 0.9f};
    return p;
  };

  // 无人 -> USER_AWAY
  auto obs = analyzer.Update({}, At(0.0), 1280, 800);
  Check(obs.state == PostureState::kUserAway, "空画面 -> USER_AWAY");

  // 正常 -> NORMAL
  std::vector<PersonPose> normal{make_person(0.f)};
  obs = analyzer.Update(normal, At(1.0), 1280, 800);
  Check(obs.state == PostureState::kNormal, "坐姿正常 -> NORMAL");

  // 肩线歪斜 ~20°（120px 宽、44px 落差 -> 20.1°），持续 9s -> SHOULDER_TILT
  std::vector<PersonPose> tilted{make_person(44.f)};
  bool triggered = false;
  for (int i = 0; i <= 9; ++i) {
    obs = analyzer.Update(tilted, At(10.0 + i), 1280, 800);
    for (const auto& t : obs.triggers) {
      if (t.key == "shoulder_tilt") triggered = true;
    }
  }
  Check(triggered, "肩线偏 20° 持续 9s 后触发 shoulder_tilt");
  Check(obs.state == PostureState::kShoulderTilt, "主状态为 SHOULDER_TILT");

  // 没有基准时不判高低肩（§4.6）
  PostureAnalyzer no_base(cfg, Baseline{});
  bool any_trigger = false;
  for (int i = 0; i <= 12; ++i) {
    obs = no_base.Update(tilted, At(100.0 + i), 1280, 800);
    any_trigger = any_trigger || !obs.triggers.empty();
  }
  Check(!any_trigger, "没有校准基准时不触发高低肩/低头规则");
}

}  // namespace

int main() {
  std::printf("=== posture rules 单测 ===\n");
  TestNormalizeLineAngle();
  TestHoldRule();
  TestMetricsAndPlausibility();
  TestAnalyzerObservation();
  if (g_failures == 0) {
    std::printf("=== 全部通过 ===\n");
    return 0;
  }
  std::printf("=== %d 项失败 ===\n", g_failures);
  return 1;
}
