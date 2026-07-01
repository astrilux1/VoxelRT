#include "core.h"
#include "scenes.h"
#include "accel.h"
#include <memory>
#include <cstdio>

int main(){
  std::unique_ptr<Scene> s(makeScene("town"));
  s->build(); s->applyEvent(); collectEmitters();
  V3 o(54.5000,3.0010,168.5000);
  V3 d(0.692609,0.201461,-0.692609);
  printf("len=%.9f\n", len(d));
  Hit a,b;
  bool ha=traceNaive(o,d,1e9f,a);
  bool hb=trace(o,d,1e9f,b);
  printf("naive: hit=%d (%d,%d,%d) f%d t=%.9f m=%d\n",ha,a.vx,a.vy,a.vz,a.face,a.t,a.m);
  printf("brick: hit=%d (%d,%d,%d) f%d t=%.9f m=%d\n",hb,b.vx,b.vy,b.vz,b.face,b.t,b.m);
  // check voxels around there
  for(int x=96;x<=100;x++) for(int z=121;z<=125;z++) {
    printf("at(%d,16,%d)=%d at(%d,15,%d)=%d\n",x,z,(int)at(x,16,z),x,z,(int)at(x,15,z));
  }
  return 0;
}
