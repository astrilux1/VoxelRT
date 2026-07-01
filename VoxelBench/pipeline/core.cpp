#include "core.h"
#include <omp.h>

Material MAT[MAXMAT];
int NX=96, NY=64, NZ=96;
int BNX=24, BNY=16, BNZ=24;
std::vector<uint8_t> grid;
std::vector<uint64_t> brick;
void setDims(int nx,int ny,int nz){
  NX=nx; NY=ny; NZ=nz; BNX=nx/4; BNY=ny/4; BNZ=nz/4;
  grid.assign((size_t)NX*NY*NZ,0);
  brick.assign((size_t)BNX*BNY*BNZ,0);
}
SkyLight SKY;
std::atomic<uint64_t> g_rays{0}, g_steps{0};
thread_local uint64_t tl_rays=0, tl_steps=0;
std::vector<EmFace> emFaces;
std::vector<GPix> gbuf(W*H);
std::vector<V3> directImg(W*H);
V3 camPos, camFwd, camRt, camUp; float camFovDeg=72.f;

// ----------------------------- traversal ------------------------------------
bool traceNaive(V3 o, V3 d, float tmax, Hit& h){
  tl_rays++;
  int x=(int)std::floor(o.x), y=(int)std::floor(o.y), z=(int)std::floor(o.z);
  if(!inB(x,y,z)) return false;
  int sx=d.x>0?1:-1, sy=d.y>0?1:-1, sz=d.z>0?1:-1;
  // accumulate t in double: long rays through large grids (e.g. 320^3 town)
  // can take 100+ steps, and float accumulation error in t+=idx is enough to
  // flip a near-tie tx<=ty comparison vs the brick traversal's recomputed-
  // from-origin t values, picking a different (adjacent) voxel. Double keeps
  // both traversals converged to the same answer.
  double idx=1.0/((double)std::fabs(d.x)+1e-12), idy=1.0/((double)std::fabs(d.y)+1e-12), idz=1.0/((double)std::fabs(d.z)+1e-12);
  double tx=((sx>0? x+1-(double)o.x : (double)o.x-x))*idx;
  double ty=((sy>0? y+1-(double)o.y : (double)o.y-y))*idy;
  double tz=((sz>0? z+1-(double)o.z : (double)o.z-z))*idz;
  double t=0; int face=-1;
  for(int it=0; it<8192; it++){
    tl_steps++;
    if(t>tmax) return false;
    uint8_t m = at(x,y,z);
    if(m && it>0){ h={x,y,z,face,(float)t,m}; return true; }
    if(tx<=ty && tx<=tz){ t=tx; tx+=idx; x+=sx; face = sx>0?1:0; }
    else if(ty<=tz)     { t=ty; ty+=idy; y+=sy; face = sy>0?3:2; }
    else                { t=tz; tz+=idz; z+=sz; face = sz>0?5:4; }
    if(!inB(x,y,z)) return false;
  }
  return false;
}

// two-level DDA: coarse over 4^3 bricks (skip mask==0), fine inside occupied bricks.
bool trace(V3 o, V3 d, float tmax, Hit& h){
  tl_rays++;
  // start voxel
  int x=(int)std::floor(o.x), y=(int)std::floor(o.y), z=(int)std::floor(o.z);
  if(!inB(x,y,z)) return false;
  int sx=d.x>0?1:-1, sy=d.y>0?1:-1, sz=d.z>0?1:-1;
  // double precision t accumulation (see traceNaive comment): keeps the
  // brick-recompute-at-entry values bit-consistent with traceNaive's
  // incremental accumulation on long rays through large (e.g. 320^3) grids.
  double adx=(double)std::fabs(d.x)+1e-12, ady=(double)std::fabs(d.y)+1e-12, adz=(double)std::fabs(d.z)+1e-12;
  double idx=1.0/adx, idy=1.0/ady, idz=1.0/adz;          // fine t per unit step
  double bidx=4.0*idx, bidy=4.0*idy, bidz=4.0*idz;        // brick t per step
  // brick coords
  int bx=x>>2, by=y>>2, bz=z>>2;
  // t to next brick boundary on each axis
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
      // fine DDA inside this brick, starting at entry point (t=bt)
      double eps=1e-7;
      double t0=bt;
      // nudge slightly backward along the ray before flooring the non-entry
      // axes: o+d*t0 can land fractionally past an integer boundary due to
      // fp error from the (potentially large) single multiply, which would
      // floor to the wrong (next) voxel on those axes. Stepping back by eps
      // keeps us on the correct side without skipping a voxel (eps << 1).
      double px=(double)o.x+(double)d.x*std::max(0.0,t0-eps);
      double py=(double)o.y+(double)d.y*std::max(0.0,t0-eps);
      double pz=(double)o.z+(double)d.z*std::max(0.0,t0-eps);
      int fx,fy,fz;
      if(first && bt==0){ fx=x; fy=y; fz=z; }
      else {
        // entry voxel: nudge along entry axis only via face info; compute from p with care
        fx=(int)std::floor(px); fy=(int)std::floor(py); fz=(int)std::floor(pz);
        // clamp into brick and snap entry axis coordinate exactly
        if(bface==0){ fx=bx*4+3; }            // entered moving -x: came through +x face
        if(bface==1){ fx=bx*4; }
        if(bface==2){ fy=by*4+3; }
        if(bface==3){ fy=by*4; }
        if(bface==4){ fz=bz*4+3; }
        if(bface==5){ fz=bz*4; }
        fx=std::min(std::max(fx,bx*4),bx*4+3);
        fy=std::min(std::max(fy,by*4),by*4+3);
        fz=std::min(std::max(fz,bz*4),bz*4+3);
      }
      // fine t to next voxel boundary from origin o (recompute; robust)
      double tx=((sx>0? fx+1-(double)o.x : (double)o.x-fx))*idx;
      double ty=((sy>0? fy+1-(double)o.y : (double)o.y-fy))*idy;
      double tz=((sz>0? fz+1-(double)o.z : (double)o.z-fz))*idz;
      double t=t0; int face=bface;
      for(int it=0; it<16; it++){
        tl_steps++;
        if(t>tmax) return false;
        uint8_t m = (mask>>bbit(fx&3,fy&3,fz&3)) & 1 ? grid[gi(fx,fy,fz)] : 0;
        if(m && !(first && t==0)){ h={fx,fy,fz,face,(float)t,m}; return true; }
        if(tx<=ty && tx<=tz){ t=tx; tx+=idx; fx+=sx; face = sx>0?1:0; }
        else if(ty<=tz)     { t=ty; ty+=idy; fy+=sy; face = sy>0?3:2; }
        else                { t=tz; tz+=idz; fz+=sz; face = sz>0?5:4; }
        if(fx<bx*4||fx>bx*4+3||fy<by*4||fy>by*4+3||fz<bz*4||fz>bz*4+3) break; // left brick
        first=false;
      }
      first=false;
    }
    // brick step
    if(btx<=bty && btx<=btz){ bt=btx; btx+=bidx; bx+=sx; bface = sx>0?1:0; }
    else if(bty<=btz)       { bt=bty; bty+=bidy; by+=sy; bface = sy>0?3:2; }
    else                    { bt=btz; btz+=bidz; bz+=sz; bface = sz>0?5:4; }
    first=false;
  }
  return false;
}

// ----------------------------- emitters + NEE -------------------------------
void collectEmitters(){
  emFaces.clear();
  for(int z=0;z<NZ;z++)for(int y=0;y<NY;y++)for(int x=0;x<NX;x++){
    uint8_t m=at(x,y,z);
    if(!m||!emissive(m)) continue;
    for(int f=0;f<6;f++) if(inB(x+OXf[f],y+OYf[f],z+OZf[f]) && at(x+OXf[f],y+OYf[f],z+OZf[f])==0)
      emFaces.push_back({x,y,z,f,m});
  }
}

V3 neeIrr(V3 p, V3 n, RNG& rng){
  V3 acc{};
  if(!emFaces.empty()){
    int i = rng.u32()%emFaces.size();
    EmFace e = emFaces[i];
    V3 fn = FN[e.f];
    V3 q = facePoint(e.x,e.y,e.z,e.f);
    V3 tu = (std::fabs(fn.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
    V3 tv = cross(fn,tu);
    q += tu*(rng.uf()-0.5f) + tv*(rng.uf()-0.5f);
    V3 w = q - p; float d2 = dot(w,w); float d = std::sqrt(d2); w = w*(1.f/d);
    float cs = dot(w,n); float cl = -dot(w,fn);
    if(cs>0&&cl>0){
      Hit h;
      if(trace(p,w,d+1.f,h) && emissive(h.m) && h.t >= d-0.87f){
        float pdfA = 1.f/(float)emFaces.size();
        acc += MAT[e.m].emi*(cs*cl/(d2*pdfA+1e-9f));
      }
    }
  }
  if(SKY.enabled && (SKY.sunE.x+SKY.sunE.y+SKY.sunE.z)>0){
    float cs = dot(SKY.sunDir,n);
    if(cs>0){
      Hit h;
      if(!trace(p,SKY.sunDir,1e9f,h)) acc += SKY.sunE*cs;
    }
  }
  return acc;
}

// ----------------------------- camera / G-buffer ----------------------------
void setupCam(V3 pos, V3 lookAt, float fovDeg){
  camPos=pos; camFovDeg=fovDeg;
  camFwd = norm(lookAt-pos);
  camRt = norm(cross(camFwd,V3(0,1,0)));
  camUp = cross(camRt,camFwd);
}
V3 pixDir(float px,float py){
  float asp=(float)W/H, tanF=std::tan(0.5f*camFovDeg*3.14159265f/180.f);
  float u=(2*(px+0.5f)/W-1)*tanF*asp, v=(1-2*(py+0.5f)/H)*tanF;
  return norm(camFwd + camRt*u + camUp*v);
}
void buildGBuffer(){
  #pragma omp parallel
  {
    #pragma omp for schedule(dynamic,8)
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
      GPix g; Hit h;
      V3 d=pixDir((float)x,(float)y);
      if(trace(camPos,d,1e9f,h)){
        g.hit=true; g.m=h.m; g.alb=MAT[h.m].alb; g.n=FN[h.face];
        g.vx=h.vx; g.vy=h.vy; g.vz=h.vz; g.face=h.face; g.depth=h.t;
        g.pos = camPos + d*h.t + g.n*0.001f;
      }
      gbuf[y*W+x]=g;
    }
    flushCounters();
  }
}

void buildDirect(int spp){
  #pragma omp parallel
  {
    #pragma omp for schedule(dynamic,16)
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i]; V3 acc{};
      if(g.hit){
        if(emissive(g.m)) acc = MAT[g.m].emi;
        else { RNG rng(i,9001,12345); V3 e{}; for(int s=0;s<spp;s++) e+=neeIrr(g.pos,g.n,rng);
               acc = g.alb*(e*(1.f/(spp*3.14159265f))); }
      } else {
        acc = SKY.skyL(pixDir(i%W,(float)(i/W)));   // sky pixels carry env radiance
      }
      directImg[i]=acc;
    }
    flushCounters();
  }
}

// ----------------------------- reference ------------------------------------
V3 pathE(V3 p, V3 n, RNG& rng, int depth){
  V3 d = cosDir(n,rng); Hit h;
  if(!trace(p,d,1e9f,h)) return SKY.skyL(d)*3.14159265f;   // sky = indirect env light
  if(emissive(h.m)) return {};                              // emitters handled by NEE
  V3 hp = facePoint(h.vx,h.vy,h.vz,h.face);
  V3 hn = FN[h.face]; V3 alb=MAT[h.m].alb;
  V3 Edir = neeIrr(hp,hn,rng);
  V3 Eind{};
  if(depth>0){
    float rr = (depth<22)? 0.85f : 1.0f;
    if(rng.uf()<rr) Eind = pathE(hp,hn,rng,depth-1)*(1.f/rr);
  }
  return alb*(Edir+Eind);
}

void renderReference(std::vector<V3>& out, int spp, uint64_t seed){
  #pragma omp parallel
  {
    #pragma omp for schedule(dynamic,8)
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i]; V3 acc{};
      if(g.hit && !emissive(g.m)){
        RNG rng(i,seed,777);
        V3 E{}; for(int s=0;s<spp;s++) E += pathE(g.pos,g.n,rng,24);
        acc = g.alb*(E*(1.f/(spp*3.14159265f)));
      }
      out[i]=acc;
    }
    flushCounters();
  }
}

// ----------------------------- image io -------------------------------------
void savePPM(const std::string& path, const std::vector<V3>& ind){
  std::vector<uint8_t> img(W*H*3);
  for(int i=0;i<W*H;i++){
    V3 c = directImg[i]+ind[i];
    img[i*3+0]=(uint8_t)std::lround(255*tonemap(c.x));
    img[i*3+1]=(uint8_t)std::lround(255*tonemap(c.y));
    img[i*3+2]=(uint8_t)std::lround(255*tonemap(c.z));
  }
  FILE* f=fopen(path.c_str(),"wb");
  fprintf(f,"P6\n%d %d\n255\n",W,H);
  fwrite(img.data(),1,img.size(),f); fclose(f);
}
void saveRawImg(const std::string& p, const std::vector<V3>& v, double cnt){
  FILE* f=fopen(p.c_str(),"wb"); fwrite(&cnt,sizeof(double),1,f);
  fwrite(v.data(),sizeof(V3),v.size(),f); fclose(f);
}
bool loadRawImg(const std::string& p, std::vector<V3>& v, double& cnt){
  FILE* f=fopen(p.c_str(),"rb"); if(!f) return false;
  if(fread(&cnt,sizeof(double),1,f)!=1){fclose(f);return false;}
  if(fread(v.data(),sizeof(V3),v.size(),f)!=v.size()){fclose(f);return false;}
  fclose(f); return true;
}
