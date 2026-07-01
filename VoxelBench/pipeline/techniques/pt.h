// Method A: 1spp PT + temporal + spatial filter (ported from prior_work/bench.cpp MethodPT)
#pragma once
#include "../technique.h"

struct MethodPT : Technique {
  std::vector<V3> hist; std::vector<float> n;
  float alpha=0.15f;
  int fcount=0;

  const char* name() override { return "PT"; }

  void init() override {
    hist.assign(W*H,{}); n.assign(W*H,0); out.assign(W*H,{});
  }

  void onEvent(const SceneEvent& e) override {
    // geomEdit: history kept (matches old onEdit, no-op).
    // lightChange only: also nothing (history continues to adapt).
    (void)e;
  }

  // budget in rays; PT uses pathsPerPixel paths, each path = 2 bounce rays + 2 shadow rays
  long frame(long budget) override {
    double ppp = (double)budget / ((double)(W*H)*5.0);
    long basePaths = std::max(1L,(long)std::floor(ppp));
    double fracP = (ppp>1.0)? ppp-std::floor(ppp) : 0.0;
    int stride = (ppp<1.0)? (int)std::round(1.0/ppp) : 1;
    uint64_t r0=tl_rays;
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i]; if(!g.hit||emissive(g.m)) continue;
      if(stride>1 && ((i+fcount)%stride)!=0) continue;
      RNG rng((uint64_t)i,(uint64_t)fcount,101);
      long paths = basePaths + ((rng.uf()<fracP)?1:0);
      V3 E{};
      for(long s=0;s<paths;s++){
        // NEE path, RR-extended to depth 4, estimating E_ind
        V3 p=g.pos, nn=g.n, tp{1,1,1}; V3 acc{};
        for(int b=0;b<8;b++){
          V3 d=cosDir(nn,rng); Hit h;
          if(!trace(p,d,1e9f,h)){ acc += tp*(SKY.skyL(d)*3.14159265f); break; }
          if(emissive(h.m)) break;
          V3 hp=facePoint(h.vx,h.vy,h.vz,h.face);
          V3 hn=FN[h.face]; tp = tp*MAT[h.m].alb;
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
    long used=(long)(tl_rays-r0);
    flushCounters();
    return used;
  }
};
