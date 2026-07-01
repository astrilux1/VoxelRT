#include "core.h"
#include "scenes.h"
#include <memory>
#include <cstdio>

int main(){
  std::unique_ptr<Scene> s(makeScene("cavern"));
  s->build(); s->applyEvent(); collectEmitters();
  V3 o(4.683f,45.576f,74.760f);
  V3 d(-0.747f,-0.611f,0.263f);
  printf("d=(%.9f,%.9f,%.9f) len=%.9f\n",d.x,d.y,d.z, len(d));
  Hit a,b;
  bool ha=traceNaive(o,d,1e9f,a);
  bool hb=trace(o,d,1e9f,b);
  printf("naive: hit=%d (%d,%d,%d) f%d t=%.6f\n",ha,a.vx,a.vy,a.vz,a.face,a.t);
  printf("brick: hit=%d (%d,%d,%d) f%d t=%.6f\n",hb,b.vx,b.vy,b.vz,b.face,b.t);
  for(int z=72;z<=76;z++) printf("at(3,45,%d)=%d at(4,45,%d)=%d\n",z,(int)at(3,45,z),z,(int)at(4,45,z));
  return 0;
}
