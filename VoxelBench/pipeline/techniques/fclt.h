// Method FCLT: FaceCache Link Transport.
//
// R&D goal: test the "Lumen-like surface cache, but voxel-thin-wall exact"
// hypothesis. FCGI retraces stochastic gather rays every frame. FCLT traces a
// fixed cosine set per exposed voxel face into stable transport links, then
// updates indirect lighting by cache-space propagation:
//
//   E_i <- avg_k sky(d_k) or albedo(hit_k) * (direct_hit + E_hit)
//
// Runtime cost is K cache reads per face, not K DDA rays per face. Thin walls
// remain safe because every link is an exact voxel-face hit key, not a card,
// SDF, mip, or probe lookup. This first version is an upper-bound experiment:
// link/direct rebuilds happen in init/onEvent; a production version would
// rebuild only links whose segment intersects the edited brick AABB.
#pragma once
#include "../technique.h"
#include "fcgi.h"

struct MethodFCLT : MethodFC {
  static constexpr float TRACE_TMAX = 384.f; // 24m at 6.25cm voxels
  int linkCount = 24;              // fixed transport directions/face
  int directSamples = 8;           // emissive NEE samples/face
  const char* methodName = "FCLT";

  struct Link {
    int hit = -1;       // packed face index, or -1 for miss/invalid
    uint8_t mat = 0;    // hit material for albedo; 0 for sky/invalid
    V3 sky{};           // irradiance contribution for miss
  };

  std::vector<Link> links;         // flattened [face * linkCount + k]
  std::vector<V3> directFace;
  std::vector<V3> nextE;
  int frameCounterLT=0;

  MethodFCLT(int linksPerFace=24, int directPerFace=8, const char* nm="FCLT")
    : linkCount(linksPerFace), directSamples(directPerFace), methodName(nm) {}

  const char* name() override { return methodName; }

  V3 neeIrrLimited(V3 p, V3 n, RNG& rng){
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
      if(d > 1e-6f && d <= TRACE_TMAX){
        w = w*(1.f/d);
        float cs = dot(w,n), cl = -dot(w,fn);
        if(cs>0&&cl>0){
          Hit h;
          if(trace(p,w,d+1.f,h) && emissive(h.m) && h.t >= d-0.87f){
            float pdfA = 1.f/(float)emFaces.size();
            acc += MAT[e.m].emi*(cs*cl/(d2*pdfA+1e-9f));
          }
        }
      }
    }
    if(SKY.enabled && (SKY.sunE.x+SKY.sunE.y+SKY.sunE.z)>0){
      float cs = dot(SKY.sunDir,n);
      if(cs>0){
        Hit h;
        if(!trace(p,SKY.sunDir,TRACE_TMAX,h)) acc += SKY.sunE*cs;
      }
    }
    return acc;
  }

  void rebuildTransport(){
    links.assign(faceId.size()*(size_t)linkCount,{});
    directFace.assign(faceId.size(),{});
    nextE.assign(faceId.size(),{});

    for(size_t i=0;i<faceId.size();i++){
      V3 n; V3 p=faceCenter(faceId[i],n);
      RNG drng((uint64_t)faceId[i], 17, 1701);
      V3 ed{};
      for(int s=0;s<directSamples;s++) ed += neeIrrLimited(p,n,drng);
      directFace[i] = ed*(1.f/(float)directSamples);

      RNG lrng((uint64_t)faceId[i], 23, 2301);
      for(int k=0;k<linkCount;k++){
        V3 d = cosDir(n,lrng);
        Hit h;
        Link L;
        if(!trace(p,d,TRACE_TMAX,h)){
          L.sky = SKY.skyL(d)*3.14159265f;
        } else if(!emissive(h.m)){
          auto it = slot.find(fid(h.vx,h.vy,h.vz,h.face));
          if(it!=slot.end()){
            L.hit = it->second;
            L.mat = h.m;
          }
        }
        links[i*(size_t)linkCount + k]=L;
      }
    }
    refreshL1FromFine();
  }

  void refreshL1FromFine(){
    for(auto& kv:l1){
      V3 e{}; int cnt=0;
      for(int idx:kv.second.fineIdx){
        if(idx>=0 && idx<(int)cache.size()){
          e += cache[idx].E;
          cnt++;
        }
      }
      if(cnt>0){
        kv.second.E = e*(1.f/(float)cnt);
        kv.second.n = 1.f;
      }
    }
  }

  void propagateOnce(){
    nextE.assign(cache.size(),{});
    for(size_t i=0;i<cache.size();i++){
      V3 e{};
      for(int k=0;k<linkCount;k++){
        const Link& L=links[i*(size_t)linkCount + k];
        if(L.hit>=0 && L.hit<(int)cache.size()){
          e += MAT[L.mat].alb * (directFace[L.hit] + cache[L.hit].E);
        } else {
          e += L.sky;
        }
      }
      nextE[i] = e*(1.f/(float)linkCount);
    }
    for(size_t i=0;i<cache.size();i++){
      cache[i].E = nextE[i];
      cache[i].n = 1.f;
    }
    refreshL1FromFine();
  }

  void init() override {
    MethodFC::init();
    rebuildTransport();
  }

  void onEvent(const SceneEvent& e) override {
    MethodFC::onEvent(e);
    // Upper-bound experiment: rebuild all links/direct terms. Production path:
    // carry links by face id, then retrace only links touching edited bricks.
    rebuildTransport();
  }

  long frame(long budget) override {
    uint64_t r0=tl_rays;
    long sweeps = 1;
    if(!cache.empty()){
      long evalsPerSweep = (long)cache.size() * linkCount;
      sweeps = std::max(1L, std::min(8L, budget / std::max(1L, evalsPerSweep)));
    }
    for(long s=0;s<sweeps;s++) propagateOnce();

    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      out[i]= (g.hit&&!emissive(g.m))? g.alb*(shade(g)*(1.f/3.14159265f)) : V3{};
    }
    frameCounterLT++;
    long used=(long)(tl_rays-r0);
    flushCounters();
    return used;
  }
};

struct MethodFCLT64 : MethodFCLT {
  MethodFCLT64() : MethodFCLT(64, 24, "FCLT64") {}
};
