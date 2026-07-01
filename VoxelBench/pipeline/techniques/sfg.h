// SFG - Split-Field Gather (novel technique #3).
//
// Diagnosis from the v1 benchmark: FCGI (pure cache read) is smooth and
// energy-exact but flat - it misses per-pixel contact occlusion. GFC (full
// per-pixel gather into the cache) restores detail in principle, but at 1-2
// rays/pixel the gather noise forces an a-trous blur that washes the whole
// image out (its error map shows error spread over every surface, not just
// contact regions).
//
// SFG splits the hemisphere integral by ray distance R (~1 m):
//   E(p) = E_far  : taken from the face cache (bilinear over the face plane,
//                   variance-free, genuinely smooth in the real solution -
//                   distant lighting varies slowly along a surface)
//        + E_near : per-pixel gather with SHORT rays (tmax=R). Rays that hit
//                   within R return pi*B(hitFace); misses contribute 0 (their
//                   energy lives in E_far by construction).
// Per-face cache update rays are traced full length and binned by t<R into
// (Enear, Efar), so the split is exact in expectation - no double counting,
// no AO-style darkening bias.
// The noisy part (E_near) is small in magnitude and short-range, so its
// per-pixel variance is low; it is temporally accumulated and filtered with a
// CONFIDENCE-ADAPTIVE a-trous (kernel fades out as history accumulates), so
// converged frames are sharp, not blurred.
// Same structural wins as FCGI/GFC: surface anchoring, 6-normal collapse,
// brick-local O(1) invalidation, feedback multibounce through B.
#pragma once
#include "../technique.h"
#include <unordered_map>
#include <cstdlib>

struct MethodSFG : Technique {
  // R defined below (env-tunable for experiments)
  static const int BK=4;
  static float R;                           // near/far split distance (voxels), env SFG_R (default 14 ~ 0.9m)
  struct FCell { V3 Ed{}, En{}, Ef{}; float nd=0, ni=0; };
  struct L1Cell { V3 Ei{}, Ef{}, B{}; float n=0; std::vector<int> fineIdx; };
  std::vector<long> faceId;
  std::vector<FCell> fc;
  std::unordered_map<long,int> slot;
  std::unordered_map<long,L1Cell> l1;
  std::vector<long> l1Ids;
  std::vector<int> visIdx;
  // per-pixel temporal state for the near field
  std::vector<V3> pixE; std::vector<float> pixN; std::vector<V3> pixDev;
  std::vector<int> pixFace;
  int frameCounter=0; size_t curVis=0, curBg=0, curL1=0;

  const char* name() override { return "SFG"; }
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
    for(auto& kv:l1){ auto it=keepL1.find(kv.first); if(it!=keepL1.end()){ kv.second.Ei=it->second.Ei; kv.second.B=it->second.B; kv.second.Ef=it->second.Ef; kv.second.n=it->second.n; } }
  }
  void rebuildVisible(){
    visIdx.clear();
    std::vector<uint8_t> seen(fc.size(),0);
    for(int i=0;i<W*H;i++){ GPix& g=gbuf[i]; if(!g.hit||emissive(g.m))continue;
      auto it=slot.find(fidOf(g.vx,g.vy,g.vz,g.face));
      if(it!=slot.end() && !seen[it->second]){ seen[it->second]=1; visIdx.push_back(it->second); } }
  }

  void init() override {
    { const char* e=getenv("SFG_R"); R = e? (float)atof(e) : 14.f; }
    out.assign(W*H,{});
    pixE.assign(W*H,{}); pixN.assign(W*H,0); pixDev.assign(W*H,{}); pixFace.assign(W*H,-1);
    rebuildIndex(); rebuildVisible();
  }

  void onEvent(const SceneEvent& e) override {
    if(e.geomEdit){
      rebuildIndexCarry(); rebuildVisible();
      float RR=e.radius+26;
      for(size_t i=0;i<faceId.size();i++){
        long id=faceId[i]; long v=id/6; int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
        float dx=x-e.center.x,dy=y-e.center.y,dz=z-e.center.z;
        if(dx*dx+dy*dy+dz*dz<RR*RR){ fc[i].nd=std::min(fc[i].nd,2.f); fc[i].ni=std::min(fc[i].ni,2.f); }
      }
      for(auto& kv:l1){
        long b=kv.first/6; int bx=b%BNX, by=(b/BNX)%BNY, bz=b/((long)BNX*BNY);
        float dx=bx*BK+2-e.center.x, dy=by*BK+2-e.center.y, dz=bz*BK+2-e.center.z;
        if(dx*dx+dy*dy+dz*dz<RR*RR) kv.second.n=std::min(kv.second.n,2.f);
      }
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

  // outgoing radiosity of a face (feedback read, trust-ramped to L1)
  inline V3 faceB(int vx,int vy,int vz,int f,uint8_t m){
    auto it=slot.find(fidOf(vx,vy,vz,f));
    if(it!=slot.end()){
      FCell& c=fc[it->second];
      float w=std::min(1.f,c.ni*0.34f);
      V3 fine = MAT[m].alb*((c.Ed+c.En+c.Ef)*(1.f/3.14159265f));
      if(w>=1.f) return fine;
      auto l=l1.find(cidOf(vx,vy,vz,f));
      V3 coarse = (l!=l1.end())? l->second.B : V3{};
      return fine*w + coarse*(1-w);
    }
    auto l=l1.find(cidOf(vx,vy,vz,f));
    return (l!=l1.end())? l->second.B : V3{};
  }

  // one gather sample, FCGI-style feedback: fresh NEE direct at the hit +
  // fast-converging L1 coarse indirect (avoids slow fine-B equilibrium chains)
  inline V3 hitRadiosity(const Hit& h, RNG& rng){
    V3 hn=FN[h.face]; V3 hp=facePoint(h.vx,h.vy,h.vz,h.face);
    V3 Ed=neeIrr(hp,hn,rng);
    auto it=l1.find(cidOf(h.vx,h.vy,h.vz,h.face));
    V3 Ei=(it!=l1.end())? it->second.Ei : V3{};
    return MAT[h.m].alb*(Ed+Ei);
  }
  // update one face: kh full-length hemisphere rays binned near/far + NEE direct
  void updateFace(int idx, uint64_t salt, int kh=2){
    long id=faceId[idx]; int f=id%6; long v=id/6;
    int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
    V3 n=FN[f]; V3 p=facePoint(x,y,z,f);
    RNG rng((uint64_t)id, salt, 808);
    V3 En{}, Ef{};
    for(int s=0;s<kh;s++){
      V3 d=cosDir(n,rng); Hit h;
      if(!trace(p,d,1e9f,h)){ Ef += SKY.skyL(d)*3.14159265f; continue; }
      if(emissive(h.m)) continue;
      V3 c = hitRadiosity(h,rng);
      if(h.t<R) En += c; else Ef += c;
    }
    En=En*(1.f/kh); Ef=Ef*(1.f/kh);
    V3 Ed = neeIrr(p,n,rng);
    FCell& c=fc[idx];
    float ai = std::max(1.f/(c.ni+1.f), 0.06f);
    float ad = std::max(1.f/(c.nd+1.f), 0.06f);
    c.En = c.En*(1-ai)+En*ai; c.Ef = c.Ef*(1-ai)+Ef*ai; c.ni+=1;
    c.Ed = c.Ed*(1-ad)+Ed*ad; c.nd+=1;
  }
  void updateL1(long cid, uint64_t salt){
    auto it=l1.find(cid); if(it==l1.end()||it->second.fineIdx.empty()) return;
    L1Cell& cell=it->second;
    RNG rng((uint64_t)cid, salt, 909);
    int idx=cell.fineIdx[rng.u32()%cell.fineIdx.size()];
    long id=faceId[idx]; int f=id%6; long v=id/6;
    int x=v%NX, y=(v/NX)%NY, z=v/((long)NX*NY);
    V3 n=FN[f]; V3 p=facePoint(x,y,z,f);
    V3 En{}, Ef{};
    for(int s=0;s<2;s++){
      V3 d=cosDir(n,rng); Hit h;
      if(!trace(p,d,1e9f,h)){ Ef += SKY.skyL(d)*3.14159265f; continue; }
      if(emissive(h.m)) continue;
      V3 c=hitRadiosity(h,rng);
      if(h.t<R) En += c; else Ef += c;
    }
    En=En*0.5f; Ef=Ef*0.5f;
    V3 Ed = neeIrr(p,n,rng);
    uint8_t m=at(x,y,z);
    V3 B = MAT[m].alb*((Ed+En+Ef)*(1.f/3.14159265f));
    float a=std::max(1.f/(cell.n+1.f),0.10f);
    cell.Ei = cell.Ei*(1-a)+(En+Ef)*a;
    cell.B = cell.B*(1-a)+B*a;
    cell.Ef = cell.Ef*(1-a)+Ef*a;
    cell.n+=1;
  }

  // bilinear interpolation of the FAR-FIELD cache across the face plane
  V3 shadeFar(GPix& g){
    int f=g.face;
    int x=g.vx,y=g.vy,z=g.vz;
    float lu,lv; int au,av;
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
      if(!inB(xx,yy,zz)||at(xx,yy,zz)==0||emissive(at(xx,yy,zz))) continue;
      int fx=xx+OXf[f],fy=yy+OYf[f],fz=zz+OZf[f];
      if(!inB(fx,fy,fz)||at(fx,fy,fz)!=0) continue;
      auto it=slot.find(fidOf(xx,yy,zz,f)); if(it==slot.end()) continue;
      FCell& cl=fc[it->second]; if(cl.ni<0.5f) continue;
      float du=ou-fu, dv=ov-fv;
      float w=std::exp(-(du*du+dv*dv)/1.1f)*std::min(1.f,cl.ni*0.25f);
      acc+=cl.Ef*w; ws+=w;
    }
    auto lit=l1.find(cidOf(x,y,z,f));
    V3 cE = (lit!=l1.end())? lit->second.Ef : V3{};
    if(ws<0.35f){ acc += cE*(0.35f-ws); ws=0.35f; }
    return acc*(1.f/ws);
  }

  long frame(long budget) override {
    uint64_t r0=tl_rays;
    frameCounter++;
    // budget: k short near rays per pixel; faces get the rest
    int k = std::max(1, (int)std::lround(budget/(double)(4*W*H)));
    long gatherCost=(long)W*H*k;
    long faceBudget=std::max(0L,budget-gatherCost);
    const long raysPerFaceUpdate=8;
    long fU=faceBudget/raysPerFaceUpdate;
    long visU=(long)(fU*0.5), l1U=(long)(fU*0.25), bgU=fU-visU-l1U;
    for(long u=0;u<visU && !visIdx.empty();u++){ updateFace(visIdx[curVis%visIdx.size()],frameCounter*131+u); curVis++; }
    for(long u=0;u<bgU && !fc.empty();u++){ updateFace((int)(curBg%fc.size()),frameCounter*131+u+77); curBg++; }
    for(long u=0;u<l1U && !l1Ids.empty();u++){ updateL1(l1Ids[curL1%l1Ids.size()],frameCounter*131+u); curL1++; }

    // per-pixel NEAR gather (short rays, tmax=R)
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      if(!g.hit||emissive(g.m)){ pixFace[i]=-1; continue; }
      pixFace[i]=(int)fidOf(g.vx,g.vy,g.vz,g.face);
      RNG rng((uint64_t)i,(uint64_t)frameCounter,1212);
      V3 E{};
      for(int s=0;s<k;s++){
        // stratified-in-time cosine sample (16 phi x 4 zenith strata)
        float u2=((frameCounter+ (i*7)) %16 + rng.uf())*(1.f/16.f);
        float u1=(((frameCounter/16)+(i*13)) %4 + rng.uf())*(1.f/4.f);
        float rr=std::sqrt(u1), phi=6.2831853f*u2;
        V3 tu=(std::fabs(g.n.x)>0.5f)?V3(0,1,0):V3(1,0,0);
        V3 tv=norm(cross(g.n,tu)); tu=cross(tv,g.n);
        V3 d=norm(tu*(rr*std::cos(phi))+tv*(rr*std::sin(phi))+g.n*std::sqrt(std::max(0.f,1-u1)));
        Hit h;
        if(trace(g.pos,d,R,h) && !emissive(h.m))
          E += faceB(h.vx,h.vy,h.vz,h.face,h.m)*3.14159265f;
      }
      E=E*(1.f/k);
      float n=pixN[i];
      V3 dev=pixDev[i];
      V3 diff=E-pixE[i];
      float dmag=std::fabs(diff.x)+std::fabs(diff.y)+std::fabs(diff.z);
      float dref=dev.x+dev.y+dev.z;
      float a=std::max(1.f/(n+1.f),0.04f);
      if(n>8 && dmag>4.f*dref+0.15f){ a=std::max(a,0.5f); pixN[i]=4; }
      pixE[i]=pixE[i]*(1-a)+E*a;
      pixDev[i]=dev*0.9f+V3(std::fabs(diff.x),std::fabs(diff.y),std::fabs(diff.z))*0.1f;
      pixN[i]+=1;
    }
    // confidence-adaptive a-trous on the near field ONLY (fades out as
    // history accumulates -> converged image stays sharp)
    static std::vector<V3> tmpA,tmpB;
    tmpA=pixE;
    auto pass=[&](std::vector<V3>& src,std::vector<V3>& dst,int step){
      dst.resize(W*H);
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit){dst[i]=src[i];continue;}
        float conf=std::min(1.f,pixN[i]/40.f);     // 1 => fully converged
        if(conf>=1.f){ dst[i]=src[i]; continue; }
        V3 acc{}; float ws=0;
        for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
          int xx=x+dx*step,yy=y+dy*step; if(xx<0||xx>=W||yy<0||yy>=H)continue;
          int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit)continue;
          if(dot(q.n,g.n)<0.95f) continue;
          if(std::fabs(q.depth-g.depth)>1.5f+0.02f*g.depth) continue;
          float w=std::exp(-(float)(dx*dx+dy*dy)/2.f);
          w*=0.25f+0.75f*std::min(1.f,pixN[j]*0.1f);
          acc+=src[j]*w; ws+=w;
        }
        V3 filt=(ws>0)? acc*(1.f/ws):src[i];
        dst[i]=filt*(1-conf)+src[i]*conf;          // adaptive: blur only while noisy
      }
    };
    pass(tmpA,tmpB,1);
    pass(tmpB,tmpA,2);
    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      if(!(g.hit&&!emissive(g.m))){ out[i]=V3{}; continue; }
      V3 E = shadeFar(g) + tmpA[i];
      out[i]= g.alb*(E*(1.f/3.14159265f));
    }
    long used=(long)(tl_rays-r0); flushCounters(); return used;
  }
};
float MethodSFG::R = 14.f;
