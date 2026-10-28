#include "posture/V4l2Probe.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace posture {
namespace {

constexpr int kEnoughSizes = 32;
constexpr int kEnoughFps = 16;

}  // namespace

const char* FourccToString(std::uint32_t fourcc, char out[5]) {
  out[0] = static_cast<char>(fourcc & 0xFF);
  out[1] = static_cast<char>((fourcc >> 8) & 0xFF);
  out[2] = static_cast<char>((fourcc >> 16) & 0xFF);
  out[3] = static_cast<char>((fourcc >> 24) & 0xFF);
  out[4] = '\0';
  for (int i = 0; i < 4; ++i) {
    if (!out[i]) out[i] = ' ';      // 'YUYV' 之外有些格式名带 0
  }
  // 去掉尾部空格，方便比较
  for (int i = 3; i >= 0 && out[i] == ' '; --i) out[i] = '\0';
  return out;
}

V4l2Capabilities ProbeV4l2(const std::string& device) {
  V4l2Capabilities caps;

  // O_RDWR | O_NONBLOCK：某些 UVC 在 O_RDONLY 下 ENUM_FMT 会 EBUSY
  const int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    caps.error = "打不开 " + device + "（" + std::strerror(errno) + "）";
    return caps;
  }

  v4l2_capability cap{};
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
    caps.error = std::string("VIDIOC_QUERYCAP 失败: ") + std::strerror(errno);
    ::close(fd);
    return caps;
  }
  caps.driver = reinterpret_cast<const char*>(cap.driver);
  caps.card = reinterpret_cast<const char*>(cap.card);
  caps.bus = reinterpret_cast<const char*>(cap.bus_info);

  const bool can_capture = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
                           (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) ||
                           (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
  if (!can_capture) {
    // /dev/video74 这类 metadata 节点会走到这里 —— 它不提供采集，跳过
    caps.error = "该节点不支持视频采集（可能是 metadata 节点）";
    ::close(fd);
    return caps;
  }

  v4l2_fmtdesc fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  for (fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; ++fmt.index) {
    V4l2Format f;
    f.fourcc = fmt.pixelformat;
    f.name = reinterpret_cast<const char*>(fmt.description);
    char code[5];
    f.code = FourccToString(fmt.pixelformat, code);

    v4l2_frmsizeenum fs{};
    fs.pixel_format = fmt.pixelformat;
    for (fs.index = 0; fs.index < kEnoughSizes && ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0;
         ++fs.index) {
      if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        V4l2FrameSize size;
        size.width = static_cast<int>(fs.discrete.width);
        size.height = static_cast<int>(fs.discrete.height);
        // 每个尺寸再问一遍支持的帧率
        v4l2_frmivalenum iv{};
        iv.pixel_format = fmt.pixelformat;
        iv.width = fs.discrete.width;
        iv.height = fs.discrete.height;
        for (iv.index = 0; iv.index < kEnoughFps && ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) == 0;
             ++iv.index) {
          if (iv.type == V4L2_FRMIVAL_TYPE_DISCRETE && iv.discrete.numerator > 0) {
            size.fps.push_back(static_cast<double>(iv.discrete.denominator) /
                               static_cast<double>(iv.discrete.numerator));
          }
        }
        std::sort(size.fps.begin(), size.fps.end(), std::greater<double>());
        f.sizes.push_back(std::move(size));
      } else {
        // STEPWISE / CONTINUOUS：只记一组区间端点
        V4l2FrameSize size;
        size.width = static_cast<int>(fs.stepwise.max_width);
        size.height = static_cast<int>(fs.stepwise.max_height);
        f.sizes.push_back(std::move(size));
        break;
      }
    }
    caps.formats.push_back(std::move(f));
    fmt = v4l2_fmtdesc{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.index = static_cast<std::uint32_t>(caps.formats.size());
  }

  ::close(fd);
  caps.ok = !caps.formats.empty();
  if (!caps.ok && caps.error.empty()) caps.error = "没有任何可用的像素格式";
  return caps;
}

CaptureChoice ChooseCapture(const V4l2Capabilities& caps, const CaptureRequest& req) {
  CaptureChoice out;
  if (!caps.ok || caps.formats.empty()) {
    out.error = caps.error.empty() ? "设备能力为空" : caps.error;
    return out;
  }

  // ---- 1. 选格式 ----
  auto find_code = [&](const std::string& code) -> const V4l2Format* {
    for (const auto& f : caps.formats) {
      if (f.code == code) return &f;
    }
    return nullptr;
  };
  // 压缩格式优先：同样分辨率下 USB 带宽占用低得多（YUYV 640x480@30 要 18MB/s）
  static const char* kCompressedPreference[] = {"MJPG", "JPEG", "H264", "HEVC"};
  const V4l2Format* chosen_format = nullptr;
  std::string want = req.preferred_format;
  std::transform(want.begin(), want.end(), want.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

  if (!want.empty() && want != "AUTO") {
    chosen_format = find_code(want);
    if (!chosen_format) {
      out.adjustments.push_back("设备不支持 " + req.preferred_format + "，已自动改选");
    }
  }
  if (!chosen_format) {
    for (const char* code : kCompressedPreference) {
      if ((chosen_format = find_code(code))) break;
    }
  }
  if (!chosen_format) chosen_format = &caps.formats.front();

  // ---- 2. 选尺寸 ----
  const V4l2FrameSize* chosen_size = nullptr;
  if (!chosen_format->sizes.empty()) {
    long want_area = static_cast<long>(req.width) * req.height;
    long best_delta = -1;
    for (const auto& s : chosen_format->sizes) {
      if (s.width == req.width && s.height == req.height) {
        chosen_size = &s;
        best_delta = 0;
        break;
      }
      const long area = static_cast<long>(s.width) * s.height;
      long delta = std::labs(area - want_area);
      // 同样接近时偏向「比期望大」，因为大图缩小的画质损失更小
      if (area < want_area) delta += want_area / 8;
      if (best_delta < 0 || delta < best_delta) {
        best_delta = delta;
        chosen_size = &s;
      }
    }
  }
  if (!chosen_size) {
    out.error = "格式 " + chosen_format->code + " 没有可用尺寸";
    return out;
  }

  // ---- 3. 选帧率 ----
  double fps = 0.0;
  if (!chosen_size->fps.empty()) {
    for (double f : chosen_size->fps) {
      if (std::fabs(f - req.fps) < 0.51) {
        fps = f;
        break;
      }
    }
    if (fps == 0.0) {
      for (double f : chosen_size->fps) {   // 最大的「不超过期望」
        if (f < req.fps) {
          fps = f;
          break;
        }
      }
    }
    if (fps == 0.0) fps = chosen_size->fps.back();
  }

  out.ok = true;
  out.fourcc = chosen_format->fourcc;
  out.format_code = chosen_format->code;
  out.width = chosen_size->width;
  out.height = chosen_size->height;
  out.fps = fps;

  if (out.width != req.width || out.height != req.height) {
    out.adjustments.push_back("分辨率 " + std::to_string(req.width) + "x" +
                              std::to_string(req.height) + " 不支持，改用 " +
                              std::to_string(out.width) + "x" + std::to_string(out.height));
  }
  if (fps > 0 && std::fabs(fps - req.fps) > 0.51) {
    out.adjustments.push_back("帧率 " + std::to_string(req.fps) + " 不支持，改用 " +
                              std::to_string(static_cast<int>(fps)) + "fps");
  }
  if (!want.empty() && want != "AUTO" && chosen_format->code != want) {
    out.adjustments.push_back("像素格式 " + req.preferred_format + " 不支持，改用 " +
                              chosen_format->code);
  }
  return out;
}

}  // namespace posture
