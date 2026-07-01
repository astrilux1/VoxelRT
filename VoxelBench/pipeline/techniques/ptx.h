// PTX: exact progressive path tracing with optional exact ODF traversal.
//
// This is intentionally closer to ground-truth path tracing than MethodPT:
// - no spatial denoiser / blur;
// - cumulative per-pixel mean, reset on scene events;
// - same NEE path estimator, but traversal can use ODF when a small probe says
//   it is faster for the current scene. ODF is exact: 0-mismatch traversal,
//   not a lighting approximation.
#pragma once
#include "../technique.h"
#include "../accel2.h"

struct MethodPTX : Technique {
  ODF odf;
  bool useOdf=false;
  std::vector<V3> hist;
  std::vector<float> n;
  int fcount=0;
  static constexpr float TRACE_TMAX = 384.f; // 24m render distance

  const char* name() override { return "PTX"; }

  bool traceFast(V3 o,V3 d,float tmax,Hit& h){
    float tm = std::min(tmax, TRACE_TMAX);
    return useOdf ? traceODF(odf,o,d,tm,h) : trace(o,d,tm,h);
  }

  V3 neeIrrFast(V3 p, V3 nn, RNG& rng){
    V3 acc{};
    if(!emFaces.empty()){
      int i = rng.u32()%emFaces.size();
      EmFace e = emFaces[i];
      V3 fn = FN[e.f];
      V3 q = facePoint(e.x,e.y,e.z,e.f);
      V3 tu = (std::fabs(fn.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
      V3 tv = cross(fn,tu);
      q += tu*(rng.uf()-0.5f) + tv*(rng.uf()-0.5f);
      V3 w = q - p; float d2 = dot(w,w); float d = std::sqrt(d2);
      if(d>1e-6f && d <= TRACE_TMAX){
        w = w*(1.f/d);
        float cs=dot(w,nn), cl=-dot(w,fn);
        if(cs>0&&cl>0){
          Hit h;
          if(traceFast(p,w,d+1.f,h) && emissive(h.m) && h.t >= d-0.87f){
            float pdfA = 1.f/(float)emFaces.size();
            acc += MAT[e.m].emi*(cs*cl/(d2*pdfA+1e-9f));
          }
        }
      }
    }
    if(SKY.enabled && (SKY.sunE.x+SKY.sunE.y+SKY.sunE.z)>0){
      float cs=dot(SKY.sunDir,nn);
      if(cs>0){
        Hit h;
        if(!traceFast(p,SKY.sunDir,TRACE_TMAX,h)) acc += SKY.sunE*cs;
      }
    }
    return acc;
  }

  V3 pathSample(V3 p,V3 nn,RNG& rng){
    V3 tp{1,1,1}, acc{};
    for(int b=0;b<8;b++){
      V3 d=cosDir(nn,rng); Hit h;
      if(!traceFast(p,d,TRACE_TMAX,h)){
        acc += tp*(SKY.skyL(d)*3.14159265f);
        break;
      }
      if(emissive(h.m)) break; // emitters are sampled by NEE
      V3 hp=facePoint(h.vx,h.vy,h.vz,h.face);
      V3 hn=FN[h.face];
      tp = tp*MAT[h.m].alb;
      acc += tp*neeIrrFast(hp,hn,rng);
      if(b>=1){
        float rr=0.7f;
        if(rng.uf()>=rr) break;
        tp=tp*(1.f/rr);
      }
      p=hp; nn=hn;
    }
    return acc;
  }

  void probeTraversal(){
    std::vector<std::pair<V3,V3>> pts;
    for(int i=0;i<W*H && pts.size()<512;i+=std::max(1,(W*H)/512)){
      GPix& g=gbuf[i];
      if(g.hit && !emissive(g.m)) pts.push_back({g.pos,g.n});
    }
    if(pts.empty()){ useOdf=false; return; }
    RNG rng(88,12,991);
    std::vector<V3> dirs; dirs.reserve(pts.size()*4);
    for(auto& pn:pts) for(int s=0;s<4;s++) dirs.push_back(cosDir(pn.second,rng));
    Hit h;
    for(size_t i=0;i<pts.size();i+=8){
      trace(pts[i].first,dirs[i*4],TRACE_TMAX,h);
      traceODF(odf,pts[i].first,dirs[i*4],TRACE_TMAX,h);
    }
    auto t0=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<pts.size();i++) for(int s=0;s<4;s++) trace(pts[i].first,dirs[i*4+s],TRACE_TMAX,h);
    auto t1=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<pts.size();i++) for(int s=0;s<4;s++) traceODF(odf,pts[i].first,dirs[i*4+s],TRACE_TMAX,h);
    auto t2=std::chrono::high_resolution_clock::now();
    double brickNs=std::chrono::duration<double,std::nano>(t1-t0).count();
    double odfNs=std::chrono::duration<double,std::nano>(t2-t1).count();
    useOdf = odfNs < brickNs;
    flushCounters();
  }

  void init() override {
    hist.assign(W*H,{});
    n.assign(W*H,0);
    out.assign(W*H,{});
    odf.build();
    probeTraversal();
  }

  void onEvent(const SceneEvent& e) override {
    (void)e;
    hist.assign(W*H,{});
    n.assign(W*H,0);
    odf.build();
    probeTraversal();
  }

  long frame(long budget) override {
    double ppp = (double)budget / ((double)(W*H)*5.0);
    long basePaths = std::max(1L,(long)std::floor(ppp));
    double fracP = (ppp>1.0)? ppp-std::floor(ppp) : 0.0;
    int stride = (ppp<1.0)? (int)std::round(1.0/ppp) : 1;
    uint64_t r0=tl_rays;

    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      if(!g.hit||emissive(g.m)) continue;
      if(stride>1 && ((i+fcount)%stride)!=0) continue;
      RNG rng((uint64_t)i,(uint64_t)fcount,17017);
      long paths = basePaths + ((rng.uf()<fracP)?1:0);
      V3 E{};
      for(long s=0;s<paths;s++) E += pathSample(g.pos,g.n,rng);
      E = E*(1.f/(float)paths);
      float old=n[i];
      n[i]=old+1.f;
      float a=1.f/n[i];
      hist[i]=hist[i]*(1.f-a)+E*a;
    }
    fcount++;
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      out[i]= g.hit ? g.alb*(hist[i]*(1.f/3.14159265f)) : V3{};
    }
    long used=(long)(tl_rays-r0);
    flushCounters();
    return used;
  }
};
