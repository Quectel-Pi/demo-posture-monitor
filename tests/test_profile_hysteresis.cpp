// 画像级联单测：迟滞行为 + 真标注数据回归
//
// 为什么需要这个测试文件（而不是并到 test_posture_rules.cpp）：
// 迟滞是 PostureProfile 层的纯函数行为，不需要摄像头/NPU/时间源，
// 独立文件可以只用一张构造出来的 postures.txt 精确控制边界。
//
// 覆盖点：
//  1. 无 active 标签时 ClassifyHysteretic 必须等价于纯 Classify（不能改灵敏度）
//  2. 进入新标签仍用原阈值（不放宽）
//  3. 离开当前标签必须退回 release_band 才算真的离开
//  4. release_ratio = 0 时完全退回纯阈值判定（可关闭）
//  5. 迟滞带不会超过「阈值到 normal 中位数」的跨度
//  6. guard 互斥在迟滞路径里同样生效
//  7. 真实板端 postures.txt 能被解析，且 4 条规则都在
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "posture/Types.hpp"
#include <string>
#include <vector>

#include "posture/PostureProfile.hpp"

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

// 写一个最小 postures.txt 到临时路径。规则故意用「干净」的数值，
// 方便手算释放带：head_drop normal 0.00 / 该类 0.20 -> 阈值 0.10，
// ReleaseScale[0]=0.08，ratio=0.5 -> band=0.04，释放阈 0.06。
std::string WriteFixture() {
  const std::string path = "/tmp/test_profile_hysteresis.txt";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::printf("!! 无法写 %s\n", path.c_str());
    std::exit(2);
  }
  std::fprintf(f, "scale\t.05\t3\t2\t.04\t.1\t.15\t.15\n");
  std::fprintf(f, "class\tnormal\t10\t0\t4\t0\t1\t0\t3\t3\t.01\t1\t1\t.01\t.01\t0\t0\n");
  std::fprintf(f, "class\thead_down\t10\t.2\t4\t0\t1\t0\t3\t3\t.01\t1\t1\t.01\t.01\t0\t0\n");
  std::fprintf(f, "class\thead_tilt\t10\t0\t20\t0\t1\t0\t3\t3\t.01\t1\t1\t.01\t.01\t0\t0\n");
  //                                   pri  label      feat  dir thr     nmed    cmed
  std::fprintf(f, "rule\t1\thead_tilt\tear_tilt\t+\t12.0000\t4.0000\t20.0000\n");
  std::fprintf(f,
               "rule\t2\thead_down\thead_drop\t+\t0.1000\t0.0000\t0.2000"
               "\tear_tilt\t12.0000\n");
  std::fclose(f);
  return path;
}

PostureProfile::FeatureVec F(double head_drop, double ear_tilt = 4.0) {
  return PostureProfile::FeatureVec{head_drop, ear_tilt, 0.0, 1.0, 0.0, 3.0, 3.0};
}

// 构造一个带「手贴脸」规则（wrist_face <= 0.95）+ 第二条门控的画像。
// 用于验证：手抬起来了但离脸远（后仰/抬手掠过）不能判成托腮。
std::string WriteHandFaceFixture() {
  const std::string path = "/tmp/test_hand_face_rule.txt";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    std::printf("!! 无法写 %s\n", path.c_str());
    std::exit(2);
  }
  std::fprintf(f, "scale\t0.0500\t3.0000\t2.0000\t0.0400\t0.1000\t0.1500\t0.1500\n");
  std::fprintf(f,
               "class\tnormal\t10\t0.0000\t4.0000\t7.0000\t1.0000\t0.0000\t3.0000\t3.0000"
               "\t0.0100\t1.0000\t1.0000\t0.0100\t0.0100\t0.0000\t0.0000\n");
  std::fprintf(f,
               "class\thand_face\t10\t0.0000\t4.0000\t7.0000\t1.0000\t0.3000\t3.0000\t0.4000"
               "\t0.0100\t1.0000\t1.0000\t0.0100\t0.0100\t0.0000\t0.0100\n");
  // 托腮主判据：wrist_face <= 0.95（手贴脸）；normal 的 3.0 远离阈值。
  std::fprintf(f, "rule\t4\thand_face\twrist_face\t-\t0.9500\t3.0000\t0.4000\n");
  std::fclose(f);
  return path;
}

void TestHandFaceNeedsArmAtFace() {
  std::printf("-- 托腮要看「手到脸的距离」，不能只看「手抬起来了」\n");
  PostureProfile p;
  std::string err;
  if (!p.Load(WriteHandFaceFixture(), &err)) {
    Check(false, "hand_face fixture 加载失败: " + err);
    return;
  }
  // wrist_face = 手到耳中点距离/肩宽；rise = 抬手量
  auto mk = [](double wrist_face, double rise) {
    return PostureProfile::FeatureVec{0.0, 4.0, 0.0, 1.0, rise, 3.0, wrist_face};
  };
  Check(p.Classify(mk(0.40, 0.30), 0.0).label == "hand_face",
        "手贴脸(0.40) -> 判托腮");
  // 这条是本次修复的核心：耗尽腕抬得一样高，但手离脸远。
  Check(p.Classify(mk(1.60, 0.30), 0.0).label != "hand_face",
        "手抬起来了但离脸远(1.60) -> 不判托腮（现场误报的主要形态）");
  Check(p.Classify(mk(3.00, 0.30), 0.0).label != "hand_face",
        "手根本不在画面/远离脸 -> 不判托腮");
}

void TestParseRealBoardProfile() {
  std::printf("-- 解析真实板端 postures.txt\n");
  // 板端 /var/lib/posture-guard/postures.txt 的副本（见 evidence）
  const std::string path = "config/postures-board-20261102.txt";
  PostureProfile p;
  std::string err;
  if (!p.Load(path, &err)) {
    // 测试环境没有该文件时不算失败，只提示（CI 精简目录）
    std::printf("  [skip] 没有 %s（%s）\n", path.c_str(), err.c_str());
    return;
  }
  Check(!p.empty(), "真实画像文件能解析出类别");
  Check(!p.rules().empty(), "真实画像文件能解析出规则");
  Check(p.labels().size() >= 4, "至少 4 个姿势类别");
  bool has_hd = false, has_ht = false;
  for (const auto& r : p.rules()) {
    if (r.label == "head_down") has_hd = true;
    if (r.label == "head_tilt") has_ht = true;
  }
  Check(has_hd, "含 head_down 规则");
  Check(has_ht, "含 head_tilt 规则");
}

void TestNoActiveEqualsPlain() {
  std::printf("-- 无 active 标签时等价于纯 Classify\n");
  PostureProfile p;
  std::string err;
  if (!p.Load(WriteFixture(), &err)) {
    Check(false, "fixture 加载失败: " + err);
    return;
  }
  const double kR = 0.5;
  const auto plain = p.Classify(F(0.15), 0.0);
  const auto hyst = p.ClassifyHysteretic(F(0.15), "", 0.5);
  Check(plain.label == "head_down", "纯 Classify 命中 head_down");
  Check(hyst.label == plain.label, "空 active 时迟滞结果与纯 Classify 一致");
}

void TestEnterUsesOriginalThreshold() {
  std::printf("-- 进入新标签仍用原阈值（不放宽）\n");
  PostureProfile p;
  std::string err;
  p.Load(WriteFixture(), &err);
  // 阈值 0.10，释放阈 0.06。当前在 normal，0.08 不该被判成 head_down。
  const auto v = p.ClassifyHysteretic(F(0.08), "normal", 0.5);
  Check(v.label == "normal", "0.08 < 阈值 0.10：从 normal 出发不进入 head_down");
}

void TestLeaveNeedsRelease() {
  std::printf("-- 离开当前标签必须退回 release_band\n");
  PostureProfile p;
  std::string err;
  p.Load(WriteFixture(), &err);
  // band = 0.08 * 0.5 = 0.04 -> 释放阈 0.06

  const auto stay = p.ClassifyHysteretic(F(0.08), "head_down", 0.5);
  Check(stay.label == "head_down",
        "已在 head_down，0.08 仍在释放带内（>=0.06）→ 保持 head_down");

  const auto leave = p.ClassifyHysteretic(F(0.05), "head_down", 0.5);
  Check(leave.label == "normal",
        "已在 head_down，0.05 低于释放阈 0.06 → 离开回到 normal");
}

void TestZeroRatioDisables() {
  std::printf("-- release_ratio = 0 时完全退回纯阈值判定\n");
  PostureProfile p;
  std::string err;
  p.Load(WriteFixture(), &err);
  const auto v = p.ClassifyHysteretic(F(0.08), "head_down", 0.0);
  Check(v.label == "normal", "关闭迟滞后 0.08 < 0.10 立刻离开 head_down");
}

void TestBandClampedBySpan() {
  std::printf("-- 释放带不超过阈值到 normal 中位数的跨度\n");
  PostureProfile p;
  std::string err;
  p.Load(WriteFixture(), &err);
  // ratio 很大时 band 想给 0.08*10=0.8，但跨度只有 0.10-0.00=0.10，
  // 必须被夹到 0.10 -> 释放阈 0.00，而不是负得离谱。
  const auto v = p.ClassifyHysteretic(F(0.001), "head_down", 10.0);
  Check(v.label == "head_down",
        "ratio=10 时释放带被夹到跨度 0.10，0.001 仍留在 head_down（不会变成负数）");
  // 0.0 恰好在边界上（thr - band = 0.0），特征 >= 释放阈仍算命中
  const auto v2 = p.ClassifyHysteretic(F(-0.05), "head_down", 10.0);
  Check(v2.label == "normal", "低于夹紧后的释放阈 -0.05 -> 离开");
}

void TestGuardInHystereticPath() {
  std::printf("-- guard 互斥在迟滞路径同样生效\n");
  PostureProfile p;
  std::string err;
  p.Load(WriteFixture(), &err);
  // head_drop=0.15 会命中 head_down，但 ear_tilt=15 超过 guard 12 -> 应被拦住，
  // 转而命中优先级更高的 head_tilt。
  const auto v = p.ClassifyHysteretic(F(0.15, 15.0), "normal", 0.5);
  Check(v.label == "head_tilt", "耳线明显歪时不判低头，而是判歪头");
}

void TestRejectedClassFallsThrough() {
  PostureProfile p;
  p.SetClass("normal", {{0,0,0,1,0,3,3}});
  PostureProfile::Rule hand;hand.label="hand_face";hand.feature=4;hand.threshold=.2;hand.priority=1;
  p.SetRule(hand);
  PostureProfile::Rule head;head.label="head_down";head.feature=0;head.threshold=.1;head.priority=2;
  p.SetRule(head);p.SortRules();
  auto f=F(.2);f[4]=.5;
  Check(p.ClassifyHysteretic(f,"",.5).label=="hand_face","原始画像候选为托腮");
  Check(p.ClassifyHysteretic(f,"",.5,"hand_face").label=="head_down","托腮几何失败后继续识别低头");
  f[0]=0;
  Check(p.ClassifyHysteretic(f,"hand_face",.5,"hand_face").label=="normal","确认手离开脸后，正常抬手不应被腕高样本范围拒绝");
  f[4]=0;
  Check(p.ClassifyHysteretic(f,"hand_face",.5,"hand_face").label=="normal","回到正常范围后可恢复正常");
}

void TestNormalDistanceAndProvenance() {
  PostureProfile p;std::string err;
  p.Load(WriteFixture(),&err);
  auto f=F(0);f[3]=1.2;
  Check(p.Classify(f,3).label.empty(),"未命中异常不等于正常：明显偏离正常尺寸必须拒绝");
  Check(p.ClassifyHysteretic(f,"head_down",.5).label.empty(),"迟滞路径同样拒绝未知姿势");
  Check(p.Classify(F(.05),3).label=="normal","正常范围内的自然微动可接受");
  Check(p.Classify(f,8).label=="normal","正常接受半径参数实际生效");
  auto farther=F(0);farther[3]=.7;farther[5]=2.5;farther[6]=2.5;
  Check(p.Classify(farther,3).label=="normal","坐远和手远离脸不能当成不健康方向的异常偏差");
  PostureProfile constant;
  constant.SetClass("normal",{{0,0,0,1,0,3,3}});
  PostureProfile::Rule r;r.label="head_down";r.feature=0;r.threshold=.5;constant.SetRule(r);
  f=F(.2,0);
  Check(constant.Classify(f,3).label.empty(),"有效常量特征的零MAD不能被当成缺测跳过");
  f[0]=std::numeric_limits<double>::quiet_NaN();
  Check(p.Classify(f,3).label.empty(),"非有限特征不得因比较失效而命中异常或正常");
  Baseline b;b.valid=true;b.created_at="2026-11-03T14:00:00";b.frame_width=640;b.frame_height=480;
  Check(!p.MatchesBaseline(b,&err),"无标定版本的旧画像拒绝运行");
  p.BindBaseline(b);
  Check(p.MatchesBaseline(b,&err),"画像绑定当前标定可用");
  const std::string file="/tmp/posture_profile_binding.txt";
  Check(p.Save(file,&err),"标定版本写入画像文件");
  PostureProfile loaded;loaded.Load(file,&err);
  Check(loaded.MatchesBaseline(b,&err),"重载后保留标定绑定");
  b.created_at="2026-11-03T14:01:00";
  Check(!loaded.MatchesBaseline(b,&err),"新的标定时间使旧画像失效");
  std::remove(file.c_str());
}

}  // namespace

int main() {
  std::printf("== 画像级联迟滞单测 ==\n");
  TestParseRealBoardProfile();
  TestNormalDistanceAndProvenance();
  TestNoActiveEqualsPlain();
  TestEnterUsesOriginalThreshold();
  TestRejectedClassFallsThrough();
  TestLeaveNeedsRelease();
  TestZeroRatioDisables();
  TestBandClampedBySpan();
  TestGuardInHystereticPath();
  TestHandFaceNeedsArmAtFace();
  if (g_failures == 0) {
    std::printf("== 全部通过 ==\n");
    return 0;
  }
  std::printf("== %d 项失败 ==\n", g_failures);
  return 1;
}
