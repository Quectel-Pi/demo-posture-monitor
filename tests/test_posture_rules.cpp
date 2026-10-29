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

void TestTopDownCamera() {
  std::printf("-- 俯视机位（摄像头装在显示器上方往��拍）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.min_head_gap = 0.08f;
  Baseline baseline;
  baseline.valid = true;
  baseline.shoulder_angle_deg = 0.0;
  baseline.head_offset_y = -0.20;      // 俯视下头肩纵向本来就小
  baseline.ear_angle_valid = true;
  baseline.ear_angle_deg = 0.0;
  PostureAnalyzer analyzer(cfg, baseline);

  // 俯视下典型的合格帧：鼻子离肩线只有 0.25 个肩宽（直视机位会是 0.6+）
  PersonPose p{};
  p.person_confidence = 0.9f;
  p.box = BBox{180, 100, 300, 340};
  p.joints[kShoulderL] = Keypoint{250, 300, 0.9f};
  p.joints[kShoulderR] = Keypoint{410, 302, 0.9f};
  p.joints[kNose] = Keypoint{330, 220, 0.9f};      // (300-220)/160 = 0.5 → 通过
  p.joints[kEarL] = Keypoint{290, 205, 0.9f};
  p.joints[kEarR] = Keypoint{372, 207, 0.9f};
  p.joints[kWristL] = Keypoint{250, 460, 0.9f};    // 手在桌面（肩线 300 之下）
  p.joints[kWristR] = Keypoint{400, 462, 0.9f};

  PostureMetrics m = analyzer.ComputeMetrics(p, 640, 480);
  Check(m.valid, "俯视机位下 head_gap 偏小不会被误判为不可用");
  Check(m.ear_angle_valid, "双耳可信时算出耳线角");
  Check(std::fabs(m.ear_angle_deg) < 3.0f, "耳线接近水平（0°）");

  // ★ 手在桌面：手腕低于肩线 → 绝不能判托腮
  Check(m.wrist_to_nose > 90.f, "手放桌面（腕低于肩线）时 wrist_to_nose 置 99，不判托腮");

  // 真托腮：手抬到脸旁
  PersonPose face_p = p;
  face_p.joints[kWristL] = Keypoint{315, 235, 0.9f};   // 贴近鼻子
  const PostureMetrics fm = analyzer.ComputeMetrics(face_p, 640, 480);
  Check(fm.wrist_to_nose < 0.95f, "手抬到脸旁（腕高于肩线）时能识别托腮候选");

  // 歪头：耳线倾斜 25° → 应该超过 18° 阈值
  PersonPose tilt_p = p;
  tilt_p.joints[kEarL] = Keypoint{290, 190, 0.9f};     // 左耳抬高
  tilt_p.joints[kEarR] = Keypoint{372, 232, 0.9f};     // 右耳压低
  const PostureMetrics tm = analyzer.ComputeMetrics(tilt_p, 640, 480);
  Check(tm.ear_angle_valid, "歪头时耳线有效");
  const float dev = PostureAnalyzer::LineAngleDelta(tm.ear_angle_deg,
                                                    static_cast<float>(baseline.ear_angle_deg));
  Check(dev > cfg.head_tilt_deg, "耳线偏 " + std::to_string(static_cast<int>(dev)) +
                                 "° 超过阈值 " + std::to_string(static_cast<int>(cfg.head_tilt_deg)) + "°");

  // 低头：鼻子压到肩线上 → 应该判低头，而不是"几何不可用"
  PersonPose down_p = p;
  down_p.joints[kNose] = Keypoint{330, 285, 0.9f};     // head_gap=(300-285)/160≈0.09... 需 >0.08
  const PostureMetrics dm = analyzer.ComputeMetrics(down_p, 640, 480);
  Check(dm.valid, "低头帧仍被当作有效数据（不能丢）");
  const float drop = dm.head_offset_y - static_cast<float>(baseline.head_offset_y);
  Check(drop > 0.0f, "低头时头肩纵向相对基准确实减小（drop=" +
                     std::to_string(drop).substr(0, 5) + "）");

  // 低头阈值随基准缩放：俯视基准 -0.20 → 阈值约 0.11（不是固定 0.35）
  Check(std::fabs(analyzer.HeadDownThreshold() - 0.112f) < 0.02f,
        "俯视机位低头阈值按基准缩放为 " +
            std::to_string(analyzer.HeadDownThreshold()).substr(0, 5) + "（固定 0.35 则永远触发不了）");

  // 直视机位基准 -0.9 → 阈值被上限 0.45 截住
  Baseline far_base;
  far_base.valid = true;
  far_base.head_offset_y = -0.9;
  PostureAnalyzer far_analyzer(cfg, far_base);
  Check(std::fabs(far_analyzer.HeadDownThreshold() - 0.45f) < 0.01f,
        "直视机位低头阈值被上限截到 0.45");

  // 趴桌：鼻子压到肩线之下一点（head_gap 为负）仍然有效 —— 这是要检测的姿态
  PersonPose prone_p = p;
  prone_p.joints[kNose] = Keypoint{330, 315, 0.9f};   // 肩线 y≈301，鼻子更靠下
  const PostureMetrics pm = analyzer.ComputeMetrics(prone_p, 640, 480);
  Check(pm.valid, "鼻子压到肩线之下（趴桌）仍判为有效，交给 head_down 规则");

  // 只有低到胸口（-0.5 肩宽以下）才认为是错检
  PersonPose bad_p = p;
  bad_p.joints[kNose] = Keypoint{330, 430, 0.9f};     // 远低于肩线
  const PostureMetrics bm = analyzer.ComputeMetrics(bad_p, 640, 480);
  Check(!bm.valid, "鼻子跑到胸口位置时判为关键点错检、丢弃");
}

void TestSmoothing() {
  std::printf("-- width_ratio 滑动中值平滑（肩点抖动不该变成距离跳变）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  Baseline b;
  b.valid = true;
  b.head_offset_y = -0.30;
  b.ear_angle_valid = true;
  b.width_ratio = 0.42;
  b.reference_distance_cm = 40.0;
  PostureAnalyzer analyzer(cfg, b);

  auto make = [](float sw_norm) {
    PersonPose p{};
    p.person_confidence = 0.9f;
    p.box = BBox{60, 80, 520, 380};
    p.joints[kShoulderL] = Keypoint{100, 300, 0.9f};
    p.joints[kShoulderR] = Keypoint{100 + sw_norm * 640, 300, 0.9f};
    p.joints[kNose] = Keypoint{100 + sw_norm * 320, 180, 0.9f};
    p.joints[kEarL] = Keypoint{100 + sw_norm * 260, 165, 0.9f};
    p.joints[kEarR] = Keypoint{100 + sw_norm * 380, 167, 0.9f};
    p.joints[kWristL] = Keypoint{150, 460, 0.9f};
    p.joints[kWristR] = Keypoint{400, 460, 0.9f};
    return p;
  };

  SteadyTime t{};
  // 先喂稳定帧建立窗口
  for (int i = 0; i < 7; ++i) analyzer.Update({make(0.42f)}, t + std::chrono::seconds(i), 640, 480);
  const auto stable = analyzer.last_metrics();
  Check(std::fabs(stable.width_ratio - 0.42f) < 0.02f,
        "稳定 7 帧后平滑值 ≈ 0.42（" + std::to_string(stable.width_ratio).substr(0, 5) + "）");
  const float d_stable = stable.distance_cm_est;
  Check(std::fabs(d_stable - 40.f) < 2.f, "距离估算 ≈ 40cm");

  // 单帧离群（肩膀被检飞：0.42 → 0.62，正常会被算成近 27cm）
  auto out = analyzer.Update({make(0.62f)}, t + std::chrono::seconds(8), 640, 480);
  Check(std::fabs(out.metrics.width_ratio_raw - 0.62f) < 0.02f, "原始值确实跳到 0.62");
  Check(std::fabs(out.metrics.width_ratio - 0.42f) < 0.05f,
        "平滑后仍 ≈ 0.42（被中值滤掉）");
  Check(std::fabs(out.metrics.distance_cm_est - 40.f) < 4.f,
        "距离估算没有因单帧离群跳变（" +
            std::to_string(out.metrics.distance_cm_est).substr(0, 5) + "cm）");

  // 连续偏移（真的挪近了）要能跟上
  for (int i = 0; i < 8; ++i) {
    out = analyzer.Update({make(0.56f)}, t + std::chrono::seconds(10 + i), 640, 480);
  }
  Check(std::fabs(out.metrics.width_ratio - 0.56f) < 0.04f,
        "持续偏移时平滑值跟得上（" + std::to_string(out.metrics.width_ratio).substr(0, 5) + "）");
}

void TestDistanceEstimate() {
  std::printf("-- 离屏过近：相对基准 + 单目距离粗估\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.too_close_ratio_rel = 1.35f;
  cfg.reference_distance_cm = 40.f;

  Baseline b;
  b.valid = true;
  b.shoulder_angle_deg = 0.0;
  b.head_offset_y = -0.20;
  b.ear_angle_valid = true;
  b.width_ratio = 0.42;              // 40cm 处实测
  b.reference_distance_cm = 40.0;
  PostureAnalyzer analyzer(cfg, b);

  PersonPose p{};
  p.person_confidence = 0.9f;
  p.box = BBox{100, 80, 420, 380};
  p.joints[kShoulderL] = Keypoint{180, 300, 0.9f};
  p.joints[kShoulderR] = Keypoint{180 + 0.42f * 640, 300, 0.9f};   // 肩宽 = 0.42*640
  p.joints[kNose] = Keypoint{300, 220, 0.9f};
  p.joints[kEarL] = Keypoint{240, 205, 0.9f};
  p.joints[kEarR] = Keypoint{330, 207, 0.9f};
  p.joints[kWristL] = Keypoint{200, 460, 0.9f};
  p.joints[kWristR] = Keypoint{420, 460, 0.9f};

  const PostureMetrics m = analyzer.ComputeMetrics(p, 640, 480);
  Check(std::fabs(m.width_ratio - 0.42f) < 0.01f, "肩宽占比 ≈ 0.42");
  Check(m.distance_cm_valid, "有校准基准时给出距离估算");
  Check(std::fabs(m.distance_cm_est - 40.0f) < 1.5f,
        "距离估算 ≈ 40cm（实际 " + std::to_string(m.distance_cm_est).substr(0, 5) + "）");

  // 靠近一倍：width_ratio 翻倍 → 估算距离减半
  PersonPose near_p = p;
  const float sw2 = 0.84f * 640;
  near_p.joints[kShoulderL] = Keypoint{60, 300, 0.9f};
  near_p.joints[kShoulderR] = Keypoint{60 + sw2, 300, 0.9f};
  const PostureMetrics nm = analyzer.ComputeMetrics(near_p, 640, 480);
  Check(nm.distance_cm_valid && std::fabs(nm.distance_cm_est - 20.0f) < 1.5f,
        "肩宽翻倍时估算距离减半 ≈ 20cm（" +
            std::to_string(nm.distance_cm_est).substr(0, 5) + "）");

  // 关键：绝对值 0.75 在这个机位下要求贴脸（0.42→0.75 需近 1.77 倍 ≈ 22.6cm）；
  // 相对倍数 1.35 才是合理的「近了 26%」
  const float rel_th = 0.42f * 1.35f;
  Check(rel_th < 0.75f, "相对阈值 " + std::to_string(rel_th).substr(0, 4) +
                            " 明显严于绝对值 0.75（后者等于失效）");
}

void TestBaselineDrift() {
  std::printf("-- 基准漂移检测（挪了摄像头必须重新校准）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  Baseline baseline;
  baseline.valid = true;
  baseline.width_ratio = 0.11f;        // 校准时的取景（人很远）
  baseline.head_offset_y = -0.45f;
  PostureAnalyzer analyzer(cfg, baseline);

  PostureMetrics far = PostureMetrics{};
  far.valid = true;
  far.width_ratio = 0.12f;
  far.head_offset_y = -0.46f;
  Check(analyzer.CheckBaselineDrift(far).empty(), "取景一致时不报警");

  PostureMetrics near_ = PostureMetrics{};
  near_.valid = true;
  near_.width_ratio = 0.40f;           // 挪近后肩宽占比涨到 3.6 倍
  near_.head_offset_y = -0.20f;
  const std::string why = analyzer.CheckBaselineDrift(near_);
  Check(!why.empty(), "取景变化 3.6 倍时报告基准失效");
  Check(why.find("肩宽占比") != std::string::npos, "原因里说清是取景变了：" + why);
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
  TestTopDownCamera();
  TestSmoothing();
  TestDistanceEstimate();
  TestBaselineDrift();
  TestAnalyzerObservation();
  if (g_failures == 0) {
    std::printf("=== 全部通过 ===\n");
    return 0;
  }
  std::printf("=== %d 项失败 ===\n", g_failures);
  return 1;
}
