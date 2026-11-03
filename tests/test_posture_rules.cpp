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

  // 低头阈值随基准缩放（俯视基准 -0.20 → 0.20*0.25=0.05 被下限 0.10 兜住）
  Check(std::fabs(analyzer.HeadDownThreshold() - 0.10f) < 0.02f,
        "俯视机位（基准 -0.20）低头阈值被下限兜到 " +
            std::to_string(analyzer.HeadDownThreshold()).substr(0, 5));

  // 实测：明显低头时耳中点从 -0.603 降到 -0.445（降 0.158 = 26%）
  // ratio 0.25 → 阈值 0.151，正好能覆盖
  Baseline mid_base;
  mid_base.valid = true;
  mid_base.ear_mid_valid = true;
  mid_base.ear_mid_offset_y = -0.603;
  PostureAnalyzer mid_analyzer(cfg, mid_base);
  const float th = mid_analyzer.HeadDownThreshold();
  Check(std::fabs(th - 0.151f) < 0.01f,
        "实测机位（耳中基准 -0.603）阈值 = " + std::to_string(th).substr(0, 5) +
            "，实降 0.158 可以覆盖");

  // 直视机位基准 -0.9 → 0.9*0.25=0.225，被上限 0.35 兜住吗？不，仍在范围内
  Baseline far_base;
  far_base.valid = true;
  far_base.head_offset_y = -1.5;
  PostureAnalyzer far_analyzer(cfg, far_base);
  Check(far_analyzer.HeadDownThreshold() <= 0.35f,
        "基准很大时阈值被上限 0.35 兜住（" +
            std::to_string(far_analyzer.HeadDownThreshold()).substr(0, 5) + "）");

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

void TestWristGateRelative() {
  std::printf("-- 托腮门控：相对校准腕高（俯拍机位）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.hand_face_wrist_rise = 0.35f;
  cfg.hand_face_ratio = 0.95f;
  cfg.min_size_ratio = 0.55f;
  cfg.too_close_ratio_rel = 1.5f;   // 本测试不关心

  Baseline b;
  b.valid = true;
  b.shoulder_angle_deg = 0.0;
  b.head_offset_y = -0.30;
  b.ear_angle_valid = true;
  b.width_ratio = 0.42;
  b.wrist_valid = true;
  b.wrist_y_rel = 1.19;             // 校准时手在桌面：比肩线低 1.19 个肩宽
  PostureAnalyzer analyzer(cfg, b);

  // sw = 0.42*640 = 268.8 px；sy = 300
  const float sw = 0.42f * 640.f;
  auto make = [&](float wrist_y) {
    PersonPose p{};
    p.person_confidence = 0.9f;
    p.box = BBox{60, 80, 480, 380};
    p.joints[kShoulderL] = Keypoint{120, 300, 0.9f};
    p.joints[kShoulderR] = Keypoint{120 + sw, 300, 0.9f};
    p.joints[kNose] = Keypoint{120 + sw * 0.5f, 180, 0.9f};
    p.joints[kEarL] = Keypoint{120 + sw * 0.25f, 165, 0.9f};
    p.joints[kEarR] = Keypoint{120 + sw * 0.75f, 167, 0.9f};
    p.joints[kWristL] = Keypoint{120 + sw * 0.5f, wrist_y, 0.9f};   // 这只手离鼻子最近
    p.joints[kWristR] = Keypoint{60, wrist_y + 40, 0.9f};           // 另一只更远
    return p;
  };

  // 手放桌面：腕高 ≈ 肩线 + 1.19*sw → 门控不过
  const PostureMetrics desk = analyzer.ComputeMetrics(make(300 + 1.19f * sw), 640, 800);
  const PostureMetrics cropped = analyzer.ComputeMetrics(make(300 + 1.19f * sw), 640, 480);
  Check(!cropped.wrist_valid, "画面外的高置信度手腕必须拒绝，不能拿外推点判托腮");
  Check(desk.wrist_valid, "手在桌面时手腕关键点有效");
  Check(!desk.wrist_above_shoulder, "手在桌面 → 门控不过");
  Check(desk.wrist_to_nose > 90.f, "手在桌面 → wrist_to_nose 置 99，不判托腮");
  Check(desk.wrist_to_nose_raw < 90.f, "但原始值仍然记录（诊断用）");

  // 托腮：腕抬到肩线下方 0.38*sw（俯拍下到不了肩线之上），鼻子也在附近
  const float wrist_y = 300 + 0.38f * sw;
  PersonPose face_p = make(wrist_y);
  face_p.joints[kNose] = Keypoint{120 + sw * 0.46f, wrist_y - 30, 0.9f};  // 手在脸旁
  const PostureMetrics face = analyzer.ComputeMetrics(face_p, 640, 480);
  Check(face.wrist_above_shoulder,
        "腕抬到肩线下 0.38 肩宽也门控通过（相对基准门控）");
  Check(face.wrist_to_nose < 0.95f,
        "托腮时候选手腕-鼻子距离 " + std::to_string(face.wrist_to_nose).substr(0, 4)
            + " < 0.95");
}

void TestWristSentinelConsistency() {
  std::printf("-- 手腕不可信时 wrist_valid / wrist_to_nose_raw 必须一致\n");
  // 回归：历史数据里出现过 wrist_valid=true 而 wrist_to_nose_raw 停在哨兵 99
  // 的帧（22 帧托腮里 13 帧如此），导致下游 guard 把托腮当「手离脸很远」排除。
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.hand_face_wrist_rise = 0.20f;
  cfg.min_size_ratio = 0.55f;
  Baseline b;
  b.valid = true;
  b.shoulder_angle_deg = 0.0;
  b.head_offset_y = -0.30;
  b.ear_angle_valid = true;
  b.width_ratio = 0.42f;
  b.wrist_valid = true;
  b.wrist_y_rel = 1.19f;
  PostureAnalyzer analyzer(cfg, b);
  const float sw = 0.42f * 640.f;

  auto base_pose = [&]() {
    PersonPose p{};
    p.person_confidence = 0.9f;
    p.box = BBox{60, 80, 480, 380};
    p.joints[kShoulderL] = Keypoint{120, 300, 0.9f};
    p.joints[kShoulderR] = Keypoint{120 + sw, 300, 0.9f};
    p.joints[kNose] = Keypoint{120 + sw * 0.5f, 180, 0.9f};
    p.joints[kEarL] = Keypoint{120 + sw * 0.25f, 165, 0.9f};
    p.joints[kEarR] = Keypoint{120 + sw * 0.75f, 167, 0.9f};
    return p;
  };

  // 两只手腕都不可信 -> 两个量必须同时回到哨兵
  PersonPose none = base_pose();
  none.joints[kWristL] = Keypoint{0, 0, 0.0f};
  none.joints[kWristR] = Keypoint{0, 0, 0.0f};
  const PostureMetrics m0 = analyzer.ComputeMetrics(none, 640, 480);
  Check(!m0.wrist_valid, "手腕都不可信 -> wrist_valid=false");
  Check(m0.wrist_to_nose_raw > 90.f,
        "手腕都不可信 -> wrist_to_nose_raw 必须是哨兵（不能是 0）");

  // 坐标外推到离谱值（超出 kMaxWristToNose）也不能算有效
  PersonPose far = base_pose();
  far.joints[kWristL] = Keypoint{120 + sw * 20.f, 300, 0.9f};
  far.joints[kWristR] = Keypoint{120 - sw * 20.f, 300, 0.9f};
  const PostureMetrics m1 = analyzer.ComputeMetrics(far, 640, 480);
  Check(!m1.wrist_valid,
        "手腕坐标外推到离谱位置 -> 不算有效（不能把垃圾坐标当托腮依据）");

  // 有效手腕 -> 两个量都有值
  PersonPose ok = base_pose();
  ok.joints[kWristL] = Keypoint{120 + sw * 0.5f, 300 + 0.38f * sw, 0.9f};
  ok.joints[kWristR] = Keypoint{60, 400, 0.9f};
  const PostureMetrics m2 = analyzer.ComputeMetrics(ok, 640, 480);
  Check(m2.wrist_valid, "有效手腕 -> wrist_valid=true");
  Check(m2.wrist_to_nose_raw < 90.f,
        "有效手腕 -> wrist_to_nose_raw 有真实值（不是哨兵）");
}

void TestFramingGate() {
  std::printf("-- 取景门：按关键点是否入画判断（不是按人体框位置）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.min_size_ratio = 0.55f;
  cfg.frame_max_person_w = 0.90f;
  Baseline b;
  b.valid = true;
  b.head_offset_y = -0.30;
  b.ear_angle_valid = true;
  b.width_ratio = 0.35;
  PostureAnalyzer analyzer(cfg, b);

  // nk_y / nk_x：把关键点整体偏移，模拟出框
  auto person = [](float box_y, float box_h, float box_w,
                   float kp_dy = 0.f, bool drop_ears = false) {
    PersonPose p{};
    p.person_confidence = 0.9f;
    p.box = BBox{60, box_y, box_w, box_h};
    p.joints[kShoulderL] = Keypoint{160, 300 + kp_dy, 0.9f};
    p.joints[kShoulderR] = Keypoint{384, 302 + kp_dy, 0.9f};
    p.joints[kNose] = Keypoint{270, 200 + kp_dy, 0.9f};
    p.joints[kEarL] = Keypoint{190, 190 + kp_dy, 0.9f};
    p.joints[kEarR] = Keypoint{350, 192 + kp_dy, 0.9f};
    p.joints[kWristL] = Keypoint{180, 430, 0.9f};
    p.joints[kWristR] = Keypoint{400, 430, 0.9f};
    if (drop_ears) {   // 模拟耳朵出画
      p.joints[kEarL] = Keypoint{190, -20, 0.9f};
      p.joints[kEarR] = Keypoint{350, -18, 0.9f};
    }
    return p;
  };

  // ① 只拍到头的桌面机位：框顶在画面外 -142，但鼻子/耳/肩都在画面内 → 必须通过
  {
    std::vector<PersonPose> head_only{person(-142, 470, 224)};
    const auto sel = analyzer.SelectPrimary(head_only, 640, 480);
    Check(sel.person != nullptr,
          "框顶在画面外 -142px 但关键点都在画面内 → 通过（这是桌面机位的常态）");
  }
  // ② 框很宽（胳膊张开）不再被拒 —— 框宽不等于肩宽
  {
    std::vector<PersonPose> wide{person(20, 330, 600)};
    const auto sel = analyzer.SelectPrimary(wide, 640, 480);
    Check(sel.person != nullptr,
          "框占画面宽 94%（胳膊张开）不拒 —— 双肩关键点在画面里就够了");
  }
  // ③ 鼻子出画（关键点上移到 y<0）
  {
    std::vector<PersonPose> nose_out{person(20, 330, 224, -260.f)};
    const auto sel = analyzer.SelectPrimary(nose_out, 640, 480);
    Check(sel.person == nullptr, "鼻子关键点出画 → 拒绝");
  }
  // ④ 耳朵出画：帧仍然可用（低头/凑近/托腮照样能判），只是耳线角不可信
  {
    std::vector<PersonPose> ear_out{person(20, 330, 224, 0.f, true)};
    const auto sel = analyzer.SelectPrimary(ear_out, 640, 480);
    Check(sel.person != nullptr,
          "双耳出画不废整帧（只是歪头判不了，别把其它姿势一起拖死）");
  }
  // ⑤ 正常取景
  {
    std::vector<PersonPose> ok{person(30, 330, 224)};
    const auto sel = analyzer.SelectPrimary(ok, 640, 480);
    Check(sel.person != nullptr, "正常取景通过");
  }
}

void TestSizeGate() {
  std::printf("-- 尺寸门：人走后椅子被当成人\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.min_size_ratio = 0.55f;
  Baseline b;
  b.valid = true;
  b.head_offset_y = -0.30;
  b.ear_angle_valid = true;
  b.width_ratio = 0.35;             // 正常坐着时肩宽占画面 35%
  PostureAnalyzer analyzer(cfg, b);

  auto fake = [](float sw_norm, float box_w, float box_h) {
    PersonPose p{};
    p.person_confidence = 0.9f;
    p.box = BBox{200, 60, box_w, box_h};   // 中心落在 ROI 内
    p.joints[kShoulderL] = Keypoint{240, 300, 0.9f};
    p.joints[kShoulderR] = Keypoint{240 + sw_norm * 640, 302, 0.9f};
    p.joints[kNose] = Keypoint{240 + sw_norm * 320, 200, 0.9f};
    p.joints[kEarL] = Keypoint{240 + sw_norm * 260, 190, 0.9f};
    p.joints[kEarR] = Keypoint{240 + sw_norm * 380, 192, 0.9f};
    p.joints[kWristL] = Keypoint{260, 430, 0.9f};
    p.joints[kWristR] = Keypoint{420, 430, 0.9f};
    return p;
  };

  // 假阳性：宽占比 0.12（远小于基准 0.35 的 55% = 0.1925）
  std::vector<PersonPose> blob{fake(0.12f, 120, 160)};
  const auto sel = analyzer.SelectPrimary(blob, 640, 480);
  Check(sel.person == nullptr, "小框假阳性（宽占比 0.12）被尺寸门拒绝 → 判 USER_AWAY");

  // 真用户：宽占比 0.35，占画面高 65%（合格取景）
  std::vector<PersonPose> real{fake(0.35f, 380, 312)};
  const auto sel2 = analyzer.SelectPrimary(real, 640, 480);
  Check(sel2.person != nullptr, "正常尺寸的用户通过尺寸门");
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
  // 单帧离群（肩膀被检飞：0.42 → 0.62，正常会被算成近 27cm）
  auto out = analyzer.Update({make(0.62f)}, t + std::chrono::seconds(8), 640, 480);
  Check(std::fabs(out.metrics.width_ratio_raw - 0.62f) < 0.02f, "原始值确实跳到 0.62");
  Check(std::fabs(out.metrics.width_ratio - 0.42f) < 0.05f,
        "平滑后仍 ≈ 0.42（被中值滤掉），从而 too_close 不会被单帧带偏");

  // 两个连续新样本即可改变 3 帧中值；无需积攒旧的 7 帧窗口。
  analyzer.Update({make(0.56f)}, t + std::chrono::milliseconds(8100), 640, 480);
  out = analyzer.Update({make(0.56f)}, t + std::chrono::milliseconds(8200), 640, 480);
  Check(std::fabs(out.metrics.width_ratio - 0.56f) < 0.04f,
        "3 帧中值在连续两帧新位置后跟上移动");

  // 连续偏移（真的挪近了）要能跟上
  for (int i = 0; i < 8; ++i) {
    out = analyzer.Update({make(0.56f)}, t + std::chrono::seconds(10 + i), 640, 480);
  }
  Check(std::fabs(out.metrics.width_ratio - 0.56f) < 0.04f,
        "持续偏移时平滑值跟得上（" + std::to_string(out.metrics.width_ratio).substr(0, 5) + "）");
}

void TestTooCloseRelative() {
  std::printf("-- 离屏过近：相对基准的倍数（不测距）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.too_close_ratio_rel = 1.35f;
  cfg.too_close_hold_s = 5.0f;

  Baseline b;
  b.valid = true;
  b.shoulder_angle_deg = 0.0;
  b.head_offset_y = -0.20;
  b.ear_angle_valid = true;
  b.width_ratio = 0.42;              // 校准时（本机实测）
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

  // 基准 0.42 → 阈值 0.42*1.35 = 0.567。与机位无关，不是写死的 0.75。
  const float expect_th = 0.42f * 1.35f;
  Check(expect_th < 0.75f, "相对阈值 0.567 明显严于旧的绝对值 0.75");

  SteadyTime t{};
  // 坐在略近但仍未超阈值的位置（0.50）：不应触发
  bool fired = false;
  for (int i = 0; i < 10; ++i) {
    const auto o = analyzer.Update({make(0.50f)}, t + std::chrono::seconds(i), 640, 480);
    for (const auto& tr : o.triggers) {
      if (tr.key == "too_close") fired = true;
    }
  }
  Check(!fired, "0.50 < 阈值 0.567，不触发 too_close");

  // 明显更近（0.60 > 0.567），持续 6s 触发
  analyzer.SetBaseline(b);   // 复位规则
  SteadyTime t2 = t + std::chrono::seconds(20);
  for (int i = 0; i < 8; ++i) {
    const auto o = analyzer.Update({make(0.60f)}, t2 + std::chrono::seconds(i), 640, 480);
    for (const auto& tr : o.triggers) {
      if (tr.key == "too_close") fired = true;
    }
  }
  Check(fired, "0.60 > 阈值 0.567，持续后触发 too_close");
}

void TestBaselineDrift() {
  std::printf("-- 基准漂移检测（挪了摄像头必须重新校准）\n");
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  Baseline baseline;
  baseline.valid = true;
  baseline.width_ratio = 0.25f;        // 校准时的取景
  baseline.head_offset_y = -0.45f;
  PostureAnalyzer analyzer(cfg, baseline);

  PostureMetrics far = PostureMetrics{};
  far.valid = true;
  far.width_ratio = 0.26f;
  far.head_offset_y = -0.46f;
  Check(analyzer.CheckBaselineDrift(far).empty(), "取景一致时不报警");

  PostureMetrics near_ = PostureMetrics{};
  near_.valid = true;
  near_.width_ratio = 0.48f;           // 挪近后肩宽占比涨到 1.9 倍
  near_.head_offset_y = -0.20f;
  const std::string why = analyzer.CheckBaselineDrift(near_);
  Check(!why.empty(), "取景变化 1.9 倍时报告基准失效");
  Check(why.find("肩宽占比") != std::string::npos, "原因里说清是取景变了：" + why);

  // 历史坏基准（宽占比 0.07）必须被判失效 —— 只看倍数会漏掉
  Baseline bad = baseline;
  bad.width_ratio = 0.07f;
  PostureAnalyzer bad_analyzer(cfg, bad);
  PostureMetrics normal_m = PostureMetrics{};
  normal_m.valid = true;
  normal_m.width_ratio = 0.36f;
  normal_m.head_offset_y = -0.45f;
  const std::string why2 = bad_analyzer.CheckBaselineDrift(normal_m);
  Check(!why2.empty() && why2.find("不合理") != std::string::npos,
        "基准宽占比 0.07 直接被判失效：" + why2);
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

void TestInterruptedDetection() {
  Config cfg;
  cfg.shoulder_hold_s = 2.0;
  cfg.missing_data_timeout_s = 1.5;
  Baseline base;
  base.valid = true;
  base.width_ratio = 0.09375;
  base.head_offset_y = -0.5;
  PersonPose p{};
  p.person_confidence = 0.9f;
  p.box = {200, 100, 240, 360};
  p.joints[kNose] = {320, 220, 0.9f};
  p.joints[kShoulderL] = {260, 300, 0.9f};
  p.joints[kShoulderR] = {380, 344, 0.9f};
  PostureAnalyzer analyzer(cfg, base);
  analyzer.Update({p}, At(0), 1280, 800);
  analyzer.Update({p}, At(1), 1280, 800);
  analyzer.Update({}, At(1.2), 1280, 800);
  auto obs = analyzer.Update({p}, At(2.1), 1280, 800);
  Check(obs.triggers.empty(), "丢点后不能沿用此前的持续异常计时");
  analyzer.Update({p}, At(3.1), 1280, 800);
  obs = analyzer.Update({p}, At(4.2), 1280, 800);
  Check(obs.state == PostureState::kShoulderTilt, "恢复有效输入后 2 秒识别高低肩");

  PostureProfile profile;
  profile.SetClass("normal", {{0, 0, 0, 1, 0, 3}});
  PostureProfile::Rule rule;
  rule.label = "head_down";
  rule.feature = 0;
  rule.threshold = 0.15;
  profile.SetRule(rule);
  p.joints[kShoulderR].y = 300;
  p.joints[kNose].y = 270;
  base.created_at = "test-base";
  profile.BindBaseline(base);
  analyzer.SetBaseline(base);
  analyzer.SetProfile(&profile, 2.0, 3.0);
  analyzer.Update({p}, At(10), 1280, 800);
  analyzer.Update({}, At(10.5), 1280, 800);
  obs = analyzer.Update({p}, At(12.1), 1280, 800);
  Check(obs.state == PostureState::kUnknown, "画像丢点后重新等待稳定识别");
  // 新契约：时间窗投票，窗口 0.6s / 80%。持续喂同一姿势约 1 秒后应稳定识别，
  // 不再要求「无间隔连续保持 2 秒」（旧契约，延迟过高）。
  double t = 12.1;
  for (int i = 0; i < 40; ++i) {
    t += 1.0 / 30.0;
    obs = analyzer.Update({p}, At(t), 1280, 800);
  }
  Check(obs.state == PostureState::kHeadDown, "画像持续约 1.3 秒后识别低头（投票制）");
  PersonPose p2 = p;
  p2.box.x += 320;
  for (auto& joint : p2.joints) joint.x += 320;
  obs = analyzer.Update({p, p2}, At(15), 1280, 800);
  Check(obs.state == PostureState::kUnknown && obs.triggers.empty(),
        "画像启用时多人保护依然有效");
  analyzer.Update({}, At(20), 1280, 800);
  obs = analyzer.Update({p}, At(21), 1280, 800);
  Check(obs.state == PostureState::kUnknown, "离开后清除旧画像标签和滤波缓存");
}

void TestHandFaceReleasesWhenArmLeavesFace() {
  std::printf("-- 托腮：手离开脸后必须退出（实测踩过：被钉住 62s）\n");
  // 实测现场：手已经离开脸（wrist_near=1.09），但腕高仍高于基准，
  // 只靠 wrist_rise 会一直判成托腮不退出。这里验证新加的「贴脸」约束。
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.hand_face_wrist_rise = 0.20f;
  cfg.hand_face_near_max = 0.85f;
  cfg.min_size_ratio = 0.55f;
  Baseline b;
  b.valid = true;
  b.shoulder_angle_deg = 0.0;
  b.head_offset_y = -0.30;
  b.ear_angle_valid = true;
  b.width_ratio = 0.42f;
  b.wrist_valid = true;
  b.wrist_y_rel = 1.19f;
  PostureAnalyzer analyzer(cfg, b);
  const float sw = 0.42f * 640.f;

  // 画一个「手抬到脸旁」的人：nose 在 (cx,180)，手腕 y 由参数给
  auto pose = [&](float wrist_x, float wrist_y) {
    PersonPose p{};
    p.person_confidence = 0.9f;
    p.box = BBox{60, 80, 480, 380};
    p.joints[kShoulderL] = Keypoint{120, 300, 0.9f};
    p.joints[kShoulderR] = Keypoint{120 + sw, 300, 0.9f};
    p.joints[kNose] = Keypoint{120 + sw * 0.5f, 180, 0.9f};
    p.joints[kEarL] = Keypoint{120 + sw * 0.25f, 165, 0.9f};
    p.joints[kEarR] = Keypoint{120 + sw * 0.75f, 167, 0.9f};
    p.joints[kWristL] = Keypoint{wrist_x, wrist_y, 0.9f};
    p.joints[kWristR] = Keypoint{60, wrist_y + 40, 0.9f};
    return p;
  };

  // 手贴脸：腕在鼻子正下方一点点（wrist_near 很小）
  const float cx = 120 + sw * 0.5f;
  const float wy = 300 + 0.38f * sw;
  const PersonPose near = pose(cx, wy);
  const PostureMetrics m_near = analyzer.ComputeMetrics(near, 640, 480);
  Check(m_near.wrist_valid, "手贴脸时手腕有效");
  Check(m_near.wrist_to_nose_raw <= 0.85f,
        "手贴脸 -> 腕鼻距离 " + std::to_string(m_near.wrist_to_nose_raw).substr(0, 4)
            + " <= 0.85（放行）");

  // 手离开脸：同样抬高，但横向移开（腕鼻距离变大）
  const PersonPose far_hand = pose(cx + sw * 1.20f, wy);
  const PostureMetrics m_far = analyzer.ComputeMetrics(far_hand, 640, 480);
  Check(m_far.wrist_valid, "手移开时手腕仍有效（不是测不到）");
  Check(m_far.wrist_to_nose_raw > 0.85f,
        "手离开脸 -> 腕鼻距离 " + std::to_string(m_far.wrist_to_nose_raw).substr(0, 4)
            + " > 0.85（该被拒）");

  // ★ 关键：测不到（哨兵）时必须放行，不能当成「手很远」
  PersonPose sentinel = pose(cx, wy);
  sentinel.joints[kWristL] = Keypoint{0, 0, 0.0f};
  sentinel.joints[kWristR] = Keypoint{0, 0, 0.0f};
  const PostureMetrics m_sent = analyzer.ComputeMetrics(sentinel, 640, 480);
  Check(!m_sent.wrist_valid, "手腕测不到 -> wrist_valid=false");
  Check(m_sent.wrist_to_nose_raw > 50.f,
        "手腕测不到 -> 停在哨兵（下游必须放行，不能当手很远）");
}

void TestVoteAcceptsFastButStable() {
  std::printf("-- 时间窗投票：既要快，也要能扛住单帧噪声\n");
  // 旧契约「同一标签无间隔连续保持 N 秒」有个死结：一个噪声帧就把计时清零，
  // 所以 N 必须很大才稳（实测 1.5~2.5s），叠加播报门限后总延迟 3.5~4.5s。
  // 投票制下噪声只影响一票，窗口可以缩到 0.6s。
  Config cfg;
  cfg.joint_confidence = 0.4f;
  cfg.pose_confidence = 0.4f;
  cfg.min_size_ratio = 0.55f;
  Baseline b;
  b.valid = true;
  b.shoulder_angle_deg = 0.0;
  b.head_offset_y = -0.30;
  b.ear_angle_valid = true;
  b.width_ratio = 0.42f;
  b.wrist_valid = true;
  b.wrist_y_rel = 1.19f;

  PostureProfile profile;
  profile.SetClass("normal", {{0, 0, 0, 1, 0, 3}});
  PostureProfile::Rule rule;
  rule.label = "head_down";
  rule.feature = 0;
  rule.threshold = 0.15;
  profile.SetRule(rule);

  // 几何沿用本文件已验证能过取景门的那套（1280x800 画布，肩宽 120px）。
  // 基准 head_offset_y = -0.5，低头帧把鼻子下移 0.25 个肩宽 -> head_drop=0.25。
  // ★ width_ratio 和 head_offset_y 必须与这套几何配套改，否则算出来的
  //   head_drop 会差一个常数（踩过：只改 width_ratio 得到 0.05，没到阈值）。
  b.width_ratio = 0.09375f;
  b.head_offset_y = -0.5f;
  PersonPose low{};
  low.person_confidence = 0.9f;
  low.box = BBox{200, 100, 240, 360};
  low.joints[kShoulderL] = Keypoint{260, 300, 0.9f};
  low.joints[kShoulderR] = Keypoint{380, 300, 0.9f};
  low.joints[kNose] = Keypoint{320, 270, 0.9f};

  // 正常坐（鼻子回到基准位置）
  PersonPose ok = low;
  // Match baseline head_offset_y=-0.5 with 120px shoulder width.
  ok.joints[kNose] = Keypoint{320, 240, 0.9f};

  // --- 场景 1：一路正常低头，测「多快能确认」---
  {
    PostureAnalyzer a(cfg, b);
    b.created_at = "test-voting";
    a.SetBaseline(b);
    profile.BindBaseline(b);
    a.SetProfile(&profile, 0.5, 3.0);
    double t = 0.0;
    double confirmed_at = -1.0;
    for (int i = 0; i < 60; ++i) {
      t += 1.0 / 30.0;
      auto o = a.Update({low}, At(t), 1280, 800);
      if (o.state == PostureState::kHeadDown && confirmed_at < 0) confirmed_at = t;
    }
    Check(confirmed_at > 0 && confirmed_at <= 0.85,
          "纯低头 -> 约 " + std::to_string(confirmed_at).substr(0, 4) + "s 内确认（<=0.85s）");
  }

  // --- 场景 2：低头过程中每 5 帧插一个噪声（判成正常），仍应快速确认 ---
  // 这是旧契约的死穴：一次噪声就清零计时，会一直确认不了。
  {
    PostureAnalyzer a(cfg, b);
    b.created_at = "test-voting";
    a.SetBaseline(b);
    profile.BindBaseline(b);
    a.SetProfile(&profile, 0.5, 3.0);
    double t = 0.0;
    double confirmed_at = -1.0;
    for (int i = 0; i < 90; ++i) {
      t += 1.0 / 30.0;
      const bool noise = (i % 5 == 0);
      auto o = a.Update({noise ? ok : low}, At(t), 1280, 800);
      if (o.state == PostureState::kHeadDown && confirmed_at < 0) confirmed_at = t;
    }
    Check(confirmed_at > 0 && confirmed_at <= 1.2,
          "每 5 帧一个噪声仍能在 " + std::to_string(confirmed_at).substr(0, 4)
              + "s 内确认（旧契约会一直卡住）");
  }

  // --- 场景 3：短暂误判（只持续 4 帧 ≈0.13s）不该报出来 ---
  {
    PostureAnalyzer a(cfg, b);
    b.created_at = "test-voting";
    a.SetBaseline(b);
    profile.BindBaseline(b);
    a.SetProfile(&profile, 0.5, 3.0);
    double t = 0.0;
    bool ever_reported = false;
    for (int i = 0; i < 60; ++i) {
      t += 1.0 / 30.0;
      // 前 20 帧正常，接着 4 帧误判成低头，再回正常
      const bool glitch = (i >= 20 && i < 24);
      auto o = a.Update({glitch ? low : ok}, At(t), 1280, 800);
      if (o.state == PostureState::kHeadDown) ever_reported = true;
    }
    Check(!ever_reported, "0.13 秒的瞬时误判不会报出来（挡住抬手/关键点跳动）");
  }

  // --- 场景 4：真低头后恢复正常，应在合理时间内退出 ---
  {
    PostureAnalyzer a(cfg, b);
    b.created_at = "test-voting";
    a.SetBaseline(b);
    profile.BindBaseline(b);
    a.SetProfile(&profile, 0.5, 3.0);
    double t = 0.0;
    for (int i = 0; i < 40; ++i) {   // 先稳定低头
      t += 1.0 / 30.0;
      a.Update({low}, At(t), 1280, 800);
    }
    double recovered_at = -1.0;
    for (int i = 0; i < 90; ++i) {
      t += 1.0 / 30.0;
      auto o = a.Update({ok}, At(t), 1280, 800);
      if (o.state == PostureState::kNormal && recovered_at < 0) recovered_at = t;
    }
    Check(recovered_at > 0, "坐正后能退回正常");
  }
}

void TestProfileAngleWrap() {
  PostureMetrics metrics;
  metrics.valid = true;
  metrics.ear_angle_valid = true;
  metrics.ear_angle_deg = -89;
  metrics.shoulder_angle_deg = -89;
  Baseline base;
  base.valid = true;
  base.ear_angle_valid = true;
  base.ear_angle_deg = 89;
  base.shoulder_angle_deg = 89;
  PostureProfile::FeatureVec f{};
  Check(BuildFeatureVector(metrics, base, &f, nullptr), "画像特征可计算");
  Check(std::fabs(f[2] - 2.0) < 0.001, "肩线跨 180 度边界只偏移 2 度");
}

void TestFastStateConfirmation() {
  HoldRule rule({"head_down", "低头", .15f, .08f, .5, .35, true, 50});
  auto r = rule.Update(.3f, At(0));
  Check(!r.latched, "单帧动作不改变界面状态");
  r = rule.Update(.3f, At(.4));
  Check(!r.latched, "0.4 秒尚未确认");
  r = rule.Update(.3f, At(.5));
  Check(r.just_activated, "0.5 秒即可更新姿势状态");
  rule.Update(.01f, At(.6));
  r = rule.Update(.01f, At(1.0));
  Check(r.just_cleared, "恢复坐正后快速清除异常状态");
}

void TestProfileRejectsWrongBaselineAndUnknownNormal() {
  Config cfg;
  Baseline b; b.valid=true; b.created_at="baseline-a";
  b.head_offset_y=-.5; b.width_ratio=.09375;
  PersonPose p{}; p.person_confidence=.9; p.box={200,100,240,360};
  p.joints[kNose]={320,240,.9};
  p.joints[kShoulderL]={260,300,.9}; p.joints[kShoulderR]={380,300,.9};
  PostureProfile profile;
  profile.SetClass("normal", {{0,0,0,1,0,3,3}});
  PostureProfile::Rule rule; rule.label="head_down";rule.feature=0;rule.threshold=.15;
  profile.SetRule(rule);
  PostureAnalyzer a(cfg,b);a.SetProfile(&profile,.5,3);
  auto obs=a.Update({p},At(0),1280,800);
  Check(obs.state==PostureState::kUnknown,"未绑定标定的旧画像不得判为正常");
  profile.BindBaseline(b);
  for(int i=1;i<=25;++i) obs=a.Update({p},At(i*.04),1280,800);
  Check(obs.state==PostureState::kNormal,"同标定且在正常样本范围内可确认正常");
  p.joints[kNose].y=160;
  obs=a.Update({p},At(1.04),1280,800);
  Check(obs.state==PostureState::kUnknown,"未命中异常但偏离正常的帧立即 UNKNOWN，不能沿用旧正常标签");
  p.joints[kNose].y=270;
  for(int i=27;i<=50;++i) obs=a.Update({p},At(i*.04),1280,800);
  Check(obs.state==PostureState::kHeadDown,"UNKNOWN 后仍能正常确认已采集的不良姿势");
  b.created_at="baseline-b";a.SetBaseline(b);
  obs=a.Update({p},At(2.04),1280,800);
  Check(obs.state==PostureState::kUnknown,"运行中重新标定立即使旧画像失效");
  profile.BindBaseline(b);
  profile.SetClass("head_tilt",{{0,20,0,1,0,3,3}});
  p.joints[kNose].y=240;
  for(int i=52;i<=80;++i) obs=a.Update({p},At(i*.04),1280,800);
  Check(obs.state==PostureState::kUnknown,"双耳无法测量时不能把缺测的0度当成正常证据");
}

void TestNormalRecoversImmediatelyAfterBriefUncertainty() {
  Config cfg;
  Baseline b;b.valid=true;b.created_at="normal-recovery";b.head_offset_y=-.5;b.width_ratio=.09375;
  PersonPose p{};p.person_confidence=.9;p.box={200,100,240,360};
  p.joints[kNose]={320,240,.9};p.joints[kShoulderL]={260,300,.9};p.joints[kShoulderR]={380,300,.9};
  PostureProfile profile;profile.BindBaseline(b);profile.SetClass("normal",{{0,0,0,1,0,3,3}});
  PostureProfile::Rule rule;rule.label="head_down";rule.feature=0;rule.threshold=.15;profile.SetRule(rule);
  PostureAnalyzer a(cfg,b);a.SetProfile(&profile,.5,3);
  PostureObservation o;
  for(int i=0;i<25;++i)o=a.Update({p},At(i*.04),1280,800);
  Check(o.state==PostureState::kNormal,"先建立已确认的正常姿势");
  PersonPose unsupported=p;unsupported.joints[kNose].y=160;
  o=a.Update({unsupported},At(1),1280,800);
  Check(o.state==PostureState::kUnknown,"短暂超范围仍如实输出UNKNOWN，不用旧正常标签掩盖");
  o=a.Update({p},At(1.04),1280,800);
  Check(o.state==PostureState::kNormal,"单帧异常后有效正常证据恢复，无需再等0.6秒投票");
  for(int i=0;i<10;++i)o=a.Update({unsupported},At(1.2+i*.04),1280,800);
  Check(o.state==PostureState::kUnknown,"持续未知不能沿用正常状态");
  o=a.Update({p},At(1.6),1280,800);
  Check(o.state==PostureState::kUnknown,"持续未知后必须重新确认，不无限缓存旧标签");
}

void TestProfileWorksAtLowFpsAndMapsShoulders() {
  Config cfg;
  Baseline b; b.valid = true; b.head_offset_y=-.5; b.width_ratio=.09375;
  PersonPose p{}; p.person_confidence=.9;
  p.box={200,100,240,360};
  p.joints[kNose]={320,270,.9};
  p.joints[kShoulderL]={260,300,.9}; p.joints[kShoulderR]={380,300,.9};
  PostureProfile profile;
  profile.SetClass("normal", {{0,0,0,1,0,3,3}});
  PostureProfile::Rule rule; rule.label="head_down"; rule.feature=0; rule.threshold=.15;
  profile.SetRule(rule);
  b.created_at = "test-low-fps"; profile.BindBaseline(b);
  PostureAnalyzer a(cfg,b);a.SetProfile(&profile,.5,3);
  PostureObservation obs;
  for(int i=0;i<9;++i) obs=a.Update({p},At(i*.125),1280,800);
  Check(obs.state==PostureState::kHeadDown,"8FPS 下无需 6 票才能采纳姿势，避免永久 UNKNOWN");
  rule.label="shoulder_tilt";rule.feature=2;rule.threshold=12;
  profile.ClearRules();profile.SetRule(rule);
  p.joints[kShoulderR].y=344;
  PostureAnalyzer shoulders(cfg,b);shoulders.SetProfile(&profile,.5,3);
  for(int i=0;i<25;++i) obs=shoulders.Update({p},At(i*.04),1280,800);
  Check(obs.state==PostureState::kShoulderTilt,"画像高低肩标签必须映射为高低肩，不能静默 UNKNOWN");
}

}  // namespace

int main() {
  std::printf("=== posture rules 单测 ===\n");
  TestNormalizeLineAngle();
  TestProfileWorksAtLowFpsAndMapsShoulders();
  TestNormalRecoversImmediatelyAfterBriefUncertainty();
  TestProfileRejectsWrongBaselineAndUnknownNormal();
  TestFastStateConfirmation();
  TestInterruptedDetection();
  TestHandFaceReleasesWhenArmLeavesFace();
  TestVoteAcceptsFastButStable();
  TestProfileAngleWrap();
  TestHoldRule();
  TestMetricsAndPlausibility();
  TestTopDownCamera();
  TestWristGateRelative();
  TestWristSentinelConsistency();
  TestFramingGate();
  TestSizeGate();
  TestSmoothing();
  TestTooCloseRelative();
  TestBaselineDrift();
  TestAnalyzerObservation();
  if (g_failures == 0) {
    std::printf("=== 全部通过 ===\n");
    return 0;
  }
  std::printf("=== %d 项失败 ===\n", g_failures);
  return 1;
}
