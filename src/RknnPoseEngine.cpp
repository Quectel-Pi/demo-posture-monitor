#include "posture/RknnPoseEngine.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <numeric>

#include <opencv2/imgproc.hpp>

#include "rknn_api.h"

#include "posture/Logger.hpp"

namespace posture {
namespace {

constexpr int kImgSize = 640;
constexpr int kStrides[3] = {8, 16, 32};
constexpr int kGrid[3] = {80, 40, 20};
// 三个尺度的 anchor 在 out3 的 8400 维里的起始偏移（80² + 40² + 20² = 8400）
constexpr int kAnchorOffset[3] = {0, 6400, 8000};
constexpr int kNumAnchors = 8400;
constexpr int kDflBins = 16;

inline float Sigmoid(float x) {
  if (x < -60.f) return 0.f;
  if (x > 60.f) return 1.f;
  return 1.f / (1.f + std::exp(-x));
}

// DFL：对 16 个 bin 做 softmax 后取期望（单位：stride 的倍数）
inline float DflExpectation(const float* chan_base, int gs, int side, int idx) {
  const int plane = gs * gs;
  const int base = (side * kDflBins) * plane + idx;
  float max_v = chan_base[base];
  for (int k = 1; k < kDflBins; ++k) {
    max_v = std::max(max_v, chan_base[base + k * plane]);
  }
  float sum = 0.f, acc = 0.f;
  for (int k = 0; k < kDflBins; ++k) {
    const float e = std::exp(chan_base[base + k * plane] - max_v);
    sum += e;
    acc += e * static_cast<float>(k);
  }
  return sum > 0.f ? acc / sum : 0.f;
}

// ---------------- SHA-256（只为记录模型指纹，不追求性能） ----------------
struct Sha256 {
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::uint64_t total = 0;
  std::uint8_t buf[64];
  size_t buf_len = 0;

  static std::uint32_t Rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void Block(const std::uint8_t* p) {
    static const std::uint32_t kK[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
             (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
      const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = s0 + maj;
      hh = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }

  void Update(const std::uint8_t* data, size_t len) {
    total += len;
    while (len > 0) {
      const size_t take = std::min(len, sizeof(buf) - buf_len);
      std::memcpy(buf + buf_len, data, take);
      buf_len += take;
      data += take;
      len -= take;
      if (buf_len == sizeof(buf)) {
        Block(buf);
        buf_len = 0;
      }
    }
  }

  std::string Final() {
    const std::uint64_t bits = total * 8;
    std::uint8_t pad = 0x80;
    Update(&pad, 1);
    std::uint8_t zero = 0;
    while (buf_len != 56) Update(&zero, 1);
    std::uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) {
      len_be[i] = static_cast<std::uint8_t>((bits >> (56 - i * 8)) & 0xFF);
    }
    Update(len_be, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    out[64] = '\0';
    return std::string(out);
  }
};

}  // namespace

std::string Sha256File(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return "";
  Sha256 sha;
  std::vector<char> chunk(1 << 16);
  while (ifs) {
    ifs.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize got = ifs.gcount();
    if (got > 0) {
      sha.Update(reinterpret_cast<const std::uint8_t*>(chunk.data()),
                 static_cast<size_t>(got));
    }
  }
  return sha.Final();
}

// ---------------------------------------------------------------------------
struct RknnPoseEngine::Impl {
  rknn_context ctx = 0;
  rknn_input_output_num io_num{};
  rknn_tensor_attr in_attr{};
  std::vector<rknn_tensor_attr> out_attr;
  bool sdk_ok = false;
};

RknnPoseEngine::RknnPoseEngine(const Config& cfg) : cfg_(cfg) {}
RknnPoseEngine::~RknnPoseEngine() {
  if (impl_) {
    if (impl_->ctx) rknn_destroy(impl_->ctx);
    delete impl_;
    impl_ = nullptr;
  }
}

bool RknnPoseEngine::Init(std::string* err) {
  impl_ = new Impl();

  std::ifstream ifs(cfg_.model_path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    if (err) *err = "无法打开模型: " + cfg_.model_path;
    return false;
  }
  const std::streamsize size = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  model_buf_.resize(static_cast<size_t>(size));
  if (!ifs.read(reinterpret_cast<char*>(model_buf_.data()), size)) {
    if (err) *err = "读取模型失败: " + cfg_.model_path;
    return false;
  }
  ifs.close();
  model_sha256_ = Sha256File(cfg_.model_path);

  // ★ model_buf_ 必须在 rknn_destroy 之前一直有效：rknn_init 不拷贝权重
  int ret = rknn_init(&impl_->ctx, model_buf_.data(),
                      static_cast<uint32_t>(model_buf_.size()), 0, nullptr);
  if (ret != RKNN_SUCC) {
    if (err) {
      *err = "rknn_init 失败 ret=" + std::to_string(ret) +
             "（检查模型目标平台是否 rk3576、runtime 版本是否匹配）";
    }
    return false;
  }

  ret = rknn_query(impl_->ctx, RKNN_QUERY_IN_OUT_NUM, &impl_->io_num, sizeof(impl_->io_num));
  if (ret != RKNN_SUCC) {
    if (err) *err = "rknn_query(IN_OUT_NUM) 失败";
    return false;
  }
  n_out_ = static_cast<int>(impl_->io_num.n_output);

  impl_->in_attr = {};
  impl_->in_attr.index = 0;
  ret = rknn_query(impl_->ctx, RKNN_QUERY_INPUT_ATTR, &impl_->in_attr, sizeof(impl_->in_attr));
  if (ret != RKNN_SUCC) {
    if (err) *err = "rknn_query(INPUT_ATTR) 失败";
    return false;
  }
  // 输入 dims 是 NCHW：dims[1]=H, dims[2]=W
  in_h_ = static_cast<int>(impl_->in_attr.dims[1]);
  in_w_ = static_cast<int>(impl_->in_attr.dims[2]);
  if (in_w_ <= 0 || in_h_ <= 0) {
    in_w_ = in_h_ = kImgSize;
  }

  impl_->out_attr.resize(n_out_);
  out_dims_.clear();
  for (int i = 0; i < n_out_; ++i) {
    impl_->out_attr[i] = {};
    impl_->out_attr[i].index = static_cast<uint32_t>(i);
    if (rknn_query(impl_->ctx, RKNN_QUERY_OUTPUT_ATTR, &impl_->out_attr[i],
                   sizeof(rknn_tensor_attr)) != RKNN_SUCC) {
      if (err) *err = "rknn_query(OUTPUT_ATTR) 失败 index=" + std::to_string(i);
      return false;
    }
    std::vector<int> dims;
    for (uint32_t d = 0; d < impl_->out_attr[i].n_dims; ++d) {
      dims.push_back(static_cast<int>(impl_->out_attr[i].dims[d]));
    }
    out_dims_.push_back(dims);
    std::string desc;
    for (int v : dims) desc += std::to_string(v) + "x";
    LOG_INFO("输出 ", i, " shape=", desc, " attr.size=", impl_->out_attr[i].size,
             " n_elems=", impl_->out_attr[i].n_elems,
             " type=", static_cast<int>(impl_->out_attr[i].type));
  }

  rknn_sdk_version ver{};
  if (rknn_query(impl_->ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC) {
    sdk_version_ = std::string(ver.api_version) + " / drv " + ver.drv_version;
    impl_->sdk_ok = true;
  }

  // 校验输出布局是否与解码逻辑匹配（不匹配就明确报错，而不是输出垃圾，§16.4）
  if (n_out_ != 4) {
    if (err) {
      *err = "模型输出张量数 " + std::to_string(n_out_) +
             " != 4，与内置 yolov8n-pose 解码器不匹配";
    }
    return false;
  }
  const bool shape_ok =
      out_dims_[0].size() == 4 && out_dims_[0][1] == 65 &&
      out_dims_[1].size() == 4 && out_dims_[1][1] == 65 &&
      out_dims_[2].size() == 4 && out_dims_[2][1] == 65 &&
      out_dims_[3].size() == 4 && out_dims_[3][1] == kNumJoints &&
      out_dims_[3][3] == kNumAnchors;
  if (!shape_ok) {
    if (err) {
      *err = "模型输出 shape 不符合期望 (65,80,80)/(65,40,40)/(65,20,20)/(17,3,8400)，"
             "请重新导出或修改解码器";
    }
    return false;
  }

  input_buf_.assign(static_cast<size_t>(in_w_) * in_h_ * 3, 114);
  candidates_.reserve(256);

  LOG_INFO("RKNN 引擎就绪: ", cfg_.model_path, " 输入 ", in_w_, "x", in_h_,
           " 输出 ", n_out_, " 个 tensor, runtime ", sdk_version_);
  LOG_INFO("模型 SHA256 ", model_sha256_);
  LOG_WARN("RKNN w8a8 模型的关键点 conf 通道被量化破坏（恒为 0），"
           "关键点可信度改用人物框置信度代理 —— 见 docs/troubleshooting.md §1");

  initialized_ = true;
  return true;
}

void RknnPoseEngine::Letterbox(const cv::Mat& bgr) {
  // 与 Python 参考实现完全一致：r = min(size/w, size/h)，nw=round(w*r)，
  // pad 用 (size-nw)/2 向下取整。参考实现见 tests/reference_decode.py。
  const int w = bgr.cols;
  const int h = bgr.rows;
  const float r = std::min(static_cast<float>(in_w_) / w, static_cast<float>(in_h_) / h);
  const int nw = static_cast<int>(std::round(w * r));
  const int nh = static_cast<int>(std::round(h * r));
  pad_x_ = (in_w_ - nw) / 2;
  pad_y_ = (in_h_ - nh) / 2;
  ratio_ = r;

  std::memset(input_buf_.data(), 114, input_buf_.size());  // 灰边 padding
  cv::Mat resized;
  cv::resize(bgr, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
  cv::Mat rgb;
  cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

  for (int y = 0; y < nh; ++y) {
    const std::uint8_t* src = rgb.ptr<std::uint8_t>(y);
    std::uint8_t* dst = input_buf_.data() +
                        (static_cast<size_t>(y + pad_y_) * in_w_ + pad_x_) * 3;
    std::memcpy(dst, src, static_cast<size_t>(nw) * 3);
  }
}

void RknnPoseEngine::Decode(const float* const* outs, const int* out_sizes) {
  candidates_.clear();
  const float conf_th = cfg_.pose_confidence;
  const float* kpt_tensor = outs[3];
  const int kpt_plane = out_sizes[3] / (kNumJoints * 3);  // 应为 8400

  for (int si = 0; si < 3; ++si) {
    const int gs = kGrid[si];
    const int st = kStrides[si];
    const int plane = gs * gs;
    const float* t = outs[si];
    const int expect = 65 * plane;
    if (out_sizes[si] < expect) continue;
    const float* conf_ch = t + 64 * plane;

    for (int idx = 0; idx < plane; ++idx) {
      const float score = Sigmoid(conf_ch[idx]);
      if (score <= conf_th) continue;

      float d[4];
      for (int side = 0; side < 4; ++side) d[side] = DflExpectation(t, gs, side, idx);

      const int gx = idx % gs;
      const int gy = idx / gs;
      const int anchor = kAnchorOffset[si] + idx;

      Candidate c{};
      c.cx = (gx + 0.5f - d[0] + d[2]) * st;
      c.cy = (gy + 0.5f - d[1] + d[3]) * st;
      c.w = (d[0] + d[2]) * st;
      c.h = (d[1] + d[3]) * st;
      c.score = score;
      c.anchor_flat = anchor;
      // 关键点跟着「同一个 anchor」走：量化后 conf 通道不可用，不能用它筛选（§5 坑）
      for (int j = 0; j < kNumJoints; ++j) {
        const size_t base = static_cast<size_t>(j * 3) * kpt_plane + anchor;
        // ★ 边界用「元素个数」比较，别再除 sizeof(float) —— 曾经把 float 元素数又除了 4，
        //   导致 j>=4 的关键点全部越界归零（只有鼻子/眼睛有值），排查了很久。
        if (static_cast<long long>(base) + 2LL * kpt_plane < out_sizes[3]) {
          c.kpt[j][0] = kpt_tensor[base];
          c.kpt[j][1] = kpt_tensor[base + kpt_plane];
          c.kpt[j][2] = kpt_tensor[base + 2 * kpt_plane];
        } else {
          c.kpt[j][0] = c.kpt[j][1] = 0.f;
          c.kpt[j][2] = 0.f;
        }
      }
      candidates_.push_back(c);
    }
  }
  Nms(&candidates_, cfg_.nms_iou);
}

void RknnPoseEngine::Nms(std::vector<Candidate>* cands, float iou_th) {
  if (cands->empty()) return;
  std::sort(cands->begin(), cands->end(),
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
  std::vector<Candidate> keep;
  std::vector<bool> removed(cands->size(), false);
  for (size_t i = 0; i < cands->size(); ++i) {
    if (removed[i]) continue;
    keep.push_back((*cands)[i]);
    const Candidate& a = (*cands)[i];
    const float ax1 = a.cx - a.w * 0.5f, ay1 = a.cy - a.h * 0.5f;
    const float ax2 = a.cx + a.w * 0.5f, ay2 = a.cy + a.h * 0.5f;
    for (size_t j = i + 1; j < cands->size(); ++j) {
      if (removed[j]) continue;
      const Candidate& b = (*cands)[j];
      const float bx1 = b.cx - b.w * 0.5f, by1 = b.cy - b.h * 0.5f;
      const float bx2 = b.cx + b.w * 0.5f, by2 = b.cy + b.h * 0.5f;
      const float ix = std::max(0.f, std::min(ax2, bx2) - std::max(ax1, bx1));
      const float iy = std::max(0.f, std::min(ay2, by2) - std::max(ay1, by1));
      const float inter = ix * iy;
      const float uni = a.w * a.h + b.w * b.h - inter;
      if (uni > 1e-6f && inter / uni > iou_th) removed[j] = true;
    }
  }
  *cands = std::move(keep);
}

void RknnPoseEngine::ToOriginal(std::vector<PersonPose>* out) const {
  out->clear();
  out->reserve(candidates_.size());
  for (const auto& c : candidates_) {
    PersonPose person{};
    person.box.x = (c.cx - c.w * 0.5f - pad_x_) / ratio_;
    person.box.y = (c.cy - c.h * 0.5f - pad_y_) / ratio_;
    person.box.w = c.w / ratio_;
    person.box.h = c.h / ratio_;
    person.person_confidence = c.score;
    for (int j = 0; j < kNumJoints; ++j) {
      person.joints[j].x = (c.kpt[j][0] - pad_x_) / ratio_;
      person.joints[j].y = (c.kpt[j][1] - pad_y_) / ratio_;
      // 量化模型的关键点 conf 不可信 → 用人物框置信度作代理
      person.joints[j].confidence = c.score;
    }
    out->push_back(person);
  }
}

bool RknnPoseEngine::Infer(const Frame& frame, std::vector<PersonPose>* out, std::string* err) {
  if (!initialized_) {
    if (err) *err = "引擎未初始化";
    return false;
  }
  if (frame.bgr.empty()) {
    if (err) *err = "空帧";
    return false;
  }

  Letterbox(frame.bgr);

  rknn_input in{};
  in.index = 0;
  in.buf = input_buf_.data();
  in.size = static_cast<uint32_t>(input_buf_.size());
  in.pass_through = 0;
  in.type = RKNN_TENSOR_UINT8;
  in.fmt = RKNN_TENSOR_NHWC;
  int ret = rknn_inputs_set(impl_->ctx, 1, &in);
  if (ret != RKNN_SUCC) {
    if (err) *err = "rknn_inputs_set 失败 ret=" + std::to_string(ret);
    return false;
  }

  const auto t0 = SteadyClock::now();
  ret = rknn_run(impl_->ctx, nullptr);
  if (ret != RKNN_SUCC) {
    if (err) *err = "rknn_run 失败 ret=" + std::to_string(ret);
    return false;
  }

  std::vector<rknn_output> outs(static_cast<size_t>(n_out_));
  for (int i = 0; i < n_out_; ++i) {
    outs[i] = {};
    outs[i].index = static_cast<uint32_t>(i);
    outs[i].want_float = 1;    // 让 runtime 反量化，省掉手写 scale/zp
    outs[i].is_prealloc = 0;
    outs[i].buf = nullptr;
    outs[i].size = 0;
  }

  ret = rknn_outputs_get(impl_->ctx, static_cast<uint32_t>(n_out_), outs.data(), nullptr);
  if (ret != RKNN_SUCC) {
    if (err) *err = "rknn_outputs_get 失败 ret=" + std::to_string(ret);
    return false;
  }

  bool ok = true;
  {
    const float* ptrs[4] = {nullptr, nullptr, nullptr, nullptr};
    int sizes[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4 && i < n_out_; ++i) {
      ptrs[i] = static_cast<const float*>(outs[i].buf);
      // ★ 不要用 outs[i].size 推元素个数：量化模型 want_float 转换后 size 的语义
      //   在不同 runtime 版本上不一致（曾导致关键点张量被截断 → 后半段全是 0）。
      //   用 query 到的 dims 乘积最可靠。
      long long elems = 1;
      for (int v : out_dims_[i]) elems *= v;
      sizes[i] = static_cast<int>(elems);
      LOG_DEBUG("输出 ", i, " buf_size=", outs[i].size, " elems=", elems);
      if (!ptrs[i] || sizes[i] <= 0) ok = false;
    }
    if (ok) {
      Decode(ptrs, sizes);
      ToOriginal(out);
    } else if (err) {
      *err = "输出张量为空";
    }
    // ★ 无论成功失败都必须释放，否则 NPU 内存会一路泄漏（§10.2 明确提醒）
    rknn_outputs_release(impl_->ctx, static_cast<uint32_t>(n_out_), outs.data());
  }

  if (ok) {
    last_infer_ms_ = std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();
    LOG_DEBUG("推理 ", last_infer_ms_, "ms 人数 ", out->size());
    for (auto& p : *out) {
      p.frame_id = frame.frame_id;
      p.captured_at = frame.captured_at;
    }
  }
  return ok;
}

std::unique_ptr<PoseEngine> CreateRknnPoseEngine(const Config& cfg) {
  return std::unique_ptr<PoseEngine>(new RknnPoseEngine(cfg));
}

}  // namespace posture
