// Acceleration-structure research track.
// All structures answer the same query as trace(): first-hit voxel + face.
// Compared on: ns/ray, steps/ray, bytes-touched/ray (GPU traffic proxy),
// memory footprint, and EDIT COST (time to restore validity after a blast).
//
// Structures:
//  A0 naive      — per-voxel DDA (baseline)
//  A1 brick      — 4^3 brickmap, 64-bit masks (production baseline; = trace())
//  A2 super      — 3-level: 16^3 superbrick bit over 4^3 bricks + A1 (sparse-64-tree style)
//  A3 csdf       — NOVEL: brick-level conservative-stale Chebyshev distance field.
//                  dist[brick] in 0..DMAX, distance (in bricks) to nearest occupied
//                  brick, CAPPED at DMAX. Ray skips (dist-1) bricks in one step.
//                  Edits: placing solids clamps dist in a DMAX-radius window (bounded,
//                  local); removals leave dist stale-but-CONSERVATIVE (skips less than
//                  optimal, never wrong). A lazy relax pass re-sharpens a few thousand
//                  bricks/frame. The cap is what makes "stale" safe: any stored d<=DMAX
//                  can never overestimate the distance to solids placed outside the
//                  clamped window (they are > DMAX away by construction).
//  A4 brickcol   — A1 + per-(x,z) column occupied-y interval: rays that leave the
//                  global y-interval of their column path exit early (sky/sun rays).
#pragma once
#include "core.h"

// ---------------- A2: superbrick (16^3) bit over bricks ----------------------
struct SuperGrid {
  int SNX=0,SNY=0,SNZ=0;
  std::vector<uint64_t> sup;   // bit per 4^3 brick within 16^3 superbrick
  void build(){
    SNX=(BNX+3)/4; SNY=(BNY+3)/4; SNZ=(BNZ+3)/4;
    sup.assign((size_t)SNX*SNY*SNZ,0);
    for(int bz=0;bz<BNZ;bz++)for(int by=0;by<BNY;by++)for(int bx=0;bx<BNX;bx++)
      if(brick[bgi(bx,by,bz)])
        sup[((bz/4)*SNY+(by/4))*SNX+(bx/4)] |= 1ULL<<bbit(bx&3,by&3,bz&3);
  }
  inline bool any(int sx,int sy,int sz) const { return sup[((size_t)sz*SNY+sy)*SNX+sx]!=0; }
  inline bool bit(int bx,int by,int bz) const {
    return (sup[((size_t)(bz/4)*SNY+(by/4))*SNX+(bx/4)]>>bbit(bx&3,by&3,bz&3))&1ULL; }
  void onVoxelSet(int x,int y,int z,bool solid){
    int bx=x>>2,by=y>>2,bz=z>>2;
    if(solid) sup[((size_t)(bz/4)*SNY+(by/4))*SNX+(bx/4)] |= 1ULL<<bbit(bx&3,by&3,bz&3);
    else if(!brick[bgi(bx,by,bz)]) sup[((size_t)(bz/4)*SNY+(by/4))*SNX+(bx/4)] &= ~(1ULL<<bbit(bx&3,by&3,bz&3));
  }
  size_t bytes() const { return sup.size()*8; }
};

// ---------------- A3: conservative-stale brick distance field ----------------
struct CSDF {
  static const int DMAX=7;     // cap (bricks). placement clamp window = DMAX^3 bricks max
  std::vector<uint8_t> dist;   // per brick
  size_t relaxCursor=0;
  void build(){                // full BFS-ish relax (init only; edits never do this)
    dist.assign((size_t)BNX*BNY*BNZ, DMAX);
    for(int bz=0;bz<BNZ;bz++)for(int by=0;by<BNY;by++)for(int bx=0;bx<BNX;bx++)
      if(brick[bgi(bx,by,bz)]) dist[bgi(bx,by,bz)]=0;
    // chamfer sweeps (Chebyshev): forward+backward
    auto relax=[&](int x,int y,int z){
      uint8_t d=dist[bgi(x,y,z)]; if(!d) return;
      uint8_t best=d;
      for(int dz=-1;dz<=1;dz++)for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        int nx=x+dx,ny=y+dy,nz=z+dz;
        if(nx<0||ny<0||nz<0||nx>=BNX||ny>=BNY||nz>=BNZ) continue;
        uint8_t cand=(uint8_t)std::min<int>(DMAX,dist[bgi(nx,ny,nz)]+1);
        if(cand<best) best=cand;
      }
      dist[bgi(x,y,z)]=best;
    };
    for(int z=0;z<BNZ;z++)for(int y=0;y<BNY;y++)for(int x=0;x<BNX;x++) relax(x,y,z);
    for(int z=BNZ-1;z>=0;z--)for(int y=BNY-1;y>=0;y--)for(int x=BNX-1;x>=0;x--) relax(x,y,z);
  }
  // O(1)-bounded edit: solids ADDED inside a sphere(c,r in voxels). Clamp the window.
  void onPlace(V3 cVox, float rVox){
    int br=(int)std::ceil(rVox/4.f);
    int cx=(int)cVox.x>>2, cy=(int)cVox.y>>2, cz=(int)cVox.z>>2;
    int R=br+DMAX;
    for(int z=std::max(0,cz-R); z<=std::min(BNZ-1,cz+R); z++)
    for(int y=std::max(0,cy-R); y<=std::min(BNY-1,cy+R); y++)
    for(int x=std::max(0,cx-R); x<=std::min(BNX-1,cx+R); x++){
      if(brick[bgi(x,y,z)]){ dist[bgi(x,y,z)]=0; continue; }
      int cheb=std::max({std::abs(x-cx),std::abs(y-cy),std::abs(z-cz)});
      int d=std::max(0,cheb-br);
      if(d<dist[bgi(x,y,z)]) dist[bgi(x,y,z)]=(uint8_t)std::min(d,(int)DMAX);
    }
  }
  // removals: stale-but-safe; call relaxBudget() amortized to re-sharpen
  void relaxBudget(int n){
    size_t total=dist.size();
    for(int i=0;i<n;i++){
      size_t b=relaxCursor++ % total;
      int x=b%BNX, y=(b/BNX)%BNY, z=b/((size_t)BNX*BNY);
      if(brick[b]){ dist[b]=0; continue; }
      int best=DMAX;
      for(int dz=-1;dz<=1;dz++)for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        int nx=x+dx,ny=y+dy,nz=z+dz;
        if(nx<0||ny<0||nz<0||nx>=BNX||ny>=BNY||nz>=BNZ) continue;
        int cand=std::min<int>(DMAX,dist[bgi(nx,ny,nz)]+1);
        if(cand<best) best=cand;
      }
      dist[b]=(uint8_t)best;
    }
  }
  size_t bytes() const { return dist.size(); }
};

// ---------------- A4: column occupancy intervals ------------------------------
struct ColMap {
  std::vector<uint8_t> ymin, ymax;  // per (x,z) column, voxel units (0..NY-1); 255,0 = empty
  std::vector<uint8_t> ymaxB;       // per (bx,bz) brick-column, brick units (conservative +1)
  void build(){
    ymin.assign((size_t)NX*NZ,255); ymax.assign((size_t)NX*NZ,0);
    for(int z=0;z<NZ;z++)for(int x=0;x<NX;x++){
      uint8_t lo=255, hi=0;
      for(int y=0;y<NY;y++) if(grid[gi(x,y,z)]){ if(y<lo)lo=(uint8_t)y; if(y>hi)hi=(uint8_t)y; }
      ymin[(size_t)z*NX+x]=lo; ymax[(size_t)z*NX+x]=hi;
    }
  }
  // per-(bx,bz) brick-column conservative max occupied brick index (brick units),
  // = max over the 16 (x,z) voxel columns of (ymax>>2), +1 margin, clamped to BNY-1.
  void buildBrickCols(){
    ymaxB.assign((size_t)BNX*BNZ,0);
    for(int bz=0;bz<BNZ;bz++)for(int bx=0;bx<BNX;bx++){
      int mx=0;
      for(int lz=0;lz<4;lz++)for(int lx=0;lx<4;lx++){
        int x=bx*4+lx, z=bz*4+lz;
        int ymb = ymax[(size_t)z*NX+x] >> 2;
        if(ymb>mx) mx=ymb;
      }
      mx += 1; // conservative margin
      if(mx>BNY-1) mx=BNY-1;
      ymaxB[(size_t)bz*BNX+bx]=(uint8_t)mx;
    }
  }
  void onVoxelSet(int x,int y,int z,bool solid){
    size_t c=(size_t)z*NX+x;
    if(solid){ if(y<ymin[c])ymin[c]=(uint8_t)y; if(y>ymax[c])ymax[c]=(uint8_t)y; }
    // removal: stale-but-safe (interval only ever too wide)
  }
  size_t bytes() const { return ymin.size()*2 + ymaxB.size(); }
};

// traversal variants (defined in accel.cpp)
bool traceSuper(const SuperGrid& S, V3 o, V3 d, float tmax, Hit& h);
bool traceCSDF(const CSDF& D, V3 o, V3 d, float tmax, Hit& h);
bool traceBrickCol(const ColMap& C, V3 o, V3 d, float tmax, Hit& h);
