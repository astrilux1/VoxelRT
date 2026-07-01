// Method B: DDGI-style probe grid (RTXGI proxy) (ported from prior_work/bench.cpp MethodDDGI)
#pragma once
#include "../technique.h"

static const int PS=8;                      // probe spacing in voxels
static const int NDIR=64;

struct MethodDDGI : Technique {
  std::vector<V3> rad;        // [probe][dir] radiance
  std::vector<float> dmean, dmean2;
  std::vector<uint8_t> alive;
  std::vector<V3> dirs;
  std::vector<V3> E6;
  int cursor=0;
  int frameCounter=0;
  int PNX=0,PNY=0,PNZ=0;       // probe grid dims, computed in init() from runtime NX/NY/NZ

  int pidx(int i,int j,int k){return (k*PNY+j)*PNX+i;}
  V3 ppos(int i,int j,int k){return V3(i*PS+PS*0.5f, j*PS+PS*0.5f, k*PS+PS*0.5f);}

  const char* name() override { return "DDGI"; }

  void init() override {
    PNX=NX/PS; PNY=NY/PS; PNZ=NZ/PS;
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

  void onEvent(const SceneEvent& e) override {
    if(e.geomEdit) markAlive();
    // lightChange only: nothing (hysteresis adapts)
  }

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

  long frame(long budget) override {
    uint64_t r0=tl_rays;
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
      RNG rng((uint64_t)pi,(uint64_t)frameCounter,202);
      for(int q=0;q<NDIR;q++){
        Hit h; V3 newRad{}; float newD=64.f;
        if(trace(pp,dirs[q],1e9f,h)){
          newD=h.t;
          if(!emissive(h.m)){
            V3 hp=facePoint(h.vx,h.vy,h.vz,h.face);
            V3 hn=FN[h.face];
            V3 Ed=neeIrr(hp,hn,rng);
            V3 Ei=sample(hp,hn);                       // probe feedback -> multibounce
            newRad = MAT[h.m].alb*((Ed+Ei)*(1.f/3.14159265f));
          }
        } else {
          newRad = SKY.skyL(dirs[q]); newD = 64.f;
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
    frameCounter++;
    long used=(long)(tl_rays-r0);
    flushCounters();
    return used;
  }
};
