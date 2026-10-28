#include "posture/Types.hpp"

namespace posture {

const char* JointName(int idx) {
  static const char* kNames[kNumJoints] = {
      "nose", "eye_l", "eye_r", "ear_l", "ear_r",
      "shoulder_l", "shoulder_r", "elbow_l", "elbow_r",
      "wrist_l", "wrist_r", "hip_l", "hip_r",
      "knee_l", "knee_r", "ankle_l", "ankle_r",
  };
  if (idx < 0 || idx >= kNumJoints) return "?";
  return kNames[idx];
}

const char* ToString(PostureState state) {
  switch (state) {
    case PostureState::kNormal: return "NORMAL";
    case PostureState::kShoulderTilt: return "SHOULDER_TILT";
    case PostureState::kHeadDown: return "HEAD_DOWN";
    case PostureState::kHeadTilt: return "HEAD_TILT";
    case PostureState::kTooClose: return "TOO_CLOSE";
    case PostureState::kHandOnFace: return "HAND_ON_FACE";
    case PostureState::kUserAway: return "USER_AWAY";
    case PostureState::kUnknown: break;
  }
  return "UNKNOWN";
}

const char* ToChinese(PostureState state) {
  switch (state) {
    case PostureState::kNormal: return "正常";
    case PostureState::kShoulderTilt: return "高低肩";
    case PostureState::kHeadDown: return "低头/趴桌";
    case PostureState::kHeadTilt: return "歪头";
    case PostureState::kTooClose: return "离屏太近";
    case PostureState::kHandOnFace: return "托腮";
    case PostureState::kUserAway: return "无人";
    case PostureState::kUnknown: break;
  }
  return "无法判定";
}

}  // namespace posture
