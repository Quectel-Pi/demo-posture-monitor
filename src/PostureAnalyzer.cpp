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
  res.value = v;
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
  // 计时进度：激活后看激活时长，未激活时看「越线已持续多久」
  if (latched_ && latched_at_) {
    res.held_s = SecondsBetween(*latched_at_, now);
  } else if (abnormal_since_) {
    res.held_s = SecondsBetween(*abnormal_since_, now);
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
  double ref0 = 0.0;
  if (baseline.valid) {
    ref0 = baseline.ear_mid_valid ? std::fabs(baseline.ear_mid_offset_y)
                                  : std::fabs(baseline.head_offset_y);
  }
  const float drop =
      ref0 > 0.02
          ? std::max(cfg.head_down_drop_min,
                     std::min(cfg.head_down_drop_max,
                              cfg.head_down_drop_ratio * static_cast<float>(ref0)))
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
  double ref = 0.0;
  if (baseline_.valid) {
    if (baseline_.ear_mid_valid) {
      ref = std::fabs(baseline_.ear_mid_offset_y);
    } else {
      ref = std::fabs(baseline_.head_offset_y);
    }
  }
  if (ref > 0.02) {
    drop = cfg_.head_down_drop_ratio * static_cast<float>(ref);
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

void PostureAnalyzer::SetProfile(const PostureProfile* profile, double hold_s,
                                 double accept_radius) {
  profile_ = profile;
  profile_hold_s_ = hold_s > 0 ? hold_s : 2.0;
  profile_accept_radius_ = accept_radius > 0 ? accept_radius : 3.0;
  pending_label_.clear();
  active_label_.clear();
  pending_since_ = SteadyTime{};
  profile_label_names_ = profile ? profile->labels() : std::vector<std::string>{};
  if (profile_ && !profile_->empty()) {
    std::string names;
    for (const auto& l : profile_label_names_) {
      names += PostureLabelZh(l);
      names += " ";
    }
    LOG_INFO("逐姿势画像已启用：", profile_label_names_.size(), " 个姿势 [", names,
             "]  总样本 ", profile_->total_samples(),
             "  保持门限 ", profile_hold_s_, "s  接受半径 ", profile_accept_radius_);
  }
}

void PostureAnalyzer::ClearProfile() {
  profile_ = nullptr;
  pending_label_.clear();
  active_label_.clear();
  profile_label_names_.clear();
}

// 返回 true 表示本帧已由画像给出结论（obs.state/explanation 已填好）
bool PostureAnalyzer::ProfileUpdate(const PostureMetrics& m, SteadyTime now,
                                    PostureObservation* obs) {
  if (!profile_active()) return false;
  // 画像特征全部是「相对校准基准」的量，没有基准就一定算不出来。
  // 这里必须让位给传统规则路径，否则会把 state 永久钉在 UNKNOWN，
  // 而 main.cpp 的自动校准条件是 state != UNKNOWN —— 两边互相堵死，
  // 结果就是「删了基准反而永远校准不了」（实测踩过）。
  if (!baseline_.valid) return false;
  PostureProfile::FeatureVec f{};
  std::string why;
  if (!BuildFeatureVector(m, baseline_, &f, &why)) {
    pending_label_.clear();
    pending_since_ = now;
    obs->profile_used = true;
    obs->profile_label.clear();
    obs->explanation = "画像分类缺关键量：" + why;
    // 关键量缺失：维持上一状态一小会儿，超时就报 UNKNOWN
    if (!active_label_.empty()) {
      obs->state = PostureState::kUnknown;
      return true;
    }
    obs->state = PostureState::kUnknown;
    return true;
  }

  const auto v = profile_->Classify(f, profile_accept_radius_);
  obs->profile_used = true;
  obs->profile_label = v.label;
  obs->profile_distance = v.distance;
  obs->profile_strength = v.strength;
  obs->profile_rule = v.matched_rule;

  // 标签保持：同一个标签连续 hold_s 秒才采纳，避免抖动
  if (v.label == pending_label_ && !v.label.empty()) {
    const double held = std::chrono::duration<double>(now - pending_since_).count();
    if (held >= profile_hold_s_ && v.label != active_label_) {
      const std::string prev = active_label_;
      active_label_ = v.label;
      LOG_INFO("画像判定切换：", prev.empty() ? "（无）" : PostureLabelZh(prev), " -> ",
               PostureLabelZh(active_label_), "  命中 ", v.matched_rule, "  强度 ",
               v.strength, "  特征 head_drop=", f[0], " ear_tilt=", f[1],
               " size=", f[3], " wrist_rise=", f[4], " wrist_near=", f[5]);
      // 切换到异常姿势时产生一次提醒触发器
      if (active_label_ != "normal" && active_label_ != "away") {
        RuleTrigger t;
        t.key = active_label_;
        t.label_zh = PostureLabelZh(active_label_);
        t.active_s = held;
        t.priority = 60;
        obs->triggers.push_back(t);
      }
    }
  } else if (v.label != pending_label_) {
    pending_label_ = v.label;
    pending_since_ = now;
  }

  // 长时间判不出来（距离一直超半径）→ 明确报 UNKNOWN
  if (pending_label_.empty()) {
    const double stuck =
        std::chrono::duration<double>(now - pending_since_).count();
    if (stuck >= profile_hold_s_ * 2.0) {
      active_label_.clear();
      obs->state = PostureState::kUnknown;
      obs->explanation = "姿势不在已校准的样本范围内（没命中任何已采集姿势）";
      return true;
    }
  }

  if (active_label_.empty()) {
    obs->state = PostureState::kUnknown;
    obs->explanation = "画像判定中…";
    return true;
  }
  if (active_label_ == "normal") {
    obs->state = PostureState::kNormal;
  } else if (active_label_ == "head_down") {
    obs->state = PostureState::kHeadDown;
  } else if (active_label_ == "head_tilt") {
    obs->state = PostureState::kHeadTilt;
  } else if (active_label_ == "too_close") {
    obs->state = PostureState::kTooClose;
  } else if (active_label_ == "hand_face") {
    obs->state = PostureState::kHandOnFace;
  } else {
    obs->state = PostureState::kNormal;
  }
  obs->active_keys.clear();
  if (active_label_ != "normal") obs->active_keys.push_back(active_label_);
  obs->explanation = std::string("（") + v.matched_rule + "）";
  return true;
}

float PostureAnalyzer::SmoothWidthRatio(float raw) {
  if (raw <= 0.f) return wr_count_ > 0 ? wr_window_[(wr_next_ + kSmoothWindow - 1) % kSmoothWindow] : 0.f;
  wr_window_[wr_next_] = raw;
  wr_next_ = (wr_next_ + 1) % kSmoothWindow;
  if (wr_count_ < kSmoothWindow) ++wr_count_;
  // 取窗口内中位数：抗离群（单帧把肩膀检飞不会带偏结果）。
  // 窗口很小，用插入排序比 std::sort 更合适，也避开 GCC 14 对
  // std::sort(std::array) 的 -Warray-bounds 误报。
  float sorted[kSmoothWindow];
  const int n = std::min(wr_count_, kSmoothWindow);
  for (int i = 0; i < n; ++i) {
    const float v = wr_window_[static_cast<size_t>(i)];
    int j2 = i - 1;
    while (j2 >= 0 && sorted[j2] > v) {
      sorted[j2 + 1] = sorted[j2];
      --j2;
    }
    sorted[j2 + 1] = v;
  }
  return sorted[n / 2];
}

void PostureAnalyzer::ResetSmoother() {
  wr_count_ = 0;
  wr_next_ = 0;
  wr_window_.fill(0.f);
  ear_count_ = 0;
  ear_next_ = 0;
  ear_window_.fill(0.f);
}

float PostureAnalyzer::SmoothEarAngle(float raw, bool valid) {
  if (!valid) {
    return ear_count_ > 0 ? ear_window_[(ear_next_ + kSmoothWindow - 1) % kSmoothWindow] : 0.f;
  }
  ear_window_[ear_next_] = raw;
  ear_next_ = (ear_next_ + 1) % kSmoothWindow;
  if (ear_count_ < kSmoothWindow) ++ear_count_;
  float sorted[kSmoothWindow];
  const int n = std::min(ear_count_, kSmoothWindow);
  for (int i = 0; i < n; ++i) {
    const float v = ear_window_[static_cast<size_t>(i)];
    int j2 = i - 1;
    while (j2 >= 0 && sorted[j2] > v) {
      sorted[j2 + 1] = sorted[j2];
      --j2;
    }
    sorted[j2 + 1] = v;
  }
  return sorted[n / 2];
}

void PostureAnalyzer::ResetRules() {
  pending_label_.clear();
  active_label_.clear();
  ResetSmoother();
  shoulder_rule_.Reset();  head_down_rule_.Reset();
  head_tilt_rule_.Reset();
  too_close_rule_.Reset();
  hand_face_rule_.Reset();
}

std::string PostureAnalyzer::CheckBaselineDrift(const PostureMetrics& m) const {
  if (!baseline_.valid || !m.valid) return {};
  if (baseline_.width_ratio <= 0.001) return {};   // 老基准没记这个字段，跳过检查

  // 基准本身的宽占比就不合理（历史坏基准，如 0.07）→ 直接判失效。
  // 只看「当前 vs 基准」的倍数会漏掉这种情况：坏基准会让倍数看起来正常。
  if (baseline_.width_ratio < 0.15 || baseline_.width_ratio > 0.80) {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "基准记录的肩宽占比 %.2f 不合理（正常 0.15~0.80），基准需重建",
                  baseline_.width_ratio);
    return buf;
  }

  // 距离：肩宽占画面宽的比例。挪动摄像头（远近变化）会让它成倍变化。
  const double ratio = m.width_ratio / baseline_.width_ratio;
  const double drift_hi = cfg_.baseline_drift_ratio;
  const double drift_lo = 1.0 / cfg_.baseline_drift_ratio;
  if (ratio > drift_hi || ratio < drift_lo) {
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
  if (std::fabs(dy) > cfg_.baseline_drift_head_dy) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "头肩纵向偏移 %.2f，校准基准是 %.2f（差 %.2f 个肩宽）",
                  m.head_offset_y, baseline_.head_offset_y, dy);
    return buf;
  }
  return {};
}

std::string PostureAnalyzer::CheckRollHint(const PostureMetrics& m) const {
  if (!baseline_.valid || !m.valid) return {};
  const float dev = LineAngleDelta(m.shoulder_angle_deg,
                                   static_cast<float>(baseline_.shoulder_angle_deg));
  // 15° 起提示。低于这个量级既可能是轻微歪坐，也可能是检测抖动，不值得打扰。
  if (dev < 15.f) return {};
  char buf[220];
  std::snprintf(buf, sizeof(buf),
                "肩线与校准基准差 %.0f°（当前 %.1f°，基准 %.1f°）。"
                "若是摄像头被转动/放歪了，请坐正后重新校准；"
                "若你确实正歪着坐，忽略此提示",
                dev, m.shoulder_angle_deg, baseline_.shoulder_angle_deg);
  return buf;
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

  // 备用头部参考点：耳朵中点 / 双眼中点。
  // 为什么需要：俯拍机位下正脸几乎不可见，鼻子的检测极不稳定
  //（实测 x 抖动 ±69px，ear 只有 ±5°），鼻子驱动的 head_offset_y 会抖 0.57，
  // 直接把 head_down 判据淹掉。耳朵在头两侧、俯拍可见，是更可靠的头高基准。
  {
    const Keypoint& ear_l = person.joints[kEarL];
    const Keypoint& ear_r = person.joints[kEarR];
    auto in_frame = [&](const Keypoint& k) {
      return k.x >= 0.f && k.x <= static_cast<float>(frame_width) && k.y >= 0.f &&
             k.y <= static_cast<float>(frame_height);
    };
    if (ear_l.confidence >= min_conf && ear_r.confidence >= min_conf &&
        in_frame(ear_l) && in_frame(ear_r)) {
      const float ear_d = Dist(ear_l.x, ear_l.y, ear_r.x, ear_r.y);
      if (ear_d > sw * 0.35f) {
        m.ear_mid_offset_y = ((ear_l.y + ear_r.y) * 0.5f - sy) / sw;
        m.ear_mid_valid = true;
      }
    }
    const Keypoint& eye_l = person.joints[kEyeL];
    const Keypoint& eye_r = person.joints[kEyeR];
    if (eye_l.confidence >= min_conf && eye_r.confidence >= min_conf) {
      const float eye_d = Dist(eye_l.x, eye_l.y, eye_r.x, eye_r.y);
      if (eye_d > sw * 0.08f) {
        m.eye_mid_offset_y = ((eye_l.y + eye_r.y) * 0.5f - sy) / sw;
        m.eye_mid_valid = true;
      }
    }
  }

  // 耳线角（歪头的可靠判据）。两只耳朵都可信时才算。
  const Keypoint& ear_l = person.joints[kEarL];
  const Keypoint& ear_r = person.joints[kEarR];
  // 耳朵出框时不认这一帧的耳线角：模型对画面外的关键点是「外推」出来的，
  // 角度会乱跳，会直接把歪头规则打成常亮。
  auto inside = [&](const Keypoint& k) {
    return k.x >= 0.f && k.x <= static_cast<float>(frame_width) && k.y >= 0.f &&
           k.y <= static_cast<float>(frame_height);
  };
  if (ear_l.confidence >= min_conf && ear_r.confidence >= min_conf &&
      inside(ear_l) && inside(ear_r)) {
    const float ear_dist = Dist(ear_l.x, ear_l.y, ear_r.x, ear_r.y);
    if (ear_dist > sw * 0.35f) {   // 两耳距离太小说明错了
      m.ear_angle_deg = NormalizeLineAngle(
          std::atan2(ear_r.y - ear_l.y, ear_r.x - ear_l.x) * kRad2Deg);
      m.ear_angle_valid = true;
    }
  }
  m.width_ratio = frame_width > 0 ? sw / static_cast<float>(frame_width) : 0.f;

  // 托腮：两只手腕里离鼻子更近的那个。
  // ★ 但只有「手腕抬到肩线之上」才算 —— 坐在桌前打字时，手在桌面上，
  //   图像里离胸/脸并不远（俯视机位尤其明显），不加上这个门控会一直误报托腮。
  //   实测：手放桌面时手腕比肩线低 95~109% 肩宽；托腮时一定高于肩线。
  // 托腮的两个量：手腕到鼻子的距离（越小越像托腮）、手腕相对肩线的高度。
  // 两者都从「最靠近鼻子的那只手腕」算，保证是同一只手。
  float best = std::numeric_limits<float>::max();
  float best_wrist_y = 0.f;
  for (const Keypoint* w : {&wr_l, &wr_r}) {
    if (w->confidence < min_conf) continue;
    m.wrist_valid = true;
    const float d = Dist(w->x, w->y, nose.x, nose.y) / sw;
    if (d < best) {
      best = d;
      best_wrist_y = w->y;
    }
  }
  m.wrist_to_nose_raw = m.wrist_valid ? best : 99.f;
  if (m.wrist_valid) m.wrist_y_rel = (best_wrist_y - sy) / sw;

  // 门控：手腕必须比「校准时的手腕高度」明显抬高。
  // ★ 不能用「手腕高于肩线」：俯拍机位下下巴与手腕的投影都落在肩线以下，
  //   实测托腮时 wrist_y_rel = 0.38（仍低于肩线），正常坐 1.19。
  //   用相对基准的高度差才分得开。
  bool gate_ok = false;
  if (!baseline_.valid || !baseline_.wrist_valid) {
    // 没有腕高基准时退回「高于肩线」这条保守规则
    gate_ok = m.wrist_valid && m.wrist_y_rel < -0.10f;
  } else {
    gate_ok = m.wrist_valid &&
              m.wrist_y_rel < static_cast<float>(baseline_.wrist_y_rel) -
                                  cfg_.hand_face_wrist_rise;
  }
  m.wrist_above_shoulder = gate_ok;
  m.wrist_to_nose = gate_ok ? m.wrist_to_nose_raw : 99.f;

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

  // ★ 取景门：直接检查「本系统真正用到的关键点」是否都在画面内。
  //
  // 为什么不用人体框判断：实测 box_y 可以稳定给出 -142px（顶边在画面外 30%），
  // 但同一时刻耳朵的关键点在 y=147~159，离上沿还有 147 像素 —— 完全可用。
  // 框是模型「外推」的，它默认人体应该延伸到画面外，拿它推断"头被裁"会误杀。
  //
  // 真正决定可判性的是：鼻子 / 双耳 / 双肩 是否落在画面内（留一点边距）。
  // 缺哪个就明确说缺哪个，比笼统报"机位不对"有用得多。
  {
    const float mx = frame_width * 0.015f;
    const float my = frame_height * 0.015f;
    auto inside = [&](const Keypoint& k) {
      return k.x >= mx && k.x <= frame_width - mx && k.y >= my &&
             k.y <= frame_height - my;
    };
    const Keypoint& nose = p.joints[kNose];
    const Keypoint& e_l = p.joints[kEarL];
    const Keypoint& e_r = p.joints[kEarR];
    const Keypoint& s_l = p.joints[kShoulderL];
    const Keypoint& s_r = p.joints[kShoulderR];
    if (!inside(nose)) return false;            // 原因由 SelectPrimary 细化
    if (!inside(s_l) || !inside(s_r)) return false;
    // 耳朵只有在本帧被判定可信时才要求它入画（不可信会自动退回别的判据）
    if (m.ear_angle_valid && (!inside(e_l) || !inside(e_r))) return false;
  }
  // ★ 不再用「人体框宽」判断双肩是否出框。
  // 框把张开的胳膊也包进去，桌面机位下框宽能到画面宽的 100%，
  // 但双肩关键点其实好端端在画面里 —— 已经用关键点判断了，这里再来一次
  // 就是误杀（实测：正常坐姿被判「双肩被裁（占宽 100%）」，一帧都采不到）。

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

  // ★ 尺寸门：主用户的肩宽占比不能远小于校准时的自己。
  // 现场实测：人离开后椅子/桌角被检出成「人」，框只有 95x118px（宽占比 0.12），
  // 而正常坐时是 0.35 —— 尺寸差 3 倍。这条同时解决了「人走了还报在座」
  // 和「小框被当成第二个人 → 误判多人」两个问题。
  if (baseline_.valid && baseline_.width_ratio > 0.01) {
    const float min_ratio =
        static_cast<float>(baseline_.width_ratio) * cfg_.min_size_ratio;
    if (m.width_ratio < min_ratio) return false;
  }

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
    if (p.box.Area() > biggest_area) {
      biggest_area = p.box.Area();
      biggest = &p;
      sel.reject_person_box = p.box;
    }
    if (!m.valid) {
      if (sel.reject_reason.empty()) sel.reject_reason = m.invalid_reason;
      continue;
    }
    // 拒因要具体到「缺哪个关键点」——现场调机位才有方向
    if (sel.reject_reason.empty()) {
      // Plausible 里因为关键点出框被拒（取景门），细化原因
      const float mx = frame_width * 0.015f;
      const float my = frame_height * 0.015f;
      auto out_of = [&](const Keypoint& k) {
        return k.x < mx || k.x > frame_width - mx || k.y < my ||
               k.y > frame_height - my;
      };
      if (out_of(p.joints[kNose])) {
        sel.reject_reason = "鼻子关键点跑到画面外了，请调整摄像头角度让正脸入画";
      } else if (out_of(p.joints[kShoulderL]) || out_of(p.joints[kShoulderR])) {
        sel.reject_reason = "双肩有被裁掉的，请坐远一点或把摄像头后退";
      } else {
        sel.reject_reason = "几何特征不可用（可能有半个身子在画面外）";
      }
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
    shoulder_rule_.Update(std::nullopt, now);
    head_down_rule_.Update(std::nullopt, now);
    head_tilt_rule_.Update(std::nullopt, now);
    too_close_rule_.Update(std::nullopt, now);
    hand_face_rule_.Update(std::nullopt, now);
    pending_label_.clear();
    pending_since_ = now;
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

  // too_close 用平滑后的 width_ratio；头肩纵向/肩线角保持原始（姿态变化要即时响应）
  m.width_ratio_raw = m.width_ratio;
  m.width_ratio = SmoothWidthRatio(m.width_ratio_raw);
  // 耳线角也平滑（单帧跳变 25° 会把歪头判据打断）
  m.ear_angle_deg = SmoothEarAngle(m.ear_angle_deg, m.ear_angle_valid);

  last_metrics_ = m;
  last_valid_at_ = now;
  obs.metrics = m;

  if (sel.ambiguous) {
    pending_label_.clear();
    pending_since_ = now;
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

  // ---- 画像模式：用「最近原型 + 标签保持」判定，不再依赖手调阈值 ----
  if (profile_active()) {
    if (ProfileUpdate(m, now, &obs)) {
      return obs;
    }
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
    // 低头：优先用耳中点（稳定），没有才退回鼻子。
    // 必须两侧用同一个参考点算差值，否则基准和当前不可比。
    if (m.ear_mid_valid && baseline_.ear_mid_valid) {
      v_head_down = std::max(0.f, m.ear_mid_offset_y -
                                      static_cast<float>(baseline_.ear_mid_offset_y));
    } else {
      v_head_down = std::max(0.f, m.head_offset_y -
                                      static_cast<float>(baseline_.head_offset_y));
    }
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
    RuleDiag dg;
    dg.key = rule.cfg().key;
    dg.has_value = r.valid;
    dg.value = r.value;
    dg.trigger = rule.cfg().trigger;
    dg.latched = r.latched;
    dg.held_s = r.held_s;
    dg.hold_s = rule.cfg().hold_s;
    obs.rule_diags.push_back(dg);
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
