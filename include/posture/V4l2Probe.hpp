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

// ---------------------------------------------------------------------------
// V4L2 控件（焦距/曝光等）
//
// 为什么要单独写这个：OpenCV 的 CAP_PROP_* 不支持 UVC 的 focus_absolute，
// 而电动对焦的 UVC 摄像头（本项目这块 UGREEN 1080P）必须显式锁定焦距，
// 否则「连续自动对焦」会每帧微调镜头，导致画面持续轻微失焦 —— 实测
// 同一位置 AF 开着时清晰度只有锁死时的 1/8（见 docs/calibration.md §焦距）。
//
// 注意：控件设置在设备内持久保存（v4l2-ctl 关掉重开仍然有效），所以可以在
// cv::VideoCapture::open() **之前** 用临时 fd 写好，再交给 OpenCV 去流化。
// ---------------------------------------------------------------------------
struct V4l2ControlValue {
  std::uint32_t id = 0;
  int value = 0;
};

// 控件名 -> V4L2_CID_*（支持 focus_absolute / focus_automatic_continuous /
// exposure_auto / exposure_time_absolute / sharpness 等常用项）
std::uint32_t ResolveControlId(const std::string& name);

// 读取一个控件当前值，失败返回 -1
int GetV4l2Control(const std::string& device, const std::string& name);

// 写入一批控件。成功返回 true，失败原因写到 err。
// order 很重要：先 focus_automatic_continuous=0 再 focus_absolute=N，
// 反了会被 AF 立刻覆盖回去。
bool SetV4l2Controls(const std::string& device,
                     const std::vector<V4l2ControlValue>& values, std::string* err);

}  // namespace posture
