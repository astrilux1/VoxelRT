// GFC — Gather-Cached GI (novel technique #1).
//
// Prior FaceCache-GI read the per-face irradiance cache DIRECTLY at shading,
// which is flat and blurry (one value per 6.25cm face, cache-space filtering).
// GFC inverts the read: the face cache stores OUTGOING RADIOSITY B per exposed
// face (direct+indirect, converged via feedback => infinite bounces), and each
// PIXEL traces its own cosine gather rays that terminate at the FIRST hit,
// returning pi*B(hitFace). Per-pixel gather restores contact occlusion,
// per-pixel gradients and directionality; rays are strictly 1 segment (bounded
// cost, perfectly coherent w.r.t. the cache); the cache provides variance-free
// multibounce. Temporal accumulation + 2-pass edge-aware a-trous denoise.
// Same structural wins as FCGI: surface anchoring (no leaks), 6-normal collapse
// (exact cache key), brick-local O(1) invalidation on edits.
#pragma once
#include "../technique.h"
#include <unordered_map>

struct MethodGFC : Technique {
  static const int BK=4;
  struct FCell { V3 Ed{}, Ei{}; float nd=0, ni=0; };   // direct/indirect irradiance at face
  struct L1Cell { V3 B{}; float n=0; std::vector<int> fineIdx; };
  // packed exposed-face storage
  std::vector<long> faceId;                 // packed -> global face id (vox*6+f)
  std::vector<FCell> fc;
  std::unordered_map<long,int> slot;        // global face id -> packed index
  std::unordered_map<long,L1Cell> l1;       // (brick*6+f) -> cell
  std::vector<long> l1Ids;
  std::vector<int> visIdx;                  // packed indices visible in gbuffer
  // per-pixel temporal state
  std::vector<V3> pixE; std::vector<float> pixN; std::vector<V3> pixDev;
  std::vector<int> pixFace;                 // primary face id last frame (history validity)
  int frameCounter=0; size_t curVis=0, curBg=0, curL1=0;

  const char* name() override { return "GFC"; }

  static long fidOf(int x,int y,int z,int f){ return ((long)gi(x,y,z))*6+f; }
  long cidOf(int x,int y,int z,int f){ return ((long)((z/BK)*BNY+(y/BK))*BNX+(x/BK))*6+f; }

  void rebuildIndex(){
    faceId.clear(); slot.clear(); l1.clear(); l1Ids.clear();
    std::vector<FCell> old; old.swap(fc);
    std::unordered_map<long,int> oldSlot; // keep old values across rebuild
    oldSlot.swap(slot); // note: slot was just cleared; use copy below instead
    for(int z=0;z<NZ;z++)for(int y=0;y<NY;y++)for(int x=0;x<NX;x++){
      uint8_t m=at(x,y,z); if(!m||emissive(m)) continue;
      for(int f=0;f<6;f++){int xx=x+OXf[f],yy=y+OYf[f],zz=z+OZf[f];
        if(inB(xx,yy,zz)&&at(xx,yy,zz)==0){
          long id=fidOf(x,y,z,f);
          slot[id]=(int)faceId.size();
          faceId.push_back(id);
          long c=cidOf(x,y,z,f);
          auto& cell=l1[c]; cell.fineIdx.push_back((int)faceId.size()-1);
        }}
    }
    fc.assign(faceId.size(),{});
    for(auto& kv:l1) l1Ids.push_back(kv.first);
    // carry over old cache values where faces persist
    (void)old; // values carried via carryMap built by caller (see onEvent)
  }
  // carry cache across edits: copy entries whose face id persists
  void rebuildIndexCarry(){
    std::unordered_map<long,FCell> keep; keep.reserve(faceId.size());
    for(size_t i=0;i<faceId.size();i++) keep[faceId[i]]=fc[i];
    std::unordered_map<long,L1Cell> keepL1; keepL1.swap(l1);
    rebuildIndex();
    for(size_t i=0;i<faceId.size();i++){ auto it=keep.find(faceId[i]); if(it!=keep.end()) fc[i]=it->second; }
    for(auto& kv:l1){ auto it=keepL1.find(kv.first); if(it!=keepL1.end()){ kv.second.B=it->second.B; kv.second.n=it->second.n; } }
  }
  void rebuildVisible(){
    visIdx.clear();
    std::vector<uint8_t> seen(fc.size(),0);
    for(int i=0;i<W*H;i++){ GPix& g=gbuf[i]; if(!g.hit||emissive(g.m))continue;
      auto it=slot.find(fidOf(g.vx,g.vy,g.vz,g.face));
      if(it!=slot.end() && !seen[it->second]){ seen[it->second]=1; visIdx.push_back(it->second); } }
  }

  void init() override {
    out.assign(W*H,{});
    pixE.assign(W*H,{}); pixN.assign(W*H,0); pixDev.assign(W*H,{}); pixFace.assign(W*H,-1);
    rebuildIndex(); rebuildVisible();
  }

  void onEvent(const SceneEvent& e) override {
    if(e.geomEdit){
      rebuildIndexCarry(); rebuildVisible();
      float R=e.radius+26;
      for(size_t i=0;i<faceId.size();i++){
        long id=faceId[i]; long v=id/6; int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
        float dx=x-e.center.x,dy=y-e.center.y,dz=z-e.center.z;
        if(dx*dx+dy*dy+dz*dz<R*R){ fc[i].nd=std::min(fc[i].nd,2.f); fc[i].ni=std::min(fc[i].ni,2.f); }
      }
      for(auto& kv:l1){
        long b=kv.first/6; int bx=b%BNX, by=(b/BNX)%BNY, bz=b/((long)BNX*BNY);
        float dx=bx*BK+2-e.center.x, dy=by*BK+2-e.center.y, dz=bz*BK+2-e.center.z;
        if(dx*dx+dy*dy+dz*dz<R*R) kv.second.n=std::min(kv.second.n,2.f);
      }
      // pixel history: invalidate pixels whose primary surface changed (gbuffer rebuilt)
      for(int i=0;i<W*H;i++){
        GPix& g=gbuf[i];
        int f = (g.hit&&!emissive(g.m))? (int)fidOf(g.vx,g.vy,g.vz,g.face) : -1;
        if(f!=pixFace[i]){ pixN[i]=0; pixFace[i]=f; }
      }
    }
    if(e.lightChange){
      for(auto& c:fc){ c.nd=std::min(c.nd,1.f); c.ni=std::min(c.ni,2.f); }
      for(auto& kv:l1) kv.second.n=std::min(kv.second.n,2.f);
      for(int i=0;i<W*H;i++) pixN[i]=std::min(pixN[i],2.f);
    }
  }

  // outgoing radiosity of a face (cache read with L1 fallback)
  inline V3 faceB(int vx,int vy,int vz,int f,uint8_t m){
    auto it=slot.find(fidOf(vx,vy,vz,f));
    if(it!=slot.end()){
      FCell& c=fc[it->second];
      float w=std::min(1.f,c.ni*0.34f);                       // trust ramp
      V3 fine = MAT[m].alb*((c.Ed+c.Ei)*(1.f/3.14159265f));
      if(w>=1.f) return fine;
      auto l=l1.find(cidOf(vx,vy,vz,f));
      V3 coarse = (l!=l1.end())? l->second.B : V3{};
      return fine*w + coarse*(1-w);
    }
    auto l=l1.find(cidOf(vx,vy,vz,f));
    return (l!=l1.end())? l->second.B : V3{};
  }

  // update one face's cache: kh hemisphere rays (indirect, feedback) + 1 NEE (direct)
  void updateFace(int idx, uint64_t salt, int kh=2){
    long id=faceId[idx]; int f=id%6; long v=id/6;
    int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
    V3 n=FN[f]; V3 p=facePoint(x,y,z,f);
    RNG rng((uint64_t)id, salt, 404);
    V3 Ei{};
    for(int s=0;s<kh;s++){
      V3 d=cosDir(n,rng); Hit h;
      if(!trace(p,d,1e9f,h)){ Ei += SKY.skyL(d)*3.14159265f; continue; }
      if(emissive(h.m)) continue;
      Ei += faceB(h.vx,h.vy,h.vz,h.face,h.m)*3.14159265f;
    }
    Ei = Ei*(1.f/kh);
    V3 Ed = neeIrr(p,n,rng);
    FCell& c=fc[idx];
    float ai = std::max(1.f/(c.ni+1.f), 0.08f);
    float ad = std::max(1.f/(c.nd+1.f), 0.06f);
    c.Ei = c.Ei*(1-ai)+Ei*ai; c.ni+=1;
    c.Ed = c.Ed*(1-ad)+Ed*ad; c.nd+=1;
  }
  void updateL1(long cid, uint64_t salt){
    auto it=l1.find(cid); if(it==l1.end()||it->second.fineIdx.empty()) return;
    L1Cell& cell=it->second;
    RNG rng((uint64_t)cid, salt, 505);
    int idx=cell.fineIdx[rng.u32()%cell.fineIdx.size()];
    long id=faceId[idx]; int f=id%6; long v=id/6;
    int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
    V3 n=FN[f]; V3 p=facePoint(x,y,z,f);
    V3 Ei{};
    for(int s=0;s<2;s++){
      V3 d=cosDir(n,rng); Hit h;
      if(!trace(p,d,1e9f,h)){ Ei += SKY.skyL(d)*3.14159265f; continue; }
      if(emissive(h.m)) continue;
      Ei += faceB(h.vx,h.vy,h.vz,h.face,h.m)*3.14159265f;
    }
    Ei=Ei*0.5f;
    V3 Ed = neeIrr(p,n,rng);
    uint8_t m=at(x,y,z);
    V3 B = MAT[m].alb*((Ed+Ei)*(1.f/3.14159265f));
    float a=std::max(1.f/(cell.n+1.f),0.10f);
    cell.B = cell.B*(1-a)+B*a; cell.n+=1;
  }

  long frame(long budget) override {
    uint64_t r0=tl_rays;
    frameCounter++;
    // ---- budget split: per-pixel gather k rays; faces get the rest ----------
    int k = std::max(1, (int)std::lround(budget/(double)(4*W*H)));   // 1 ray/px at 1x
    long gatherCost = (long)W*H*k;
    long faceBudget = std::max(0L, budget-gatherCost);
    const long raysPerFaceUpdate = 2+2;            // 2 hemi + neeIrr(<=2)
    long fU = faceBudget/raysPerFaceUpdate;
    long visU = (long)(fU*0.45), l1U=(long)(fU*0.25), bgU = fU-visU-l1U;
    // visible faces first (round-robin), background faces (all-world round-robin), L1
    for(long u=0;u<visU && !visIdx.empty();u++){ updateFace(visIdx[curVis%visIdx.size()],frameCounter*131+u); curVis++; }
    for(long u=0;u<bgU && !fc.empty();u++){ updateFace((int)(curBg%fc.size()),frameCounter*131+u+77); curBg++; }
    for(long u=0;u<l1U && !l1Ids.empty();u++){ updateL1(l1Ids[curL1%l1Ids.size()],frameCounter*131+u); curL1++; }

    // ---- per-pixel gather ---------------------------------------------------
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      if(!g.hit||emissive(g.m)){ pixFace[i]=-1; continue; }
      pixFace[i]=(int)fidOf(g.vx,g.vy,g.vz,g.face);
      RNG rng((uint64_t)i, (uint64_t)frameCounter, 606);
      V3 E{};
      for(int s=0;s<k;s++){
        V3 d=cosDir(g.n,rng); Hit h;
        if(!trace(g.pos,d,1e9f,h)){ E += SKY.skyL(d)*3.14159265f; continue; }
        if(emissive(h.m)) continue;
        E += faceB(h.vx,h.vy,h.vz,h.face,h.m)*3.14159265f;
      }
      E=E*(1.f/k);
      // temporal accumulation with deviation-based outlier reset
      float n=pixN[i];
      V3 dev = pixDev[i];
      V3 diff = E-pixE[i];
      float dmag = std::fabs(diff.x)+std::fabs(diff.y)+std::fabs(diff.z);
      float dref = dev.x+dev.y+dev.z;
      float a = std::max(1.f/(n+1.f), 0.12f);
      if(n>8 && dmag > 4.f*dref+0.15f) { a=std::max(a,0.45f); pixN[i]=4; } // lighting changed
      pixE[i] = pixE[i]*(1-a)+E*a;
      pixDev[i] = dev*0.9f + V3(std::fabs(diff.x),std::fabs(diff.y),std::fabs(diff.z))*0.1f;
      pixN[i]+=1;
    }
    // ---- edge-aware a-trous (2 passes) on accumulated irradiance ------------
    static std::vector<V3> tmpA, tmpB;
    tmpA=pixE;
    auto pass=[&](std::vector<V3>& src, std::vector<V3>& dst, int step){
      dst.resize(W*H);
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit){dst[i]=src[i];continue;}
        V3 acc{}; float ws=0;
        for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){
          int xx=x+dx*step,yy=y+dy*step; if(xx<0||xx>=W||yy<0||yy>=H)continue;
          int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit)continue;
          if(dot(q.n,g.n)<0.9f) continue;
          if(std::fabs(q.depth-g.depth)>3.0f+0.02f*g.depth) continue;
          float w=std::exp(-(float)(dx*dx+dy*dy)/4.5f);
          // confidence weighting: trust accumulated pixels more
          w *= 0.25f+0.75f*std::min(1.f,pixN[j]*0.1f);
          acc+=src[j]*w; ws+=w;
        }
        dst[i]= (ws>0)? acc*(1.f/ws) : src[i];
      }
    };
    pass(tmpA,tmpB,1);
    pass(tmpB,tmpA,2);
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      out[i]= (g.hit&&!emissive(g.m))? g.alb*(tmpA[i]*(1.f/3.14159265f)) : V3{};
    }
    long used=(long)(tl_rays-r0); flushCounters(); return used;
  }
};
