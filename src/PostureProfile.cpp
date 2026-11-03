#include "posture/PostureProfile.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "posture/Logger.hpp"
#include "posture/Types.hpp"

namespace posture {
namespace {

const char* kFeatureNames[PostureProfile::kNumFeatures] = {
    "head_drop",   "ear_tilt",   "shoulder_tilt", "size",
    "wrist_rise",  "wrist_near", "wrist_face"};

double Median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5;
}

std::vector<std::string> SplitTab(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

bool ParseD(const std::string& s, double* out) {
  if (s.empty()) return false;
  try {
    size_t used = 0;
    *out = std::stod(s, &used);
    return used > 0;
  } catch (...) {
    return false;
  }
}

void EnsureParentDir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return;
  const std::string dir = path.substr(0, slash);
  std::string cur;
  std::istringstream is(dir);
  std::string part;
  if (!dir.empty() && dir[0] == '/') cur = "/";
  while (std::getline(is, part, '/')) {
    if (part.empty()) continue;
    cur += part;
    ::mkdir(cur.c_str(), 0755);
    cur += "/";
  }
}

}  // namespace

const char* PostureProfile::FeatureName(int i) {
  if (i < 0 || i >= kNumFeatures) return "?";
  return kFeatureNames[i];
}

PostureProfile::FeatureVec PostureProfile::DefaultFloors() {
  // 每格都是「低于这个变化量就没有实际意义」的量级：
  //   head_drop 0.05 肩宽 ≈ 12px（肩宽 240px 时）—— 低于此算噪声
  //   ear_tilt 3°、shoulder_tilt 2° —— 关键点量化噪声的本机实测水平
  //   size 0.04 —— 5% 以内的距离变化不值得报
  //   wrist_rise 0.10 肩宽 —— 手腕没明显抬起来
  //   wrist_near 0.15 肩宽 —— 手腕离鼻子差 0.15 肩宽以上才有意义
  //   wrist_face 0.15 肩宽 —— 腕到耳中点同理
  return FeatureVec{0.05, 3.0, 2.0, 0.04, 0.10, 0.15, 0.15};
}

PostureProfile::FeatureVec PostureProfile::ReleaseScale() {
  // 迟滞带的基准量：比 DefaultFloors 大约一个数量级，取「能看得出来的变化」。
  // 实测对照（114 帧真标注的特征分布，见 evidence/posture-precision-2026-11-02）：
  //   head_drop   normal -0.033 -> head_down 0.174，跨距 0.207；迟滞带取本色量级
  //   ear_tilt    normal 3.96  -> head_tilt 18.79，跨距 14.8
  //   size        normal 1.056 -> too_close 1.438，跨距 0.38（注意 size 尺度很大，
  //               旧注释里“0.04 = 5% 变化”是错的：0.04 实际上是相对于基准
  //               肩宽占比的 4%，在不同机位下对应像素差异不等，真正能区分的
  //               量级在 0.1 以上）
  //   wrist_rise  normal -0.044 -> hand_face 0.340，跨距 0.384
  return FeatureVec{0.08, 2.0, 2.0, 0.10, 0.10, 0.15, 0.15};
}

const char* PostureLabelZh(const std::string& label) {
  if (label == "normal") return "正常";
  if (label == "head_down") return "低头/趴桌";
  if (label == "head_tilt") return "歪头";
  if (label == "shoulder_tilt") return "高低肩";
  if (label == "too_close") return "离屏太近";
  if (label == "hand_face") return "托腮";
  if (label == "away") return "无人";
  return "未知";
}

bool PostureProfile::Load(const std::string& path, std::string* err) {
  std::ifstream ifs(path);
  if (!ifs) {
    if (err) *err = "打不开画像文件 " + path;
    return false;
  }
  classes_.clear();
  rules_.clear();
  scales_ = floors_;
  baseline_id_.clear();
  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto cols = SplitTab(line);
    if (cols.empty()) continue;
    if (cols[0] == "baseline_id" && cols.size() == 2) {
      baseline_id_ = cols[1];
    } else if (cols[0] == "scale" && cols.size() >= 1 + kNumFeatures) {
      for (int i = 0; i < kNumFeatures; ++i) {
        double v = 0;
        if (ParseD(cols[1 + i], &v) && v > 0) scales_[i] = v;
      }
    } else if (cols[0] == "class" && cols.size() >= 3 + kNumFeatures) {
      ClassStat cs;
      cs.label = cols[1];
      double n = 0;
      if (ParseD(cols[2], &n)) cs.samples = static_cast<int>(n);
      bool ok = true;
      for (int i = 0; i < kNumFeatures; ++i) {
        if (!ParseD(cols[3 + i], &cs.median[i])) ok = false;
      }
      for (int i = 0; i < kNumFeatures && 3 + kNumFeatures + i < static_cast<int>(cols.size());
           ++i) {
        if (!ParseD(cols[3 + kNumFeatures + i], &cs.mad[i])) ok = false;
      }
      // Zero MAD is also legitimate for stable measurements. Only absent
      // wrist-distance sentinels are missing; valid constants still use floors.
      for (int i = 0; i < kNumFeatures; ++i) {
        cs.mad_missing[i] = !std::isfinite(cs.median[i]) ||
            (i >= 5 && cs.median[i] >= 2.99 && cs.mad[i] <= 0.0);
      }
      if (ok) classes_.push_back(std::move(cs));
    } else if (cols[0] == "rule" && cols.size() >= 7) {
      // rule <priority> <label> <feature> <direction> <threshold> <normal_med> <class_med>
      Rule r;
      double v = 0;
      if (ParseD(cols[1], &v)) r.priority = static_cast<int>(v);
      r.label = cols[2];
      for (int i = 0; i < kNumFeatures; ++i) {
        if (cols[3] == kFeatureNames[i]) r.feature = i;
      }
      r.direction = (cols[4] == "-") ? -1 : 1;
      if (!ParseD(cols[5], &r.threshold)) continue;
      if (cols.size() > 6) ParseD(cols[6], &r.normal_median);
      if (cols.size() > 7) ParseD(cols[7], &r.class_median);
      if (cols.size() > 9) {
        for (int i = 0; i < kNumFeatures; ++i) {
          if (cols[8] == kFeatureNames[i]) r.guard_feature = i;
        }
        ParseD(cols[9], &r.guard_max);
      }
      // 第二条门控：rule ... <g1> <max1> <g2> <dir2> <max2>
      if (cols.size() > 12) {
        for (int i = 0; i < kNumFeatures; ++i) {
          if (cols[10] == kFeatureNames[i]) r.guard2_feature = i;
        }
        double d2 = -1;
        ParseD(cols[11], &d2);
        r.guard2_direction = d2 < 0 ? -1 : 1;
        ParseD(cols[12], &r.guard2_max);
      }
      if (r.feature < 0) continue;
      rules_.push_back(std::move(r));
    }
  }
  if (classes_.empty() && rules_.empty()) {
    if (err) *err = "画像文件里没有任何 class / rule 行: " + path;
    return false;
  }
  SortRules();
  return true;
}

bool PostureProfile::Save(const std::string& path, std::string* err) const {
  EnsureParentDir(path);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream ofs(tmp, std::ios::trunc);
    if (!ofs) {
      if (err) *err = "写不了 " + tmp;
      return false;
    }
    ofs << "# posture-profile v2 —— 逐姿势校准画像（有序级联判定）\n";
    ofs << "# rule 行：判定用。按 priority 从小到大依次试，第一个命中的就是结论。\n";
    ofs << "#   rule <优先级> <标签> <特征> <方向> <阈值> <normal中位> <该类中位>\n";
    ofs << "#   方向 + 表示特征 >= 阈值命中，- 表示特征 <= 阈值命中\n";
    ofs << "#   阈值 = (normal中位 + 该类中位) / 2，全部由采集数据自动算出\n";
    ofs << "# class 行：原始统计（诊断用，不参与判定）\n";
    ofs << "#   class <标签> <样本数> <特征中位数> <特征MAD>\n";
    ofs << "# 特征顺序：";
    for (int i = 0; i < kNumFeatures; ++i) {
      ofs << (i ? " " : "") << kFeatureNames[i];
    }
    ofs << "\n";
    if (!baseline_id_.empty()) ofs << "baseline_id\t" << baseline_id_ << "\n";
    ofs << "scale";
    for (int i = 0; i < kNumFeatures; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "\t%.4f", scales_[i]);
      ofs << buf;
    }
    ofs << "\n";
    for (const auto& c : classes_) {
      ofs << "class\t" << c.label << "\t" << c.samples;
      for (int i = 0; i < kNumFeatures; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\t%.4f", c.median[i]);
        ofs << buf;
      }
      for (int i = 0; i < kNumFeatures; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\t%.4f", c.mad[i]);
        ofs << buf;
      }
      ofs << "\n";
    }
    for (const auto& r : rules_) {
      char buf[160];
      if (r.guard2_feature >= 0) {
        std::snprintf(buf, sizeof(buf),
                      "rule\t%d\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%s\t%.4f\t%s\t%d\t%.4f\n",
                      r.priority, r.label.c_str(), kFeatureNames[r.feature],
                      r.direction < 0 ? "-" : "+", r.threshold, r.normal_median,
                      r.class_median,
                      r.guard_feature >= 0 ? kFeatureNames[r.guard_feature] : "none",
                      r.guard_feature >= 0 ? r.guard_max : 0.0,
                      kFeatureNames[r.guard2_feature], r.guard2_direction, r.guard2_max);
      } else if (r.guard_feature >= 0) {
        std::snprintf(buf, sizeof(buf),
                      "rule\t%d\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%s\t%.4f\n",
                      r.priority, r.label.c_str(), kFeatureNames[r.feature],
                      r.direction < 0 ? "-" : "+", r.threshold, r.normal_median,
                      r.class_median, kFeatureNames[r.guard_feature], r.guard_max);
      } else {
        std::snprintf(buf, sizeof(buf), "rule\t%d\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\n",
                      r.priority, r.label.c_str(), kFeatureNames[r.feature],
                      r.direction < 0 ? "-" : "+", r.threshold, r.normal_median,
                      r.class_median);
      }
      ofs << buf;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = "rename 失败 " + tmp + " -> " + path;
    return false;
  }
  return true;
}

bool PostureProfile::Has(const std::string& label) const {
  for (const auto& c : classes_) {
    if (c.label == label) return true;
  }
  return false;
}

std::vector<std::string> PostureProfile::labels() const {
  std::vector<std::string> out;
  for (const auto& c : classes_) out.push_back(c.label);
  return out;
}

int PostureProfile::total_samples() const {
  int n = 0;
  for (const auto& c : classes_) n += c.samples;
  return n;
}

void PostureProfile::SetClass(const std::string& label,
                              const std::vector<FeatureVec>& samples) {
  if (samples.empty()) return;
  ClassStat cs;
  cs.label = label;
  cs.samples = static_cast<int>(samples.size());
  for (int i = 0; i < kNumFeatures; ++i) {
    std::vector<double> vals;
    vals.reserve(samples.size());
    for (const auto& s : samples) vals.push_back(s[i]);
    const double med = Median(vals);
    cs.median[i] = med;
    // MAD：中位绝对偏差，比标准差稳健（不受个别跳变帧影响）
    std::vector<double> dev;
    dev.reserve(vals.size());
    for (double v : vals) dev.push_back(std::fabs(v - med));
    cs.mad[i] = Median(dev);
    cs.mad_missing[i] = !std::isfinite(med) ||
        (i >= 5 && med >= 2.99 && cs.mad[i] <= 0.0);
  }
  for (auto& c : classes_) {
    if (c.label == label) {
      c = cs;
      RecomputeScales();
      return;
    }
  }
  classes_.push_back(std::move(cs));
  RecomputeScales();
}

void PostureProfile::SetRule(const Rule& r) {
  for (auto& e : rules_) {
    if (e.label == r.label) {
      e = r;
      return;
    }
  }
  rules_.push_back(r);
}

// 门控统一定义在这里，Classify / ClassifyHysteretic 共用，避免两条路径漂移。
// guard : 单向，需 f[g] <  guard_max
// guard2: 双向（guard2_direction 决定 <= 还是 >=）
bool PostureProfile::GuardsPass(const Rule& r, const FeatureVec& f) {
  if (r.guard_feature >= 0 && r.guard_feature < kNumFeatures &&
      f[r.guard_feature] >= r.guard_max) {
    return false;
  }
  if (r.guard2_feature >= 0 && r.guard2_feature < kNumFeatures) {
    const double g = f[r.guard2_feature];
    if (r.guard2_direction < 0) {
      if (g > r.guard2_max) return false;
    } else {
      if (g < r.guard2_max) return false;
    }
  }
  return true;
}

void PostureProfile::SortRules() {
  std::sort(rules_.begin(), rules_.end(),
            [](const Rule& a, const Rule& b) { return a.priority < b.priority; });
}

void PostureProfile::RecomputeScales() {
  for (int i = 0; i < kNumFeatures; ++i) {
    std::vector<double> mads;
    for (const auto& c : classes_) mads.push_back(c.mad[i]);
    double s = mads.empty() ? floors_[i] : Median(mads);
    if (!(s > floors_[i])) s = floors_[i];
    scales_[i] = s;
  }
}

PostureProfile::Verdict PostureProfile::Classify(const FeatureVec& f,
                                                 double accept_radius,
                                                 const std::string& excluded_label) const {
  Verdict v;
  if (rules_.empty()) return v;
  if (!std::all_of(f.begin(), f.end(), [](double x) { return std::isfinite(x); })) return v;

  // 按优先级依次试，第一个命中的就是结论。
  // 「越过阈值多少」除以该特征的有意义尺度，作为确信度。
  for (const auto& r : rules_) {
    if (r.label == excluded_label) continue;
    if (r.feature < 0 || r.feature >= kNumFeatures) continue;
    // 门控：两条都不满足就跳过这条规则，试下一条
    if (!GuardsPass(r, f)) {
      v.all.emplace_back(r.label, 1e9);
      continue;
    }
    const double floor = std::max(floors_[r.feature], 1e-9);
    const double over = (f[r.feature] - r.threshold) * r.direction;
    v.all.emplace_back(r.label, over > 0 ? 0.0 : -over / floor);
    if (over <= 0) continue;                    // 没命中，试下一条

    v.label = r.label;
    v.strength = over / floor;
    v.distance = 1.0 / (v.strength + 1.0);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s %s %.3f（阈值 %.3f，实测 %.3f）",
                  kFeatureNames[r.feature], r.direction < 0 ? "<=" : ">=",
                  r.threshold, r.threshold, f[r.feature]);
    v.matched_rule = buf;
    return v;
  }

  return NormalVerdict(f, accept_radius, excluded_label);
}

PostureProfile::Verdict PostureProfile::NormalVerdict(const FeatureVec& f,
                                                     double radius,
                                                     const std::string& excluded_label) const {
  Verdict v;
  if (!(radius > 0.0) || !std::isfinite(radius)) radius = 3.0;
  for (const auto& c : classes_) {
    if (c.label != "normal") continue;
    double worst = 0.0;
    int measured = 0;
    for (int i = 0; i < kNumFeatures; ++i) {
      if (c.mad_missing[i]) continue;
      // A rejected contact candidate (hand measurably away from face) does not
      // make an otherwise normal pose abnormal just because the arm was raised.
      if (excluded_label == "hand_face" && i >= 4) continue;
      // Missing live wrist distances cannot prove normality in that dimension.
      if (i >= 5 && f[i] >= 2.99) continue;
      if (!std::isfinite(f[i])) return v;
      const double spread = std::max({c.mad[i], floors_[i], 1e-9});
      double deviation = std::fabs(f[i] - c.median[i]);
      // Moving farther from the screen/face is evidence against too-close/contact,
      // not an unsupported unhealthy pose. Frame geometry still has its own gate.
      if (i == 3) deviation = std::max(0.0, f[i] - c.median[i]);
      if (i >= 5) deviation = std::max(0.0, c.median[i] - f[i]);
      worst = std::max(worst, deviation / spread);
      ++measured;
    }
    v.strength = -worst;
    v.distance = worst;
    if (measured > 0 && worst <= radius) v.label = "normal";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "未命中已知异常；正常样本最大偏差 %.1f / %.1f%s",
                  worst, radius, v.label.empty() ? "，无法可靠判断" : "");
    v.matched_rule = buf;
    return v;
  }
  return v;
}

PostureProfile::Verdict PostureProfile::ClassifyHysteretic(
    const FeatureVec& f, const std::string& active_label,
    double release_ratio, const std::string& excluded_label, double normal_radius) const {
  Verdict v;
  if (rules_.empty()) return v;
  if (!std::all_of(f.begin(), f.end(), [](double x) { return std::isfinite(x); })) return v;

  // 没有当前标签（刚启动 / 刚丢过状态）时，迟滞毫无意义，走纯阈值判定。
  if (active_label.empty()) return Classify(f, normal_radius, excluded_label);

  const Rule* active_rule = nullptr;
  for (const auto& r : rules_) {
    if (r.label == active_label) {
      active_rule = &r;
      break;
    }
  }

  for (const auto& r : rules_) {
    if (r.label == excluded_label) continue;
    if (r.feature < 0 || r.feature >= kNumFeatures) continue;
    if (!GuardsPass(r, f)) {
      v.all.emplace_back(r.label, 1e9);
      continue;
    }
    const double floor = std::max(floors_[r.feature], 1e-9);
    // 当前已采纳的标签用【释放阈值】判定：必须退回 release_band 才算离开。
    // 其它标签仍用原阈值 —— 只给“退出”加惰性，不降低“进入”的灵敏度。
    const bool is_active = (active_rule != nullptr && r.label == active_rule->label);
    const double thr =
        is_active ? r.threshold - r.direction * RuleReleaseBand(r, release_ratio)
                  : r.threshold;
    const double over = (f[r.feature] - thr) * r.direction;
    v.all.emplace_back(r.label, over > 0 ? 0.0 : -over / floor);
    if (over <= 0) continue;

    v.label = r.label;
    v.strength = over / floor;
    v.distance = 1.0 / (v.strength + 1.0);
    char buf[96];
    if (is_active) {
      std::snprintf(buf, sizeof(buf), "%s %s %.3f（释放阈 %.3f，实测 %.3f）",
                    kFeatureNames[r.feature], r.direction < 0 ? "<=" : ">=",
                    thr, thr, f[r.feature]);
    } else {
      std::snprintf(buf, sizeof(buf), "%s %s %.3f（阈值 %.3f，实测 %.3f）",
                    kFeatureNames[r.feature], r.direction < 0 ? "<=" : ">=",
                    r.threshold, r.threshold, f[r.feature]);
    }
    v.matched_rule = buf;
    return v;
  }

  return NormalVerdict(f, normal_radius, excluded_label);
}

std::string PostureProfile::BaselineId(const Baseline& b) {
  if (!b.valid || b.created_at.empty()) return "";
  return b.created_at + "|" + b.camera_id + "|" + b.model_sha256 + "|" +
      std::to_string(b.frame_width) + "x" + std::to_string(b.frame_height);
}

void PostureProfile::BindBaseline(const Baseline& baseline) {
  baseline_id_ = BaselineId(baseline);
}

bool PostureProfile::MatchesBaseline(const Baseline& baseline, std::string* why) const {
  const auto current = BaselineId(baseline);
  if (baseline_id_.empty() || current.empty()) {
    if (why) *why = "姿势画像缺少标定版本，请重新采集并生成画像";
    return false;
  }
  if (baseline_id_ != current) {
    if (why) *why = "姿势画像与当前标定不匹配，请按当前标定重新采集画像";
    return false;
  }
  return true;
}

double PostureProfile::RuleReleaseBand(const Rule& r, double release_ratio) const {
  if (release_ratio <= 0.0) return 0.0;
  if (r.feature < 0 || r.feature >= kNumFeatures) return 0.0;
  const FeatureVec rs = ReleaseScale();
  // 同时被 DefaultFloors 封顶，避免某个特征的 release 带小到不起作用。
  const double band = std::max(rs[r.feature], floors_[r.feature]) * release_ratio;
  // 不能超过「阈值到 normal 中位数」的跨度，否则释放带会比整条规则还宽。
  const double span = std::fabs(r.threshold - r.normal_median);
  if (span > 1e-9) return std::min(band, span);
  return band;
}

std::vector<PostureProfile::Separability> PostureProfile::separability() const {
  std::vector<Separability> out(kNumFeatures);
  if (classes_.size() < 2) return out;
  for (int i = 0; i < kNumFeatures; ++i) {
    // 找中位数差最大的两类，再看它们各自的 MAD 有多小
    double best_gap = -1.0;
    const ClassStat* pa = nullptr;
    const ClassStat* pb = nullptr;
    for (size_t a = 0; a < classes_.size(); ++a) {
      for (size_t b = a + 1; b < classes_.size(); ++b) {
        const double gap = std::fabs(classes_[a].median[i] - classes_[b].median[i]);
        if (gap > best_gap) {
          best_gap = gap;
          pa = &classes_[a];
          pb = &classes_[b];
        }
      }
    }
    Separability sp;
    if (pa && pb) {
      // 类内离散：取两类的平均 MAD（离得最远的那两类），再与最小 MAD 比较
      const double within = std::max(1e-6, (pa->mad[i] + pb->mad[i]) * 0.5);
      // 全体类里最小的 MAD —— 用于判断「能不能分开」
      double min_mad = 1e9;
      for (const auto& c : classes_) min_mad = std::min(min_mad, c.mad[i]);
      sp.gap = best_gap;
      sp.worst_label = pa->label;
      sp.other_label = pb->label;
      sp.fisher = best_gap / within;
      (void)min_mad;
    }
    out[i] = sp;
  }
  return out;
}

bool BuildFeatureVector(const PostureMetrics& m, const Baseline& b,
                        PostureProfile::FeatureVec* out, std::string* why) {
  if (!b.valid) {
    if (why) *why = "没有校准基准";
    return false;
  }
  if (!m.valid) {
    if (why) *why = m.invalid_reason.empty() ? "本帧指标无效" : m.invalid_reason;
    return false;
  }
  const bool use_ear = m.ear_mid_valid && b.ear_mid_valid;
  double head_drop = 0.0;
  if (use_ear) {
    head_drop = m.ear_mid_offset_y - b.ear_mid_offset_y;   // 正 = 头更低
  } else {
    head_drop = m.head_offset_y - b.head_offset_y;
  }
  // 歪头用「耳线相对肩线的夹角」，不用耳线的绝对角。
  // 为什么：绝对耳线角跨会话能漂 8°（人抬头/转头的姿势、关键点 stride-8 量化），
  // 实测同一人「正常坐」两次校准能差 16°，直接让歪头规则常亮。
  // 减去肩线角可以消掉摄像头 roll 和整个人歪着坐的影响 —— 实测类内 MAD
  // 从 0.2° 降到 0.1°，分离比从 2 提到 226。
  const double ear_tilt =
      (m.ear_angle_valid && b.ear_angle_valid)
          ? std::fabs(std::remainder((m.ear_angle_deg - m.shoulder_angle_deg) -
                      (b.ear_angle_deg - b.shoulder_angle_deg), 180.0))
          : 0.0;
  const double shoulder_tilt = std::fabs(std::remainder(m.shoulder_angle_deg - b.shoulder_angle_deg, 180.0));
  const double size = (b.width_ratio > 0.01) ? (m.width_ratio / b.width_ratio) : 1.0;
  double wrist_rise = 0.0;
  if (m.wrist_valid && b.wrist_valid) {
    wrist_rise = b.wrist_y_rel - m.wrist_y_rel;            // 正 = 手抬起来了
  }
  // 手腕到鼻子的距离（未经门控的原始值；99 是"手腕不可信"的哨兵，映射成 3.0）
  double wrist_near = m.wrist_valid ? static_cast<double>(m.wrist_to_nose_raw) : 3.0;
  if (wrist_near > 3.0) wrist_near = 3.0;
  // 手腕到耳中点距离 / 肩宽 —— 「手真的贴在脸旁」的直接度量。
  // 比 wrist_near（到鼻子）稳：鼻子在俯拍下不可靠，耳中点稳得多。
  double wrist_face = m.face_geom_valid ? static_cast<double>(m.face_wrist_ear) : 3.0;
  if (wrist_face > 3.0) wrist_face = 3.0;
  *out = PostureProfile::FeatureVec{head_drop, ear_tilt, shoulder_tilt, size,
                                    wrist_rise, wrist_near, wrist_face};
  return true;
}

}  // namespace posture
