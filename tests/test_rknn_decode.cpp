#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
#include "posture/RknnPoseEngine.hpp"

namespace posture {
// Exercise tensor decoding without initializing the NPU or opening a camera.
struct RknnDecodeTestAccess {
  static std::vector<PersonPose> Run(RknnPoseEngine& engine,
                                     const float* const* outs, const int* sizes) {
    engine.Decode(outs, sizes);
    std::vector<PersonPose> people;
    engine.ToOriginal(&people);
    return people;
  }
};
}

int main() {
  using namespace posture;
  std::array<std::vector<float>,4> tensors;
  const int sizes[4]={65*80*80,65*40*40,65*20*20,17*3*8400};
  for(int i=0;i<4;++i) tensors[i].assign(sizes[i],-50.f);
  const int anchors[2]={10*80+10,18*80+18};
  const int distances[2][4]={{1,1,9,9},{9,9,1,1}};
  for(int c=0;c<2;++c) {
    const int index=anchors[c];
    tensors[0][64*6400+index]=c==0 ? 3.f : 2.f;
    for(int side=0;side<4;++side)
      tensors[0][(side*16+distances[c][side])*6400+index]=50.f;
    for(int joint=0;joint<17;++joint) {
      tensors[3][(joint*3)*8400+index]=100.f+joint+c*100;
      tensors[3][(joint*3+1)*8400+index]=110.f+joint+c*100;
      tensors[3][(joint*3+2)*8400+index]=.8f;
    }
  }
  const float* outs[4]={tensors[0].data(),tensors[1].data(),tensors[2].data(),tensors[3].data()};
  Config cfg;
  RknnPoseEngine engine(cfg);
  const auto people=RknnDecodeTestAccess::Run(engine,outs,sizes);
  // Both anchors encode the exact same [76,76,156,156] rectangle.
  // Missing the center's /2 shifts them apart and NMS incorrectly retains both.
  if(people.size()!=1) {
    std::printf("FAIL: two anchors for one person produced %zu persons\n",people.size());return 1;
  }
  const auto& p=people.front();
  if(std::fabs(p.box.x-76)>0.01 || std::fabs(p.box.y-76)>0.01 ||
     std::fabs(p.box.w-80)>0.01 || std::fabs(p.box.h-80)>0.01) {
    std::printf("FAIL: decoded box [%f,%f,%f,%f] differs from independently specified corners\n",p.box.x,p.box.y,p.box.w,p.box.h);return 1;
  }
  if(std::fabs(p.joints[kNose].x-100)>0.01 ||std::fabs(p.joints[kNose].y-110)>0.01 ||
     std::fabs(p.joints[kWristR].x-110)>0.01) {
    std::puts("FAIL: NMS must retain the highest-scoring anchor's matching keypoints");return 1;
  }
  std::puts("PASS: DFL corners, asymmetric distances, duplicate suppression, anchor-keypoint association");
  return 0;
}
