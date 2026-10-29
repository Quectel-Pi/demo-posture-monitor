#include "posture/PostureAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "posture/Logger.hpp"

namespace posture {
namespace {

constexpr float kRad2Deg = 57.29577951308232f;
constexpr float kDeg2Rad = 0.017453292519943295f;
constexpr float kMinShoulderPx = 24.f;

float Dist(float ax, float ay, float bx, float by) {
  const float dx = ax - bx;
  const float dy = ay - by;
  return std::sqrt(dx * dx + dy * dy);
}

double SecondsBetween(SteadyTime a, SteadyTime b) {
  return std::chrono::duration<double>(b - a).count();
}

}  // namespace

// --------------------------------------------------------------------------
float PostureAnalyzer::NormalizeLineAngle(float deg) {
  // 直线方向以 180° 为周期：折叠到 [-90, 90)
  while (deg >= 90.f) deg -= 180.f;
  while (deg < -90.f) deg += 180.f;
  return deg;
}

float PostureAnalyzer::NormalizeAngle180(float deg) {
  while (deg >= 180.f) deg -= 360.f;
  while (deg < -180.f) deg += 360.f;
  return deg;
}

float PostureAnalyzer::LineAngleDelta(float current, float baseline) {
  return std::fabs(NormalizeLineAngle(current - baseline));
}

// --------------------------------------------------------------------------
HoldRule::Result HoldRule::Update(std::optional<float> value, SteadyTime now) {
  Result res;
  if (!value.has_value()) {
    // 没有有效输入：不新增异常，也不维持「持续计时」，但已激活的保持（迟滞），
    // 由 PostureAnalyzer 在 missing_data_timeout 后统一 Reset。
    abnormal_since_.reset();
    recovery_since_.reset();
    res.valid = false;
    res.latched = latched_;
    res.active_s = latched_at_ ? SecondsBetween(*latched_at_, now) : 0.0;
    return res;
  }
  res.valid = true;
  const float v = *value;
  const bool above = cfg_.higher_is_bad ? (v > cfg_.trigger) : (v < cfg_.trigger);
  const bool below_recover =
      cfg_.higher_is_bad ? (v < cfg_.recover) : (v > cfg_.recover);

  if (!latched_) {
    if (!above) {
      abnormal_since_.reset();
    } else {
      if (!abnormal_since_) abnormal_since_ = now;
      if (SecondsBetween(*abnormal_since_, now) >= cfg_.hold_s) {
        res.active_s = SecondsBetween(*abnormal_since_, now);   // 报告真实持续时间
        latched_ = true;
        latched_at_ = now;
        abnormal_since_.reset();
        recovery_since_.reset();
        res.just_activated = true;
      }
    }
  } else {
    if (below_recover) {
      if (!recovery_since_) recovery_since_ = now;
      if (SecondsBetween(*recovery_since_, now) >= cfg_.recover_hold_s) {
        latched_ = false;
        recovery_since_.reset();
        res.just_cleared = true;
      }
    } else {
      recovery_since_.reset();
    }
  }
  res.latched = latched_;
  if (!res.just_activated) {
    res.active_s = latched_at_ ? SecondsBetween(*latched_at_, now) : 0.0;
  }
  return res;
}

void HoldRule::Reset() {
  latched_ = false;
  abnormal_since_.reset();
  recovery_since_.reset();
  latched_at_.reset();
}

// --------------------------------------------------------------------------
PostureAnalyzer::PostureAnalyzer(const Config& cfg, const Baseline& baseline)
    : cfg_(cfg),
      baseline_(baseline),
      shoulder_rule_({.key = "shoulder_tilt",
                      .label_zh = "高低肩",
                      .trigger = cfg.shoulder_trigger_deg,
                      .recover = cfg.shoulder_recover_deg,
                      .hold_s = cfg.shoulder_hold_s,
                      .recover_hold_s = cfg.recovery_hold_s,
                      .higher_is_bad = true,
                      .priority = 40}),
      head_down_rule_({.key = "head_down",
                       .label_zh = "低头/趴桌",
                       .trigger = cfg.head_down_drop_legacy,   // SetBaseline 后按基准重算
                       .recover = cfg.head_down_drop_legacy * 0.5f,
                       .hold_s = cfg.head_down_hold_s,
                       .recover_hold_s = cfg.recovery_hold_s,
                       .higher_is_bad = true,
                       .priority = 50}),
      head_tilt_rule_({.key = "head_tilt",
                       .label_zh = "歪头",
                       .trigger = cfg.head_tilt_deg,
                       .recover = cfg.head_tilt_deg * 0.6f,
                       .hold_s = cfg.head_tilt_hold_s,
                       .recover_hold_s = cfg.recovery_hold_s,
                       .higher_is_bad = true,
                       .priority = 30}),
      too_close_rule_({.key = "too_close",
                       .label_zh = "离屏太近",
                       .trigger = cfg.too_close_ratio,
                       .recover = cfg.too_close_ratio - 0.08f,
                       .hold_s = cfg.too_close_hold_s,
                       .recover_hold_s = cfg.recovery_hold_s,
                       .higher_is_bad = true,
                       .priority = 20}),
      hand_face_rule_({.key = "hand_on_face",
                       .label_zh = "托腮",
                       .trigger = cfg.hand_face_ratio,
                       .recover = cfg.hand_face_ratio * 1.25f,
                       .hold_s = cfg.hand_face_hold_s,
                       .recover_hold_s = cfg.recovery_hold_s,
                       .higher_is_bad = false,   // 距离越小越可疑
                       .priority = 60}) {
  // 构造时传入的基准也要参与阈值缩放
  const float drop =
      (baseline.valid && std::fabs(baseline.head_offset_y) > 0.02)
          ? std::max(cfg.head_down_drop_min,
                     std::min(cfg.head_down_drop_max,
                              cfg.head_down_drop_ratio *
                                  std::fabs(static_cast<float>(baseline.head_offset_y))))
          : cfg.head_down_drop_legacy;
  head_down_rule_.SetTrigger(drop, drop * 0.5f);

  float close_th = cfg.too_close_ratio;
  if (baseline.valid && baseline.width_ratio > 0.001) {
    close_th = static_cast<float>(baseline.width_ratio * cfg.too_close_ratio_rel);
  }
  too_close_rule_.SetTrigger(close_th, close_th * 0.92f);
}

void PostureAnalyzer::SetBaseline(const Baseline& baseline) {
  baseline_ = baseline;
  ResetSmoother();
  // 低头阈值随基准的头肩距离缩放：俯视机位基准只有 -0.20 肩宽，
  // 固定 0.35 的阈值在那里永远达不到（鼻子压到肩线上最多降 0.20）。
  float drop = cfg_.head_down_drop_legacy;
  if (baseline_.valid && std::fabs(baseline_.head_offset_y) > 0.02) {
    drop = cfg_.head_down_drop_ratio * std::fabs(static_cast<float>(baseline_.head_offset_y));
    drop = std::max(cfg_.head_down_drop_min, std::min(cfg_.head_down_drop_max, drop));
  }
  head_down_rule_.SetTrigger(drop, drop * 0.5f);

  // 离屏过近：同样改成「相对基准」。
  // 绝对值 0.75 在俯视机位下要求人脸贴屏幕（实测基准 0.42 → 需 ×1.77），
  // 而在直视机位基准 0.11 时又永远达不到 —— 绝对值注定失效。
  float close_th = cfg_.too_close_ratio;
  if (baseline_.valid && baseline_.width_ratio > 0.001) {
    close_th = static_cast<float>(baseline_.width_ratio * cfg_.too_close_ratio_rel);
  }
  too_close_rule_.SetTrigger(close_th, close_th * 0.92f);   // 迟滞恢复带宽 8%
  ResetRules();
}

float PostureAnalyzer::HeadDownThreshold() const { return head_down_rule_.trigger(); }

float PostureAnalyzer::SmoothWidthRatio(float raw) {
  if (raw <= 0.f) return wr_count_ > 0 ? wr_window_[(wr_next_ + 7 - 1) % 7] : 0.f;
  wr_window_[wr_next_] = raw;
  wr_next_ = (wr_next_ + 1) % 7;
  if (wr_count_ < 7) ++wr_count_;
  // 取窗口内中位数：抗离群（单帧把肩膀检飞不会带偏结果）
  std::array<float, 7> sorted = wr_window_;
  const int n = std::min(wr_count_, 7);
  std::sort(sorted.data(), sorted.data() + n);
  return sorted[static_cast<size_t>(n / 2)];
}

void PostureAnalyzer::ResetSmoother() {
  wr_count_ = 0;
  wr_next_ = 0;
  wr_window_.fill(0.f);
}

void PostureAnalyzer::ResetRules() {
  shoulder_rule_.Reset();
  head_down_rule_.Reset();
  head_tilt_rule_.Reset();
  too_close_rule_.Reset();
  hand_face_rule_.Reset();
}

std::string PostureAnalyzer::CheckBaselineDrift(const PostureMetrics& m) const {
  if (!baseline_.valid || !m.valid) return {};
  if (baseline_.width_ratio <= 0.001) return {};   // 老基准没记这个字段，跳过检查

  // 距离：肩宽占画面宽的比例。挪动摄像头（远近变化）会让它成倍变化。
  const double ratio = m.width_ratio / baseline_.width_ratio;
  if (ratio > 1.8 || ratio < 0.55) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "画面里肩宽占比 %.2f，校准基准是 %.2f（差 %.1f 倍）——"
                  "摄像头或人就位变了",
                  m.width_ratio, baseline_.width_ratio,
                  ratio >= 1.0 ? ratio : 1.0 / ratio);
    return buf;
  }
  // 头肩纵向偏移：机位高度/俯仰变了会明显偏移
  const double dy = m.head_offset_y - baseline_.head_offset_y;
  if (std::fabs(dy) > 0.45) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "头肩纵向偏移 %.2f，校准基准是 %.2f（差 %.2f 个肩宽）",
                  m.head_offset_y, baseline_.head_offset_y, dy);
    return buf;
  }
  return {};
}

PostureMetrics PostureAnalyzer::ComputeMetrics(const PersonPose& person, int frame_width,
                                               int frame_height) const {
  PostureMetrics m;
  const float min_conf = cfg_.joint_confidence;

  const Keypoint& nose = person.joints[kNose];
  const Keypoint& sh_l = person.joints[kShoulderL];
  const Keypoint& sh_r = person.joints[kShoulderR];
  const Keypoint& wr_l = person.joints[kWristL];
  const Keypoint& wr_r = person.joints[kWristR];

  if (!person.JointValid(kNose, min_conf)) {
    m.invalid_reason = "鼻子关键点不可信";
    return m;
  }
  if (!person.JointValid(kShoulderL, min_conf) || !person.JointValid(kShoulderR, min_conf)) {
    m.invalid_reason = "肩点不可信（肩部被裁掉或遮挡）";
    return m;
  }

  const float sw = Dist(sh_l.x, sh_l.y, sh_r.x, sh_r.y);
  if (sw < kMinShoulderPx) {
    m.invalid_reason = "肩宽过小（" + std::to_string(static_cast<int>(sw)) + "px）";
    return m;
  }

  const float sx = (sh_l.x + sh_r.x) * 0.5f;
  const float sy = (sh_l.y + sh_r.y) * 0.5f;

  // §4.2：肩线方向角，atan2(dy, dx)，y 向下
  m.shoulder_angle_deg =
      NormalizeLineAngle(std::atan2(sh_r.y - sh_l.y, sh_r.x - sh_l.x) * kRad2Deg);
  m.shoulder_width_px = sw;
  if (baseline_.valid) {
    m.shoulder_dev_deg =
        LineAngleDelta(m.shoulder_angle_deg, static_cast<float>(baseline_.shoulder_angle_deg));
  }
  // §4.3：归一化头肩相对位置
  m.head_offset_x = (nose.x - sx) / sw;
  m.head_offset_y = (nose.y - sy) / sw;
  // 歪头代理量：肩中点 -> 鼻子的连线相对竖直方向的偏角
  m.head_axis_deg = std::atan2(nose.x - sx, sy - nose.y) * kRad2Deg;

  // 耳线角（歪头的可靠判据）。两只耳朵都可信时才算。
  const Keypoint& ear_l = person.joints[kEarL];
  const Keypoint& ear_r = person.joints[kEarR];
  if (ear_l.confidence >= min_conf && ear_r.confidence >= min_conf) {
    const float ear_dist = Dist(ear_l.x, ear_l.y, ear_r.x, ear_r.y);
    if (ear_dist > sw * 0.35f) {   // 两耳距离太小说明错了
      m.ear_angle_deg = NormalizeLineAngle(
          std::atan2(ear_r.y - ear_l.y, ear_r.x - ear_l.x) * kRad2Deg);
      m.ear_angle_valid = true;
    }
  }
  m.width_ratio = frame_width > 0 ? sw / static_cast<float>(frame_width) : 0.f;

  // 单目距离粗估：Z = K / width_ratio，K = 校准距离 × 校准时的 width_ratio
  if (baseline_.valid && baseline_.width_ratio > 0.001 &&
      baseline_.reference_distance_cm > 0.0 && m.width_ratio > 0.001f) {
    const double K = baseline_.reference_distance_cm * baseline_.width_ratio;
    m.distance_cm_est = static_cast<float>(K / m.width_ratio);
    m.distance_cm_valid = true;
  }

  // 托腮：两只手腕里离鼻子更近的那个。
  // ★ 但只有「手腕抬到肩线之上」才算 —— 坐在桌前打字时，手在桌面上，
  //   图像里离胸/脸并不远（俯视机位尤其明显），不加上这个门控会一直误报托腮。
  //   实测：手放桌面时手腕比肩线低 95~109% 肩宽；托腮时一定高于肩线。
  float best = std::numeric_limits<float>::max();
  bool any_wrist = false;
  const float wrist_above_limit = sy - sw * 0.10f;   // 留 10% 肩宽容差
  for (const Keypoint* w : {&wr_l, &wr_r}) {
    if (w->confidence < min_conf) continue;
    if (w->y > wrist_above_limit) continue;          // 手在下方 → 不是托腮
    any_wrist = true;
    const float d = Dist(w->x, w->y, nose.x, nose.y) / sw;
    if (d < best) best = d;
  }
  m.wrist_to_nose = any_wrist ? best : 99.f;

  m.person_confidence = person.person_confidence;

  // ★ 取景门（现场实测补充）：摄像头太低/人太近时鼻子会贴到画面顶部甚至出框，
  //   此时肩宽、头肩偏移都是被裁掉后的畸形几何。宁可判 UNKNOWN，也不要用它
  //   校准或报警 —— 否则会得到一个畸形的基准，之后一直误报。
  const float margin_x = frame_width * 0.02f;
  const float margin_y = frame_height * 0.02f;
  const float top_limit = frame_height * 0.03f;
  if (nose.y < top_limit) {
    m.invalid_reason = "头顶贴到画面上边缘（摄像头偏低或人太近），请把摄像头抬高";
    return m;
  }
  for (const Keypoint* k : {&sh_l, &sh_r}) {
    if (k->y < margin_y || k->y > frame_height - margin_y ||
        k->x < margin_x || k->x > frame_width - margin_x) {
      m.invalid_reason = "肩部贴边/出框，双肩没完整入画";
      return m;
    }
  }
  // 鼻子到肩线的距离：只做「模型明显检错」的兜底，不做「机位好坏」的判据。
  // 俯视机位下这个量天然偏小，低头时会进一步压到肩线上 —— 那是要检测的信号，
  // 不是要丢弃的坏帧（之前设 0.22 把低头帧全扔了）。
  //
  // 下限是负的：鼻子压到肩线**之下**（head_gap < 0）正是「趴桌」的几何表现，
  // 必须留给 head_down 规则去判。只有低到 -0.5 肩宽（鼻子跑到胸口）才认为是错检。
  const float head_gap = (sy - nose.y) / sw;
  const float hard_floor = -0.5f;
  if (head_gap < hard_floor) {
    m.invalid_reason = "鼻子明显低于肩线（疑似关键点错检）";
    return m;
  }
  m.valid = true;
  return m;
}

// 几何合理性门：挡住「空场景被判成人」「框到半个身子」这类误判（§11.4）
bool PostureAnalyzer::Plausible(const PostureMetrics& m, const PersonPose& p,
                                int frame_width, int frame_height) const {
  if (!m.valid) return false;
  if (p.person_confidence < cfg_.pose_confidence) return false;

  // 人体框要有实际面积（防止把噪点当人）
  const float frame_area = static_cast<float>(frame_width) * frame_height;
  if (frame_area <= 0.f) return false;
  if (p.box.w < 32.f || p.box.h < 48.f) return false;
  if (p.box.Area() < frame_area * 0.01f) return false;

  const float sx = (p.joints[kShoulderL].x + p.joints[kShoulderR].x) * 0.5f;
  const float sy = (p.joints[kShoulderL].y + p.joints[kShoulderR].y) * 0.5f;
  const Keypoint& nose = p.joints[kNose];

  // 鼻子必须在肩线之上（正常坐姿 / 站姿的必然几何关系）
  if (nose.y > sy + 0.15f * m.shoulder_width_px) return false;
  // 头肩横向偏移不能离谱
  if (std::fabs(m.head_offset_x) > 2.5f) return false;
  // 头肩纵向距离应在一个区间内（趴桌时接近 0，站立时更大）
  if (m.head_offset_y < -2.0f || m.head_offset_y > 0.6f) return false;

  // ROI：主用户必须在配置区域内（§4.1）
  const float cx = p.box.CenterX() / frame_width;
  const float cy = p.box.CenterY() / frame_height;
  if (cx < cfg_.roi_x || cx > cfg_.roi_x + cfg_.roi_w) return false;
  if (cy < cfg_.roi_y || cy > cfg_.roi_y + cfg_.roi_h) return false;

  (void)sx;
  return true;
}

PrimarySelection PostureAnalyzer::SelectPrimary(const std::vector<PersonPose>& persons,
                                                int frame_width, int frame_height) const {
  PrimarySelection sel;
  std::vector<std::pair<float, const PersonPose*>> scored;
  const PersonPose* biggest = nullptr;
  float biggest_area = 0.f;
  for (const auto& p : persons) {
    if (p.person_confidence < cfg_.pose_confidence) continue;
    sel.saw_detection = true;
    const PostureMetrics m = ComputeMetrics(p, frame_width, frame_height);
    if (m.valid && Plausible(m, p, frame_width, frame_height)) {
      scored.emplace_back(p.box.Area() * p.person_confidence, &p);
      continue;
    }
    // 记下最像人的那个被拒原因，供上层区分"无人"与"机位不对"
    if (!m.valid) {
      if (p.box.Area() > biggest_area) {
        biggest_area = p.box.Area();
        biggest = &p;
        sel.reject_reason = m.invalid_reason;
        sel.reject_person_box = p.box;
      }
    } else if (p.box.Area() > biggest_area) {
      biggest_area = p.box.Area();
      biggest = &p;
      sel.reject_reason = "人体相对画面的位置/比例不合理（可能有半个身子在画面外）";
      sel.reject_person_box = p.box;
    }
  }
  if (scored.empty()) {
    sel.reason = sel.saw_detection ? "检出人体但几何特征不可用"
                                   : "画面内没有可信的人体";
    (void)biggest;
    return sel;
  }
  std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  // ★ 先去掉「同一个人被多尺度重复检出」的框。
  //   w8a8 量化后 conf 通道很粗，同一个人会有好几个高分框，而且中心抖动几十像素，
  //   IoU 达不到 NMS 阈值。如果不去重，会把一个人判成「多人尺寸接近」而拒绝判定。
  std::vector<const PersonPose*> cluster;
  for (const auto& item : scored) {
    bool duplicate = false;
    for (const PersonPose* kept : cluster) {
      const BBox& a = kept->box;
      const BBox& b = item.second->box;
      const float ix = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
      const float iy = std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
      const float inter = ix * iy;
      const float uni = a.Area() + b.Area() - inter;
      const float iou = uni > 1e-6f ? inter / uni : 0.f;
      // 中心点很近也算同一个人（量化导致框中心偏移）
      const float dc = std::hypot(a.CenterX() - b.CenterX(), a.CenterY() - b.CenterY());
      const float scale = std::max(a.w, b.w);
      if (iou > 0.35f || (scale > 1.f && dc < scale * 0.35f)) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) cluster.push_back(item.second);
  }
  sel.plausible_count = static_cast<int>(cluster.size());
  sel.person = cluster.front();
  // §11.4：确实有两个人尺寸接近时不做个体判定，避免混用关键点
  if (cluster.size() >= 2) {
    const float a0 = cluster[0]->box.Area() * cluster[0]->person_confidence;
    const float a1 = cluster[1]->box.Area() * cluster[1]->person_confidence;
    if (a1 > a0 * 0.45f) {
      sel.ambiguous = true;
      sel.reason = "画面内有多人，无法锁定主用户";
    }
  }
  return sel;
}

bool PostureAnalyzer::CalibrateFeed(const std::vector<PersonPose>& persons, int frame_width,
                                    int frame_height, PostureMetrics* out_metrics) {
  const PrimarySelection sel = SelectPrimary(persons, frame_width, frame_height);
  if (!sel.person || sel.ambiguous) return false;
  const PostureMetrics m = ComputeMetrics(*sel.person, frame_width, frame_height);
  if (!m.valid) return false;
  if (m.person_confidence < cfg_.pose_confidence) return false;
  if (out_metrics) *out_metrics = m;
  return true;
}

PostureObservation PostureAnalyzer::Update(const std::vector<PersonPose>& persons,
                                           SteadyTime now, int frame_width, int frame_height) {
  PostureObservation obs;

  const PrimarySelection sel = SelectPrimary(persons, frame_width, frame_height);
  if (!sel.person) {
    last_metrics_ = PostureMetrics{};
    // 长时间没有有效数据 -> 复位规则，避免用旧异常状态无限延续（§11.3 末尾要求）
    if (last_valid_at_ && SecondsBetween(*last_valid_at_, now) > cfg_.missing_data_timeout_s) {
      ResetRules();
    }
    if (sel.saw_detection) {
      // 有人入镜但几何量不可用：这是「机位/取景问题」，不是「无人」。
      // 必须说清楚，否则用户只会看到"无人"，永远想不到要调摄像头。
      obs.state = PostureState::kUnknown;
      obs.explanation = sel.reject_reason.empty() ? sel.reason : sel.reject_reason;
    } else {
      obs.state = PostureState::kUserAway;
      obs.explanation = sel.reason;
    }
    return obs;
  }

  PostureMetrics m = ComputeMetrics(*sel.person, frame_width, frame_height);
  if (!Plausible(m, *sel.person, frame_width, frame_height)) {
    last_metrics_ = PostureMetrics{};
    if (last_valid_at_ && SecondsBetween(*last_valid_at_, now) > cfg_.missing_data_timeout_s) {
      ResetRules();
    }
    obs.state = PostureState::kUnknown;
    obs.metrics = m;
    obs.explanation = m.valid ? "几何特征不合理，丢弃本帧" : m.invalid_reason;
    return obs;
  }

  // 距离相关用平滑后的 width_ratio，其余量保持原始（姿态变化需要即时响应）
  m.width_ratio_raw = m.width_ratio;
  m.width_ratio = SmoothWidthRatio(m.width_ratio_raw);
  // 重算估算距离（用平滑值）
  if (baseline_.valid && baseline_.width_ratio > 0.001 &&
      baseline_.reference_distance_cm > 0.0 && m.width_ratio > 0.001f) {
    const double K = baseline_.reference_distance_cm * baseline_.width_ratio;
    m.distance_cm_est = static_cast<float>(K / m.width_ratio);
    m.distance_cm_valid = true;
  }

  last_metrics_ = m;
  last_valid_at_ = now;
  obs.metrics = m;

  if (sel.ambiguous) {
    // 多人：不做个体判定，但规则计时也被挂起（不给 optional -> 不新增异常）
    auto r1 = shoulder_rule_.Update(std::nullopt, now);
    auto r2 = head_down_rule_.Update(std::nullopt, now);
    auto r3 = head_tilt_rule_.Update(std::nullopt, now);
    auto r4 = too_close_rule_.Update(std::nullopt, now);
    auto r5 = hand_face_rule_.Update(std::nullopt, now);
    if (r1.latched) obs.active_keys.push_back("shoulder_tilt");
    if (r2.latched) obs.active_keys.push_back("head_down");
    if (r3.latched) obs.active_keys.push_back("head_tilt");
    if (r4.latched) obs.active_keys.push_back("too_close");
    if (r5.latched) obs.active_keys.push_back("hand_on_face");
    obs.state = PostureState::kUnknown;
    obs.explanation = sel.reason;
    return obs;
  }

  // ---- 特征 -> 规则输入 ----
  std::optional<float> v_shoulder;
  std::optional<float> v_head_down;
  std::optional<float> v_head_tilt;
  std::optional<float> v_too_close;
  std::optional<float> v_hand_face;

  // 高低肩需要个人基准；没有基准只能给出「未校准」，不判异常（§4.6）
  if (baseline_.valid) {
    v_shoulder = LineAngleDelta(m.shoulder_angle_deg,
                                static_cast<float>(baseline_.shoulder_angle_deg));
    const float drop = m.head_offset_y - static_cast<float>(baseline_.head_offset_y);
    v_head_down = std::max(0.f, drop);
    // 歪头：优先用耳线（稳定）；耳朵不可信时退回旧的头轴角（俯视下会抖，但至少不瞎报）
    if (m.ear_angle_valid && baseline_.ear_angle_valid) {
      v_head_tilt = LineAngleDelta(m.ear_angle_deg,
                                   static_cast<float>(baseline_.ear_angle_deg));
    } else {
      v_head_tilt = std::fabs(NormalizeAngle180(
          m.head_axis_deg - static_cast<float>(baseline_.head_axis_deg)));
    }
  }
  v_too_close = m.width_ratio;
  v_hand_face = m.wrist_to_nose;

  auto r_shoulder = shoulder_rule_.Update(v_shoulder, now);
  auto r_head_down = head_down_rule_.Update(v_head_down, now);
  auto r_head_tilt = head_tilt_rule_.Update(v_head_tilt, now);
  auto r_too_close = too_close_rule_.Update(v_too_close, now);
  auto r_hand_face = hand_face_rule_.Update(v_hand_face, now);

  auto push = [&](const HoldRule::Result& r, const HoldRule& rule) {
    if (r.latched) obs.active_keys.push_back(rule.cfg().key);
    if (r.just_activated) {
      obs.triggers.push_back(
          RuleTrigger{rule.cfg().key, rule.cfg().label_zh, r.active_s, rule.cfg().priority});
    }
  };
  push(r_shoulder, shoulder_rule_);
  push(r_head_down, head_down_rule_);
  push(r_head_tilt, head_tilt_rule_);
  push(r_too_close, too_close_rule_);
  push(r_hand_face, hand_face_rule_);

  // 多规则同时成立时按优先级取主状态（§5.3：合并为一个通知）
  if (r_hand_face.latched) {
    obs.state = PostureState::kHandOnFace;
  } else if (r_head_down.latched) {
    obs.state = PostureState::kHeadDown;
  } else if (r_shoulder.latched) {
    obs.state = PostureState::kShoulderTilt;
  } else if (r_head_tilt.latched) {
    obs.state = PostureState::kHeadTilt;
  } else if (r_too_close.latched) {
    obs.state = PostureState::kTooClose;
  } else {
    obs.state = PostureState::kNormal;
  }

  if (baseline_.valid) {
    obs.explanation = std::string(ToChinese(obs.state));
  } else {
    obs.explanation = "未校准：只做距离/托腮提示，高低肩与低头不判定";
    if (obs.state == PostureState::kNormal) obs.state = PostureState::kNormal;
  }
  return obs;
}

}  // namespace posture
