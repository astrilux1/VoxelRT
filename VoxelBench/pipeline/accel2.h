// Acceleration research, round 2 (24m render distance track).
// HX = brick masks + TWO conservative-stale Chebyshev distance fields:
//        distB: per 4^3 brick   (cap DMAXB) -> skip up to (k-1)*4 - 1.5 voxels
//        distS: per 16^3 super  (cap DMAXS) -> skip up to (k-1)*16 - 1.5 voxels
// Same jump mechanics as CSDF (conservative mid-air landing, re-derive brick
// coords from ray position, finish with exact brick DDA steps) — that recipe
// measured 0 mismatches on every scene/workload. The per-jump division of
// CSDF is hoisted out of the loop (precomputed 1/maxAbsComp): that division
// is why CSDF's low step count did not translate into low ns/ray.
// Edits: placements clamp both fields in a bounded window (O(1) wrt world
// size); removals leave fields stale-but-conservative (skips less than
// optimal, never wrong); relaxBudget() re-sharpens amortized.
#pragma once
#include "core.h"

struct HX {
  static const int DMAXB=14;   // brick-level cap (bricks)
  static const int DMAXS=12;   // super-level cap (supers; 1 super = 16 voxels)
  int SBX=0,SBY=0,SBZ=0;       // super grid dims (= ceil(BN*/4))
  std::vector<uint8_t> distB;  // per brick: 0 = occupied
  std::vector<uint8_t> distS;  // per super: 0 = some brick occupied
  size_t curB=0, curS=0;       // relax cursors

  inline size_t sgi(int sx,int sy,int sz) const { return ((size_t)sz*SBY+sy)*SBX+sx; }

  void build(){
    SBX=(BNX+3)/4; SBY=(BNY+3)/4; SBZ=(BNZ+3)/4;
    distB.assign((size_t)BNX*BNY*BNZ, DMAXB);
    distS.assign((size_t)SBX*SBY*SBZ, DMAXS);
    for(int bz=0;bz<BNZ;bz++)for(int by=0;by<BNY;by++)for(int bx=0;bx<BNX;bx++)
      if(brick[bgi(bx,by,bz)]){ distB[bgi(bx,by,bz)]=0; distS[sgi(bx>>2,by>>2,bz>>2)]=0; }
    chamfer(distB,BNX,BNY,BNZ,DMAXB);
    chamfer(distS,SBX,SBY,SBZ,DMAXS);
  }

  static void chamfer(std::vector<uint8_t>& D,int nx,int ny,int nz,int cap){
    auto idx=[&](int x,int y,int z){ return ((size_t)z*ny+y)*nx+x; };
    // two-pass Chebyshev chamfer (forward+backward), neighbor min+1
    for(int z=0;z<nz;z++)for(int y=0;y<ny;y++)for(int x=0;x<nx;x++){
      uint8_t d=D[idx(x,y,z)]; if(!d) continue;
      int best=d;
      for(int dz=-1;dz<=0;dz++)for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        if(dz==0&&(dy>0||(dy==0&&dx>=0))) continue;
        int X=x+dx,Y=y+dy,Z=z+dz;
        if(X<0||Y<0||Z<0||X>=nx||Y>=ny||Z>=nz) continue;
        int c=D[idx(X,Y,Z)]+1; if(c<best) best=c;
      }
      D[idx(x,y,z)]=(uint8_t)std::min(best,cap);
    }
    for(int z=nz-1;z>=0;z--)for(int y=ny-1;y>=0;y--)for(int x=nx-1;x>=0;x--){
      uint8_t d=D[idx(x,y,z)]; if(!d) continue;
      int best=d;
      for(int dz=0;dz<=1;dz++)for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        if(dz==0&&(dy<0||(dy==0&&dx<=0))) continue;
        int X=x+dx,Y=y+dy,Z=z+dz;
        if(X<0||Y<0||Z<0||X>=nx||Y>=ny||Z>=nz) continue;
        int c=D[idx(X,Y,Z)]+1; if(c<best) best=c;
      }
      D[idx(x,y,z)]=(uint8_t)std::min(best,cap);
    }
  }

  // O(1)-bounded edit: solids ADDED inside sphere(cVox, rVox voxels).
  void onPlace(V3 cVox, float rVox){
    { int br=(int)std::ceil(rVox/4.f);
      int cx=(int)cVox.x>>2, cy=(int)cVox.y>>2, cz=(int)cVox.z>>2;
      int R=br+DMAXB;
      for(int z=std::max(0,cz-R); z<=std::min(BNZ-1,cz+R); z++)
      for(int y=std::max(0,cy-R); y<=std::min(BNY-1,cy+R); y++)
      for(int x=std::max(0,cx-R); x<=std::min(BNX-1,cx+R); x++){
        if(brick[bgi(x,y,z)]){ distB[bgi(x,y,z)]=0; continue; }
        int cheb=std::max({std::abs(x-cx),std::abs(y-cy),std::abs(z-cz)});
        int d=std::max(0,cheb-br);
        if(d<distB[bgi(x,y,z)]) distB[bgi(x,y,z)]=(uint8_t)std::min(d,DMAXB);
      } }
    { int sr=(int)std::ceil(rVox/16.f);
      int cx=(int)cVox.x>>4, cy=(int)cVox.y>>4, cz=(int)cVox.z>>4;
      int R=sr+DMAXS;
      for(int z=std::max(0,cz-R); z<=std::min(SBZ-1,cz+R); z++)
      for(int y=std::max(0,cy-R); y<=std::min(SBY-1,cy+R); y++)
      for(int x=std::max(0,cx-R); x<=std::min(SBX-1,cx+R); x++){
        int cheb=std::max({std::abs(x-cx),std::abs(y-cy),std::abs(z-cz)});
        int d=std::max(0,cheb-sr);
        if(d<distS[sgi(x,y,z)]) distS[sgi(x,y,z)]=(uint8_t)std::min(d,DMAXS);
      } }
  }

  // amortized re-sharpen after removals (stale values only ever UNDER-skip)
  void relaxBudget(int nB,int nS){
    for(int i=0;i<nB;i++){
      size_t b=curB++ % distB.size();
      int x=b%BNX, y=(b/BNX)%BNY, z=(int)(b/((size_t)BNX*BNY));
      if(brick[b]){ distB[b]=0; continue; }
      int best=DMAXB;
      for(int dz=-1;dz<=1;dz++)for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        int X=x+dx,Y=y+dy,Z=z+dz;
        if(X<0||Y<0||Z<0||X>=BNX||Y>=BNY||Z>=BNZ) continue;
        int c=std::min<int>(DMAXB,distB[bgi(X,Y,Z)]+1);
        if(c<best) best=c;
      }
      distB[b]=(uint8_t)best;
    }
    for(int i=0;i<nS;i++){
      size_t s=curS++ % distS.size();
      int x=s%SBX, y=(s/SBX)%SBY, z=(int)(s/((size_t)SBX*SBY));
      // occupied if any of 4^3 bricks occupied: distB==0 check is O(64) bounded
      bool occ=false;
      for(int dz=0;dz<4&&!occ;dz++)for(int dy=0;dy<4&&!occ;dy++)for(int dx=0;dx<4&&!occ;dx++){
        int bx=x*4+dx,by=y*4+dy,bz=z*4+dz;
        if(bx<BNX&&by<BNY&&bz<BNZ&&brick[bgi(bx,by,bz)]) occ=true;
      }
      if(occ){ distS[s]=0; continue; }
      int best=DMAXS;
      for(int dz=-1;dz<=1;dz++)for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        int X=x+dx,Y=y+dy,Z=z+dz;
        if(X<0||Y<0||Z<0||X>=SBX||Y>=SBY||Z>=SBZ) continue;
        int c=std::min<int>(DMAXS,distS[sgi(X,Y,Z)]+1);
        if(c<best) best=c;
      }
      distS[s]=(uint8_t)best;
    }
  }
  size_t bytes() const { return distB.size()+distS.size(); }
};

bool traceHX(const HX& D, V3 o, V3 d, float tmax, Hit& h);

// ODF — per-OCTANT brick distance field (directional empty-space skipping).
// Isotropic distance dies on grazing rays (ground 2 bricks below caps every
// skip at ~2 bricks even though nothing lies AHEAD). Per-octant Chebyshev
// distance counts only solids the ray can actually reach (all axis deltas
// signed like the ray direction), so a ray skimming the ground or a street
// wall still takes long jumps. Storage: 8 bytes per brick, contiguous =>
// the hot loop does ONE 8-byte load and selects the byte by ray octant.
// Build: one single-pass sweep per octant (forward neighbors only).
// Edits: onPlace clamps the (r+DMAXO) window per octant (bounded, O(1) wrt
// world size); removals stale-but-conservative; relaxBudget re-sharpens.
struct ODF {
  static const int DMAXO=28;
  // SoA: one array per octant. A ray uses exactly one octant => working set
  // per ray is nb bytes (not 8*nb), and consecutive bricks share cachelines.
  // d[oct][b]==0 <=> brick occupied: traversal tests occupancy from the SAME
  // load that yields the skip distance (no brick-mask load on empty steps).
  std::vector<uint8_t> d[8];
  size_t cur=0;
  void build(){
    size_t nb=(size_t)BNX*BNY*BNZ;
    for(int oct=0; oct<8; oct++){ d[oct].assign(nb, DMAXO); sweep(oct); }
  }
  // octant oct: solids at deltas with sign(dx)=ox?+:-, 0 allowed on all axes.
  void sweep(int oct){
    int ox=(oct&1)?1:-1, oy=(oct&2)?1:-1, oz=(oct&4)?1:-1;
    // process opposite to octant direction so forward neighbors are done first
    int x0=ox>0?BNX-1:0, x1=ox>0?-1:BNX, xs=ox>0?-1:1;
    int y0=oy>0?BNY-1:0, y1=oy>0?-1:BNY, ys=oy>0?-1:1;
    int z0=oz>0?BNZ-1:0, z1=oz>0?-1:BNZ, zs=oz>0?-1:1;
    std::vector<uint8_t>& D=d[oct];
    for(int z=z0;z!=z1;z+=zs)for(int y=y0;y!=y1;y+=ys)for(int x=x0;x!=x1;x+=xs){
      size_t b=bgi(x,y,z);
      if(brick[b]){ D[b]=0; continue; }
      int best=DMAXO;
      for(int n=1;n<8;n++){           // 7 forward neighbors (offset bits)
        int X=x+((n&1)?ox:0), Y=y+((n&2)?oy:0), Z=z+((n&4)?oz:0);
        if(X<0||Y<0||Z<0||X>=BNX||Y>=BNY||Z>=BNZ) continue;
        int c=D[bgi(X,Y,Z)]+1; if(c<best) best=c;
      }
      D[b]=(uint8_t)best;
    }
  }
  void onPlace(V3 cVox, float rVox){
    int br=(int)std::ceil(rVox/4.f);
    int cx=(int)cVox.x>>2, cy=(int)cVox.y>>2, cz=(int)cVox.z>>2;
    int R=br+DMAXO;
    for(int z=std::max(0,cz-R); z<=std::min(BNZ-1,cz+R); z++)
    for(int y=std::max(0,cy-R); y<=std::min(BNY-1,cy+R); y++)
    for(int x=std::max(0,cx-R); x<=std::min(BNX-1,cx+R); x++){
      size_t b=bgi(x,y,z);
      if(brick[b]){ for(int o=0;o<8;o++) d[o][b]=0; continue; }
      // new solids fill sphere(c,br): for each octant, distance to the part
      // of the solid ball lying in that octant of (x,y,z). Conservative: use
      // the Chebyshev distance to the ball, but only clamp octants whose cone
      // contains some part of the ball (sign test per axis with br slack).
      int dxc=cx-x, dyc=cy-y, dzc=cz-z;
      int cheb=std::max({std::abs(dxc),std::abs(dyc),std::abs(dzc)});
      int dmin=std::max(0,cheb-br);
      for(int o=0;o<8;o++){
        int ox=(o&1)?1:-1, oy=(o&2)?1:-1, oz=(o&4)?1:-1;
        if(ox*dxc < -br || oy*dyc < -br || oz*dzc < -br) continue; // ball outside cone
        if(dmin < d[o][b]) d[o][b]=(uint8_t)std::min(dmin,DMAXO);
      }
    }
  }
  void relaxBudget(int n){
    size_t nb=(size_t)BNX*BNY*BNZ;
    for(int i=0;i<n;i++){
      size_t b=cur++ % nb;
      int x=b%BNX, y=(b/BNX)%BNY, z=(int)(b/((size_t)BNX*BNY));
      if(brick[b]){ for(int o=0;o<8;o++) d[o][b]=0; continue; }
      for(int o=0;o<8;o++){
        int ox=(o&1)?1:-1, oy=(o&2)?1:-1, oz=(o&4)?1:-1;
        int best=DMAXO;
        for(int nn=1;nn<8;nn++){
          int X=x+((nn&1)?ox:0), Y=y+((nn&2)?oy:0), Z=z+((nn&4)?oz:0);
          if(X<0||Y<0||Z<0||X>=BNX||Y>=BNY||Z>=BNZ) continue;
          int c=std::min<int>(DMAXO,d[o][bgi(X,Y,Z)]+1); if(c<best) best=c;
        }
        d[o][b]=(uint8_t)best;
      }
    }
  }
  size_t bytes() const { size_t t=0; for(int o=0;o<8;o++) t+=d[o].size(); return t; }
};
bool traceODF(const ODF& D, V3 o, V3 d, float tmax, Hit& h);
