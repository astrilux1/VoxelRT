// Method C: FaceCache-GI (proposed) (ported from prior_work/bench.cpp MethodFC)
// Per exposed voxel face irradiance cache + cache-feedback multibounce +
// visibility-prioritized amortized updates + brick-local invalidation on edits.
//
// Storage: packed sparse (mirrors gfc.h). Dense NX*NY*NZ*6 caches don't scale
// to 10M-voxel scenes (1GB+), so we keep only exposed faces:
//   faceId[packed] -> global face id (vox*6+f)
//   slot[global face id] -> packed index
//   cache[packed]  -> Cell (fine per-face cache)
//   l1[brickFaceId] -> L1Cell (coarse per-brick-face cache, feedback cascade)
// brickFaceId = ((z/4)*BNY+(y/4))*BNX+(x/4))*6+f
#pragma once
#include "../technique.h"
#include <unordered_map>



struct MethodFC2 : Technique {
  struct Cell{ V3 E{}; float n=0; };
  struct L1Cell{ V3 E{}; float n=0; std::vector<int> fineIdx; };
  std::vector<long> faceId;                 // packed -> global face id (vox*6+f)
  std::vector<Cell> cache;                  // L0: fine cache, indexed by packed slot
  std::unordered_map<long,int> slot;        // global face id -> packed index
  std::unordered_map<long,L1Cell> l1;       // L1: brickFaceId -> coarse cache (feedback cascade)
  std::vector<long> l1Active;               // active L1 brick-face ids
  std::vector<int> visList;                 // packed indices visible in G-buffer
  int curVis=0, curL1=0;
  int frameCounter=0;

  static long fid(int x,int y,int z,int f){ return ((long)gi(x,y,z))*6+f; }
  static long cid(int x,int y,int z,int f){ return ((long)((z/BK)*BNY+(y/BK))*BNX+(x/BK))*6+f; }

  const char* name() override { return "FCGI2"; }

  void rebuildIndex(){
    faceId.clear(); slot.clear(); l1.clear(); l1Active.clear();
    for(int z=0;z<NZ;z++)for(int y=0;y<NY;y++)for(int x=0;x<NX;x++){
      uint8_t m=at(x,y,z); if(!m||emissive(m)) continue;
      for(int f=0;f<6;f++){int xx=x+OXf[f],yy=y+OYf[f],zz=z+OZf[f];
        if(inB(xx,yy,zz)&&at(xx,yy,zz)==0){
          long id=fid(x,y,z,f);
          slot[id]=(int)faceId.size();
          faceId.push_back(id);
          long c=cid(x,y,z,f);
          auto& cell=l1[c]; cell.fineIdx.push_back((int)faceId.size()-1);
        }}
    }
    cache.assign(faceId.size(),{});
    for(auto& kv:l1) l1Active.push_back(kv.first);
  }

  // carry cache across edits: copy entries whose face id persists
  void rebuildIndexCarry(){
    std::unordered_map<long,Cell> keep; keep.reserve(faceId.size());
    for(size_t i=0;i<faceId.size();i++) keep[faceId[i]]=cache[i];
    std::unordered_map<long,L1Cell> keepL1; keepL1.swap(l1);
    rebuildIndex();
    for(size_t i=0;i<faceId.size();i++){ auto it=keep.find(faceId[i]); if(it!=keep.end()) cache[i]=it->second; }
    for(auto& kv:l1){ auto it=keepL1.find(kv.first); if(it!=keepL1.end()){ kv.second.E=it->second.E; kv.second.n=it->second.n; } }
  }

  void rebuildVisible(){
    visList.clear();
    std::vector<uint8_t> seen(cache.size(),0);
    for(int i=0;i<W*H;i++){ GPix& g=gbuf[i]; if(!g.hit||emissive(g.m))continue;
      auto it=slot.find(fid(g.vx,g.vy,g.vz,g.face));
      if(it!=slot.end() && !seen[it->second]){ seen[it->second]=1; visList.push_back(it->second); } }
  }

  void init() override {
    out.assign(W*H,{});
    rebuildIndex(); rebuildVisible();
  }

  void onEvent(const SceneEvent& e) override {
    if(e.geomEdit){
      rebuildIndexCarry(); rebuildVisible();
      // brick-local invalidation: age-reset faces near the edit so they re-converge fast
      float r2 = (e.radius+26.f)*(e.radius+26.f);
      for(size_t i=0;i<faceId.size();i++){
        long id=faceId[i]; long v=id/6; int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
        float dx=x-e.center.x,dy=y-e.center.y,dz=z-e.center.z;
        float d2=dx*dx+dy*dy+dz*dz;
        if(d2 < r2) cache[i].n = std::min(cache[i].n, 2.f);
      }
      for(auto& kv:l1){
        long b=kv.first/6; int bx=b%BNX, by=(b/BNX)%BNY, bz=b/((long)BNX*BNY);
        float dx=bx*BK+BK*0.5f-e.center.x, dy=by*BK+BK*0.5f-e.center.y, dz=bz*BK+BK*0.5f-e.center.z;
        if(dx*dx+dy*dy+dz*dz < r2) kv.second.n = std::min(kv.second.n, 2.f);
      }
    } else if(e.lightChange){
      // global relight must re-converge fast: clamp ALL fine cache n and ALL active L1 n
      for(size_t i=0;i<cache.size();i++) cache[i].n = std::min(cache[i].n, 2.f);
      for(auto& kv:l1) kv.second.n = std::min(kv.second.n, 2.f);
    }
  }

  V3 faceCenter(long id,V3& n){
    int f=id%6; long v=id/6; int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
    n=FN[f];
    return facePoint(x,y,z,f);
  }

  V3 gatherE(V3 p, V3 n, int S, RNG& rng, int strat0=-1){
    V3 E{};
    for(int s=0;s<S;s++){
      V3 d;
      if(strat0>=0){
        int st=strat0+s;
        float u2=(st%16 + rng.uf())*(1.f/16.f);
        float u1=((st/16)%4 + rng.uf())*(1.f/4.f);
        float rr=std::sqrt(u1), phi=6.2831853f*u2;
        V3 tu=(std::fabs(n.x)>0.5f)?V3(0,1,0):V3(1,0,0);
        V3 tv=norm(cross(n,tu)); tu=cross(tv,n);
        d=norm(tu*(rr*std::cos(phi))+tv*(rr*std::sin(phi))+n*std::sqrt(std::max(0.f,1-u1)));
      } else d=cosDir(n,rng);
      Hit h;
      if(!trace(p,d,1e9f,h)){ E += SKY.skyL(d)*3.14159265f; continue; }
      if(emissive(h.m)) continue;
      V3 hn=FN[h.face];
      V3 hp=facePoint(h.vx,h.vy,h.vz,h.face);
      V3 Ed=neeIrr(hp,hn,rng);
      auto it=l1.find(cid(h.vx,h.vy,h.vz,h.face));
      V3 Ei = (it!=l1.end())? it->second.E : V3{};   // coarse-cascade feedback -> infinite bounces
      E += MAT[h.m].alb*(Ed+Ei);
    }
    return E*(1.f/S);
  }

  void updateFace(int idx){
    long id=faceId[idx];
    V3 n; V3 p=faceCenter(id,n);
    RNG rng((uint64_t)id,(uint64_t)curVis,303);
    Cell& c=cache[idx];
    V3 E=gatherE(p,n,4,rng,(int)(c.n*4));
    float a = std::max(1.f/(c.n+1.f), 0.05f);  // adaptive blend: fast warmup, stable steady state
    c.E = c.E*(1-a)+E*a; c.n+=1;
  }

  void updateL1(long id){
    auto it=l1.find(id); if(it==l1.end()) return;
    auto& fl=it->second.fineIdx; if(fl.empty()) return;
    RNG rng((uint64_t)id,(uint64_t)curL1,303);
    int idx=fl[rng.u32()%fl.size()];          // stochastic representative face within brick
    long fineId=faceId[idx];
    V3 n; V3 p=faceCenter(fineId,n);
    L1Cell& c=it->second;
    V3 E=gatherE(p,n,4,rng,(int)(c.n*4));
    float a = std::max(1.f/(c.n+1.f), 0.10f);
    c.E = c.E*(1-a)+E*a; c.n+=1;
  }

  // bilinear interpolation of cache across the face plane
  V3 shade(GPix& g){
    int f=g.face;
    // local position within voxel on the face plane
    float lu,lv; int au,av; // tangent axes
    int x=g.vx,y=g.vy,z=g.vz;
    V3 lp = g.pos - V3((float)x,(float)y,(float)z);
    if(f<2){ au=1;av=2; lu=lp.y; lv=lp.z; }
    else if(f<4){ au=0;av=2; lu=lp.x; lv=lp.z; }
    else { au=0;av=1; lu=lp.x; lv=lp.y; }
    float fu=lu-0.5f, fv=lv-0.5f;
    V3 acc{}; float ws=0;
    for(int ou=-1;ou<=1;ou++)for(int ov=-1;ov<=1;ov++){
      int xx=x,yy=y,zz=z;
      if(au==0)xx+=ou; else if(au==1)yy+=ou; else zz+=ou;
      if(av==0)xx+=ov; else if(av==1)yy+=ov; else zz+=ov;
      if(!inB(xx,yy,zz)||at(xx,yy,zz)==0||emissive(at(xx,yy,zz))) continue;   // same-plane surface only
      int fx=xx+OXf[f],fy=yy+OYf[f],fz=zz+OZf[f];
      if(!inB(fx,fy,fz)||at(fx,fy,fz)!=0) continue;                            // face must be exposed
      long id=fid(xx,yy,zz,f);
      auto it=slot.find(id); if(it==slot.end()) continue;                      // missing => skip neighbor
      Cell& cl=cache[it->second]; if(cl.n<0.5f) continue;
      float du=ou-fu, dv=ov-fv;
      float w=std::exp(-(du*du+dv*dv)/1.1f);
      acc+=cl.E*w; ws+=w;
    }
    auto lit=l1.find(cid(x,y,z,f));
    V3 cE = (lit!=l1.end())? lit->second.E : V3{};
    if(ws<0.35f){ acc += cE*(0.35f-ws); ws=0.35f; }   // coarse-cascade fallback
    return acc*(1.f/ws);
  }

  long frame(long budget) override {
    uint64_t r0=tl_rays;
    const long raysPerUpdate=8; // 4 hemi + 4 shadow
    long updates = std::max(1L,budget/raysPerUpdate);
    long visU = (long)(updates*0.6), l1U=updates-visU;
    for(long u=0;u<visU && !visList.empty();u++){
      updateFace(visList[curVis%visList.size()]); curVis++;
    }
    for(long u=0;u<l1U && !l1Active.empty();u++){
      updateL1(l1Active[curL1%l1Active.size()]); curL1++;
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
