// 摄像头采集（对应实施方案 §3.2 CaptureThread / §11.4 掉线降级）
//
// 用 OpenCV V4L2 后端（§10.1：第一次集成用 OpenCV，性能不够再上 RGA/DMABUF）。
// 关键行为：
//   * 掉线不退出进程，指数退避重连（板端 UVC 偶发 ESHUTDOWN）；
//   * 每帧记录 steady_clock 时间戳与自增 frame_id；
//   * 只保留「最新帧」，绝不排队（§14.3）。
#pragma once

#include <atomic>
#include <string>

#include <opencv2/videoio.hpp>

#include "posture/Config.hpp"
#include "posture/Types.hpp"

namespace posture {

class CameraCapture {
 public:
  explicit CameraCapture(const Config& cfg);
  ~CameraCapture();

  // 打开设备（含格式/分辨率设置）。失败填 err 并返回 false。
  bool Open(std::string* err);

  // 抓一帧（阻塞）。内部自动重连；返回 false 表示这一轮没拿到帧（已记录日志）。
  bool Read(Frame* frame);

  // 稳定的设备标识，用于基线记录（优先 /dev/v4l/by-id 路径）
  std::string DeviceId() const;
  void Close();
  bool is_open() const { return cap_.isOpened(); }

  // 统计信息（自检/诊断输出）
  int reconnect_count() const { return reconnect_count_; }
  double last_read_ms() const { return last_read_ms_; }
  std::string actual_fourcc() const { return actual_fourcc_; }
  int actual_width() const { return actual_width_; }
  int actual_height() const { return actual_height_; }

 private:
  bool TryOpen(std::string* err);
  static std::string ResolveStablePath(const std::string& device);
  // 把 /dev/videoN 或 by-id 链接解析成 OpenCV 需要的 V4L2 索引
  static int DeviceIndex(const std::string& device);

  Config cfg_;
  cv::VideoCapture cap_;
  std::uint64_t frame_counter_ = 0;
  int reconnect_count_ = 0;
  double backoff_s_ = 1.0;
  double last_read_ms_ = 0.0;
  int actual_width_ = 0;
  int actual_height_ = 0;
  std::string actual_fourcc_;
  std::string requested_fourcc_;
  std::string device_id_;
};

}  // namespace posture
