#include "core.h"
#include "scenes.h"
#include "accel.h"
#include <memory>
#include <cstdio>

int main(){
  std::unique_ptr<Scene> s(makeScene("town"));
  s->build(); s->applyEvent(); collectEmitters();
  SuperGrid S; S.build();
  V3 o(209.9990,46.5000,150.5000);
  V3 d(-0.698720,-0.052407,-0.713473);
  Hit a,b;
  bool ha=traceNaive(o,d,1e9f,a);
  bool hb=traceSuper(S,o,d,1e9f,b);
  printf("naive: hit=%d (%d,%d,%d) f%d t=%.9f m=%d\n",ha,a.vx,a.vy,a.vz,a.face,a.t,a.m);
  printf("super: hit=%d (%d,%d,%d) f%d t=%.9f m=%d\n",hb,b.vx,b.vy,b.vz,b.face,b.t,b.m);
  Hit c; bool hc=trace(o,d,1e9f,c);
  printf("brick: hit=%d (%d,%d,%d) f%d t=%.9f m=%d\n",hc,c.vx,c.vy,c.vz,c.face,c.t,c.m);
  // Check geometry near (127,40,66) and (127,40,75)
  printf("at(127,40,66)=%d at(126,40,66)=%d\n",(int)at(127,40,66),(int)at(126,40,66));
  printf("at(127,40,75)=%d at(127,41,75)=%d\n",(int)at(127,40,75),(int)at(127,41,75));
  // superbrick coords
  printf("BNX=%d BNY=%d BNZ=%d SNX=%d SNY=%d SNZ=%d\n",BNX,BNY,BNZ,S.SNX,S.SNY,S.SNZ);
  printf("bx,by,bz of (127,40,66): %d %d %d -> sup any=%d\n",127>>2,40>>2,66>>2, S.any((127>>2)/4,(40>>2)/4,(66>>2)/4));
  printf("bx,by,bz of (127,40,75): %d %d %d -> sup any=%d\n",127>>2,40>>2,75>>2, S.any((127>>2)/4,(40>>2)/4,(75>>2)/4));
  return 0;
}
