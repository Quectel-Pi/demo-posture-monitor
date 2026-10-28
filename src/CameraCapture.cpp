#include "posture/CameraCapture.hpp"

#include <dirent.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstring>
#include <thread>
#include <vector>

#include "posture/Logger.hpp"

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
  if (!cap_.open(index, cv::CAP_V4L2)) {
    if (err) *err = "无法打开摄像头 " + cfg_.camera_device +
                    "（索引 " + std::to_string(index) + "）";
    return false;
  }
  cap_.set(cv::CAP_PROP_FRAME_WIDTH, cfg_.camera_width);
  cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.camera_height);
  cap_.set(cv::CAP_PROP_FPS, cfg_.capture_fps);
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

  // 有些 UVC 不支持 MJPEG，会在 set 之后回落到 YUYV；不用报错，但要记录下来
  LOG_INFO("摄像头已打开 ", cfg_.camera_device, " 实际 ", fw, "x", fh, " 格式 ", actual_fourcc_);
  if (fw != cfg_.camera_width || fh != cfg_.camera_height) {
    LOG_WARN("摄像头未接受请求分辨率 ", cfg_.camera_width, "x", cfg_.camera_height,
             "，实际 ", fw, "x", fh);
  }
  if (cfg_.preferred_pixel_format == "MJPEG" && actual_fourcc_ != "MJPG") {
    LOG_WARN("期望 MJPEG 但实际是 ", actual_fourcc_,
             "（不影响功能，只是 USB 带宽占用更高）");
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
