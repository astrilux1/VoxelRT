// Acceleration-structure traversal variants. All must return EXACTLY the same
// Hit (voxel+face+t within 1e-3) as trace()/traceNaive() — same double-precision
// discipline and entry-voxel snapping tricks as core.cpp's trace().
#include "accel.h"

// Shared fine in-brick DDA, identical in spirit to the inner loop of trace().
static inline bool fineDDA(uint64_t mask, int bx,int by,int bz,
                            V3 o, V3 d, int sx,int sy,int sz,
                            double idx,double idy,double idz,
                            double t0, int bface, bool first, bool firstAtZero,
                            float tmax, Hit& h){
  double eps=1e-7;
  double px=(double)o.x+(double)d.x*std::max(0.0,t0-eps);
  double py=(double)o.y+(double)d.y*std::max(0.0,t0-eps);
  double pz=(double)o.z+(double)d.z*std::max(0.0,t0-eps);
  int fx,fy,fz;
  if(first && t0==0){ fx=(int)std::floor((double)o.x); fy=(int)std::floor((double)o.y); fz=(int)std::floor((double)o.z); }
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
    if(fx<bx*4||fx>bx*4+3||fy<by*4||fy>by*4+3||fz<bz*4||fz>bz*4+3) break; // left brick
  }
  return false;
}

// ---------------- A2: 3-level DDA (16^3 superbrick over 4^3 bricks) -----------
bool traceSuper(const SuperGrid& S, V3 o, V3 d, float tmax, Hit& h){
  tl_rays++;
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
  for(int bit=0; bit<4096; bit++){
    if(bt>tmax) return false;
    if(bx<0||bx>=BNX||by<0||by>=BNY||bz<0||bz>=BNZ) return false;
    int sbx=bx>>2, sby=by>>2, sbz=bz>>2;
    tl_steps++; // superbrick-level node visit
    if(!S.any(sbx,sby,sbz)){
      // whole 16^3 superbrick is empty: skip it in one step. Advance bt to the
      // superbrick boundary on the driving axis (exact integer crossing), then
      // RE-DERIVE all three brick coords from the true ray position at the new
      // bt (the non-driving axes may have crossed brick-row boundaries during
      // the skip) -- recompute-from-floor-of-position, with a small backward
      // eps nudge so landing exactly on a boundary floors to the entered side.
      double stx=((sx>0? (sbx+1)*16-(double)o.x : (double)o.x-sbx*16))*idx;
      double sty=((sy>0? (sby+1)*16-(double)o.y : (double)o.y-sby*16))*idy;
      double stz=((sz>0? (sbz+1)*16-(double)o.z : (double)o.z-sbz*16))*idz;
      int dax;
      if(stx<=sty && stx<=stz){ bt=stx; dax=0; bface=sx>0?1:0; }
      else if(sty<=stz)       { bt=sty; dax=1; bface=sy>0?3:2; }
      else                    { bt=stz; dax=2; bface=sz>0?5:4; }
      double eps=1e-7;
      double tp=std::max(0.0,bt-eps);
      double px=(double)o.x+(double)d.x*tp;
      double py=(double)o.y+(double)d.y*tp;
      double pz=(double)o.z+(double)d.z*tp;
      int nbx=((int)std::floor(px))>>2, nby=((int)std::floor(py))>>2, nbz=((int)std::floor(pz))>>2;
      if(nbx<0)nbx=0; if(nbx>=BNX)nbx=BNX-1;
      if(nby<0)nby=0; if(nby>=BNY)nby=BNY-1;
      if(nbz<0)nbz=0; if(nbz>=BNZ)nbz=BNZ-1;
      // driving axis lands exactly on the superbrick boundary: use the exact
      // integer brick coordinate (avoids any eps-induced off-by-one there).
      if(dax==0)      nbx = (sx>0)? (sbx+1)*4 : sbx*4-1;
      else if(dax==1) nby = (sy>0)? (sby+1)*4 : sby*4-1;
      else            nbz = (sz>0)? (sbz+1)*4 : sbz*4-1;
      bx=nbx; by=nby; bz=nbz;
      // recompute the brick-boundary trackers for the new bx/by/bz so the
      // following normal brick-stepping stays consistent (same formula as init).
      btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
      bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
      btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
      first=false;
      continue;
    }
    if(S.bit(bx,by,bz)){
      uint64_t mask = brick[bgi(bx,by,bz)];
      if(mask){
        tl_steps++; // brick-level node visit (occupied)
        if(fineDDA(mask,bx,by,bz,o,d,sx,sy,sz,idx,idy,idz,bt,bface,first,(first&&bt==0),tmax,h)) return true;
        first=false;
      } else {
        tl_steps++; // brick-level node visit (empty but in occupied superbrick)
      }
    }
    // brick step
    if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
    else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
    else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
    first=false;
  }
  return false;
}

// ---------------- A3: brick DDA + conservative-stale distance jump ------------
bool traceCSDF(const CSDF& D, V3 o, V3 d, float tmax, Hit& h){
  tl_rays++;
  int x=(int)std::floor(o.x), y=(int)std::floor(o.y), z=(int)std::floor(o.z);
  if(!inB(x,y,z)) return false;
  int sx=d.x>0?1:-1, sy=d.y>0?1:-1, sz=d.z>0?1:-1;
  double adx=(double)std::fabs(d.x)+1e-12, ady=(double)std::fabs(d.y)+1e-12, adz=(double)std::fabs(d.z)+1e-12;
  double idx=1.0/adx, idy=1.0/ady, idz=1.0/adz;
  double bidx=4.0*idx, bidy=4.0*idy, bidz=4.0*idz;
  double maxAbsComp = std::max(adx,std::max(ady,adz));
  int bx=x>>2, by=y>>2, bz=z>>2;
  double btx=((sx>0? (bx+1)*4-(double)o.x : (double)o.x-bx*4))*idx;
  double bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
  double btz=((sz>0? (bz+1)*4-(double)o.z : (double)o.z-bz*4))*idz;
  double bt=0; int bface=-1;
  bool first=true;
  for(int bit=0; bit<4096; bit++){
    if(bt>tmax) return false;
    if(bx<0||bx>=BNX||by<0||by>=BNY||bz<0||bz>=BNZ) return false;
    tl_steps++;
    uint64_t mask = brick[bgi(bx,by,bz)];
    if(mask){
      if(fineDDA(mask,bx,by,bz,o,d,sx,sy,sz,idx,idy,idz,bt,bface,first,(first&&bt==0),tmax,h)) return true;
      first=false;
      if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
      else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
      else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
      continue;
    }
    int k = D.dist[bgi(bx,by,bz)];
    double normalStep = std::min(btx, std::min(bty,btz)) - bt;
    if(k>=2){
      double safe = ((double)(k-1)*4.0 - 1.5) / maxAbsComp;
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

// ---------------- A4: brick DDA + per-(bx,bz) column y-skip --------------------
bool traceBrickCol(const ColMap& C, V3 o, V3 d, float tmax, Hit& h){
  tl_rays++;
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
  bool suppressed=false;
  for(int bit=0; bit<4096; bit++){
    if(bt>tmax) return false;
    if(bx<0||bx>=BNX||by<0||by>=BNY||bz<0||bz>=BNZ) return false;
    tl_steps++;
    if(suppressed){
      double py=(double)o.y+(double)d.y*bt;
      int ny=(int)std::floor(py);
      int nby=ny>>2;
      if(nby<0) nby=0; if(nby>=BNY) nby=BNY-1;
      by=nby;
      bty=((sy>0? (by+1)*4-(double)o.y : (double)o.y-by*4))*idy;
      suppressed=false;
    }
    uint64_t mask = brick[bgi(bx,by,bz)];
    if(mask){
      if(fineDDA(mask,bx,by,bz,o,d,sx,sy,sz,idx,idy,idz,bt,bface,first,(first&&bt==0),tmax,h)) return true;
      first=false;
      if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
      else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
      else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
      continue;
    }
    bool suppressY = (sy>0) && (by > C.ymaxB[(size_t)bz*BNX+bx]);
    if(suppressY){
      if(btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
      else        { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
      suppressed=true;
    } else {
      if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
      else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
      else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
    }
    first=false;
  }
  return false;
}
