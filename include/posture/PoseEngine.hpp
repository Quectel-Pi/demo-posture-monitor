// 姿态推理引擎接口（对应实施方案 §6.2 PoseEngine / §11.4 降级策略）
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "posture/Config.hpp"
#include "posture/Types.hpp"

namespace posture {

class PoseEngine {
 public:
  virtual ~PoseEngine() = default;

  virtual bool Init(std::string* err) = 0;

  // 推理一帧，输出所有人（按置信度降序）。
  // 返回值 false 表示本次推理失败（调用方应保持上一状态并记 UNKNOWN）。
  virtual bool Infer(const Frame& frame, std::vector<PersonPose>* out, std::string* err) = 0;

  virtual std::string name() const = 0;
  virtual std::string runtime_version() const = 0;
  virtual int input_width() const = 0;
  virtual int input_height() const = 0;

  // w8a8 量化的 YOLOv8n-pose 会把 17x3 关键点张量整体量化（scale≈3.19），
  // 导致关键点自带的 conf 通道只剩 0~1 个量化级、恒为 0 —— 不可用于过滤。
  // 返回 false 时调用方必须改用「人物框置信度」作为关键点可信度代理，
  // 并在日志里明确标注（见 docs/troubleshooting.md §1）。
  virtual bool joint_confidence_reliable() const = 0;

  virtual std::string model_sha256() const = 0;

  // 最近一次推理的纯 NPU 耗时（ms），用于状态面板展示
  virtual double last_latency_ms() const = 0;
};

std::unique_ptr<PoseEngine> CreateRknnPoseEngine(const Config& cfg);

// 文件 SHA-256（用于基线记录模型版本，§4.6）
std::string Sha256File(const std::string& path);

}  // namespace posture
