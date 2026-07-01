// traceHX: brick DDA + two-level conservative distance jumps.
// Exact-match discipline identical to accel.cpp's traceCSDF (0 mismatches):
// jumps land conservatively SHORT of the nearest possible solid, in mid-air,
// then brick coords are re-derived from the true ray position and traversal
// continues with exact brick steps. fineDDA entry mechanics shared via the
// same code shape as core.cpp's trace().
#include "accel2.h"

// duplicated from accel.cpp (static there); identical inner loop to trace()
static inline bool fineDDA2(uint64_t mask, int bx,int by,int bz,
                            V3 o, V3 d, int sx,int sy,int sz,
                            double idx,double idy,double idz,
                            double t0, int bface, bool firstAtZero,
                            float tmax, Hit& h){
  double eps=1e-7;
  double px=(double)o.x+(double)d.x*std::max(0.0,t0-eps);
  double py=(double)o.y+(double)d.y*std::max(0.0,t0-eps);
  double pz=(double)o.z+(double)d.z*std::max(0.0,t0-eps);
  int fx,fy,fz;
  if(firstAtZero && t0==0){ fx=(int)std::floor((double)o.x); fy=(int)std::floor((double)o.y); fz=(int)std::floor((double)o.z); }
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
  double tx=((sx>0? fx+1-(double)o.x : (double)o.x-fx))*idx;
  double ty=((sy>0? fy+1-(double)o.y : (double)o.y-fy))*idy;
  double tz=((sz>0? fz+1-(double)o.z : (double)o.z-fz))*idz;
  double t=t0; int face=bface;
  for(int it=0; it<16; it++){
    tl_steps++;
    if(t>tmax) return false;
    uint8_t m = (mask>>bbit(fx&3,fy&3,fz&3)) & 1 ? grid[gi(fx,fy,fz)] : 0;
    if(m && !(firstAtZero && t==0)){ h={fx,fy,fz,face,(float)t,m}; return true; }
    if(tx<=ty && tx<=tz){ t=tx; tx+=idx; fx+=sx; face = sx>0?1:0; }
    else if(ty<=tz)     { t=ty; ty+=idy; fy+=sy; face = sy>0?3:2; }
    else                { t=tz; tz+=idz; fz+=sz; face = sz>0?5:4; }
    if(fx<bx*4||fx>bx*4+3||fy<by*4||fy>by*4+3||fz<bz*4||fz>bz*4+3) break;
  }
  return false;
}

bool traceHX(const HX& D, V3 o, V3 d, float tmax, Hit& h){
  tl_rays++;
  int x=(int)std::floor(o.x), y=(int)std::floor(o.y), z=(int)std::floor(o.z);
  if(!inB(x,y,z)) return false;
  int sx=d.x>0?1:-1, sy=d.y>0?1:-1, sz=d.z>0?1:-1;
  double adx=(double)std::fabs(d.x)+1e-12, ady=(double)std::fabs(d.y)+1e-12, adz=(double)std::fabs(d.z)+1e-12;
  double idx=1.0/adx, idy=1.0/ady, idz=1.0/adz;
  double bidx=4.0*idx, bidy=4.0*idy, bidz=4.0*idz;
  double rml = 1.0/std::max(adx,std::max(ady,adz));   // hoisted (CSDF divided per jump)
  int bx=x>>2, by=y>>2, bz=z>>2;
  double btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
  double bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
  double btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
  double bt=0; int bface=-1;
  bool first=true;
  for(int it=0; it<8192; it++){
    if(bt>tmax) return false;
    if(bx<0||bx>=BNX||by<0||by>=BNY||bz<0||bz>=BNZ) return false;
    tl_steps++;
    uint64_t mask = brick[bgi(bx,by,bz)];
    if(mask){
      if(fineDDA2(mask,bx,by,bz,o,d,sx,sy,sz,idx,idy,idz,bt,bface,(first&&bt==0),tmax,h)) return true;
      first=false;
      if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
      else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
      else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
      continue;
    }
    // empty brick: take the longest conservative jump available
    int kB = D.distB[bgi(bx,by,bz)];
    double skipVox = (kB>=2)? (double)(kB-1)*4.0-1.5 : 0.0;
    int kS = D.distS[D.sgi(bx>>2,by>>2,bz>>2)];
    if(kS>=2){ double s=(double)(kS-1)*16.0-1.5; if(s>skipVox) skipVox=s; }
    if(skipVox>0.0){
      double safe = skipVox*rml;
      double normalStep = std::min(btx,std::min(bty,btz)) - bt;
      if(safe > normalStep){
        bt += safe;
        if(bt>tmax) return false;
        double px=(double)o.x+(double)d.x*bt;
        double py=(double)o.y+(double)d.y*bt;
        double pz=(double)o.z+(double)d.z*bt;
        int nx=(int)std::floor(px), ny=(int)std::floor(py), nz=(int)std::floor(pz);
        // grid is convex: if the conservative jump landed outside, the ray
        // left the world through guaranteed-empty space => definitive miss.
        // (clamping instead re-enters the grid and can jump-loop forever)
        if(nx<0||ny<0||nz<0||nx>=NX||ny>=NY||nz>=NZ) return false;
        bx=nx>>2; by=ny>>2; bz=nz>>2;
        btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
        bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
        btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
        first=false;
        continue;
      }
    }
    if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
    else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
    else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
    first=false;
  }
  return false;
}

bool traceODF(const ODF& D, V3 o, V3 d, float tmax, Hit& h){
  tl_rays++;
  int x=(int)std::floor(o.x), y=(int)std::floor(o.y), z=(int)std::floor(o.z);
  if(!inB(x,y,z)) return false;
  int sx=d.x>0?1:-1, sy=d.y>0?1:-1, sz=d.z>0?1:-1;
  int oct=(d.x>0?1:0)|(d.y>0?2:0)|(d.z>0?4:0);
  const uint8_t* DIST = D.d[oct].data();
  double adx=(double)std::fabs(d.x)+1e-12, ady=(double)std::fabs(d.y)+1e-12, adz=(double)std::fabs(d.z)+1e-12;
  double idx=1.0/adx, idy=1.0/ady, idz=1.0/adz;
  double bidx=4.0*idx, bidy=4.0*idy, bidz=4.0*idz;
  double rml = 1.0/std::max(adx,std::max(ady,adz));
  int bx=x>>2, by=y>>2, bz=z>>2;
  double btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
  double bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
  double btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
  double bt=0; int bface=-1;
  bool first=true;
  for(int it=0; it<8192; it++){
    if(bt>tmax) return false;
    if(bx<0||bx>=BNX||by<0||by>=BNY||bz<0||bz>=BNZ) return false;
    tl_steps++;
    int k = DIST[bgi(bx,by,bz)];     // 0 <=> occupied: one load tests both
    if(k==0){
      uint64_t mask = brick[bgi(bx,by,bz)];
      if(mask){
        if(fineDDA2(mask,bx,by,bz,o,d,sx,sy,sz,idx,idy,idz,bt,bface,(first&&bt==0),tmax,h)) return true;
      }
      first=false;
      if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
      else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
      else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
      continue;
    }
    if(k>=2){
      double safe = ((double)(k-1)*4.0-1.5)*rml;
      double normalStep = std::min(btx,std::min(bty,btz)) - bt;
      if(safe > normalStep){
        bt += safe;
        if(bt>tmax) return false;
        double px=(double)o.x+(double)d.x*bt;
        double py=(double)o.y+(double)d.y*bt;
        double pz=(double)o.z+(double)d.z*bt;
        int nx=(int)std::floor(px), ny=(int)std::floor(py), nz=(int)std::floor(pz);
        if(nx<0||ny<0||nz<0||nx>=NX||ny>=NY||nz>=NZ) return false; // convex exit => miss
        bx=nx>>2; by=ny>>2; bz=nz>>2;
        btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
        bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
        btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
        first=false;
        continue;
      }
    }
    if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
    else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
    else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
    first=false;
  }
  return false;
}
