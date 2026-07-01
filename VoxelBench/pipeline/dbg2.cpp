#include "core.h"
#include "scenes.h"
#include <memory>
#include <cstdio>

int main(){
  std::unique_ptr<Scene> s(makeScene("town"));
  s->build(); s->applyEvent(); collectEmitters();
  V3 o(111.264f,79.551f,92.793f);
  V3 d(-0.488f,-0.490f,0.722f);
  printf("d=(%.9f,%.9f,%.9f) len=%.9f\n",d.x,d.y,d.z, len(d));
  Hit a,b;
  bool ha=traceNaive(o,d,1e9f,a);
  bool hb=trace(o,d,1e9f,b);
  printf("naive: hit=%d (%d,%d,%d) f%d t=%.6f\n",ha,a.vx,a.vy,a.vz,a.face,a.t);
  printf("brick: hit=%d (%d,%d,%d) f%d t=%.6f\n",hb,b.vx,b.vy,b.vz,b.face,b.t);
  for(int x=33;x<=36;x++) printf("at(%d,2,205)=%d\n",x,(int)at(x,2,205));
  // brick coords for x=34,35 -> bx = x>>2
  printf("bx(34)=%d bx(35)=%d\n", 34>>2, 35>>2);
  return 0;
}
