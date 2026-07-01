// GFC-AO — cheap variant (novel technique #2).
// Far-field indirect comes straight from the face cache (FCGI-style bilinear
// read, no per-pixel gather of the full hemisphere); near-field detail is
// restored by ONE short per-pixel bent-occlusion ray (range ~1m) that modulates
// the cache value. Cost: ~1 short ray/pixel + face updates. Biased (AO darkens
// energy the reference keeps) but targets the flat look of FCGI at minimal
// cost. The question this method answers: is per-pixel *occlusion* enough, or
// do we need per-pixel *gather* (GFC)?
#pragma once
#include "../technique.h"
#include <unordered_map>

struct MethodGFCAO : Technique {
  static const int BK=4;
  struct FCell { V3 Ed{}, Ei{}; float nd=0, ni=0; };
  struct L1Cell { V3 B{}; float n=0; std::vector<int> fineIdx; };
  std::vector<long> faceId;
  std::vector<FCell> fc;
  std::unordered_map<long,int> slot;
  std::unordered_map<long,L1Cell> l1;
  std::vector<long> l1Ids;
  std::vector<int> visIdx;
  std::vector<float> aoHist; std::vector<float> aoN;
  int frameCounter=0; size_t curVis=0, curBg=0, curL1=0;
  static constexpr float AOR=16.f;          // occlusion radius (voxels) = 1m

  const char* name() override { return "GFCAO"; }
  static long fidOf(int x,int y,int z,int f){ return ((long)gi(x,y,z))*6+f; }
  long cidOf(int x,int y,int z,int f){ return ((long)((z/BK)*BNY+(y/BK))*BNX+(x/BK))*6+f; }

  void rebuildIndex(){
    faceId.clear(); slot.clear(); l1.clear(); l1Ids.clear();
    for(int z=0;z<NZ;z++)for(int y=0;y<NY;y++)for(int x=0;x<NX;x++){
      uint8_t m=at(x,y,z); if(!m||emissive(m)) continue;
      for(int f=0;f<6;f++){int xx=x+OXf[f],yy=y+OYf[f],zz=z+OZf[f];
        if(inB(xx,yy,zz)&&at(xx,yy,zz)==0){
          long id=fidOf(x,y,z,f);
          slot[id]=(int)faceId.size(); faceId.push_back(id);
          l1[cidOf(x,y,z,f)].fineIdx.push_back((int)faceId.size()-1);
        }}
    }
    fc.assign(faceId.size(),{});
    for(auto& kv:l1) l1Ids.push_back(kv.first);
  }
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
    out.assign(W*H,{}); aoHist.assign(W*H,1.f); aoN.assign(W*H,0);
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
      for(int i=0;i<W*H;i++){ aoN[i]=0; }
    }
    if(e.lightChange){
      for(auto& c:fc){ c.nd=std::min(c.nd,1.f); c.ni=std::min(c.ni,2.f); }
      for(auto& kv:l1) kv.second.n=std::min(kv.second.n,2.f);
    }
  }
  inline V3 faceB(int vx,int vy,int vz,int f,uint8_t m){
    auto it=slot.find(fidOf(vx,vy,vz,f));
    if(it!=slot.end()){
      FCell& c=fc[it->second];
      float w=std::min(1.f,c.ni*0.34f);
      V3 fine = MAT[m].alb*((c.Ed+c.Ei)*(1.f/3.14159265f));
      if(w>=1.f) return fine;
      auto l=l1.find(cidOf(vx,vy,vz,f));
      V3 coarse=(l!=l1.end())? l->second.B:V3{};
      return fine*w+coarse*(1-w);
    }
    auto l=l1.find(cidOf(vx,vy,vz,f));
    return (l!=l1.end())? l->second.B:V3{};
  }
  void updateFace(int idx, uint64_t salt){
    long id=faceId[idx]; int f=id%6; long v=id/6;
    int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
    V3 n=FN[f]; V3 p=facePoint(x,y,z,f);
    RNG rng((uint64_t)id, salt, 707);
    V3 Ei{};
    for(int s=0;s<2;s++){
      V3 d=cosDir(n,rng); Hit h;
      if(!trace(p,d,1e9f,h)){ Ei += SKY.skyL(d)*3.14159265f; continue; }
      if(emissive(h.m)) continue;
      Ei += faceB(h.vx,h.vy,h.vz,h.face,h.m)*3.14159265f;
    }
    Ei=Ei*0.5f;
    V3 Ed = neeIrr(p,n,rng);
    FCell& c=fc[idx];
    float ai=std::max(1.f/(c.ni+1.f),0.08f), ad=std::max(1.f/(c.nd+1.f),0.06f);
    c.Ei=c.Ei*(1-ai)+Ei*ai; c.ni+=1;
    c.Ed=c.Ed*(1-ad)+Ed*ad; c.nd+=1;
  }
  void updateL1(long cid, uint64_t salt){
    auto it=l1.find(cid); if(it==l1.end()||it->second.fineIdx.empty()) return;
    L1Cell& cell=it->second;
    RNG rng((uint64_t)cid, salt, 808);
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
    V3 Ed=neeIrr(p,n,rng);
    uint8_t m=at(x,y,z);
    V3 B=MAT[m].alb*((Ed+Ei)*(1.f/3.14159265f));
    float a=std::max(1.f/(cell.n+1.f),0.10f);
    cell.B=cell.B*(1-a)+B*a; cell.n+=1;
  }
  // FCGI-style bilinear read of INDIRECT irradiance over the face plane
  V3 shadeFar(GPix& g){
    int f=g.face;
    int x=g.vx,y=g.vy,z=g.vz;
    V3 lp = g.pos - V3((float)x,(float)y,(float)z);
    int au,av; float lu,lv;
    if(f<2){ au=1;av=2; lu=lp.y; lv=lp.z; }
    else if(f<4){ au=0;av=2; lu=lp.x; lv=lp.z; }
    else { au=0;av=1; lu=lp.x; lv=lp.y; }
    float fu=lu-0.5f, fv=lv-0.5f;
    V3 acc{}; float ws=0;
    for(int ou=-1;ou<=1;ou++)for(int ov=-1;ov<=1;ov++){
      int xx=x,yy=y,zz=z;
      if(au==0)xx+=ou; else if(au==1)yy+=ou; else zz+=ou;
      if(av==0)xx+=ov; else if(av==1)yy+=ov; else zz+=ov;
      if(!inB(xx,yy,zz)||at(xx,yy,zz)==0||emissive(at(xx,yy,zz))) continue;
      int fx=xx+OXf[f],fy=yy+OYf[f],fz=zz+OZf[f];
      if(!inB(fx,fy,fz)||at(fx,fy,fz)!=0) continue;
      auto it=slot.find(fidOf(xx,yy,zz,f)); if(it==slot.end()) continue;
      FCell& cl=fc[it->second]; if(cl.ni<0.5f) continue;
      float du=ou-fu, dv=ov-fv;
      float w=std::exp(-(du*du+dv*dv)/1.1f);
      acc+=cl.Ei*w; ws+=w;
    }
    auto l=l1.find(cidOf(x,y,z,f));
    V3 cB=(l!=l1.end())? l->second.B*3.14159265f : V3{};  // rough irradiance proxy
    if(ws<0.35f){ acc += cB*(0.35f-ws); ws=0.35f; }
    return acc*(1.f/ws);
  }
  long frame(long budget) override {
    uint64_t r0=tl_rays;
    frameCounter++;
    long aoCost=(long)W*H;                       // 1 short ray/pixel
    long faceBudget=std::max(0L,budget-aoCost);
    long fU=faceBudget/4;
    long visU=(long)(fU*0.5), l1U=(long)(fU*0.2), bgU=fU-visU-l1U;
    for(long u=0;u<visU && !visIdx.empty();u++){ updateFace(visIdx[curVis%visIdx.size()],frameCounter*131+u); curVis++; }
    for(long u=0;u<bgU && !fc.empty();u++){ updateFace((int)(curBg%fc.size()),frameCounter*131+u+77); curBg++; }
    for(long u=0;u<l1U && !l1Ids.empty();u++){ updateL1(l1Ids[curL1%l1Ids.size()],frameCounter*131+u); curL1++; }
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      if(!g.hit||emissive(g.m)){ out[i]=V3{}; continue; }
      RNG rng((uint64_t)i,(uint64_t)frameCounter,909);
      V3 d=cosDir(g.n,rng); Hit h;
      float ao=1.f;
      if(trace(g.pos,d,AOR,h)){ float t=h.t/AOR; ao=t*t; }
      float n=aoN[i]; float a=std::max(1.f/(n+1.f),0.10f);
      aoHist[i]=aoHist[i]*(1-a)+ao*a; aoN[i]+=1;
    }
    // small spatial blur of AO (it's 1spp) then shade
    static std::vector<float> aoF; aoF.assign(W*H,1.f);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
      int i=y*W+x; GPix& g=gbuf[i]; if(!g.hit){continue;}
      float acc=0,ws=0;
      for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){
        int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H)continue;
        int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit)continue;
        if(dot(q.n,g.n)<0.9f||std::fabs(q.depth-g.depth)>3.f) continue;
        float w=std::exp(-(float)(dx*dx+dy*dy)/4.5f);
        acc+=aoHist[j]*w; ws+=w;
      }
      aoF[i]=ws>0?acc/ws:aoHist[i];
    }
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      if(!g.hit||emissive(g.m)){ out[i]=V3{}; continue; }
      float ao=0.25f+0.75f*aoF[i];               // soften bias
      out[i]= g.alb*(shadeFar(g)*(ao/3.14159265f));
    }
    long used=(long)(tl_rays-r0); flushCounters(); return used;
  }
};
