// Method FCGIX: FCGI with ODF (octant distance field) traversal — EXACT hits,
// bit-identical output to FCGI, lower wall time. This is a pure traversal
// substitution: gather rays and sun-NEE shadow rays trace via the ODF
// (validated 0 mismatches vs naive on every scene/workload); emissive-NEE
// rays keep core trace() (they are short and bounded — ODF's extra distance
// load loses there, measured on city emshadow).
//
// Far-field LOD (terminate at first occupied brick beyond ~1.5m) was tried
// and REJECTED: entry-face fallback hits poison grazing rays that skim the
// one-voxel air layer above dense ground bricks (-4 dB on town, see rnd-log
// 2026-06-11). Exact traversal is the production configuration.
//
// Edit response (next-frame requirement): the face cache is world-space, so
// camera motion costs nothing; geometry edits flip brick bits O(1) and the
// ODF is conservative-stale for removals (correct immediately, re-sharpened
// by relaxBudget inside frame()); placements would call onPlace (bounded
// window, world-size independent).
#pragma once
#include "../technique.h"
#include <chrono>
#include "../accel2.h"
#include "fcgi.h"

struct MethodFCX : MethodFC {
  ODF odf;
  bool useOdf=true;
  const char* name() override { return "FCGIX"; }

  // Auto traversal selection (ATS): cramped interiors (bunker/cavern) have no
  // room to skip — ODF's extra distance load per empty step loses to plain
  // brick DDA there, while open scenes (town/city) gain 1.5x+. Probe ~2k real
  // gather rays from visible faces at init and pick the measured winner.
  void probeTraversal(){
    std::vector<std::pair<V3,V3>> pts;
    for(size_t i=0;i<visList.size() && pts.size()<256;i+=std::max((size_t)1,visList.size()/256)){
      V3 n; V3 p=faceCenter(faceId[visList[i]],n); pts.push_back({p,n});
    }
    if(pts.empty()){ useOdf=true; return; }
    RNG rng(7,7,7);
    std::vector<V3> dirs; dirs.reserve(pts.size()*8);
    for(auto& pn:pts) for(int s8=0;s8<8;s8++) dirs.push_back(cosDir(pn.second,rng));
    Hit h;
    // warmup both paths (cold-cache bias otherwise favors whichever runs 2nd)
    for(size_t i=0;i<pts.size();i+=4){ trace(pts[i].first,dirs[i*8],1e9f,h); traceODF(odf,pts[i].first,dirs[i*8],1e9f,h); }
    auto t0=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<pts.size();i++) for(int s8=0;s8<8;s8++) trace(pts[i].first,dirs[i*8+s8],1e9f,h);
    auto t1=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<pts.size();i++) for(int s8=0;s8<8;s8++) traceODF(odf,pts[i].first,dirs[i*8+s8],1e9f,h);
    auto t2=std::chrono::high_resolution_clock::now();
    double brickNs=std::chrono::duration<double,std::nano>(t1-t0).count();
    double odfNs  =std::chrono::duration<double,std::nano>(t2-t1).count();
    useOdf = odfNs < brickNs;
    flushCounters();
  }

  void init() override {
    MethodFC::init();
    odf.build();
    probeTraversal();
  }
  void onEvent(const SceneEvent& e) override {
    MethodFC::onEvent(e);
    // removals: ODF stale-but-conservative (still exact traversal, only
    // slightly under-skips near the edit until relaxBudget re-sharpens).
    // placements: odf.onPlace(e.center, e.radius) — bounded O(1).
  }

  // neeIrr with sun visibility via ODF (exact); emissive ray via core trace()
  V3 neeIrrX(V3 p, V3 n, RNG& rng){
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
        bool blocked = useOdf? traceODF(odf,p,SKY.sunDir,1e9f,h) : trace(p,SKY.sunDir,1e9f,h);
        if(!blocked) acc += SKY.sunE*cs;
      }
    }
    return acc;
  }

  V3 gatherEX(V3 p, V3 n, int S, RNG& rng){
    V3 E{};
    for(int s=0;s<S;s++){
      V3 d=cosDir(n,rng); Hit h;
      bool hit = useOdf? traceODF(odf,p,d,1e9f,h) : trace(p,d,1e9f,h);
      if(!hit){ E += SKY.skyL(d)*3.14159265f; continue; }
      if(emissive(h.m)) continue;
      V3 hn=FN[h.face];
      V3 hp=facePoint(h.vx,h.vy,h.vz,h.face);
      V3 Ed=neeIrrX(hp,hn,rng);
      auto it=l1.find(cid(h.vx,h.vy,h.vz,h.face));
      V3 Ei = (it!=l1.end())? it->second.E : V3{};
      E += MAT[h.m].alb*(Ed+Ei);
    }
    return E*(1.f/S);
  }

  void updateFaceX(int idx){
    long id=faceId[idx];
    V3 n; V3 p=faceCenter(id,n);
    RNG rng((uint64_t)id,(uint64_t)curVis,303);
    V3 E=gatherEX(p,n,4,rng);
    Cell& c=cache[idx];
    float a = std::max(1.f/(c.n+1.f), 0.08f);
    c.E = c.E*(1-a)+E*a; c.n+=1;
  }
  void updateL1X(long id){
    auto it=l1.find(id); if(it==l1.end()) return;
    auto& fl=it->second.fineIdx; if(fl.empty()) return;
    RNG rng((uint64_t)id,(uint64_t)curL1,303);
    int idx=fl[rng.u32()%fl.size()];
    long fineId=faceId[idx];
    V3 n; V3 p=faceCenter(fineId,n);
    V3 E=gatherEX(p,n,4,rng);
    L1Cell& c=it->second;
    float a = std::max(1.f/(c.n+1.f), 0.12f);
    c.E = c.E*(1-a)+E*a; c.n+=1;
  }

  long frame(long budget) override {
    uint64_t r0=tl_rays;
    odf.relaxBudget(4096);                    // staleness recovery, in-budget
    const long raysPerUpdate=8;
    long updates = std::max(1L,budget/raysPerUpdate);
    long visU = (long)(updates*0.6), l1U=updates-visU;
    for(long u=0;u<visU && !visList.empty();u++){
      updateFaceX(visList[curVis%visList.size()]); curVis++;
    }
    for(long u=0;u<l1U && !l1Active.empty();u++){
      updateL1X(l1Active[curL1%l1Active.size()]); curL1++;
    }
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      out[i]= (g.hit&&!emissive(g.m))? g.alb*(shade(g)*(1.f/3.14159265f)) : V3{};
    }
    frameCounter++;
    long used=(long)(tl_rays-r0);
    flushCounters();
    return used;
  }
};
