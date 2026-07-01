#include "core.h"
#include "scenes.h"
#include "accel.h"
#include <memory>
#include <cstdio>
#include <cmath>
#include <algorithm>

// copy of traceSuper with debug printing
static bool traceSuperDbg(const SuperGrid& S, V3 o, V3 d, float tmax, Hit& h){
  int x=(int)std::floor(o.x), y=(int)std::floor(o.y), z=(int)std::floor(o.z);
  if(!inB(x,y,z)) return false;
  int sx=d.x>0?1:-1, sy=d.y>0?1:-1, sz=d.z>0?1:-1;
  double adx=(double)std::fabs(d.x)+1e-12, ady=(double)std::fabs(d.y)+1e-12, adz=(double)std::fabs(d.z)+1e-12;
  double idx=1.0/adx, idy=1.0/ady, idz=1.0/adz;
  double bidx=4.0*idx, bidy=4.0*idy, bidz=4.0*idz;
  int bx=x>>2, by=y>>2, bz=z>>2;
  double btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
  double bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
  double btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
  double bt=0; int bface=-1;
  bool first=true;
  for(int bit=0; bit<60; bit++){
    if(bt>tmax) return false;
    if(bx<0||bx>=BNX||by<0||by>=BNY||bz<0||bz>=BNZ) return false;
    int sbx=bx>>2, sby=by>>2, sbz=bz>>2;
    printf("it=%d bx=%d by=%d bz=%d bt=%.6f bface=%d sbx=%d sby=%d sbz=%d any=%d\n",
        bit,bx,by,bz,bt,bface,sbx,sby,sbz,(int)S.any(sbx,sby,sbz));
    if(!S.any(sbx,sby,sbz)){
      double stx=((sx>0? (sbx+1)*16-(double)o.x : (double)o.x-sbx*16))*idx;
      double sty=((sy>0? (sby+1)*16-(double)o.y : (double)o.y-sby*16))*idy;
      double stz=((sz>0? (sbz+1)*16-(double)o.z : (double)o.z-sbz*16))*idz;
      printf("   SKIP stx=%.6f sty=%.6f stz=%.6f\n",stx,sty,stz);
      if(stx<=sty && stx<=stz){ bt=stx; bx=(sx>0)? (sbx+1)*4 : sbx*4-1; bface=sx>0?1:0; }
      else if(sty<=stz){ bt=sty; by=(sy>0)? (sby+1)*4 : sby*4-1; bface=sy>0?3:2; }
      else { bt=stz; bz=(sz>0)? (sbz+1)*4 : sbz*4-1; bface=sz>0?5:4; }
      btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
      bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
      btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
      printf("   -> bx=%d by=%d bz=%d bt=%.6f bface=%d\n",bx,by,bz,bt,bface);
      first=false;
      continue;
    }
    if(S.bit(bx,by,bz)){
      uint64_t mask = brick[bgi(bx,by,bz)];
      if(mask){
        printf("   OCCUPIED brick, run fineDDA\n");
        // simplified fine DDA check
        double eps=1e-7;
        double px=(double)o.x+(double)d.x*std::max(0.0,bt-eps);
        double py=(double)o.y+(double)d.y*std::max(0.0,bt-eps);
        double pz=(double)o.z+(double)d.z*std::max(0.0,bt-eps);
        int fx,fy,fz;
        if(first && bt==0){ fx=(int)std::floor((double)o.x); fy=(int)std::floor((double)o.y); fz=(int)std::floor((double)o.z); }
        else {
          fx=(int)std::floor(px); fy=(int)std::floor(py); fz=(int)std::floor(pz);
          if(bface==0){ fx=bx*4+3; }
          if(bface==1){ fx=bx*4; }
          if(bface==2){ fy=by*4+3; }
          if(bface==3){ fy=by*4; }
          if(bface==4){ fz=bz*4+3; }
          if(bface==5){ fz=bz*4; }
          fx=std::min(std::max(fx,bx*4),bx*4+3);
          fy=std::min(std::max(fy,by*4),by*4+3);
          fz=std::min(std::max(fz,bz*4),bz*4+3);
        }
        printf("     entry voxel fx=%d fy=%d fz=%d (px=%.4f py=%.4f pz=%.4f)\n",fx,fy,fz,px,py,pz);
        printf("     at(fx,fy,fz)=%d\n",(int)at(fx,fy,fz));
        if(at(fx,fy,fz)) { printf("     -> HIT here (bogus?)\n"); return true; }
        first=false;
      }
    }
    if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
    else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
    else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
    first=false;
  }
  return false;
}

int main(){
  std::unique_ptr<Scene> s(makeScene("town"));
  s->build(); s->applyEvent(); collectEmitters();
  SuperGrid S; S.build();
  V3 o(209.9990,46.5000,150.5000);
  V3 d(-0.698720,-0.052407,-0.713473);
  Hit h;
  traceSuperDbg(S,o,d,1e9f,h);
  return 0;
}
