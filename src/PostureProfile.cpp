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
    "head_drop", "ear_tilt", "shoulder_tilt", "size", "wrist_rise", "wrist_near"};

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
  return FeatureVec{0.05, 3.0, 2.0, 0.04, 0.10, 0.15};
}

const char* PostureLabelZh(const std::string& label) {
  if (label == "normal") return "正常";
  if (label == "head_down") return "低头/趴桌";
  if (label == "head_tilt") return "歪头";
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
  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto cols = SplitTab(line);
    if (cols.empty()) continue;
    if (cols[0] == "scale" && cols.size() >= 1 + kNumFeatures) {
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
      char buf[96];
      if (r.guard_feature >= 0) {
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
                                                 double /*accept_radius*/) const {
  Verdict v;
  if (rules_.empty()) return v;

  // 按优先级依次试，第一个命中的就是结论。
  // 「越过阈值多少」除以该特征的有意义尺度，作为确信度。
  for (const auto& r : rules_) {
    if (r.feature < 0 || r.feature >= kNumFeatures) continue;
    // 互斥门控：不满足就跳过这条规则，试下一条
    if (r.guard_feature >= 0 && r.guard_feature < kNumFeatures &&
        f[r.guard_feature] >= r.guard_max) {
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

  // 全部没命中：如果 normal 存在且各特征都在 normal 的合理范围内 → normal
  for (const auto& c : classes_) {
    if (c.label != "normal") continue;
    double worst = 0.0;
    for (int i = 0; i < kNumFeatures; ++i) {
      const double spread = std::max({c.mad[i], floors_[i], 1e-9});
      worst = std::max(worst, std::fabs(f[i] - c.median[i]) / spread);
    }
    v.strength = -worst;
    v.distance = 1.0 / (std::fabs(v.strength) + 1.0);
    v.label = "normal";
    char buf[80];
    std::snprintf(buf, sizeof(buf), "全部规则未命中（离正常最近的特征偏差 %.1fσ）",
                  worst);
    v.matched_rule = buf;
    return v;
  }
  return v;
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
  *out = PostureProfile::FeatureVec{head_drop, ear_tilt, shoulder_tilt, size,
                                    wrist_rise, wrist_near};
  return true;
}

}  // namespace posture
