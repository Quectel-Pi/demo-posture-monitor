// 逐姿势校准画像（per-posture profile）
//
// 为什么要有它：靠手调阈值判断坐姿非常脆弱 —— 每个阈值都强依赖机位、镜头、
// 人的体型。本项目已经踩过好几次（低头阈值在俯视机位下物理上不可能达到、
// 离屏阈值写死绝对值换机位就失效……）。换个思路：
//   让用户对每个姿势各采集一段样本，存下该姿势在「本机 + 本人 + 本机位」下
//   的特征分布，运行期用「最近原型」分类。
//
// 【v2：有序级联判定】
// v1 用的是「最近原型」（六维欧氏距离），实测不行：
//   · 歪头时一只耳朵下沉，head_drop 涨到 0.18，和低头的 0.32 混（10/25 帧判错）
//   · 前倾时手也跟着靠近脸，wrist 特征和托腮完全重叠（7/21 帧判错）
// 六个特征等权必然互相串。改成「每个姿势只用它自己那一个最可靠的特征，
// 按物理优先级依次试，第一个命中的就是结论」：
//
//   优先级 1  ear_tilt   歪头     —— 耳线角度和你离屏幕多远无关，最干净
//   优先级 2  head_drop  低头     —— 必须在歪头之后，因为歪头也会压低耳中点
//   优先级 3  size       凑近     —— 必须在托腮之前，因为前倾时手一定靠脸
//   优先级 4  wrist_near 托腮     —— 手到鼻子的距离，托腮独有
//
// 每个规则只用 1 个特征 + 1 个阈值，阈值取「normal 中位数」与「该姿势中位数」
// 的中点。全部由采集数据自动算出，不是手调的。
//
// 特征全部是「相对正常坐姿基准」的物理量，因此天然与机型/机位无关：
//   0 head_drop      头部比基准低多少（正 = 更低，单位：肩宽）
//   1 ear_tilt       耳线比基准歪多少（绝对值，度）—— 歪头
//   2 shoulder_tilt  肩线比基准歪多少（绝对值，度）—— 高低肩
//   3 size           肩宽占比 / 基准肩宽占比（>1 = 更近）—— 离屏过近
//   4 wrist_rise     手腕比基准抬高多少（正 = 抬手，单位：肩宽）
//   5 wrist_near     手腕到鼻子的距离（肩宽为单位，越小越像托腮）
//
// 为什么要 5 和 6 两个都留：前倾时手会跟着抬，单看 wrist_rise 会把
// 「凑近」误判成「托腮」（实测 7/21 帧）。而托腮时手腕到鼻子只有 0.59 肩宽，
// 前倾时是 1.12 —— 加上 wrist_near 两者才分得开。
//
// 存储格式故意用「行式文本」而不是 JSON：板端没有 JSON 解析器，
// 而这个文件人也要能直接看懂、能 diff。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace posture {

class PostureProfile {
 public:
  static constexpr int kNumFeatures = 6;

  using FeatureVec = std::array<double, kNumFeatures>;

  struct ClassStat {
    std::string label;                        // normal / head_down / head_tilt / ...
    int samples = 0;
    FeatureVec median{};                      // 每个特征的样本中位数
    FeatureVec mad{};                         // 每个特征的中位绝对偏差（稳健离散度）
  };

  // 一条判定规则：某姿势用「某一个特征」过阈值来识别
  struct Rule {
    int priority = 0;          // 越小越先判定
    std::string label;         // 命中后返回的标签
    int feature = -1;          // 用哪个特征（下标）
    int direction = 1;         // +1 = 特征 >= 阈值触发；-1 = <= 触发
    double threshold = 0.0;    // 阈值（normal 与 该类 中位数的中点）
    double normal_median = 0.0;// 诊断用
    double class_median = 0.0; // 诊断用
    // 互斥门控：guard_feature >= 0 时，只有 f[guard_feature] < guard_max 才允许命中。
    // 用途：歪头会压低一侧耳朵，head_drop 跟着涨（实测实测 0.145），
    // 于是「歪头」会被「低头」抢走。耳线已经明显歪了就不该再判低头。
    int guard_feature = -1;
    double guard_max = 0.0;
  };

  static const char* FeatureName(int i);
  // 各特征的「最小有意义尺度」，用于防止某一特征 MAD 过小而独占权重
  static FeatureVec DefaultFloors();

  // ---- 文件读写（行式文本，见本文件顶部说明）----
  bool Load(const std::string& path, std::string* err);
  bool Save(const std::string& path, std::string* err) const;

  bool empty() const { return classes_.empty(); }
  bool Has(const std::string& label) const;
  const std::vector<ClassStat>& classes() const { return classes_; }
  const FeatureVec& scales() const { return scales_; }
  std::vector<std::string> labels() const;

  // 用一批原始样本建立/替换某个类（会自动算中位数与 MAD）
  void SetClass(const std::string& label, const std::vector<FeatureVec>& samples);
  // 设定某类的判定规则（由采集脚本按数据算出）
  void SetRule(const Rule& r);
  const std::vector<Rule>& rules() const { return rules_; }
  // 清空全部规则（重建画像时用）
  void ClearRules() { rules_.clear(); }
  // 按 priority 排序
  void SortRules();
  // 重新计算跨类尺度（取各类 MAD 的中位数，并用 floor 兜底）
  void RecomputeScales();

  // ---- 分类 ----
  struct Verdict {
    std::string label;          // 命中类；空 = 都不像（UNKNOWN）
    // 命中规则时 = 该特征越过阈值多少倍「有意义尺度」，越大越确定
    double strength = 0.0;
    double distance = 0.0;      // 兼容字段：= 1/(strength+1)，<=accept_radius 视为接受
    std::string matched_rule;   // 例如 "ear_tilt >= 16.45"
    std::vector<std::pair<std::string, double>> all;  // 未命中的各规则距离（升序）
  };
  // accept_radius 已不再决定是否命中（规则命中与否由阈值决定），
  // 仅用于兼容旧调用；保留参数。
  Verdict Classify(const FeatureVec& f, double accept_radius) const;

  // 分离度诊断：某特征在各类中位数之间的最大差 / 类内离散度。
  // 值 < 1 说明该特征区分不开这些姿势（数据本身不可分，调阈值无用）。
  struct Separability {
    double fisher = 0.0;        // 最大类间差 / 平均类内离散
    std::string worst_label;    // 最难分的两类
    std::string other_label;
    double gap = 0.0;           // 这两类在该特征上的中位数差
  };
  std::vector<Separability> separability() const;

  int total_samples() const;

 private:
  std::vector<ClassStat> classes_;
  std::vector<Rule> rules_;
  FeatureVec scales_ = DefaultFloors();
  FeatureVec floors_ = DefaultFloors();
};

// 从运行时指标构造特征向量。baseline 必需（都是相对量）。
// 返回 false 表示关键量缺失（例如没有耳朵、没有基准），这一帧不参与分类。
bool BuildFeatureVector(const struct PostureMetrics& m, const struct Baseline& b,
                        PostureProfile::FeatureVec* out, std::string* why);

// 姿势标签 -> 中文名（给界面/日志用）
const char* PostureLabelZh(const std::string& label);

}  // namespace posture
