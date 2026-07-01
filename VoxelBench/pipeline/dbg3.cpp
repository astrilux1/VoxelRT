#include "core.h"
#include "scenes.h"
#include <memory>
#include <cstdio>

int main(){
  std::unique_ptr<Scene> s(makeScene("town"));
  s->build(); s->applyEvent(); collectEmitters();
  // reproduce harness RNG: si=3 (town), post=1 -> seed = si*2+post = 7
  RNG rng(7,42,777);
  for(int i=0;i<40000;i++){
    V3 o(1.f+rng.uf()*(NX-2), 1.f+rng.uf()*(NY-2), 1.f+rng.uf()*(NZ-2));
    float z=rng.uf()*2-1, ph=rng.uf()*6.2831853f, rr=std::sqrt(std::max(0.f,1-z*z));
    V3 d(rr*std::cos(ph), z, rr*std::sin(ph));
    Hit a,b;
    bool ha=traceNaive(o,d,1e9f,a);
    bool hb=trace(o,d,1e9f,b);
    bool ok = (ha==hb) && (!ha || (a.vx==b.vx&&a.vy==b.vy&&a.vz==b.vz&&a.face==b.face));
    if(!ok){
      printf("MISMATCH i=%d\n",i);
      printf("o=(%.9f,%.9f,%.9f)\n",o.x,o.y,o.z);
      printf("d=(%.9f,%.9f,%.9f)\n",d.x,d.y,d.z);
      printf("naive: hit=%d (%d,%d,%d) f%d t=%.9f\n",ha,a.vx,a.vy,a.vz,a.face,a.t);
      printf("brick: hit=%d (%d,%d,%d) f%d t=%.9f\n",hb,b.vx,b.vy,b.vz,b.face,b.t);
    }
  }
  return 0;
}
