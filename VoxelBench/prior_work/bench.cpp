// Voxel GI benchmark: PT+temporal vs DDGI-style probes vs FaceCache-GI (proposed)
// All methods estimate INDIRECT irradiance; a shared converged direct-light pass is added.
// Single-file CPU implementation; identical DDA traversal for all methods.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>

// ----------------------------- basic math -----------------------------------
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
static inline V3 norm(V3 a){float l=len(a);return a*(1.f/l);}
static inline V3 cross(V3 a,V3 b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}

// PCG32
struct RNG{ uint64_t state, inc;
  RNG(uint64_t s=1,uint64_t q=1){state=0;inc=(q<<1)|1;next();state+=s;next();}
  uint32_t next(){uint64_t o=state;state=o*6364136223846793005ULL+inc;
    uint32_t xs=(uint32_t)(((o>>18u)^o)>>27u);uint32_t r=o>>59u;return (xs>>r)|(xs<<((-r)&31));}
  float uf(){return (next()>>8)*(1.f/16777216.f);}
};

// ----------------------------- scene ----------------------------------------
static const int NX=96, NY=64, NZ=96;
static const float VOX=0.0625f; // meters per voxel (informational; units below are voxels)
static uint8_t grid[NX*NY*NZ];
static inline int gi(int x,int y,int z){return (z*NY+y)*NX+x;}
static inline bool inB(int x,int y,int z){return x>=0&&x<NX&&y>=0&&y<NY&&z>=0&&z<NZ;}
static inline uint8_t at(int x,int y,int z){return inB(x,y,z)?grid[gi(x,y,z)]:0;}

// materials: 0 air, 1 white, 2 red, 3 green, 4 LIGHT, 5 dark gray, 6 blue
static V3 ALB[7] = {{0,0,0},{0.73f,0.73f,0.73f},{0.70f,0.13f,0.13f},{0.15f,0.55f,0.15f},
                    {0.78f,0.78f,0.78f},{0.35f,0.35f,0.38f},{0.2f,0.3f,0.65f}};
static V3 EMI[7] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0},{26.f,24.f,20.f},{0,0,0},{0,0,0}};
static inline bool emissive(uint8_t m){return m==4;}

static void box(int x0,int y0,int z0,int x1,int y1,int z1,uint8_t m){
  for(int z=z0;z<=z1;z++)for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++) if(inB(x,y,z)) grid[gi(x,y,z)]=m;
}
static void buildScene(){
  memset(grid,0,sizeof(grid));
  // shell
  box(0,0,0,NX-1,NY-1,NZ-1,1);
  box(1,1,1,NX-2,NY-2,NZ-2,0);
  // colored side walls of main room (z- red, z+ green) within main room x range
  box(1,1,0,60,NY-2,0,2);      // keep wall voxels at z=0 red for x<=60
  box(1,1,NZ-1,60,NY-2,NZ-1,3);
  // dividing wall between main room (x in 1..60) and light room (x in 64..94)
  box(61,1,1,63,NY-2,NZ-2,1);
  // doorway in dividing wall
  box(61,1,34,63,26,56,0);
  // emissive ceiling panel in light room
  box(70,NY-2,20,90,NY-2,76,4);
  // blue accent wall far side of light room
  box(NX-1,1,1,NX-1,NY-2,NZ-2,6);
  // obstacles in main room
  box(20,1,8,32,18,20,5);          // dark box
  box(38,1,60,46,30,68,1);          // white pillar
  box(14,1,72,26,10,86,6);          // low blue slab
}
// destruction event: blast a hole in dividing wall (away from doorway)
static V3 editCenter(62.f, 36.f, 22.f);
static float editRad = 11.f;
static void applyEdit(){
  for(int z=1;z<NZ-1;z++)for(int y=1;y<NY-1;y++)for(int x=61;x<=63;x++){
    float dx=x-editCenter.x, dy=y-editCenter.y, dz=z-editCenter.z;
    if(dx*dx*0.15f+dy*dy+dz*dz < editRad*editRad) grid[gi(x,y,z)]=0;
  }
}

// ----------------------------- DDA traversal --------------------------------
static uint64_t g_steps=0, g_rays=0; // cost counters
struct Hit{ int vx,vy,vz; int face; float t; uint8_t m; }; // face 0..5 = +x,-x,+y,-y,+z,-z
static V3 FN[6]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

static bool trace(V3 o, V3 d, float tmax, Hit& h){
  g_rays++;
  int x=(int)std::floor(o.x), y=(int)std::floor(o.y), z=(int)std::floor(o.z);
  if(!inB(x,y,z)) return false;
  int sx=d.x>0?1:-1, sy=d.y>0?1:-1, sz=d.z>0?1:-1;
  float idx=1.f/(std::fabs(d.x)+1e-12f), idy=1.f/(std::fabs(d.y)+1e-12f), idz=1.f/(std::fabs(d.z)+1e-12f);
  float tx=((sx>0? x+1-o.x : o.x-x))*idx;
  float ty=((sy>0? y+1-o.y : o.y-y))*idy;
  float tz=((sz>0? z+1-o.z : o.z-z))*idz;
  float t=0; int face=-1;
  for(int it=0; it<512; it++){
    g_steps++;
    if(t>tmax) return false;
    uint8_t m = at(x,y,z);
    if(m && it>0){ h={x,y,z,face,t,m}; return true; }
    if(m && it==0){ /* started inside solid: ignore, step out */ }
    if(tx<=ty && tx<=tz){ t=tx; tx+=idx; x+=sx; face = sx>0?1:0; }
    else if(ty<=tz)     { t=ty; ty+=idy; y+=sy; face = sy>0?3:2; }
    else                { t=tz; tz+=idz; z+=sz; face = sz>0?5:4; }
    if(!inB(x,y,z)) return false;
  }
  return false;
}

// --------------------------- emitter sampling (NEE) -------------------------
struct EmFace{ int x,y,z,f; };
static std::vector<EmFace> emFaces;
static void collectEmitters(){
  emFaces.clear();
  for(int z=0;z<NZ;z++)for(int y=0;y<NY;y++)for(int x=0;x<NX;x++){
    if(!emissive(at(x,y,z))) continue;
    static int OX[6]={1,-1,0,0,0,0},OY[6]={0,0,1,-1,0,0},OZ[6]={0,0,0,0,1,-1};
    for(int f=0;f<6;f++) if(inB(x+OX[f],y+OY[f],z+OZ[f]) && at(x+OX[f],y+OY[f],z+OZ[f])==0)
      emFaces.push_back({x,y,z,f});
  }
}
// one-sample NEE estimate of DIRECT irradiance at point p with normal n (counts as 1 shadow ray)
static V3 neeIrr(V3 p, V3 n, RNG& rng){
  if(emFaces.empty()) return {};
  int i = rng.next()%emFaces.size();
  EmFace e = emFaces[i];
  V3 fn = FN[e.f];
  V3 base((float)e.x,(float)e.y,(float)e.z);
  V3 q = base + V3(0.5f,0.5f,0.5f) + fn*0.501f;
  V3 tu = (std::fabs(fn.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
  V3 tv = cross(fn,tu);
  q += tu*(rng.uf()-0.5f) + tv*(rng.uf()-0.5f);
  V3 w = q - p; float d2 = dot(w,w); float d = std::sqrt(d2); w = w*(1.f/d);
  float cs = dot(w,n); float cl = -dot(w,fn);
  if(cs<=0||cl<=0) return {};
  Hit h;
  if(!trace(p,w,d+1.f,h)) return {};
  if(!emissive(h.m)) return {};            // blocked by non-emitter
  if(h.t < d-0.87f) return {};             // blocked by closer emitter? accept ~same depth
  float pdfA = 1.f/(float)emFaces.size();  // area of each face = 1 voxel^2
  return EMI[4]*(cs*cl/(d2*pdfA+1e-9f));
}

// cosine hemisphere sample around axis-aligned normal
static V3 cosDir(V3 n, RNG& rng){
  float u1=rng.uf(), u2=rng.uf();
  float r=std::sqrt(u1), phi=6.2831853f*u2;
  float lx=r*std::cos(phi), ly=r*std::sin(phi), lz=std::sqrt(std::max(0.f,1-u1));
  V3 tu = (std::fabs(n.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
  V3 tv = norm(cross(n,tu)); tu = cross(tv,n);
  return norm(tu*lx + tv*ly + n*lz);
}

// ----------------------------- camera / G-buffer ----------------------------
static const int W=128, H=80;
struct GPix{ bool hit=false; V3 pos,n; V3 alb; uint8_t m=0; int vx,vy,vz,face; float depth; };
static std::vector<GPix> gbuf(W*H);
static V3 camPos(7.f, 40.f, 46.f);
static V3 camFwd, camRt, camUp;
static void setupCam(){
  camFwd = norm(V3(63.f,16.f,52.f)-camPos);
  camRt = norm(cross(camFwd,V3(0,1,0)));
  camUp = cross(camRt,camFwd);
}
static V3 pixDir(float px,float py){
  float asp=(float)W/H, tanF=std::tan(0.5f*72.f*3.14159265f/180.f);
  float u=(2*(px+0.5f)/W-1)*tanF*asp, v=(1-2*(py+0.5f)/H)*tanF;
  return norm(camFwd + camRt*u + camUp*v);
}
static void buildGBuffer(){
  for(int y=0;y<H;y++)for(int x=0;x<W;x++){
    GPix g; Hit h;
    if(trace(camPos,pixDir((float)x,(float)y),1e9f,h)){
      g.hit=true; g.m=h.m; g.alb=ALB[h.m]; g.n=FN[h.face];
      g.vx=h.vx; g.vy=h.vy; g.vz=h.vz; g.face=h.face; g.depth=h.t;
      g.pos = camPos + pixDir((float)x,(float)y)*h.t + g.n*0.001f;
    }
    gbuf[y*W+x]=g;
  }
}

// shared converged direct pass (identical for all methods; excluded from budgets)
static std::vector<V3> directImg(W*H);
static void buildDirect(int spp){
  RNG rng(777,99);
  for(int i=0;i<W*H;i++){
    GPix& g=gbuf[i]; V3 acc{};
    if(g.hit){
      if(emissive(g.m)) acc = EMI[4];
      else { V3 e{}; for(int s=0;s<spp;s++) e+=neeIrr(g.pos,g.n,rng);
             acc = g.alb*(e*(1.f/(spp*3.14159265f))); }
    }
    directImg[i]=acc;
  }
}

// ----------------------------- reference indirect ---------------------------
// path-traced indirect irradiance E_ind at each G-buffer point; pixel L_ind = alb/pi * E
static V3 pathE(V3 p, V3 n, RNG& rng, int depth){
  // returns one-sample estimate of indirect irradiance at (p,n): cosine sample * pi * L_surface
  V3 d = cosDir(n,rng); Hit h;
  if(!trace(p,d,1e9f,h)) return {};
  if(emissive(h.m)) return {}; // direct light handled by NEE chain; emitter seen by bounce ray would double count
  V3 hp = V3((float)h.vx,(float)h.vy,(float)h.vz)+V3(0.5f,0.5f,0.5f)+FN[h.face]*0.501f;
  V3 hn = FN[h.face]; V3 alb=ALB[h.m];
  V3 Edir = neeIrr(hp,hn,rng);
  V3 Eind{};
  if(depth>0){
    float rr = (depth<22)? 0.85f : 1.0f;   // depth counts down from 24
    if(rng.uf()<rr) Eind = pathE(hp,hn,rng,depth-1)*(1.f/rr);
  }
  // outgoing radiance of hit = alb/pi*(Edir+Eind); irradiance sample = pi * L (cosine pdf)
  return alb*(Edir+Eind);
}
static void renderReference(std::vector<V3>& out, int spp){
  RNG rng(12345,55);
  for(int i=0;i<W*H;i++){
    GPix& g=gbuf[i]; V3 acc{};
    if(g.hit && !emissive(g.m)){
      V3 E{}; for(int s=0;s<spp;s++) E += pathE(g.pos,g.n,rng,24);
      acc = g.alb*(E*(1.f/(spp*3.14159265f)));
    }
    out[i]=acc;
  }
}

// ----------------------------- image / metrics ------------------------------
static inline float tm(float c){ c=std::max(0.f,c); c=c/(1.f+c); return std::pow(c,1.f/2.2f); }
static double psnrFinal(const std::vector<V3>& ind, const std::vector<V3>& refInd){
  double mse=0; int n=0;
  for(int i=0;i<W*H;i++){
    if(!gbuf[i].hit) continue;
    V3 a=directImg[i]+ind[i], b=directImg[i]+refInd[i];
    float da=tm(a.x)-tm(b.x), db=tm(a.y)-tm(b.y), dc=tm(a.z)-tm(b.z);
    mse += (da*da+db*db+dc*dc)/3.0; n++;
  }
  mse/=n;
  return 10.0*std::log10(1.0/std::max(mse,1e-12));
}
static void savePNG(const std::string& path, const std::vector<V3>& ind){
  std::vector<uint8_t> img(W*H*3);
  for(int i=0;i<W*H;i++){
    V3 c = directImg[i]+ind[i];
    img[i*3+0]=(uint8_t)std::round(255*tm(c.x));
    img[i*3+1]=(uint8_t)std::round(255*tm(c.y));
    img[i*3+2]=(uint8_t)std::round(255*tm(c.z));
  }
  stbi_write_png(path.c_str(),W,H,3,img.data(),W*3);
}

// =================== METHOD A: 1spp PT + temporal + spatial =================
struct MethodPT {
  std::vector<V3> hist; std::vector<float> n; std::vector<V3> out;
  RNG rng{42,7};
  float alpha=0.15f;
  void init(){ hist.assign(W*H,{}); n.assign(W*H,0); out.assign(W*H,{}); }
  void onEdit(){ /* temporal history kept (camera static); accumulation continues */ }
  // budget in rays; PT uses pathsPerPixel paths, each path = 2 bounce rays + 2 shadow rays
  int fcount=0;
  long frame(long budget){
    // ~5 rays per path (RR depth-4 with NEE). Allow fractional paths/pixel by interleaving pixels.
    double ppp = (double)budget / ((double)(W*H)*5.0);
    long basePaths = std::max(1L,(long)std::floor(ppp));
    double fracP = (ppp>1.0)? ppp-std::floor(ppp) : 0.0;
    int stride = (ppp<1.0)? (int)std::round(1.0/ppp) : 1;
    uint64_t r0=g_rays;
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i]; if(!g.hit||emissive(g.m)) continue;
      if(stride>1 && ((i+fcount)%stride)!=0) continue;
      long paths = basePaths + ((rng.uf()<fracP)?1:0);
      V3 E{};
      for(long s=0;s<paths;s++){
        // NEE path, RR-extended to depth 4, estimating E_ind
        V3 p=g.pos, nn=g.n, tp{1,1,1}; V3 acc{};
        for(int b=0;b<8;b++){
          V3 d=cosDir(nn,rng); Hit h;
          if(!trace(p,d,1e9f,h)) break;
          if(emissive(h.m)) break;
          V3 hp=V3((float)h.vx,(float)h.vy,(float)h.vz)+V3(0.5f,0.5f,0.5f)+FN[h.face]*0.501f;
          V3 hn=FN[h.face]; tp = tp*ALB[h.m];
          acc += tp*neeIrr(hp,hn,rng);
          if(b>=1){ float rr=0.7f; if(rng.uf()>=rr) break; tp=tp*(1.f/rr); }
          p=hp; nn=hn;
        }
        E += acc;
      }
      E = E*(1.f/paths);
      float a = (n[i]<1)?1.f:alpha;
      hist[i] = hist[i]*(1-a) + E*a; n[i]+=1;
    }
    fcount++;
    // edge-aware 5x5 spatial filter on accumulated irradiance
    std::vector<V3> f(W*H);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
      int i=y*W+x; GPix& g=gbuf[i];
      if(!g.hit){f[i]=hist[i];continue;}
      V3 acc{}; float ws=0;
      for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){
        int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H)continue;
        int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit)continue;
        if(dot(q.n,g.n)<0.9f) continue;
        if(std::fabs(q.depth-g.depth)>2.5f) continue;
        float w=std::exp(-(dx*dx+dy*dy)/4.5f);
        acc+=hist[j]*w; ws+=w;
      }
      f[i]= (ws>0)? acc*(1.f/ws) : hist[i];
    }
    for(int i=0;i<W*H;i++){ GPix& g=gbuf[i]; out[i]= g.hit? g.alb*(f[i]*(1.f/3.14159265f)) : V3{}; }
    return (long)(g_rays-r0);
  }
};

// ============== METHOD B: DDGI-style probe grid (RTXGI proxy) ===============
static const int PS=8;                      // probe spacing in voxels
static const int PNX=NX/PS, PNY=NY/PS, PNZ=NZ/PS;
static const int NDIR=64;
struct MethodDDGI {
  std::vector<V3> rad;        // [probe][dir] radiance
  std::vector<float> dmean, dmean2;
  std::vector<uint8_t> alive;
  std::vector<V3> dirs;
  std::vector<V3> E6;
  std::vector<V3> out;
  int cursor=0; RNG rng{99,13};
  static int pidx(int i,int j,int k){return (k*PNY+j)*PNX+i;}
  V3 ppos(int i,int j,int k){return V3(i*PS+PS*0.5f, j*PS+PS*0.5f, k*PS+PS*0.5f);}
  void init(){
    int NP=PNX*PNY*PNZ;
    rad.assign(NP*NDIR,{}); dmean.assign(NP*NDIR,1.f); dmean2.assign(NP*NDIR,1.f);
    E6.assign(NP*6,{});
    alive.assign(NP,1); out.assign(W*H,{});
    dirs.resize(NDIR);
    for(int i=0;i<NDIR;i++){ // spherical Fibonacci
      float ph=std::acos(1-2*(i+0.5f)/NDIR), th=3.14159265f*(1+std::sqrt(5.f))*(i+0.5f);
      dirs[i]=V3(std::sin(ph)*std::cos(th),std::cos(ph),std::sin(ph)*std::sin(th));
    }
    markAlive();
  }
  void markAlive(){
    for(int k=0;k<PNZ;k++)for(int j=0;j<PNY;j++)for(int i=0;i<PNX;i++){
      V3 p=ppos(i,j,k);
      alive[pidx(i,j,k)] = at((int)p.x,(int)p.y,(int)p.z)==0;
    }
  }
  void onEdit(){ markAlive(); }
  // sample indirect irradiance field at (p,n) from 8 surrounding probes
  V3 sample(V3 p, V3 n){
    float fx=p.x/PS-0.5f, fy=p.y/PS-0.5f, fz=p.z/PS-0.5f;
    int i0=(int)std::floor(fx), j0=(int)std::floor(fy), k0=(int)std::floor(fz);
    float tx=fx-i0, ty=fy-j0, tz=fz-k0;
    V3 acc{}; float ws=0;
    for(int c=0;c<8;c++){
      int i=i0+(c&1), j=j0+((c>>1)&1), k=k0+((c>>2)&1);
      if(i<0||i>=PNX||j<0||j>=PNY||k<0||k>=PNZ) continue;
      int pi=pidx(i,j,k); if(!alive[pi]) continue;
      V3 pp=ppos(i,j,k);
      float w=( (c&1)?tx:1-tx )*( ((c>>1)&1)?ty:1-ty )*( ((c>>2)&1)?tz:1-tz );
      V3 toP = pp-p; float dist=len(toP); V3 dir = (dist>1e-4f)? toP*(1.f/dist) : n;
      float wn=(dot(dir,n)+1)*0.5f; w *= 0.05f+wn*wn;                  // backface (smooth)
      // Chebyshev visibility using nearest stored direction probe->p
      V3 d2p = dir*-1.f; int best=0; float bd=-2;
      for(int q=0;q<NDIR;q++){ float t=dot(dirs[q],d2p); if(t>bd){bd=t;best=q;} }
      float mu=dmean[pi*NDIR+best], mu2=dmean2[pi*NDIR+best];
      if(dist>mu){ float var=std::max(mu2-mu*mu,0.01f);
        float ch=var/(var+(dist-mu)*(dist-mu)); w*=std::max(ch*ch*ch,0.f); }
      if(w<=0) continue;
      // precomputed cosine-convolved irradiance for axis-aligned normal
      int axis = (std::fabs(n.x)>0.5f)? (n.x>0?0:1) : (std::fabs(n.y)>0.5f? (n.y>0?2:3) : (n.z>0?4:5));
      acc += E6[pi*6+axis]*w; ws += w;
    }
    return (ws>1e-5f)? acc*(1.f/ws) : V3{};
  }
  long frame(long budget){
    uint64_t r0=g_rays;
    int NP=PNX*PNY*PNZ;
    long raysPerProbe = NDIR*2; // dir ray + shadow ray
    int updates = (int)std::max(1L, budget/raysPerProbe);
    int u=0, attempts=0;
    while(u<updates && attempts<NP*2){
      attempts++;
      int pi = cursor; cursor=(cursor+1)%NP;
      if(!alive[pi]) continue;
      u++;
      int k=pi/(PNX*PNY), j=(pi/PNX)%PNY, i=pi%PNX;
      V3 pp=ppos(i,j,k);
      for(int q=0;q<NDIR;q++){
        Hit h; V3 newRad{}; float newD=64.f;
        if(trace(pp,dirs[q],1e9f,h)){
          newD=h.t;
          if(!emissive(h.m)){
            V3 hp=V3((float)h.vx,(float)h.vy,(float)h.vz)+V3(0.5f,0.5f,0.5f)+FN[h.face]*0.501f;
            V3 hn=FN[h.face];
            V3 Ed=neeIrr(hp,hn,rng);
            V3 Ei=sample(hp,hn);                       // probe feedback -> multibounce
            newRad = ALB[h.m]*((Ed+Ei)*(1.f/3.14159265f));
          }
        }
        int idx=pi*NDIR+q;
        // perception-based hysteresis: boost alpha on large change (RTXGI-style)
        V3 old=rad[idx]; float ch=len(newRad-old)/(len(old)+0.05f);
        float a = (ch>0.6f)? 0.45f : 0.12f;
        rad[idx]=old*(1-a)+newRad*a;
        dmean[idx]=dmean[idx]*(1-a)+newD*a;
        dmean2[idx]=dmean2[idx]*(1-a)+newD*newD*a;
      }
      // refresh pre-convolved irradiance for the 6 axis normals (4pi/N * sum L cos+)
      for(int ax=0;ax<6;ax++){
        V3 E{};
        for(int q=0;q<NDIR;q++){ float c2=dot(dirs[q],FN[ax]); if(c2>0) E+=rad[pi*NDIR+q]*c2; }
        E6[pi*6+ax]=E*(4.f*3.14159265f/NDIR);
      }
    }
    for(int px=0;px<W*H;px++){
      GPix& g=gbuf[px];
      out[px] = (g.hit&&!emissive(g.m)) ? g.alb*(sample(g.pos+g.n*1.5f,g.n)*(1.f/3.14159265f)) : V3{};
    }
    return (long)(g_rays-r0);
  }
};

// ============== METHOD C: FaceCache-GI (proposed) ============================
// Per exposed voxel face irradiance cache + cache-feedback multibounce +
// visibility-prioritized amortized updates + brick-local invalidation on edits.
static const int BK=4;                       // coarse brick size (voxels)
static const int CX=NX/BK, CY=NY/BK, CZ=NZ/BK;
struct MethodFC {
  struct Cell{ V3 E{}; float n=0; };
  std::vector<Cell> cache;            // L0: [voxel*6+face] fine cache
  std::vector<Cell> l1;               // L1: [brick*6+face] coarse cache (feedback cascade)
  std::vector<std::vector<int>> l1Faces; // exposed fine faces per L1 cell
  std::vector<int> l1Active;          // active L1 ids
  std::vector<int> exposed;           // list of exposed fine face ids
  std::vector<int> visList;           // face ids visible in G-buffer
  std::vector<V3> out;
  int curVis=0, curL1=0; RNG rng{7,3};
  static long fid(int x,int y,int z,int f){ return ((long)gi(x,y,z))*6+f; }
  static long cid(int x,int y,int z,int f){ return ((long)((z/BK)*CY+(y/BK))*CX+(x/BK))*6+f; }
  void rebuildExposed(){
    exposed.clear();
    for(auto& v:l1Faces) v.clear();
    l1Active.clear();
    static int OX[6]={1,-1,0,0,0,0},OY[6]={0,0,1,-1,0,0},OZ[6]={0,0,0,0,1,-1};
    for(int z=0;z<NZ;z++)for(int y=0;y<NY;y++)for(int x=0;x<NX;x++){
      uint8_t m=at(x,y,z); if(!m||emissive(m)) continue;
      for(int f=0;f<6;f++){int xx=x+OX[f],yy=y+OY[f],zz=z+OZ[f];
        if(inB(xx,yy,zz)&&at(xx,yy,zz)==0){
          int id=(int)fid(x,y,z,f);
          exposed.push_back(id);
          l1Faces[cid(x,y,z,f)].push_back(id);
        }}
    }
    for(size_t i=0;i<l1Faces.size();i++) if(!l1Faces[i].empty()) l1Active.push_back((int)i);
  }
  void rebuildVisible(){
    visList.clear();
    std::vector<uint8_t> seen; seen.assign(cache.size(),0);
    for(int i=0;i<W*H;i++){ GPix& g=gbuf[i]; if(!g.hit||emissive(g.m))continue;
      long id=fid(g.vx,g.vy,g.vz,g.face);
      if(!seen[id]){seen[id]=1; visList.push_back((int)id);} }
  }
  void init(){
    cache.assign((size_t)NX*NY*NZ*6,{});
    l1.assign((size_t)CX*CY*CZ*6,{});
    l1Faces.assign((size_t)CX*CY*CZ*6,{});
    rebuildExposed(); rebuildVisible();
  }
  void onEdit(){
    rebuildExposed(); rebuildVisible();
    // brick-local invalidation: age-reset faces near the edit so they re-converge fast
    for(int id : exposed){
      int f=id%6; long v=id/6; int x=v%NX, y=(v/NX)%NY, z=v/(NX*NY);
      float dx=x-editCenter.x,dy=y-editCenter.y,dz=z-editCenter.z;
      float d2=dx*dx+dy*dy+dz*dz;
      if(d2< (editRad+26)*(editRad+26)) cache[id].n = std::min(cache[id].n, 2.f);
      (void)f;
    }
    for(int id : l1Active){
      int f=id%6; long b=id/6; int bx=b%CX, by=(b/CX)%CY, bz=b/(CX*CY);
      float dx=bx*BK+BK*0.5f-editCenter.x, dy=by*BK+BK*0.5f-editCenter.y, dz=bz*BK+BK*0.5f-editCenter.z;
      if(dx*dx+dy*dy+dz*dz < (editRad+26)*(editRad+26)) l1[id].n = std::min(l1[id].n, 2.f);
      (void)f;
    }
  }
  V3 faceCenter(int id,V3& n){
    int f=id%6; long v=id/6; int x=v%NX, y=(v/NX)%NY, z=v/(NX*NY);
    n=FN[f];
    return V3(x+0.5f,y+0.5f,z+0.5f)+n*0.501f;
  }
  V3 gatherE(V3 p, V3 n, int S){
    V3 E{};
    for(int s=0;s<S;s++){
      V3 d=cosDir(n,rng); Hit h;
      if(!trace(p,d,1e9f,h)) continue;
      if(emissive(h.m)) continue;
      V3 hn=FN[h.face];
      V3 hp=V3((float)h.vx,(float)h.vy,(float)h.vz)+V3(0.5f,0.5f,0.5f)+hn*0.501f;
      V3 Ed=neeIrr(hp,hn,rng);
      V3 Ei=l1[cid(h.vx,h.vy,h.vz,h.face)].E;  // coarse-cascade feedback -> infinite bounces
      E += ALB[h.m]*(Ed+Ei);
    }
    return E*(1.f/S);
  }
  void updateFace(int id){
    V3 n; V3 p=faceCenter(id,n);
    V3 E=gatherE(p,n,4);
    Cell& c=cache[id];
    float a = std::max(1.f/(c.n+1.f), 0.08f);  // adaptive blend: fast warmup, stable steady state
    c.E = c.E*(1-a)+E*a; c.n+=1;
  }
  void updateL1(int id){
    auto& fl=l1Faces[id]; if(fl.empty()) return;
    int fineId = fl[rng.next()%fl.size()];     // stochastic representative face within brick
    V3 n; V3 p=faceCenter(fineId,n);
    V3 E=gatherE(p,n,4);
    Cell& c=l1[id];
    float a = std::max(1.f/(c.n+1.f), 0.12f);
    c.E = c.E*(1-a)+E*a; c.n+=1;
  }
  // bilinear interpolation of cache across the face plane
  V3 shade(GPix& g){
    int f=g.face; V3 n=g.n;
    // local position within voxel on the face plane
    float lu,lv; int au,av; // tangent axes
    int x=g.vx,y=g.vy,z=g.vz;
    V3 lp = g.pos - V3((float)x,(float)y,(float)z);
    if(f<2){ au=1;av=2; lu=lp.y; lv=lp.z; }
    else if(f<4){ au=0;av=2; lu=lp.x; lv=lp.z; }
    else { au=0;av=1; lu=lp.x; lv=lp.y; }
    float fu=lu-0.5f, fv=lv-0.5f;
    V3 acc{}; float ws=0;
    static int OX[6]={1,-1,0,0,0,0},OY[6]={0,0,1,-1,0,0},OZ[6]={0,0,0,0,1,-1};
    for(int ou=-1;ou<=1;ou++)for(int ov=-1;ov<=1;ov++){
      int xx=x,yy=y,zz=z;
      if(au==0)xx+=ou; else if(au==1)yy+=ou; else zz+=ou;
      if(av==0)xx+=ov; else if(av==1)yy+=ov; else zz+=ov;
      if(!inB(xx,yy,zz)||at(xx,yy,zz)==0||emissive(at(xx,yy,zz))) continue;   // same-plane surface only
      int fx=xx+OX[f],fy=yy+OY[f],fz=zz+OZ[f];
      if(!inB(fx,fy,fz)||at(fx,fy,fz)!=0) continue;                            // face must be exposed
      long id=fid(xx,yy,zz,f);
      Cell& cl=cache[id]; if(cl.n<0.5f) continue;
      float du=ou-fu, dv=ov-fv;
      float w=std::exp(-(du*du+dv*dv)/1.1f);
      acc+=cl.E*w; ws+=w;
    }
    V3 cE = l1[cid(x,y,z,f)].E;
    if(ws<0.35f){ acc += cE*(0.35f-ws); ws=0.35f; }   // coarse-cascade fallback
    return acc*(1.f/ws);
  }
  long frame(long budget){
    uint64_t r0=g_rays;
    const long raysPerUpdate=8; // 4 hemi + 4 shadow
    long updates = std::max(1L,budget/raysPerUpdate);
    long visU = (long)(updates*0.6), l1U=updates-visU;
    for(long u=0;u<visU && !visList.empty();u++){
      updateFace(visList[curVis%visList.size()]); curVis++;
    }
    for(long u=0;u<l1U && !l1Active.empty();u++){
      updateL1(l1Active[curL1%l1Active.size()]); curL1++;
    }
    if(out.empty()) out.assign(W*H,{});
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      out[i]= (g.hit&&!emissive(g.m))? g.alb*(shade(g)*(1.f/3.14159265f)) : V3{};
    }
    return (long)(g_rays-r0);
  }
};

// ------------------------------- experiment ---------------------------------
static const int FRAMES=150, EDIT_FRAME=75;
struct Row{int frame; double psnr; long rays; double ms; long steps;};

template<class M>
void runMethod(const char* name, long budget, std::vector<Row>& rows,
               std::vector<V3>& refPre, std::vector<V3>& refPost, bool dumpImgs, const char* tag){
  buildScene(); collectEmitters(); buildGBuffer(); buildDirect(96);
  M m; m.init();
  for(int f=0; f<FRAMES; f++){
    if(f==EDIT_FRAME){
      applyEdit(); collectEmitters(); buildGBuffer(); buildDirect(96); m.onEdit();
    }
    uint64_t s0=g_steps;
    auto t0=std::chrono::high_resolution_clock::now();
    long rays = m.frame(budget);
    auto t1=std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double,std::milli>(t1-t0).count();
    auto& ref = (f<EDIT_FRAME)? refPre : refPost;
    rows.push_back({f, psnrFinal(m.out, ref), rays, ms, (long)(g_steps-s0)});
    if(dumpImgs && (f==74||f==76||f==80||f==90||f==149))
      savePNG(std::string("out/")+name+"_"+tag+"_f"+std::to_string(f)+".png", m.out);
  }
}

static bool loadRaw(const char* p, std::vector<V3>& v, double& cnt){
  FILE* f=fopen(p,"rb"); if(!f) return false;
  fread(&cnt,sizeof(double),1,f);
  fread(v.data(),sizeof(V3),v.size(),f); fclose(f); return true;
}
static void saveRaw(const char* p, std::vector<V3>& v, double cnt){
  FILE* f=fopen(p,"wb"); fwrite(&cnt,sizeof(double),1,f);
  fwrite(v.data(),sizeof(V3),v.size(),f); fclose(f);
}

int main(int argc,char**argv){
  setupCam();
  std::string mode = argc>1? argv[1] : "";
  if(mode=="ref"){
    // ./bench ref pre|post <spp> <seed>
    bool post = std::string(argv[2])=="post";
    int spp=atoi(argv[3]); uint64_t seed=atoll(argv[4]);
    buildScene(); if(post) applyEdit();
    collectEmitters(); buildGBuffer(); buildDirect(96);
    std::vector<V3> acc(W*H, V3()); double cnt=0;
    std::string raw=std::string("out/ref_")+(post?"post":"pre")+".raw";
    loadRaw(raw.c_str(),acc,cnt);
    std::vector<V3> cur(W*H);
    RNG dummy; (void)dummy;
    {
      RNG rng(12345+seed*7919,55+seed);
      for(int i=0;i<W*H;i++){
        GPix& g=gbuf[i]; V3 a{};
        if(g.hit && !emissive(g.m)){
          V3 E{}; for(int s2=0;s2<spp;s2++) E += pathE(g.pos,g.n,rng,24);
          a = g.alb*(E*(1.f/(spp*3.14159265f)));
        }
        cur[i]=a;
      }
    }
    double nc=cnt+spp;
    for(int i=0;i<W*H;i++) acc[i]= acc[i]*(float)(cnt/nc) + cur[i]*(float)(spp/nc);
    saveRaw(raw.c_str(),acc,nc);
    savePNG(std::string("out/reference_")+(post?"post":"pre")+".png",acc);
    fprintf(stderr,"ref %s total spp=%.0f\n",post?"post":"pre",nc);
    return 0;
  }
  if(mode=="run"){
    // ./bench run <PT|DDGI|FCGI> <budgetMult>
    std::string meth=argv[2]; float mu=atof(argv[3]);
    std::vector<V3> refPre(W*H), refPost(W*H); double c1=0,c2=0;
    buildScene(); collectEmitters(); buildGBuffer(); // for sizes only
    if(!loadRaw("out/ref_pre.raw",refPre,c1)||!loadRaw("out/ref_post.raw",refPost,c2)){
      fprintf(stderr,"missing refs\n"); return 1; }
    long b=(long)(4L*W*H*mu);
    char tag[32]; snprintf(tag,32,"b%.1fx",mu);
    bool dump=(mu==1.f);
    std::vector<Row> rows;
    if(meth=="PT") runMethod<MethodPT>("PT",b,rows,refPre,refPost,dump,tag);
    else if(meth=="DDGI") runMethod<MethodDDGI>("DDGI",b,rows,refPre,refPost,dump,tag);
    else runMethod<MethodFC>("FCGI",b,rows,refPre,refPost,dump,tag);
    FILE* fp=fopen("out/results.csv","a");
    if(ftell(fp)==0) fprintf(fp,"method,budget,frame,psnr,rays,ms,steps\n");
    fseek(fp,0,SEEK_END);
    for(auto&r:rows)fprintf(fp,"%s,%.1f,%d,%.4f,%ld,%.3f,%ld\n",meth.c_str(),mu,r.frame,r.psnr,r.rays,r.ms,r.steps);
    fclose(fp);
    fprintf(stderr,"done %s %.1fx\n",meth.c_str(),mu);
    return 0;
  }
  fprintf(stderr,"usage: bench ref pre|post spp seed | bench run METHOD MULT\n");
  return 1;
}
