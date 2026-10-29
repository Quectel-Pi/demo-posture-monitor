// 主程序（对应实施方案 §3.2 线程模型 / §13.2 systemd 部署）
//
// 线程：
//   CaptureThread   摄像头 -> 最新帧槽（容量 1，覆盖旧帧）
//   MonitorThread   推理 -> PostureAnalyzer -> EyeCareMonitor -> ReminderManager -> Notifier
//   main            信号处理、命令分发、退出清理
//
// 关键原则（§3.2）：视频流绝不排队；业务计时用 steady_clock；不在多线程共享
// 同一个 RKNN context（只有 MonitorThread 碰引擎）。
#include <signal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <algorithm>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "posture/CameraCapture.hpp"
#include "posture/Config.hpp"
#include "posture/EyeCareMonitor.hpp"
#include "posture/Json.hpp"
#include "posture/Logger.hpp"
#include "posture/Notifier.hpp"
#include "posture/PoseEngine.hpp"
#include "posture/PostureAnalyzer.hpp"
#include "posture/PostureProfile.hpp"
#include "posture/ReminderManager.hpp"
#include "posture/UserCalibration.hpp"

namespace {

using namespace posture;

constexpr const char* kVersion = "1.0.0";

std::atomic<bool> g_running{true};

void OnSignal(int sig) {
  (void)sig;
  g_running = false;
}

struct Args {
  std::string config_path = "/etc/posture-guard/posture.yaml";
  std::string device_override;
  std::string image_path;
  std::string source_image;   // 用静态图循环喂帧（离线回归/取证，替代摄像头）
  std::string dump_keypoints_path;
  int width = 0;
  int height = 0;
  double eye_seconds = 0.0;
  bool headless = true;
  bool verbose = false;
  bool calibrate = false;
  bool calibrate_only = false;
  bool reset_baseline = false;
  bool print_config = false;
  bool no_socket = false;
  double run_seconds = 0.0;   // >0 时跑这么久自动退出（测试/验收用）
};

void PrintUsage() {
  std::printf(
      "posture_guard %s —— RK3576 坐姿检测与护眼提醒\n"
      "用法: posture_guard [选项]\n"
      "  --config PATH           配置文件 (默认 /etc/posture-guard/posture.yaml)\n"
      "  --device PATH           覆盖摄像头设备 (如 /dev/video73)\n"
      "  --width / --height N    覆盖采集分辨率\n"
      "  --eye-seconds N         覆盖连续用眼阈值（测试用，正式 1200）\n"
      "  --image PATH            对单张静态图推理后退出（M2 验收用）\n"
      "  --source-image PATH     用静态图循环喂帧替代摄像头（离线回归/取证）\n"
      "  --dump-keypoints PATH   把关键点写入 JSON 文件（配合 --image）\n"
      "  --calibrate             启动后立即进入校准\n"
      "  --calibrate-only        只做校准，完成后退出\n"
      "  --reset-baseline        删除已有校准基准后退出\n"
      "  --headless              无界面模式（默认，systemd 用）\n"
      "  --no-socket             不创建状态 socket（调试用）\n"
      "  --run-seconds N         运行 N 秒后自动退出（验收脚本用）\n"
      "  --print-config          打印生效配置后退出\n"
      "  -v/--verbose            输出 DEBUG 日志\n"
      "  --version               版本号\n",
      kVersion);
}

bool ParseArgs(int argc, char** argv, Args* a, std::string* err) {
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto need_value = [&](std::string* out) -> bool {
      if (i + 1 >= argc) {
        *err = k + " 需要一个参数";
        return false;
      }
      *out = argv[++i];
      return true;
    };
    std::string v;
    if (k == "--config") {
      if (!need_value(&a->config_path)) return false;
    } else if (k == "--device") {
      if (!need_value(&a->device_override)) return false;
    } else if (k == "--width") {
      if (!need_value(&v)) return false;
      a->width = std::atoi(v.c_str());
    } else if (k == "--height") {
      if (!need_value(&v)) return false;
      a->height = std::atoi(v.c_str());
    } else if (k == "--eye-seconds") {
      if (!need_value(&v)) return false;
      a->eye_seconds = std::atof(v.c_str());
    } else if (k == "--source-image") {
      if (!need_value(&a->source_image)) return false;
    } else if (k == "--image") {
      if (!need_value(&a->image_path)) return false;
    } else if (k == "--dump-keypoints") {
      if (!need_value(&a->dump_keypoints_path)) return false;
    } else if (k == "--run-seconds") {
      if (!need_value(&v)) return false;
      a->run_seconds = std::atof(v.c_str());
    } else if (k == "--headless") {
      a->headless = true;
    } else if (k == "--no-socket") {
      a->no_socket = true;
    } else if (k == "--calibrate") {
      a->calibrate = true;
    } else if (k == "--calibrate-only") {
      a->calibrate = true;
      a->calibrate_only = true;
    } else if (k == "--reset-baseline") {
      a->reset_baseline = true;
    } else if (k == "--print-config") {
      a->print_config = true;
    } else if (k == "-v" || k == "--verbose") {
      a->verbose = true;
    } else if (k == "--version") {
      std::printf("posture_guard %s\n", kVersion);
      std::exit(0);
    } else if (k == "-h" || k == "--help") {
      PrintUsage();
      std::exit(0);
    } else {
      *err = "未知参数: " + k;
      return false;
    }
  }
  return true;
}

// 容量 1 的帧槽：采集写得快、推理读得慢时直接覆盖（§14.3）
class FrameSlot {
 public:
  void Put(Frame&& f) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame_ = std::move(f);
      seq_++;
    }
    cv_.notify_one();
  }
  // 等到「比 last_seq 更新」的帧；超时返回 false
  bool WaitGet(Frame* out, std::uint64_t* last_seq, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [&] { return seq_ > *last_seq || !g_running; })) {
      return false;
    }
    if (seq_ <= *last_seq) return false;
    *out = frame_;          // cv::Mat 浅拷贝（引用计数），零成本
    *last_seq = seq_;
    return true;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  Frame frame_;
  std::uint64_t seq_ = 0;
};

// 单张静态图推理（M2 验收：与官方 demo / Python 参考实现比对）
int RunImageMode(const Args& args, const Config& cfg) {
  cv::Mat img = cv::imread(args.image_path, cv::IMREAD_COLOR);
  if (img.empty()) {
    LOG_ERROR("无法读取图片: ", args.image_path);
    return 2;
  }
  Frame frame;
  frame.bgr = img;
  frame.width = img.cols;
  frame.height = img.rows;
  frame.frame_id = 1;
  frame.captured_at = SteadyClock::now();
  frame.wall_at = WallClock::now();

  auto engine = CreateRknnPoseEngine(cfg);
  std::string err;
  if (!engine->Init(&err)) {
    LOG_ERROR("引擎初始化失败: ", err);
    return 3;
  }
  std::vector<PersonPose> persons;
  if (!engine->Infer(frame, &persons, &err)) {
    LOG_ERROR("推理失败: ", err);
    return 4;
  }
  LOG_INFO("图片 ", args.image_path, " (", frame.width, "x", frame.height, ") 检出 ",
           persons.size(), " 人");

  Json root;
  root.Str("image", args.image_path)
      .Int("width", frame.width)
      .Int("height", frame.height)
      .Str("model_sha256", engine->model_sha256())
      .Str("runtime", engine->runtime_version());

  std::string persons_json = "[";
  for (size_t i = 0; i < persons.size(); ++i) {
    const PersonPose& p = persons[i];
    if (i) persons_json += ",";
    Json pj;
    pj.Num("score", p.person_confidence, 4);
    std::vector<double> box{p.box.x, p.box.y, p.box.w, p.box.h};
    pj.Array("box", box, 2);
    std::string kpts = "[";
    for (int j = 0; j < kNumJoints; ++j) {
      if (j) kpts += ",";
      Json kj;
      kj.Num("x", p.joints[j].x, 2)
          .Num("y", p.joints[j].y, 2)
          .Num("c", p.joints[j].confidence, 3)
          .Str("name", JointName(j));
      kpts += kj.Build();
    }
    kpts += "]";
    pj.Raw("keypoints", kpts);
    persons_json += pj.Build();

    // 控制台摘要：方便和官方 demo 对照
    std::printf("person[%zu] score=%.3f box=(%.1f,%.1f,%.1f,%.1f)\n", i,
                p.person_confidence, p.box.x, p.box.y, p.box.w, p.box.h);
    for (int j = 0; j < kNumJoints; ++j) {
      std::printf("  %-11s %7.1f %7.1f\n", JointName(j), p.joints[j].x, p.joints[j].y);
    }
  }
  persons_json += "]";
  root.Raw("persons", persons_json);

  const std::string out_path =
      args.dump_keypoints_path.empty() ? std::string() : args.dump_keypoints_path;
  if (!out_path.empty()) {
    std::ofstream ofs(out_path, std::ios::trunc);
    ofs << root.Build() << "\n";
    LOG_INFO("关键点已写入 ", out_path);
  } else {
    std::printf("%s\n", root.Build().c_str());
  }

  // 画一张带骨架的图，肉眼核对用（§9.7 的 out.png 等价物）
  if (cfg.model_path.find(".rknn") != std::string::npos) {
    static const int kSkeleton[][2] = {{15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12},
                                       {5, 11},  {6, 12},  {5, 6},   {5, 7},   {6, 8},
                                       {7, 9},   {8, 10},  {1, 2},   {0, 1},   {0, 2},
                                       {1, 3},   {2, 4},   {3, 5},   {4, 6}};
    cv::Mat canvas = img.clone();
    for (const auto& p : persons) {
      cv::rectangle(canvas,
                    cv::Rect(static_cast<int>(p.box.x), static_cast<int>(p.box.y),
                             static_cast<int>(p.box.w), static_cast<int>(p.box.h)),
                    cv::Scalar(0, 200, 0), 2);
      for (const auto& edge : kSkeleton) {
        const auto& a = p.joints[edge[0]];
        const auto& b = p.joints[edge[1]];
        cv::line(canvas, cv::Point(static_cast<int>(a.x), static_cast<int>(a.y)),
                 cv::Point(static_cast<int>(b.x), static_cast<int>(b.y)),
                 cv::Scalar(0, 255, 255), 2);
      }
      for (int j = 0; j < kNumJoints; ++j) {
        cv::circle(canvas,
                   cv::Point(static_cast<int>(p.joints[j].x), static_cast<int>(p.joints[j].y)),
                   3, cv::Scalar(0, 0, 255), -1);
      }
    }
    const std::string png = "posture_guard_out.png";
    cv::imwrite(png, canvas);
    LOG_INFO("已生成骨架图 ", png);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string err;
  if (!ParseArgs(argc, argv, &args, &err)) {
    std::fprintf(stderr, "参数错误: %s\n", err.c_str());
    PrintUsage();
    return 1;
  }
  SetLogLevel(args.verbose ? LogLevel::kDebug : LogLevel::kInfo);

  Config cfg;
  if (!Config::Load(args.config_path, &cfg, &err)) {
    LOG_ERROR("加载配置失败: ", err);
    return 1;
  }
  if (!args.device_override.empty()) cfg.camera_device = args.device_override;
  if (args.width > 0) cfg.camera_width = args.width;
  if (args.height > 0) cfg.camera_height = args.height;

  if (args.print_config) {
    std::printf("%s", cfg.Describe().c_str());
    return 0;
  }
  if (args.reset_baseline) {
    if (std::remove(cfg.baseline_file.c_str()) == 0) {
      LOG_INFO("已删除校准基准 ", cfg.baseline_file);
    } else {
      LOG_WARN("没有可删除的校准基准 ", cfg.baseline_file);
    }
    return 0;
  }

  LOG_INFO("posture_guard ", kVersion, " 启动，配置 ", args.config_path);
  LOG_INFO("生效配置:\n", cfg.Describe());

  if (!args.image_path.empty()) {
    return RunImageMode(args, cfg);
  }

  // ---- 引擎 ----
  auto engine = CreateRknnPoseEngine(cfg);
  if (!engine->Init(&err)) {
    LOG_ERROR("姿态引擎初始化失败: ", err);
    return 1;
  }

  // ---- 校准基准 ----
  Baseline baseline;
  const bool loaded = UserCalibration::LoadBaseline(cfg.baseline_file, &baseline);
  if (loaded) {
    LOG_INFO("已加载校准基准（", baseline.created_at, "，样本 ", baseline.samples,
             " 帧，肩线基准 ", baseline.shoulder_angle_deg, "°）");
  } else {
    LOG_WARN("没有校准基准：高低肩/低头判定将不可用，"
             "检测到用户在座后会自动进入 5 秒校准（或手动 --calibrate）");
  }

  // ---- 摄像头 ----
  CameraCapture camera(cfg);
  if (!args.source_image.empty()) {
    LOG_WARN("使用 --source-image 离线送帧，跳过摄像头（不能用于真实现场验收）");
  } else if (!camera.Open(&err)) {
    LOG_ERROR("摄像头打开失败: ", err,
             "（用 scripts/smoke_camera.sh 排查设备节点与权限）");
    return 1;
  }

  // ---- 子系统 ----
  PostureAnalyzer analyzer(cfg, baseline);

  // ---- 逐姿势校准画像（存在则用画像分类，替代手调阈值）----
  PostureProfile profile;
  bool profile_loaded = false;
  {
    std::string perr;
    if (profile.Load(cfg.profile_file, &perr)) {
      profile_loaded = true;
      profile_loaded = !profile.empty();
    }
    if (profile_loaded) {
      analyzer.SetProfile(&profile, cfg.profile_hold_s, cfg.profile_accept_radius);
      LOG_INFO("已加载逐姿势画像 ", cfg.profile_file, "（", profile.total_samples(),
               " 个样本）");
    } else {
      LOG_WARN("没有逐姿势画像（", cfg.profile_file,
               "），使用阈值规则判定。想提高精度就跑 scripts/teach_posture.py 逐姿势采集");
    }
  }
  UserCalibration calibration(cfg);
  EyeCareMonitor eye_care(cfg, args.eye_seconds);
  ReminderManager reminders(cfg);
  StatsStore stats(cfg);
  stats.Init(&err);

  Notifier notifier(cfg);
  if (!args.no_socket) {
    if (!notifier.Init(&err)) {
      LOG_WARN("状态 socket 初始化失败（不影响检测，只是没有界面）: ", err);
    }
  }

  // 基准与当前设备/模型不匹配时提示重新校准（§4.6 / TC-14）
  if (loaded) {
    std::string why;
    if (!UserCalibration::BaselineMatches(baseline, camera.DeviceId(), cfg.camera_width,
                                         cfg.camera_height, engine->model_sha256(), &why)) {
      LOG_WARN("校准基准与当前环境不匹配（", why, "），请在界面上重新校准");
    }
  }
  if (args.calibrate) {
    calibration.Start(SteadyClock::now(), camera.DeviceId(), cfg.camera_width,
                      cfg.camera_height, engine->model_sha256());
    LOG_INFO("进入校准：请保持正常坐姿 ", static_cast<int>(cfg.calibration_duration_s),
             " 秒");
  }

  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);
  signal(SIGPIPE, SIG_IGN);

  FrameSlot slot;
  std::atomic<int> frames_captured{0};

  // ---- 采集线程 ----
  std::thread capture_thread([&] {
    if (!args.source_image.empty()) {
      // 离线回归模式：把同一张图按 capture_fps 反复送进流水线。
      // 用来在没有人在镜头前的时候验证「引擎→规则→界面」整条链路。
      cv::Mat img = cv::imread(args.source_image, cv::IMREAD_COLOR);
      if (img.empty()) {
        LOG_ERROR("读不到 --source-image ", args.source_image);
        g_running = false;
        return;
      }
      LOG_INFO("离线回归模式：循环送帧 ", args.source_image, " (", img.cols, "x", img.rows, ")");
      const auto interval = std::chrono::milliseconds(
          cfg.capture_fps > 0 ? 1000 / cfg.capture_fps : 50);
      std::uint64_t id = 0;
      while (g_running) {
        Frame frame;
        frame.bgr = img;
        frame.width = img.cols;
        frame.height = img.rows;
        frame.frame_id = ++id;
        frame.captured_at = SteadyClock::now();
        frame.wall_at = WallClock::now();
        slot.Put(std::move(frame));
        frames_captured++;
        std::this_thread::sleep_for(interval);
      }
      LOG_INFO("采集线程退出，共送 ", frames_captured.load(), " 帧");
      return;
    }
    while (g_running) {
      Frame frame;
      if (!camera.Read(&frame)) continue;
      frames_captured++;
      slot.Put(std::move(frame));
    }
    LOG_INFO("采集线程退出，共采集 ", frames_captured.load(), " 帧");
  });

  // ---- 监控线程（推理 + 规则 + 计时 + 通知）----
  std::thread monitor_thread([&] {
    std::uint64_t last_seq = 0;
    const double min_interval =
        cfg.infer_fps > 0 ? 1.0 / cfg.infer_fps : 0.0;
    SteadyTime last_infer{};
    bool has_last_infer = false;
    double fps_window_start = SteadyNowMs();
    int fps_frames = 0;
    double fps = 0.0;

    double use_accum = 0.0;      // 待写入统计的在座秒
    double bad_accum = 0.0;      // 待写入统计的不良秒
    double stats_tick_at = SteadyNowMs();
    SteadyTime last_tick = SteadyClock::now();
    StatusSnapshot snapshot;
    snapshot.model_path = cfg.model_path;
    snapshot.model_sha256 = engine->model_sha256();
    snapshot.backend = engine->name();
    snapshot.runtime_version = engine->runtime_version();
    snapshot.joint_confidence_reliable = engine->joint_confidence_reliable();
    snapshot.device_id = camera.DeviceId();
    snapshot.roi_x = cfg.roi_x;
    snapshot.roi_y = cfg.roi_y;
    snapshot.roi_w = cfg.roi_w;
    snapshot.roi_h = cfg.roi_h;
    double last_notification_at = -1.0;
    // 启动后的基准一致性检查：收够样本后比对一次「当前画面 vs 校准基准」。
    // 只做一次，避免用户中途换个坐姿就被反复判为漂移。
    std::vector<double> drift_wr;
    std::vector<double> drift_oy;
    std::vector<double> drift_sh;
    bool drift_checked = false;
    std::string checker_roll_hint;
    double last_video_ms = 0.0;
    std::uint64_t video_seq = 0;
    std::vector<unsigned char> video_jpeg;
    bool auto_calibrate_armed = true;

    while (g_running) {
      Frame frame;
      if (!slot.WaitGet(&frame, &last_seq, 200)) {
        // 没新帧也要走一遍计时状态机（护眼计时与 FPS 解耦，§3.2）
        const SteadyTime now = SteadyClock::now();
        const auto eye = eye_care.Update(false, now);
        snapshot.session_state = ToString(eye.state);
        snapshot.use_s = eye.active_s;
        snapshot.remaining_s = eye.remaining_s;
        continue;
      }
      const SteadyTime now = SteadyClock::now();
      if (has_last_infer && min_interval > 0 &&
          std::chrono::duration<double>(now - last_infer).count() < min_interval) {
        continue;   // 抽帧：采集 20fps，推理只跑 infer_fps
      }
      last_infer = now;
      has_last_infer = true;

      std::vector<PersonPose> persons;
      const bool ok = engine->Infer(frame, &persons, &err);
      if (!ok) {
        LOG_WARN("推理失败: ", err);
        continue;
      }
      fps_frames++;
      const double now_ms = SteadyNowMs();
      if (now_ms - fps_window_start >= 1000.0) {
        fps = fps_frames * 1000.0 / (now_ms - fps_window_start);
        fps_window_start = now_ms;
        fps_frames = 0;
      }

      const PostureObservation obs =
          analyzer.Update(persons, now, frame.width, frame.height);

      // 被几何闸门拒掉的帧要能看见原因，否则"校准采样不足"根本没法排查。
      // 限流 5s 一条，避免刷屏。
      if (!obs.metrics.valid && obs.state != PostureState::kUserAway) {
        static double last_reject_log = 0.0;
        const double t = SteadyNowMs();
        if (t - last_reject_log > 5000.0) {
          last_reject_log = t;
          // 原因在 explanation 里：几何闸门拒（有 invalid_reason）和
          // 「选不出主用户」（Plausible 拒）两条路径都会写进 explanation
          LOG_WARN("本帧几何不可用：",
                   obs.explanation.empty() ? "（未给出原因）" : obs.explanation,
                   "  检出 ", persons.size(), " 人");
        }
      }

      // ---- 主用户是否在座（用于护眼计时）----
      // UNKNOWN 且有人入镜时仍算在座：只是姿态判不出来，人确实在
      const bool present = obs.state != PostureState::kUserAway;
      const auto eye = eye_care.Update(present, now);

      // ---- 统计：在座/不良时长 ----
      const double dt = std::chrono::duration<double>(now - last_tick).count();
      last_tick = now;
      if (present) use_accum += dt;
      const bool bad =
          obs.state != PostureState::kNormal && obs.state != PostureState::kUnknown &&
          obs.state != PostureState::kUserAway;
      if (bad) bad_accum += dt;
      if (now_ms - stats_tick_at >= 10000.0) {
        stats.AddUseTime(use_accum, bad_accum);
        use_accum = bad_accum = 0.0;
        stats_tick_at = now_ms;
        if (!obs.active_keys.empty()) {
          stats.AddViolation(obs.active_keys.front(), 10.0);
        }
      }

      // ---- 坐姿提醒 ----
      Notification notify;
      if (reminders.Consider(obs.triggers, now, &notify)) {
        notifier.EmitEvent(notify.key, notify.title, notify.detail, notify.active_s);
        stats.AddAlert(notify.key, notify.active_s);
        last_notification_at = now_ms;
        snapshot.last_notification_title = notify.title;
      }
      // ---- 护眼休息提醒 ----
      if (eye.rest_due_now) {
        Notification rest;
        if (reminders.ConsiderRestReminder(now, &rest)) {
          notifier.EmitEvent(rest.key, rest.title, rest.detail, rest.active_s);
          stats.AddAlert(rest.key, rest.active_s);
          last_notification_at = now_ms;
          snapshot.last_notification_title = rest.title;
        }
      }

      // ---- 基准一致性检查（挪了摄像头/换了距离时必须重新校准）----
      // 只做一次：收够 20 个有效样本后取中位数，与校准基准比对。
      // 为什么必须做：BaselineMatches() 只能比 camera_id/分辨率/模型 SHA256，
      // 摄像头没换但位置变了时它认为基准有效，于是所有阈值都算偏。
      if (analyzer.HasBaseline() && !calibration.active()) {
        if (!drift_checked) {
          if (obs.metrics.valid && obs.state != PostureState::kUserAway) {
            drift_wr.push_back(obs.metrics.width_ratio);
            drift_oy.push_back(obs.metrics.head_offset_y);
            drift_sh.push_back(obs.metrics.shoulder_angle_deg);
            if (drift_wr.size() >= 20) {
              drift_checked = true;
              std::vector<double> wr = drift_wr, oy = drift_oy;
              std::sort(wr.begin(), wr.end());
              std::sort(oy.begin(), oy.end());
              PostureMetrics probe = obs.metrics;
              probe.width_ratio = static_cast<float>(wr[wr.size() / 2]);
              probe.head_offset_y = static_cast<float>(oy[oy.size() / 2]);
              const std::string why = analyzer.CheckBaselineDrift(probe);
              if (!why.empty()) {
                LOG_WARN("校准基准已失效：", why);
                LOG_WARN("旧基准会让所有阈值都算偏，现作废并自动重新校准");
                std::remove(cfg.baseline_file.c_str());
                analyzer.SetBaseline(Baseline{});
                calibration.Start(now, camera.DeviceId(), frame.width, frame.height,
                                  engine->model_sha256());
              } else {
                LOG_INFO("校准基准与当前画面对得上（肩宽占比 ",
                         static_cast<double>(probe.width_ratio), "），继续使用");
                // 距离对得上，但肩线角可能因摄像头 roll 而偏 —— 只提示，不动基准
                std::vector<double> sh = drift_sh;
                std::sort(sh.begin(), sh.end());
                probe.shoulder_angle_deg =
                    static_cast<float>(sh[sh.size() / 2]);
                const std::string hint = analyzer.CheckRollHint(probe);
                if (!hint.empty()) {
                  LOG_WARN("基准提示：", hint);
                  checker_roll_hint = hint;
                }
              }
            }
          }
        }
      } else if (!analyzer.HasBaseline()) {
        // 没有基准：等校准完成后再武装检查
        drift_wr.clear();
        drift_oy.clear();
        drift_sh.clear();
        drift_checked = false;
      }

      // ---- 自动校准：有基准就不做；没基准且确认在座则自动开始 ----
      // 只排除「确认无人」——不能排除 kUnknown：没有基准时规则路径本来就会报
      // UNKNOWN（"未校准"），把它排除掉就永远进不了校准（实测死锁）。
      // 人的几何是否可用由 CalibrateFeed 自己把关。
      if (!analyzer.HasBaseline() && !calibration.active() && auto_calibrate_armed &&
          obs.state != PostureState::kUserAway) {
        PostureMetrics probe;
        if (analyzer.CalibrateFeed(persons, frame.width, frame.height, &probe)) {
          calibration.Start(now, camera.DeviceId(), frame.width, frame.height,
                            engine->model_sha256());
          LOG_INFO("检测到在座用户且没有校准基准，自动开始 ",
                   static_cast<int>(cfg.calibration_duration_s), " 秒校准");
        }
      }
      if (calibration.active()) {
        PostureMetrics probe;
        if (analyzer.CalibrateFeed(persons, frame.width, frame.height, &probe)) {
          Baseline fresh;
          std::string message;
          const auto result = calibration.Feed(probe, now, &fresh, &message,
                                               calibration.min_samples());
          if (result == UserCalibration::FeedResult::kDone) {
            if (UserCalibration::SaveBaseline(cfg.baseline_file, fresh, &err)) {
              LOG_INFO(message, "；基准已保存到 ", cfg.baseline_file);
            } else {
              LOG_ERROR("校准结果保存失败: ", err);
            }
            analyzer.SetBaseline(fresh);
            if (analyzer.profile_active()) {
              LOG_WARN("校准基准刚被更新，但逐姿势画像的特征是相对旧基准算的 —— "
                       "请重新跑 scripts/teach_posture.py 采集，否则分类会偏");
            }
            if (args.calibrate_only) {
              g_running = false;
            }
          } else if (result == UserCalibration::FeedResult::kFailed) {
            LOG_WARN(message, "（30 秒后会自动重试）");
            auto_calibrate_armed = false;
            // 简单退避：由下面这一行把它重新武装（用时间而不是计数器）
          }
        }
      } else if (!analyzer.HasBaseline() && !auto_calibrate_armed) {
        static double rearm_at = 0.0;
        if (rearm_at == 0.0) rearm_at = now_ms + 30000.0;
        if (now_ms >= rearm_at) {
          auto_calibrate_armed = true;
          rearm_at = 0.0;
        }
      }

      // ---- 视频流（让界面能看到真实画面，也是调机位的唯一手段）----
      if (cfg.ui_stream_video && cfg.ui_video_fps > 0) {
        const double interval = 1000.0 / cfg.ui_video_fps;
        if (now_ms - last_video_ms >= interval) {
          last_video_ms = now_ms;
          cv::Mat shown = frame.bgr;
          cv::Mat scaled;
          if (cfg.ui_video_width > 0 && frame.bgr.cols > cfg.ui_video_width) {
            const double s2 = static_cast<double>(cfg.ui_video_width) / frame.bgr.cols;
            cv::resize(frame.bgr, scaled, cv::Size(), s2, s2, cv::INTER_AREA);
            shown = scaled;
          }
          const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, cfg.ui_jpeg_quality};
          if (cv::imencode(".jpg", shown, video_jpeg, params)) {
            notifier.PublishVideo(video_jpeg, shown.cols, shown.rows, ++video_seq);
          }
        }
      }

      // ---- 更新快照并推送 ----
      snapshot.posture_state = ToString(obs.state);
      snapshot.posture_state_zh = ToChinese(obs.state);
      snapshot.explanation = obs.explanation;
      snapshot.present = present;
      snapshot.persons = static_cast<int>(persons.size());
      {
        const PrimarySelection ps = analyzer.SelectPrimary(persons, frame.width, frame.height);
        snapshot.persons_primary = ps.plausible_count;
      }
      snapshot.latency_ms = engine->last_latency_ms();
      snapshot.fps = fps;
      snapshot.frame_id = frame.frame_id;
      snapshot.has_baseline = analyzer.HasBaseline();
      snapshot.calibrating = calibration.active();
      snapshot.calibration_progress = calibration.progress(now);
      snapshot.baseline_created_at = analyzer.baseline().created_at;
      snapshot.metrics = analyzer.last_metrics();
      snapshot.session_state = ToString(eye.state);
      snapshot.use_s = eye.active_s;
      snapshot.remaining_s = eye.remaining_s;
      snapshot.profile_active = analyzer.profile_active();
      snapshot.profile_label = obs.profile_label;
      snapshot.profile_distance = obs.profile_distance;
      snapshot.profile_strength = obs.profile_strength;
      snapshot.profile_rule = obs.profile_rule;
      snapshot.profile_classes = analyzer.profile_labels();
      snapshot.active_rules = obs.active_keys;
      snapshot.rules.clear();
      for (const auto& dg : obs.rule_diags) {
        StatusSnapshot::RuleView rv;
        rv.key = dg.key;
        rv.has_value = dg.has_value;
        rv.value = dg.value;
        rv.trigger = dg.trigger;
        rv.latched = dg.latched;
        rv.held_s = dg.held_s;
        rv.hold_s = dg.hold_s;
        snapshot.rules.push_back(rv);
      }
      {
        const PrimarySelection ps = analyzer.SelectPrimary(persons, frame.width, frame.height);
        if (ps.person) {
          snapshot.box_valid = true;
          snapshot.box_x = ps.person->box.x;
          snapshot.box_y = ps.person->box.y;
          snapshot.box_w = ps.person->box.w;
          snapshot.box_h = ps.person->box.h;
        } else {
          // ★ 必须清空：否则 status 里会一直报「上一次有效」的框，
          //   现场看到的就是一个恒定不变、与实际不符的取景框（踩过）
          snapshot.box_valid = false;
          snapshot.box_x = 0.0;
          snapshot.box_y = 0.0;
          snapshot.box_w = 0.0;
          snapshot.box_h = 0.0;
        }
        // 被拒时也报出「最大的检出框」，现场调机位时才有数看
        if (ps.reject_person_box.w > 0.f) {
          snapshot.raw_box_valid = true;
          snapshot.raw_box_x = ps.reject_person_box.x;
          snapshot.raw_box_y = ps.reject_person_box.y;
          snapshot.raw_box_w = ps.reject_person_box.w;
          snapshot.raw_box_h = ps.reject_person_box.h;
          snapshot.reject_reason = ps.reject_reason;
        } else {
          snapshot.raw_box_valid = false;
          snapshot.reject_reason = ps.reason;
        }
      }
      snapshot.frame_width = frame.width;
      snapshot.frame_height = frame.height;
      snapshot.today_bad_s = stats.today().bad_s + bad_accum;
      snapshot.today_use_s = stats.today().use_s + use_accum;
      snapshot.today_alerts = stats.today().alerts;
      // 归一化骨架点发给 UI（本地 socket，不落盘、不出网）
      //
      // ★ 不再要求 metrics.valid：调机位时正是「几何被拒」的时候最需要看
      //   关键点在哪。之前卡了这个条件，导致取景不合格时 socket 里没有骨架，
      //   现场只能看到「无人/几何不可用」，完全不知道差在哪（实测踩过）。
      snapshot.keypoints_valid = false;
      snapshot.keypoints_norm.clear();
      if (frame.width > 0 && frame.height > 0) {
        const PrimarySelection primary =
            analyzer.SelectPrimary(persons, frame.width, frame.height);
        if (primary.person && !primary.ambiguous) {
          snapshot.keypoints_norm.reserve(kNumJoints * 2);
          for (int j = 0; j < kNumJoints; ++j) {
            const Keypoint& kp = primary.person->joints[j];
            snapshot.keypoints_norm.push_back(kp.x / static_cast<float>(frame.width));
            snapshot.keypoints_norm.push_back(kp.y / static_cast<float>(frame.height));
          }
          snapshot.keypoints_valid = true;
        }
      }

      snapshot.snoozed = reminders.snoozed(now);
      snapshot.baseline_hint = checker_roll_hint;
      snapshot.last_notification_ago_s =
          last_notification_at > 0 ? (now_ms - last_notification_at) / 1000.0 : -1.0;
      notifier.Publish(snapshot);

      // ---- UI 命令 ----
      for (const auto& cmd : notifier.PollCommands()) {
        if (cmd.name == "ack") {
          eye_care.Acknowledge();
        } else if (cmd.name == "snooze") {
          reminders.Snooze(cmd.minutes * 60.0, now);
        } else if (cmd.name == "clear_snooze") {
          reminders.ClearSnooze();
        } else if (cmd.name == "calibrate") {
          calibration.Start(now, camera.DeviceId(), frame.width, frame.height,
                            engine->model_sha256());
          LOG_INFO("界面请求重新校准，请保持正常坐姿 ",
                   static_cast<int>(cfg.calibration_duration_s), " 秒");
        } else if (cmd.name == "quit") {
          LOG_INFO("收到界面退出命令");
          g_running = false;
        }
      }

    }

    // 收尾：把残余统计落盘
    if (use_accum > 0.0 || bad_accum > 0.0) stats.AddUseTime(use_accum, bad_accum);
    LOG_INFO("监控线程退出");
  });

  // ---- 主线程：等待退出条件 ----
  const SteadyTime started = SteadyClock::now();
  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (args.run_seconds > 0 &&
        std::chrono::duration<double>(SteadyClock::now() - started).count() >=
            args.run_seconds) {
      LOG_INFO("已运行 ", args.run_seconds, "s，按 --run-seconds 退出");
      g_running = false;
    }
  }
  // 唤醒可能阻塞在 WaitGet 的监控线程
  {
    Frame wake;
    wake.frame_id = 0;
    slot.Put(std::move(wake));
  }
  if (capture_thread.joinable()) capture_thread.join();
  if (monitor_thread.joinable()) monitor_thread.join();

  notifier.Close();
  camera.Close();
  LOG_INFO("posture_guard 已退出");

  if (args.calibrate_only) {
    Baseline check;
    return UserCalibration::LoadBaseline(cfg.baseline_file, &check) ? 0 : 5;
  }
  return 0;
}
