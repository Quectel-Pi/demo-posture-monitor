#include "posture/CameraCapture.hpp"

#include <dirent.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstring>
#include <thread>
#include <vector>

#include "posture/Logger.hpp"
#include "posture/V4l2Probe.hpp"

namespace posture {
namespace {

// 在 /dev/v4l/by-id 里找指向 device 的符号链接，得到热插拔后也不变的路径
std::string FindByIdLink(const std::string& device) {
  const char* dir_path = "/dev/v4l/by-id";
  DIR* dir = opendir(dir_path);
  if (!dir) return {};
  std::string found;
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    const std::string full = std::string(dir_path) + "/" + entry->d_name;
    char resolved[512] = {0};
    if (realpath(full.c_str(), resolved) && device == resolved) {
      found = full;
      break;
    }
  }
  closedir(dir);
  return found;
}

}  // namespace

CameraCapture::CameraCapture(const Config& cfg)
    : cfg_(cfg), backoff_s_(cfg.camera_reconnect_backoff_s) {}

CameraCapture::~CameraCapture() { Close(); }

int CameraCapture::DeviceIndex(const std::string& device) {
  // 跟随符号链接拿到真实节点，再取尾部数字
  char resolved[512] = {0};
  std::string real = device;
  if (realpath(device.c_str(), resolved)) real = resolved;
  size_t i = real.size();
  while (i > 0 && std::isdigit(static_cast<unsigned char>(real[i - 1]))) --i;
  if (i == real.size()) return -1;
  try {
    return std::stoi(real.substr(i));
  } catch (...) {
    return -1;
  }
}

std::string CameraCapture::ResolveStablePath(const std::string& device) {
  // 用户直接给的 /dev/v4l/by-id/... 就用它
  if (device.find("/by-id/") != std::string::npos) return device;
  const std::string link = FindByIdLink(device);
  return link.empty() ? device : link;
}

bool CameraCapture::Open(std::string* err) {
  device_id_ = ResolveStablePath(cfg_.camera_device);
  if (!TryOpen(err)) return false;
  backoff_s_ = cfg_.camera_reconnect_backoff_s;
  return true;
}

bool CameraCapture::TryOpen(std::string* err) {
  cap_.release();
  // ★ OpenCV 的 V4L2 后端「不认设备路径」，只认索引（会报
  //   "backend is generally available but can't be used to capture by name"）。
  //   所以这里把 /dev/video73（或 /dev/v4l/by-id/... 链接）解析出索引再打开。
  const int index = DeviceIndex(cfg_.camera_device);
  if (index < 0) {
    if (err) *err = "无法从 " + cfg_.camera_device + " 解析出 V4L2 索引";
    return false;
  }
  // ---- 先问设备支持什么，再决定请求什么（而不是硬编码）----
  CaptureRequest want;
  want.width = cfg_.camera_width;
  want.height = cfg_.camera_height;
  want.fps = cfg_.capture_fps;
  want.preferred_format = cfg_.preferred_pixel_format;

  const V4l2Capabilities caps = ProbeV4l2(cfg_.camera_device);
  CaptureChoice choice;
  if (caps.ok) {
    choice = ChooseCapture(caps, want);
    std::string desc;
    for (const auto& f : caps.formats) {
      desc += f.code + "(";
      for (size_t i = 0; i < f.sizes.size(); ++i) {
        if (i) desc += ",";
        desc += std::to_string(f.sizes[i].width) + "x" + std::to_string(f.sizes[i].height);
      }
      desc += ") ";
    }
    LOG_INFO("摄像头能力：", caps.card, " [", desc, "]");
    if (!choice.ok) {
      LOG_WARN("能力协商失败：", choice.error, "，退回 OpenCV 默认参数");
    } else {
      for (const auto& a : choice.adjustments) LOG_WARN("参数适配：", a);
    }
  } else {
    LOG_WARN("读不到设备能力（", caps.error, "），按配置原样请求");
  }

  if (!cap_.open(index, cv::CAP_V4L2)) {
    if (err) *err = "无法打开摄像头 " + cfg_.camera_device +
                    "（索引 " + std::to_string(index) + "）";
    return false;
  }

  if (choice.ok) {
    // 顺序很重要：先 FOURCC 再分辨率。反了的话驱动会按默认格式重新协商尺寸，
    // 导致「明明设了 640x480，出来却是 1280x720」。
    cap_.set(cv::CAP_PROP_FOURCC, static_cast<double>(choice.fourcc));
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, choice.width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, choice.height);
    if (choice.fps > 0) cap_.set(cv::CAP_PROP_FPS, choice.fps);
    requested_fourcc_ = choice.format_code;
  } else {
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, cfg_.camera_width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.camera_height);
    cap_.set(cv::CAP_PROP_FPS, cfg_.capture_fps);
    requested_fourcc_ = cfg_.preferred_pixel_format;
  }
  // 缓冲 1 帧：永远只取最新画面，不排队（§14.3）
  cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);

  const int fw = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
  const int fh = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
  const int fourcc = static_cast<int>(cap_.get(cv::CAP_PROP_FOURCC));
  char code[5] = {0};
  code[0] = static_cast<char>(fourcc & 0xFF);
  code[1] = static_cast<char>((fourcc >> 8) & 0xFF);
  code[2] = static_cast<char>((fourcc >> 16) & 0xFF);
  code[3] = static_cast<char>((fourcc >> 24) & 0xFF);
  actual_fourcc_ = code;
  actual_width_ = fw;
  actual_height_ = fh;

  const bool res_ok = choice.ok ? (fw == choice.width && fh == choice.height)
                                : (fw == cfg_.camera_width && fh == cfg_.camera_height);
  LOG_INFO("摄像头已打开 ", cfg_.camera_device, " 实际 ", fw, "x", fh, "@",
           static_cast<int>(cap_.get(cv::CAP_PROP_FPS)), "fps 格式 ", actual_fourcc_,
           "（请求 ", choice.ok ? (std::to_string(choice.width) + "x" +
                                   std::to_string(choice.height) + " " + choice.format_code)
                                : (std::to_string(cfg_.camera_width) + "x" +
                                   std::to_string(cfg_.camera_height) + " " +
                                   cfg_.preferred_pixel_format),
           "）");
  if (!res_ok) {
    LOG_WARN("驱动最终给的是 ", fw, "x", fh, "，与协商结果不一致；"
             "画面仍可用，后续 letterbox 会按实际尺寸处理");
  }
  return true;
}

bool CameraCapture::Read(Frame* frame) {
  if (!cap_.isOpened()) {
    std::string err;
    if (!TryOpen(&err)) {
      ++reconnect_count_;
      LOG_WARN("摄像头重连失败（第 ", reconnect_count_, " 次）：", err,
               "，", backoff_s_, "s 后重试");
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<int>(backoff_s_ * 1000)));
      backoff_s_ = std::min<double>(backoff_s_ * 1.6, cfg_.camera_reconnect_backoff_max_s);
      return false;
    }
    LOG_INFO("摄像头已重连 ", cfg_.camera_device);
    backoff_s_ = cfg_.camera_reconnect_backoff_s;
  }

  cv::Mat bgr;
  const auto t0 = SteadyClock::now();
  if (!cap_.read(bgr) || bgr.empty()) {
    LOG_WARN("读帧失败，准备重连");
    cap_.release();
    return false;
  }
  last_read_ms_ = std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();

  frame->bgr = bgr;
  frame->width = bgr.cols;
  frame->height = bgr.rows;
  frame->frame_id = ++frame_counter_;
  frame->captured_at = SteadyClock::now();
  frame->wall_at = WallClock::now();
  return true;
}

std::string CameraCapture::DeviceId() const { return device_id_; }

void CameraCapture::Close() {
  if (cap_.isOpened()) cap_.release();
}

}  // namespace posture
