// V4L2 能力枚举与参数协商
//
// 为什么要这个：不同 UVC 摄像头支持的分辨率/像素格式/帧率组合差别很大
//（本项目这块 icSpring 只给 YUYV 640x480 的 5/10/15/20/25/30fps，
// 连 MJPG 都没有）。硬编码「MJPEG + 640x480」在别的摄像头上一开就失败，
// 或者被驱动悄悄改成别的格式却没人知道。所以先用 VIDIOC_ENUM_* 问清楚，
// 再挑最接近用户期望的一组，最后把「实际协商到的值」记进日志与状态。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace posture {

struct V4l2FrameSize {
  int width = 0;
  int height = 0;
  std::vector<double> fps;   // 该尺寸支持的帧率（可能为空 = 驱动不报，任意）
};

struct V4l2Format {
  std::uint32_t fourcc = 0;
  std::string name;          // 如 "YUYV 4:2:2"
  std::string code;          // 如 "YUYV"
  std::vector<V4l2FrameSize> sizes;
};

struct V4l2Capabilities {
  bool ok = false;
  std::string error;
  std::string driver;
  std::string card;
  std::string bus;
  std::vector<V4l2Format> formats;
};

// 打开设备枚举能力（只读打开，不影响正在运行的服务；失败时 error 有原因）
V4l2Capabilities ProbeV4l2(const std::string& device);

// 用户期望的采集参数
struct CaptureRequest {
  int width = 640;
  int height = 480;
  int fps = 20;
  // "auto" / "MJPEG" / "YUYV" / "RGB24" ...；auto = 优先压缩格式（省 USB 带宽）
  std::string preferred_format = "auto";
};

struct CaptureChoice {
  bool ok = false;
  std::string error;
  std::uint32_t fourcc = 0;
  std::string format_code;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  // 与用户期望的差异说明（空 = 完全一致）
  std::vector<std::string> adjustments;
};

// 在能力列表里挑最接近的一组。规则：
//   1. 格式：preferred_format 命中则用它；auto 时优先 MJPG（USB 带宽低），否则用第一个
//   2. 尺寸：优先完全相等；否则选面积最接近的（宁可大一点，后续由 OpenCV 缩放）
//   3. 帧率：优先完全相等；否则选不超过期望的最大值；再没有就选最小
CaptureChoice ChooseCapture(const V4l2Capabilities& caps, const CaptureRequest& req);

const char* FourccToString(std::uint32_t fourcc, char out[5]);

}  // namespace posture
