#include "core.h"
#include "scenes.h"
#include "accel.h"
#include <memory>
#include <cstdio>
#include <cmath>
#include <algorithm>

// trace through trace()-equivalent brick DDA, printing each brick visited
static void traceBrickPath(V3 o, V3 d, float tmax){
  int x=(int)std::floor(o.x), y=(int)std::floor(o.y), z=(int)std::floor(o.z);
  int sx=d.x>0?1:-1, sy=d.y>0?1:-1, sz=d.z>0?1:-1;
  double adx=(double)std::fabs(d.x)+1e-12, ady=(double)std::fabs(d.y)+1e-12, adz=(double)std::fabs(d.z)+1e-12;
  double idx=1.0/adx, idy=1.0/ady, idz=1.0/adz;
  double bidx=4.0*idx, bidy=4.0*idy, bidz=4.0*idz;
  int bx=x>>2, by=y>>2, bz=z>>2;
  double btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
  double bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
  double btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
  double bt=0; int bface=-1;
  for(int bit=0; bit<60; bit++){
    if(bt>tmax) break;
    if(bx<0||bx>=BNX||by<0||by>=BNY||bz<0||bz>=BNZ) break;
    uint64_t mask = brick[bgi(bx,by,bz)];
    printf("  [brick] bx=%d by=%d bz=%d bt=%.6f bface=%d mask=%s\n",bx,by,bz,bt,bface,mask?"NONZERO":"0");
    if(mask) break;
    if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
    else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
    else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
  }
}

int main(){
  std::unique_ptr<Scene> s(makeScene("town"));
  s->build(); s->applyEvent(); collectEmitters();
  SuperGrid S; S.build();
  V3 o(209.9990,46.5000,150.5000);
  V3 d(-0.698720,-0.052407,-0.713473);
  printf("=== trace() brick path ===\n");
  traceBrickPath(o,d,1e9f);
  return 0;
}
