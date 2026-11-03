// 配置解析单测：确保 config/posture.yaml 里的每个键都被正确读进来
#include <cmath>
#include <cstdio>
#include <string>

#include "posture/Config.hpp"
#include "posture/Yaml.hpp"

using namespace posture;

namespace {

int g_failures = 0;

void Check(bool cond, const std::string& what) {
  std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", what.c_str());
  if (!cond) ++g_failures;
}

void TestMiniYaml() {
  std::printf("-- YAML 子集解析\n");
  const std::string text =
      "# 注释\n"
      "top: 1\n"
      "parent:\n"
      "  child: \"hello world\"\n"
      "  nested:\n"
      "    deep: 3.5\n"
      "list: [0.1, 0.2, 0.3]   # 行内注释\n"
      "flag: true\n"
      "inline_hash: \"a#b\"\n";
  Yaml y;
  std::string err;
  Check(y.Parse(text, &err), "解析成功");
  Check(y.Int("top", 0) == 1, "top = 1");
  Check(y.Str("parent.child", "") == "hello world", "带引号的字符串去引号");
  Check(std::fabs(y.Num("parent.nested.deep", 0) - 3.5) < 1e-9, "三级嵌套");
  const auto list = y.Nums("list");
  Check(list.size() == 3 && std::fabs(list[2] - 0.3) < 1e-9, "行内数组");
  Check(y.Bool("flag", false), "bool 解析");
  Check(y.Str("inline_hash", "") == "a#b", "引号内的 # 不当注释");
  Check(y.Int("missing.key", 42) == 42, "缺省值生效");
}

void TestRealConfig(const std::string& path) {
  std::printf("-- 读取 %s\n", path.c_str());
  Config cfg;
  std::string err;
  const bool ok = Config::Load(path, &cfg, &err);
  Check(ok, "配置加载成功" + (ok ? std::string() : (": " + err)));
  if (!ok) return;

  Check(cfg.camera_width == 640 && cfg.camera_height == 480, "分辨率 640x480");
  Check(cfg.infer_fps > 0 && cfg.capture_fps >= cfg.infer_fps, "推理帧率 <= 采集帧率");
  Check(cfg.pose_confidence > 0.1f && cfg.pose_confidence < 0.9f, "姿态置信度在合理区间");
  Check(cfg.shoulder_trigger_deg > cfg.shoulder_recover_deg,
        "触发阈值 > 恢复阈值（迟滞带有效）");
  Check(cfg.shoulder_hold_s >= 0.2f && cfg.shoulder_hold_s < 1.0f,
        "界面确认窗口小于 1 秒且保留防抖");
  // 语音播报门限：姿势层已用时间窗投票去抖（0.6s/80%），这里只补一道防抖。
  // 上界 1.5s 是延迟预算：投票 0.6 + 本门限 + 播报启动 < 2s，
  // 否则用户会感觉「姿势错了半天才响」。
  Check(cfg.posture_reminder_frames == 10, "连续10个有效不良姿势帧触发语音");
  Check(cfg.posture_reminder_hold_s >= 0.3 && cfg.posture_reminder_hold_s <= 1.5,
        "语音播报门限在 0.3~1.5s（投票已去抖，不再重复等 2 秒）");
  Check(!cfg.baseline_file.empty() && cfg.baseline_file[0] == '/', "基线文件是绝对路径");
  Check(!cfg.model_path.empty(), "模型路径非空");
  Check(std::fabs(cfg.roi_w - 0.80f) < 1e-6f, "ROI 数组解析正确");
  Check(cfg.continuous_use_s >= 60.f, "用眼阈值 >= 60s");
  Check(!cfg.save_raw_video && !cfg.save_face_images, "隐私项默认关闭");
  Check(cfg.cooldown_s >= 10.f, "冷却时间合理");
  // 迟滞比例必须能被配置读进来，且 0~1 之间才有物理意义
  // （>1 时释放带可能宽于整个规则跨度，已在 PostureProfile 里夹住，
  //  但配置层面仍应当报错而不是默默夹）。
  Check(cfg.profile_release_ratio >= 0.0 && cfg.profile_release_ratio <= 1.0,
        "画像迟滞比例在 0~1 之间");
  Check(cfg.profile_release_ratio > 0.0,
        "生产配置未关闭迟滞（关闭会回到 514 次/1.7h 的抖动水平）");
}

void TestPrivacyGuard() {
  std::printf("-- 隐私红线：开启录像/存脸必须拒绝启动\n");
  Yaml y;
  const std::string path = "/tmp/posture_test_privacy.yaml";
  FILE* f = std::fopen(path.c_str(), "w");
  std::fprintf(f, "privacy:\n  save_raw_video: true\n");
  std::fclose(f);
  Config cfg;
  std::string err;
  const bool ok = Config::Load(path, &cfg, &err);
  Check(!ok, "save_raw_video=true 时拒绝加载");
  Check(err.find("隐私") != std::string::npos || err.find("原始画面") != std::string::npos,
        "错误信息说明原因");
  std::remove(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::printf("=== config 单测 ===\n");
  TestMiniYaml();
  TestPrivacyGuard();
  if (argc > 1) {
    TestRealConfig(argv[1]);
  } else {
    std::printf("  (跳过真实配置检查：未提供路径)\n");
  }
  if (g_failures == 0) {
    std::printf("=== 全部通过 ===\n");
    return 0;
  }
  std::printf("=== %d 项失败 ===\n", g_failures);
  return 1;
}
