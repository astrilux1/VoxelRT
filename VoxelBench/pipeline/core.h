// VoxelBench core: math, RNG, voxel grid + brickmap, traversal, sky/sun, NEE,
// camera/G-buffer, shared direct pass, path-traced reference.
// All units in VOXELS (1 voxel = 0.0625 m). CPU, OpenMP-parallel, deterministic
// (stateless per-(id,frame) RNG so thread schedule doesn't change results).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>
#include <chrono>

// ----------------------------- math -----------------------------------------
struct V3 { float x=0,y=0,z=0;
  V3(){} V3(float a,float b,float c):x(a),y(b),z(c){}
  V3 operator+(V3 o)const{return{x+o.x,y+o.y,z+o.z};}
  V3 operator-(V3 o)const{return{x-o.x,y-o.y,z-o.z};}
  V3 operator*(float s)const{return{x*s,y*s,z*s};}
  V3 operator*(V3 o)const{return{x*o.x,y*o.y,z*o.z};}
  V3& operator+=(V3 o){x+=o.x;y+=o.y;z+=o.z;return *this;}
};
static inline float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static inline float len(V3 a){return std::sqrt(dot(a,a));}
static inline V3 norm(V3 a){float l=len(a);return a*(1.f/(l+1e-20f));}
static inline V3 cross(V3 a,V3 b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static inline V3 vmax(V3 a,V3 b){return{std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)};}
static inline float luma(V3 c){return 0.2126f*c.x+0.7152f*c.y+0.0722f*c.z;}

// stateless hash RNG (deterministic under threading): stream from (id, frame, salt)
struct RNG{
  uint64_t s;
  explicit RNG(uint64_t a=1,uint64_t b=0,uint64_t c=0){
    s = a*0x9E3779B97F4A7C15ULL ^ (b+0xBF58476D1CE4E5B9ULL)*0x94D049BB133111EBULL ^ c*0xD6E8FEB86659FD93ULL;
    next(); next();
  }
  uint64_t next(){ s += 0x9E3779B97F4A7C15ULL; uint64_t z=s;
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ULL; z=(z^(z>>27))*0x94D049BB133111EBULL; return z^(z>>31); }
  uint32_t u32(){ return (uint32_t)(next()>>32); }
  float uf(){ return (u32()>>8)*(1.f/16777216.f); }
};

// ----------------------------- grid + materials -----------------------------
// Materials are per-scene palettes. id 0 = air.
static const int MAXMAT=16;
struct Material { V3 alb; V3 emi; };
extern Material MAT[MAXMAT];
static inline bool emissive(uint8_t m){ return MAT[m].emi.x+MAT[m].emi.y+MAT[m].emi.z > 0.f; }

// grid dims are RUNTIME (scenes range from 96^3 unit tests to 320x96x320 town);
// must be multiples of brick size 4. Call setDims() before building a scene.
extern int NX, NY, NZ;
static const float VOX_M=0.0625f;             // meters per voxel (informational)
extern std::vector<uint8_t> grid;
static inline int gi(int x,int y,int z){return (z*NY+y)*NX+x;}
static inline bool inB(int x,int y,int z){return x>=0&&x<NX&&y>=0&&y<NY&&z>=0&&z<NZ;}
static inline uint8_t at(int x,int y,int z){return inB(x,y,z)?grid[gi(x,y,z)]:0;}

// brickmap: 4^3 bricks with 64-bit occupancy masks (the editable-world
// acceleration structure used by modern voxel engines; O(1) updates)
static const int BS=4;
extern int BNX, BNY, BNZ;
extern std::vector<uint64_t> brick;
void setDims(int nx,int ny,int nz);           // allocates + zeroes grid/bricks
static inline int bgi(int bx,int by,int bz){return (bz*BNY+by)*BNX+bx;}
static inline int bbit(int lx,int ly,int lz){return (lz*4+ly)*4+lx;}
static inline void rebuildBrick(int bx,int by,int bz){
  uint64_t m=0;
  for(int lz=0;lz<4;lz++)for(int ly=0;ly<4;ly++)for(int lx=0;lx<4;lx++)
    if(grid[gi(bx*4+lx,by*4+ly,bz*4+lz)]) m |= 1ULL<<bbit(lx,ly,lz);
  brick[bgi(bx,by,bz)]=m;
}
static inline void rebuildAllBricks(){
  for(int bz=0;bz<BNZ;bz++)for(int by=0;by<BNY;by++)for(int bx=0;bx<BNX;bx++) rebuildBrick(bx,by,bz);
}
static inline void setVox(int x,int y,int z,uint8_t m){
  if(!inB(x,y,z))return; grid[gi(x,y,z)]=m;
  int b=bgi(x/4,y/4,z/4);
  if(m) brick[b] |= 1ULL<<bbit(x&3,y&3,z&3); else brick[b] &= ~(1ULL<<bbit(x&3,y&3,z&3));
}

// ----------------------------- sun + sky ------------------------------------
// Sun: delta directional light. sunDir points TOWARD the sun. sunE = irradiance
// on a perpendicular surface. Sky: simple two-color gradient environment.
struct SkyLight {
  bool enabled=false;
  V3 sunDir{0,1,0}; V3 sunE{0,0,0};       // irradiance perpendicular to sun
  V3 zenith{0,0,0}, horizon{0,0,0};       // sky radiance gradient
  V3 skyL(V3 d) const {
    if(!enabled) return {};
    float t = std::max(0.f, d.y);
    t = std::sqrt(t); // brighten horizon band
    return horizon*(1-t) + zenith*t;
  }
};
extern SkyLight SKY;

// ----------------------------- traversal ------------------------------------
// cost counters: thread-local, flushed to atomics at end of parallel sections
extern std::atomic<uint64_t> g_rays, g_steps;
extern thread_local uint64_t tl_rays, tl_steps;
static inline void flushCounters(){ g_rays += tl_rays; g_steps += tl_steps; tl_rays=0; tl_steps=0; }

struct Hit{ int vx,vy,vz; int face; float t; uint8_t m; }; // face 0..5 = +x,-x,+y,-y,+z,-z
static V3 FN[6]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
static int OXf[6]={1,-1,0,0,0,0},OYf[6]={0,0,1,-1,0,0},OZf[6]={0,0,0,0,1,-1};

// Reference naive DDA (used for validation only)
bool traceNaive(V3 o, V3 d, float tmax, Hit& h);
// Production traversal: brick-level DDA skipping empty 4^3 bricks, fine DDA inside.
// Identical results to traceNaive (validated by harness "test" mode).
bool trace(V3 o, V3 d, float tmax, Hit& h);

// ----------------------------- emitters + NEE -------------------------------
struct EmFace{ int x,y,z,f; uint8_t m; };
extern std::vector<EmFace> emFaces;
void collectEmitters();
// one-sample estimate of DIRECT irradiance at (p,n): emissive-face NEE (1 shadow
// ray) + sun NEE (1 shadow ray, deterministic direction). Costs <=2 rays.
V3 neeIrr(V3 p, V3 n, RNG& rng);

// cosine hemisphere sample
static inline V3 cosDir(V3 n, RNG& rng){
  float u1=rng.uf(), u2=rng.uf();
  float r=std::sqrt(u1), phi=6.2831853f*u2;
  float lx=r*std::cos(phi), ly=r*std::sin(phi), lz=std::sqrt(std::max(0.f,1-u1));
  V3 tu = (std::fabs(n.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
  V3 tv = norm(cross(n,tu)); tu = cross(tv,n);
  return norm(tu*lx + tv*ly + n*lz);
}
static inline V3 facePoint(int vx,int vy,int vz,int f){
  return V3((float)vx,(float)vy,(float)vz)+V3(0.5f,0.5f,0.5f)+FN[f]*0.501f;
}

// ----------------------------- camera / G-buffer ----------------------------
static const int W=240, H=135;
struct GPix{ bool hit=false; V3 pos,n; V3 alb; uint8_t m=0; int vx,vy,vz,face; float depth; };
extern std::vector<GPix> gbuf;
extern V3 camPos, camFwd, camRt, camUp; extern float camFovDeg;
void setupCam(V3 pos, V3 lookAt, float fovDeg);
V3 pixDir(float px,float py);
void buildGBuffer();

// shared converged direct pass: L_direct per pixel (identical for all methods,
// excluded from all budgets). Sky pixels get sky radiance here.
extern std::vector<V3> directImg;
void buildDirect(int spp);

// path-traced reference of INDIRECT light: per-pixel L_ind = alb/pi * E_ind.
// Sky/environment light counts as indirect (it arrives via the hemisphere, not NEE).
V3 pathE(V3 p, V3 n, RNG& rng, int depth);     // one-sample irradiance estimate
void renderReference(std::vector<V3>& out, int spp, uint64_t seed);

// ----------------------------- image io -------------------------------------
static inline float tonemap(float c){ c=std::max(0.f,c); c=c/(1.f+c); return std::pow(c,1.f/2.2f); }
void savePPM(const std::string& path, const std::vector<V3>& indirect); // direct+indirect, tonemapped
void saveRawImg(const std::string& path, const std::vector<V3>& v, double cnt);
bool loadRawImg(const std::string& path, std::vector<V3>& v, double& cnt);
