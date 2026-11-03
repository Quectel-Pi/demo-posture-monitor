// RKNN YOLOv8n-Pose 引擎（librknnrt.so 直调）
//
// 模型：yolov8n-pose, w8a8 量化, 输入 640x640 RGB uint8
// 输出（RKNNAI/RK3576-CNN-yolov8n-pose 4 输出布局）：
//   out0 (1,65,80,80)  out1 (1,65,40,40)  out2 (1,65,20,20)   <- NCHW，64ch DFL + 1ch logits
//   out3 (1,17,3,8400)                                      <- 17 关键点 x,y,conf
//
// 与官方 model_zoo 后处理的差异：官方 demo 假定 int8 + 自有 rknn_app_context；
// 这里用 rknn_output.want_float=1 让 runtime 反量化成 float32，再由本文件完成
// DFL 解码 / NMS / 坐标还原，逻辑与 tests/reference_decode.py 逐点对齐（误差 <7px）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "posture/PoseEngine.hpp"

namespace posture {

class RknnPoseEngine final : public PoseEngine {
 public:
  explicit RknnPoseEngine(const Config& cfg);
  ~RknnPoseEngine() override;

  bool Init(std::string* err) override;
  bool Infer(const Frame& frame, std::vector<PersonPose>* out, std::string* err) override;

  std::string name() const override { return "rknn"; }
  std::string runtime_version() const override { return sdk_version_; }
  int input_width() const override { return in_w_; }
  int input_height() const override { return in_h_; }
  bool joint_confidence_reliable() const override { return false; }
  std::string model_sha256() const override { return model_sha256_; }
  double last_latency_ms() const override { return last_infer_ms_; }

 private:
  friend struct RknnDecodeTestAccess;
  struct Candidate {
    float cx, cy, w, h;
    float score;
    // 归一化空间的关键点（0..1，相对 640 输入）
    float kpt[kNumJoints][3];
    int anchor_flat;
  };

  // RKNN 张量解码：输出到 candidates_（640 输入空间）
  // outs 里给的是「可直接读的 float 视图」：量化模型的 int8 张量在读取时就地反量化
  void Decode(const float* const* outs, const int* out_sizes);
  // letterbox：把 BGR 帧缩放填充到 640x640 的 RGB 缓冲
  void Letterbox(const cv::Mat& bgr);
  // 把模型坐标还原到原图
  void ToOriginal(std::vector<PersonPose>* out) const;
  static void Nms(std::vector<Candidate>* cands, float iou_th);

  Config cfg_;
  bool initialized_ = false;

  struct Impl;                  // 隐藏 rknn 上下文，避免头文件污染
  Impl* impl_ = nullptr;

  std::vector<std::uint8_t> model_buf_;
  std::string model_sha256_;
  std::string sdk_version_ = "?";

  int in_w_ = 640;
  int in_h_ = 640;
  int n_out_ = 0;
  std::vector<std::vector<int>> out_dims_;

  // 预处理缓冲（预分配，避免每帧 malloc，§14.2 优化步骤 3）
  std::vector<std::uint8_t> input_buf_;
  // letterbox 中间缓冲：只在本帧输入尺寸变化时重新分配，正常情况零分配
  cv::Mat lb_resized_;
  cv::Mat lb_rgb_;
  int lb_src_w_ = 0;
  int lb_src_h_ = 0;
  float ratio_ = 1.f;
  int pad_x_ = 0;
  int pad_y_ = 0;

  // 输出缓冲。runtime 直接把 int8 张量拷进 out_q_buf_（pass_through），
  // 解码时按 (q - zp) * scale 就地反量化到 out_f_buf_。
  // 这样避免让 runtime 做全量反量化：它的 want_float=1 路径要把 2.5M 个
  // 元素全部转成 float32（实测多花 1.8~2.7ms），而我们只用到其中一部分。
  std::vector<std::vector<std::int8_t>> out_q_buf_;
  std::vector<std::vector<float>> out_f_buf_;
  int out_zp_[4] = {0, 0, 0, 0};
  float out_scale_[4] = {0.f, 0.f, 0.f, 0.f};
  int out_elems_[4] = {0, 0, 0, 0};
  bool out_is_int8_ = false;

  std::vector<Candidate> candidates_;
  double last_infer_ms_ = 0.0;
};

}  // namespace posture
