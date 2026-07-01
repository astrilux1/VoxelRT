// Method PTG: guided voxel path tracing.
//
// This is deliberately not a FaceCache method. It keeps the path-traced
// estimator, but makes the baseline PT friendlier to sparse voxel worlds:
//   - exact ODF traversal, auto-enabled only when a local probe says it wins;
//   - stratified cosine bounce directions to reduce standing variance;
//   - emissive-power NEE sampling instead of uniform emissive-face sampling;
//   - per-pixel history invalidation after geometry edits.
#pragma once
#include "../technique.h"
#include "../accel2.h"
#include <chrono>
#include <cstdint>
#include <vector>

struct MethodPTG : Technique {
  ODF odf;
  bool useOdf=false;
  std::vector<V3> hist;
  std::vector<float> n;
  std::vector<uint64_t> pixKey;
  std::vector<float> lightCdf;
  float lightSum=0.f;
  int fcount=0;
  int boostFrames=0;

  const char* name() override { return "PTG"; }

  static uint64_t keyFor(const GPix& g){
    if(!g.hit) return 0;
    uint64_t k=1469598103934665603ULL;
    auto mix=[&](uint64_t v){ k^=v; k*=1099511628211ULL; };
    mix((uint64_t)g.m);
    mix((uint64_t)g.face);
    mix((uint64_t)g.vx);
    mix((uint64_t)g.vy);
    mix((uint64_t)g.vz);
    return k;
  }

  void captureKeys(){
    pixKey.resize(W*H);
    for(int i=0;i<W*H;i++) pixKey[i]=keyFor(gbuf[i]);
  }

  bool traceFast(V3 o,V3 d,float tmax,Hit& h){
    return useOdf ? traceODF(odf,o,d,tmax,h) : trace(o,d,tmax,h);
  }

  V3 cosDirStrat(V3 nn, RNG& rng, int stratum){
    int st = stratum & 63;
    float u1 = ((st >> 4) + rng.uf()) * 0.25f;      // 4 zenith strata
    float u2 = ((st & 15) + rng.uf()) * 0.0625f;    // 16 azimuth strata
    float r=std::sqrt(u1), phi=6.2831853f*u2;
    float lx=r*std::cos(phi), ly=r*std::sin(phi), lz=std::sqrt(std::max(0.f,1-u1));
    V3 tu = (std::fabs(nn.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
    V3 tv = norm(cross(nn,tu)); tu = cross(tv,nn);
    return norm(tu*lx + tv*ly + nn*lz);
  }

  void buildLightCdf(){
    lightCdf.clear();
    lightCdf.reserve(emFaces.size());
    lightSum=0.f;
    for(const EmFace& e: emFaces){
      float w=std::max(luma(MAT[e.m].emi), 1e-6f);
      lightSum += w;
      lightCdf.push_back(lightSum);
    }
  }

  int sampleLight(RNG& rng){
    if(lightCdf.empty() || lightSum<=0.f) return -1;
    float x = rng.uf()*lightSum;
    auto it=std::lower_bound(lightCdf.begin(), lightCdf.end(), x);
    int idx=(int)(it-lightCdf.begin());
    return std::min(idx, (int)lightCdf.size()-1);
  }

  V3 neeIrrG(V3 p, V3 nn, RNG& rng){
    V3 acc{};
    int li=sampleLight(rng);
    if(li>=0){
      EmFace e = emFaces[li];
      V3 fn = FN[e.f];
      V3 q = facePoint(e.x,e.y,e.z,e.f);
      V3 tu = (std::fabs(fn.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
      V3 tv = cross(fn,tu);
      q += tu*(rng.uf()-0.5f) + tv*(rng.uf()-0.5f);
      V3 w = q - p; float d2 = dot(w,w); float d = std::sqrt(d2);
      if(d>1e-6f){
        w = w*(1.f/d);
        float cs = dot(w,nn), cl = -dot(w,fn);
        if(cs>0&&cl>0){
          Hit h;
          if(traceFast(p,w,d+1.f,h) && emissive(h.m) && h.t >= d-0.87f){
            float faceWeight=std::max(luma(MAT[e.m].emi), 1e-6f);
            float pdfA = faceWeight / std::max(lightSum, 1e-9f);
            acc += MAT[e.m].emi*(cs*cl/(d2*pdfA+1e-9f));
          }
        }
      }
    }
    if(SKY.enabled && (SKY.sunE.x+SKY.sunE.y+SKY.sunE.z)>0){
      float cs = dot(SKY.sunDir,nn);
      if(cs>0){
        Hit h;
        if(!traceFast(p,SKY.sunDir,1e9f,h)) acc += SKY.sunE*cs;
      }
    }
    return acc;
  }

  V3 pathSample(V3 p,V3 nn,RNG& rng,int pixel,long pathIdx){
    V3 tp{1,1,1}, acc{};
    for(int b=0;b<8;b++){
      int stratum = fcount + pixel*17 + (int)pathIdx*23 + b*11;
      V3 d=cosDirStrat(nn,rng,stratum);
      Hit h;
      if(!traceFast(p,d,1e9f,h)){
        acc += tp*(SKY.skyL(d)*3.14159265f);
        break;
      }
      if(emissive(h.m)) break;
      V3 hp=facePoint(h.vx,h.vy,h.vz,h.face);
      V3 hn=FN[h.face];
      tp = tp*MAT[h.m].alb;
      acc += tp*neeIrrG(hp,hn,rng);
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
    RNG rng(91,17,1201);
    std::vector<V3> dirs; dirs.reserve(pts.size()*4);
    for(auto& pn:pts) for(int s=0;s<4;s++) dirs.push_back(cosDirStrat(pn.second,rng,(int)(dirs.size()+s)));
    Hit h;
    for(size_t i=0;i<pts.size();i+=8){
      trace(pts[i].first,dirs[i*4],1e9f,h);
      traceODF(odf,pts[i].first,dirs[i*4],1e9f,h);
    }
    auto t0=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<pts.size();i++) for(int s=0;s<4;s++) trace(pts[i].first,dirs[i*4+s],1e9f,h);
    auto t1=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<pts.size();i++) for(int s=0;s<4;s++) traceODF(odf,pts[i].first,dirs[i*4+s],1e9f,h);
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
    buildLightCdf();
    odf.build();
    probeTraversal();
    captureKeys();
  }

  void onEvent(const SceneEvent& e) override {
    buildLightCdf();
    odf.build();
    probeTraversal();
    if(e.geomEdit){
      for(int i=0;i<W*H;i++){
        uint64_t k=keyFor(gbuf[i]);
        if(i>=(int)pixKey.size() || k!=pixKey[i]){
          hist[i]=V3{};
          n[i]=0.f;
        }
      }
      captureKeys();
      boostFrames=28;
    }
    if(e.lightChange){
      boostFrames=std::max(boostFrames,36);
    }
  }

  long frame(long budget) override {
    double ppp = (double)budget / ((double)(W*H)*5.0);
    bool subPixel = ppp < 1.0;
    long basePaths = subPixel ? 1L : std::max(1L,(long)std::floor(ppp));
    double fracP = (ppp>1.0)? ppp-std::floor(ppp) : 0.0;
    uint64_t r0=tl_rays;

    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      if(!g.hit||emissive(g.m)) continue;
      if(subPixel){
        RNG gate((uint64_t)i,(uint64_t)fcount,91117);
        if(gate.uf() >= ppp) continue;
      }
      RNG rng((uint64_t)i,(uint64_t)fcount,24091);
      long paths = basePaths + ((rng.uf()<fracP)?1:0);
      V3 E{};
      for(long s=0;s<paths;s++) E += pathSample(g.pos,g.n,rng,i,s);
      E = E*(1.f/(float)paths);
      float a;
      if(n[i]<1.f) a=1.f;
      else a = boostFrames>0 ? 0.32f : 0.13f;
      hist[i] = hist[i]*(1-a) + E*a;
      n[i]+=1.f;
    }
    fcount++;

    std::vector<V3> filt(W*H);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
      int i=y*W+x; GPix& g=gbuf[i];
      if(!g.hit){ filt[i]=hist[i]; continue; }
      V3 acc{}; float ws=0;
      for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){
        int xx=x+dx, yy=y+dy;
        if(xx<0||xx>=W||yy<0||yy>=H) continue;
        int j=yy*W+xx; GPix& q=gbuf[j];
        if(!q.hit || q.m!=g.m || q.face!=g.face) continue;
        if(dot(q.n,g.n)<0.94f) continue;
        float dd=std::fabs(q.depth-g.depth);
        if(dd>1.75f) continue;
        float w=std::exp(-(dx*dx+dy*dy)/3.8f) * std::exp(-(dd*dd)/2.0f);
        acc += hist[j]*w; ws += w;
      }
      filt[i] = (ws>0) ? acc*(1.f/ws) : hist[i];
    }

    for(int i=0;i<W*H;i++){
      GPix& g=gbuf[i];
      out[i]= g.hit ? g.alb*(filt[i]*(1.f/3.14159265f)) : V3{};
    }
    if(boostFrames>0) boostFrames--;
    long used=(long)(tl_rays-r0);
    flushCounters();
    return used;
  }
};

struct MethodPTO : Technique {
  ODF odf;
  bool useOdf=false;
  std::vector<V3> hist;
  std::vector<float> n;
  std::vector<float> lumVar;
  std::vector<uint64_t> pixKey;
  float alpha=0.15f;
  int fcount=0;
  bool progressive=false;
  bool resetHistoryOnEvent=false;
  bool localizedEventHistory=false;
  float geomHistoryCap=4.f;
  float lightHistoryCap=2.f;
  bool robustClamp=false;
  float robustClampMul=12.f;
  float robustClampAdd=3.f;
  bool varianceFilter=false;
  bool trackLumaVariance=false;
  bool stratifiedDirs=false;
  bool stratifiedFirstBounceOnly=false;
  bool stratifiedMaterialChromaGuard=false;
  bool stratifiedHistoryChromaGuard=false;
  bool stratifiedHistoryGreenOppPositiveOnly=false;
  bool stratifiedHistoryChromaSupport=false;
  float stratifiedMaterialChromaMin=0.30f;
  float stratifiedMaterialDirectMax=0.20f;
  float stratifiedHistoryChromaMin=0.024f;
  float stratifiedHistoryGreenOppMin=0.004f;
  float stratifiedHistoryMinN=3.0f;
  float stratifiedHistoryDirectMax=0.20f;
  int stratifiedHistoryChromaSupportRadius=1;
  int stratifiedHistoryChromaSupportMinCount=3;
  float stratifiedHistoryChromaSupportMinFrac=0.35f;
  float stratifiedHistoryChromaSupportDepthMax=1.75f;
  float stratifiedHistoryChromaFallbackProb=1.0f;
  bool weightedNee=false;
  bool finalFireflyFilter=false;
  bool spatialSampleReuse=false;
  bool adaptiveSampleReuse=false;
  bool adaptivePixelSampling=false;
  bool adaptivePixelUseVariance=false;
  bool surfaceBasisSampleReuse=false;
  bool persistentSurfaceModel=false;
  bool winsorizedSampleReuse=false;
  bool stochasticSampleReuse=false;
  bool surfaceBasisQuadraticFit=true;
  bool skyOnlyFallback=false;
  bool preserveCenterSamples=false;
  bool historyGuidedSampleReuse=false;
  bool historyPlaneAcceptance=false;
  bool reconstructionOnlyReuse=false;
  bool reconstructionHintSkippedOnly=false;
  bool warmupOnlyReuse=false;
  float reuseDirectMax=0.20f;
  float reuseCenterWeight=2.0f;
  int reuseMinSamples=1;
  int reuseRadius=2;
  float reuseSpatialSigma=4.0f;
  float reuseNormalMin=0.94f;
  float reuseDepthMax=1.75f;
  float reuseStochasticKeep=0.58f;
  int reuseWinsorMinSamples=5;
  float reuseWinsorSigma=1.35f;
  float reuseWinsorAdd=0.10f;
  float reuseBorrowHistoryWeight=1.0f;
  float reuseHistoryBase=0.08f;
  float reuseHistoryScale=0.08f;
  float reconstructionHintBlend=0.35f;
  float reuseWarmupMaxN=16.0f;
  int historyPlaneRadius=5;
  int historyPlaneMinSamples=18;
  float historyPlaneMinN=3.0f;
  float historyPlaneAreaThreshold=0.64f;
  float historyPlaneAreaSoftness=0.22f;
  float historyPlaneBase=0.045f;
  float historyPlaneScale=0.16f;
  float historyPlaneRmsScale=1.6f;
  float historyPlaneCenterBlend=0.32f;
  float historyPlaneBorrowBlend=0.72f;
  float historyPlaneWeightCut=0.45f;
  float adaptivePixelHighWeight=0.42f;
  float adaptivePixelLowWeight=2.4f;
  float adaptivePixelBroadBoost=1.55f;
  float adaptivePixelAreaThreshold=0.64f;
  float adaptivePixelAreaSoftness=0.22f;
  float adaptivePixelMinProb=0.025f;
  float adaptivePixelMaxProb=0.92f;
  int adaptivePixelBroadRadius=2;
  float adaptivePixelVarianceBase=0.025f;
  float adaptivePixelVarianceScale=0.14f;
  float adaptivePixelVarianceBoost=1.8f;
  int surfaceBasisMinPixels=180;
  int surfaceBasisMinValid=28;
  int surfaceBasisMinSpan=18;
  float surfaceBasisMinFill=0.36f;
  float surfaceBasisBlend=0.75f;
  float surfaceBasisBorrowHistoryWeight=0.85f;
  float surfaceBasisRejectSigma=3.0f;
  float surfaceBasisRejectAdd=0.30f;
  float surfaceBasisRidge=1e-3f;
  float persistentSurfaceModelAlpha=0.10f;
  float persistentSurfaceModelWarmup=8.0f;
  float persistentSurfaceModelBlend=0.28f;
  float persistentSurfaceModelBiasBase=0.05f;
  float persistentSurfaceModelBiasScale=0.18f;
  bool strictFinalFilter=false;
  bool rangeFinalFilter=false;
  bool planarFinalFit=false;
  bool fastPlanarFinalFit=false;
  bool separablePlanarFinalFit=false;
  bool fastPlanarDepthBins=false;
  bool connectedPlaneFinalFit=false;
  bool broadPlaneEnergyLift=false;
  bool broadPlaneLiftSkyDisabledOnly=false;
  bool finalDetailRestore=false;
  bool broadPlaneTextureRestore=false;
  bool broadPlaneChromaRestore=false;
  bool broadPlaneLowFreqRestore=false;
  bool materialChromaFloor=false;
  bool connectedPlaneQuadraticFit=true;
  bool connectedPlaneInteriorOnly=false;
  bool lowDiscrepancyFineGate=false;
  bool transportChromaModel=false;
  int finalFilterRadius=2;
  float finalFilterNormalMin=0.9f;
  float finalFilterDepthMax=2.5f;
  float finalFilterSigma=4.5f;
  bool finalFilterMaterialEdgeOnly=false;
  int finalFilterEdgeRadius=1;
  float finalFilterEdgeMinSame=0.92f;
  float finalRangeBase=0.08f;
  float finalRangeScale=0.08f;
  int planarFitRadius=5;
  int planarFitStep=1;
  int planarFitMinSamples=18;
  float planarFitBlend=0.50f;
  float planarFitNormalMin=0.96f;
  float planarFitDepthMax=1.5f;
  bool planarFitLowDirectOnly=true;
  bool planarFitResidualGate=false;
  bool planarFitAreaBlend=false;
  bool planarFitDirectStableBlend=false;
  bool planarFitDirectGradientGate=false;
  bool planarFitVoxelContactGate=false;
  bool planarFitEdgeConfidence=false;
  bool planarFitGradientLossGate=false;
  bool planarFitLumaOnly=false;
  bool planarFitChromaAnchor=false;
  bool planarFitMaterialChromaAnchor=false;
  bool planarFitMaterialDarkBlendCap=false;
  bool planarFitGreenOppLossGate=false;
  bool planarFitMultiscaleResidual=false;
  bool planarFitStableHistoryGate=false;
  bool planarFitChromaLossGate=false;
  bool planarFitCoherentChromaLossGate=false;
  bool planarFitMeanChromaLossGate=false;
  bool planarFitConnectedChromaLossGate=false;
  bool planarFitTemporalChromaLossGate=false;
  bool planarFitLowPassChromaField=false;
  bool planarFitTransportChromaField=false;
  bool planarFitLowPassChromaSatWeight=false;
  bool planarFitCoherentResidualGate=false;
  bool planarFitCoherentPatchDamp=false;
  float planarFitResidualBase=0.06f;
  float planarFitResidualScale=0.12f;
  float planarFitDepthBinSize=1.5f;
  float planarFitBlendHigh=0.80f;
  float planarFitAreaThreshold=0.72f;
  float planarFitAreaSoftness=0.20f;
  float planarFitDirectMeanMax=0.18f;
  float planarFitDirectStdMax=0.035f;
  float planarFitDirectGradientStdMin=0.012f;
  float planarFitDirectGradientStdMax=0.040f;
  float planarFitDirectGradientBlendCap=0.48f;
  float planarFitGradientLossMin=0.010f;
  float planarFitGradientLossStart=0.25f;
  float planarFitGradientLossEnd=0.65f;
  float planarFitGradientLossBlendCap=0.58f;
  float planarFitChromaAnchorAmount=0.25f;
  float planarFitChromaAnchorScaleMin=0.82f;
  float planarFitChromaAnchorScaleMax=1.22f;
  float planarFitChromaAnchorClampBase=0.006f;
  float planarFitChromaAnchorClampScale=0.08f;
  float planarFitMaterialChromaAmount=0.20f;
  float planarFitMaterialChromaMin=0.14f;
  float planarFitMaterialChromaSoftness=0.18f;
  int planarFitMaterialChromaSurfaceRadius=1;
  float planarFitMaterialChromaSurfaceMinSame=0.82f;
  float planarFitMaterialChromaSurfaceSoftness=0.14f;
  float planarFitMaterialChromaClampBase=0.004f;
  float planarFitMaterialChromaClampScale=0.055f;
  float planarFitMaterialDarkSatMin=0.12f;
  float planarFitMaterialDarkSatSoftness=0.18f;
  float planarFitMaterialDarkLumaMin=0.04f;
  float planarFitMaterialDarkLumaMax=0.28f;
  float planarFitMaterialDarkBlendCapValue=0.48f;
  float planarFitGreenOppMin=0.006f;
  float planarFitGreenOppSoftness=0.020f;
  float planarFitGreenOppLossMin=0.004f;
  float planarFitGreenOppLossSoftness=0.018f;
  float planarFitGreenOppBlendCap=0.48f;
  int planarFitMultiscaleRadius=4;
  int planarFitMultiscaleMinSamples=12;
  float planarFitMultiscaleAmount=0.20f;
  float planarFitMultiscaleClampBase=0.004f;
  float planarFitMultiscaleClampScale=0.055f;
  bool planarFitMultiscaleMaterialGate=false;
  bool planarFitMultiscaleDarkGate=false;
  float planarFitMultiscaleMaterialMin=0.14f;
  float planarFitMultiscaleMaterialSoftness=0.18f;
  float planarFitMultiscaleLumaMin=0.05f;
  float planarFitMultiscaleLumaMax=0.32f;
  float planarFitStableHistoryMinN=18.0f;
  float planarFitStableHistoryStdMax=0.035f;
  float planarFitStableHistoryStdSoftness=0.045f;
  float planarFitStableHistoryBlendCap=0.58f;
  float planarFitChromaLossSatMin=0.035f;
  float planarFitChromaLossSatSoftness=0.080f;
  float planarFitChromaLossMin=0.12f;
  float planarFitChromaLossSoftness=0.30f;
  float planarFitChromaLossBlendCap=0.46f;
  int planarFitCoherentChromaRadius=1;
  float planarFitCoherentChromaSatMin=0.035f;
  float planarFitCoherentChromaLossMin=0.10f;
  float planarFitCoherentChromaLossSoftness=0.24f;
  float planarFitCoherentChromaDirMin=0.40f;
  float planarFitCoherentChromaSupportMin=0.50f;
  float planarFitCoherentChromaBlendCap=0.46f;
  float planarFitMeanChromaSatMin=0.025f;
  float planarFitMeanChromaLossMin=0.08f;
  float planarFitMeanChromaLossSoftness=0.24f;
  float planarFitMeanChromaBlendCap=0.48f;
  int planarFitConnectedChromaMinPixels=160;
  int planarFitConnectedChromaMinSpan=12;
  float planarFitConnectedChromaMinFill=0.34f;
  float planarFitConnectedChromaSatMin=0.026f;
  float planarFitConnectedChromaSatSoftness=0.055f;
  float planarFitConnectedChromaSupportMin=0.38f;
  float planarFitConnectedChromaLossMin=0.10f;
  float planarFitConnectedChromaLossSoftness=0.22f;
  float planarFitConnectedChromaBlendCap=0.48f;
  int planarFitTemporalChromaRadius=1;
  int planarFitTemporalChromaMinSamples=3;
  float planarFitTemporalChromaMinN=8.0f;
  float planarFitTemporalChromaStdMax=0.080f;
  float planarFitTemporalChromaStdSoftness=0.080f;
  float planarFitTemporalChromaSatMin=0.024f;
  float planarFitTemporalChromaSatSoftness=0.050f;
  float planarFitTemporalChromaSupportMin=0.38f;
  float planarFitTemporalChromaLossMin=0.075f;
  float planarFitTemporalChromaLossSoftness=0.20f;
  float planarFitTemporalChromaBlendCap=0.48f;
  float planarFitLowPassChromaMinN=2.0f;
  float planarFitLowPassChromaMinWeight=12.0f;
  float planarFitLowPassChromaSatMin=0.010f;
  float planarFitLowPassChromaSatSoftness=0.035f;
  float planarFitLowPassChromaWeightSatMin=0.010f;
  float planarFitLowPassChromaWeightSatSoftness=0.050f;
  float planarFitLowPassChromaAmount=0.35f;
  float planarFitLowPassChromaClampBase=0.004f;
  float planarFitLowPassChromaClampScale=0.060f;
  int planarFitCoherentRadius=1;
  float planarFitCoherentResidualMin=0.006f;
  float planarFitCoherentResidualScale=0.024f;
  float planarFitCoherentSignMin=0.60f;
  float planarFitCoherentSignSoftness=0.20f;
  float planarFitCoherentBlendCap=0.58f;
  float planarFitCoherentPatchFillStart=0.72f;
  float planarFitCoherentPatchFillEnd=0.96f;
  float planarFitCoherentPatchMinScale=0.20f;
  int planarFitEdgeRadius=1;
  float planarFitEdgeMinSame=0.78f;
  float planarFitEdgeSoftness=0.18f;
  float planarFitLumaScaleMin=0.78f;
  float planarFitLumaScaleMax=1.28f;
  int connectedPlaneMinPixels=180;
  int connectedPlaneMinSpan=18;
  float connectedPlaneMinFill=0.42f;
  float connectedPlaneBlend=0.35f;
  int connectedPlaneInteriorRadius=2;
  float connectedPlaneInteriorMinSame=0.86f;
  int broadPlaneLiftRadius=5;
  float broadPlaneLiftAmount=0.04f;
  float broadPlaneLiftAreaThreshold=0.68f;
  float broadPlaneLiftAreaSoftness=0.20f;
  bool broadPlaneLiftEdgeConfidence=false;
  bool broadPlaneLiftLumaGate=false;
  bool broadPlaneLiftVoxelContactGate=false;
  int broadPlaneLiftEdgeRadius=1;
  float broadPlaneLiftEdgeMinSame=0.78f;
  float broadPlaneLiftEdgeSoftness=0.18f;
  float broadPlaneLiftLumaMin=0.035f;
  float broadPlaneLiftLumaMax=0.18f;
  float broadPlaneLiftDarkScale=0.35f;
  float planarFitVoxelContactAmount=0.55f;
  float broadPlaneLiftVoxelContactAmount=0.55f;
  int detailRestoreRadius=2;
  int detailRestoreEdgeRadius=1;
  float detailRestoreAmount=0.28f;
  float detailRestoreEdgeMinSame=0.92f;
  float detailRestoreEdgeSoftness=0.12f;
  float detailRestoreClampBase=0.012f;
  float detailRestoreClampScale=0.18f;
  int textureRestoreRadius=2;
  int textureRestoreSurfaceRadius=1;
  float textureRestoreAmount=0.14f;
  float textureRestoreSurfaceMinSame=0.82f;
  float textureRestoreSurfaceSoftness=0.14f;
  float textureRestoreLumaMin=0.05f;
  float textureRestoreLumaMax=0.30f;
  float textureRestoreClampBase=0.006f;
  float textureRestoreClampScale=0.08f;
  float chromaRestoreAmount=0.20f;
  int chromaRestoreSurfaceRadius=1;
  float chromaRestoreSurfaceMinSame=0.82f;
  float chromaRestoreSurfaceSoftness=0.14f;
  float chromaRestoreLumaMin=0.05f;
  float chromaRestoreLumaMax=0.30f;
  float chromaRestoreClampBase=0.006f;
  float chromaRestoreClampScale=0.08f;
  int lowFreqRestoreRadius=4;
  int lowFreqRestoreSurfaceRadius=1;
  float lowFreqRestoreAmount=0.20f;
  float lowFreqRestoreSurfaceMinSame=0.82f;
  float lowFreqRestoreSurfaceSoftness=0.14f;
  float lowFreqRestoreLumaMin=0.05f;
  float lowFreqRestoreLumaMax=0.32f;
  float lowFreqRestoreClampBase=0.004f;
  float lowFreqRestoreClampScale=0.055f;
  float materialChromaAmount=0.25f;
  int materialChromaSurfaceRadius=1;
  float materialChromaSurfaceMinSame=0.82f;
  float materialChromaSurfaceSoftness=0.14f;
  float materialChromaLumaMin=0.04f;
  float materialChromaLumaMax=0.32f;
  float materialChromaClampBase=0.006f;
  float materialChromaClampScale=0.10f;
  bool lowDiscrepancyGate=false;
  std::vector<float> lightCdf;
  std::vector<float> adaptivePixelProbCache;
  std::vector<V3> surfaceModelHist;
  std::vector<float> surfaceModelN;
  std::vector<V3> transportChromaHist;
  std::vector<float> transportChromaN;
  float lightSum=0.f;
  float adaptivePixelProbCachePpp=-1.f;
  const char* methodName="PTO";

  const char* name() override { return methodName; }

  bool traceFast(V3 o,V3 d,float tmax,Hit& h){
    return useOdf ? traceODF(odf,o,d,tmax,h) : trace(o,d,tmax,h);
  }

  V3 cosDirStratO(V3 nn, RNG& rng, int stratum){
    int st = stratum & 63;
    float u1 = ((st >> 4) + rng.uf()) * 0.25f;
    float u2 = ((st & 15) + rng.uf()) * 0.0625f;
    float r=std::sqrt(u1), phi=6.2831853f*u2;
    float lx=r*std::cos(phi), ly=r*std::sin(phi), lz=std::sqrt(std::max(0.f,1-u1));
    V3 tu = (std::fabs(nn.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
    V3 tv = norm(cross(nn,tu)); tu = cross(tv,nn);
    return norm(tu*lx + tv*ly + nn*lz);
  }

  void buildLightCdfO(){
    lightCdf.clear();
    lightCdf.reserve(emFaces.size());
    lightSum=0.f;
    for(const EmFace& e: emFaces){
      float w=std::max(luma(MAT[e.m].emi), 1e-6f);
      lightSum += w;
      lightCdf.push_back(lightSum);
    }
  }

  int sampleLightO(RNG& rng){
    if(lightCdf.empty() || lightSum<=0.f) return -1;
    float x = rng.uf()*lightSum;
    auto it=std::lower_bound(lightCdf.begin(), lightCdf.end(), x);
    int idx=(int)(it-lightCdf.begin());
    return std::min(idx, (int)lightCdf.size()-1);
  }

  void captureKeys(){
    pixKey.resize(W*H);
    for(int i=0;i<W*H;i++) pixKey[i]=MethodPTG::keyFor(gbuf[i]);
  }

  V3 neeIrrO(V3 p, V3 nn, RNG& rng){
    V3 acc{};
    if(!emFaces.empty()){
      int i = weightedNee ? sampleLightO(rng) : (int)(rng.u32()%emFaces.size());
      if(i<0) return acc;
      EmFace e = emFaces[i];
      V3 fn = FN[e.f];
      V3 q = facePoint(e.x,e.y,e.z,e.f);
      V3 tu = (std::fabs(fn.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
      V3 tv = cross(fn,tu);
      q += tu*(rng.uf()-0.5f) + tv*(rng.uf()-0.5f);
      V3 w = q - p; float d2 = dot(w,w); float d = std::sqrt(d2); w = w*(1.f/d);
      float cs = dot(w,nn); float cl = -dot(w,fn);
      if(cs>0&&cl>0){
        Hit h;
        if(traceFast(p,w,d+1.f,h) && emissive(h.m) && h.t >= d-0.87f){
          float pdfA = weightedNee
            ? std::max(luma(MAT[e.m].emi), 1e-6f) / std::max(lightSum, 1e-9f)
            : 1.f/(float)emFaces.size();
          acc += MAT[e.m].emi*(cs*cl/(d2*pdfA+1e-9f));
        }
      }
    }
    if(SKY.enabled && (SKY.sunE.x+SKY.sunE.y+SKY.sunE.z)>0){
      float cs = dot(SKY.sunDir,nn);
      if(cs>0){
        Hit h;
        if(!traceFast(p,SKY.sunDir,1e9f,h)) acc += SKY.sunE*cs;
      }
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
      trace(pts[i].first,dirs[i*4],1e9f,h);
      traceODF(odf,pts[i].first,dirs[i*4],1e9f,h);
    }
    auto t0=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<pts.size();i++) for(int s=0;s<4;s++) trace(pts[i].first,dirs[i*4+s],1e9f,h);
    auto t1=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<pts.size();i++) for(int s=0;s<4;s++) traceODF(odf,pts[i].first,dirs[i*4+s],1e9f,h);
    auto t2=std::chrono::high_resolution_clock::now();
    double brickNs=std::chrono::duration<double,std::nano>(t1-t0).count();
    double odfNs=std::chrono::duration<double,std::nano>(t2-t1).count();
    useOdf = odfNs*1.08 < brickNs;
    flushCounters();
  }

  void init() override {
    if(skyOnlyFallback && SKY.enabled && emFaces.empty()){
      spatialSampleReuse=false;
      adaptivePixelSampling=false;
      adaptivePixelUseVariance=false;
      surfaceBasisSampleReuse=false;
      persistentSurfaceModel=false;
      winsorizedSampleReuse=false;
      stochasticSampleReuse=false;
      adaptiveSampleReuse=false;
      historyPlaneAcceptance=false;
      robustClamp=false;
      varianceFilter=false;
      stratifiedDirs=false;
      stratifiedFirstBounceOnly=false;
      stratifiedMaterialChromaGuard=false;
      stratifiedHistoryChromaGuard=false;
      stratifiedHistoryGreenOppPositiveOnly=false;
      stratifiedHistoryChromaSupport=false;
      weightedNee=false;
      finalFireflyFilter=false;
      strictFinalFilter=false;
      rangeFinalFilter=false;
      planarFinalFit=false;
      fastPlanarFinalFit=false;
      separablePlanarFinalFit=false;
      connectedPlaneFinalFit=false;
      broadPlaneEnergyLift=false;
      broadPlaneLiftSkyDisabledOnly=false;
      finalDetailRestore=false;
      broadPlaneTextureRestore=false;
      broadPlaneChromaRestore=false;
      materialChromaFloor=false;
      planarFitChromaLossGate=false;
      planarFitCoherentChromaLossGate=false;
      planarFitMeanChromaLossGate=false;
      planarFitConnectedChromaLossGate=false;
      planarFitTemporalChromaLossGate=false;
      planarFitLowPassChromaField=false;
      planarFitTransportChromaField=false;
      planarFitLowPassChromaSatWeight=false;
      planarFitMaterialDarkBlendCap=false;
      planarFitGreenOppLossGate=false;
      transportChromaModel=false;
      planarFitCoherentResidualGate=false;
      lowDiscrepancyFineGate=false;
      lowDiscrepancyGate=false;
      localizedEventHistory=false;
      resetHistoryOnEvent=true;
    }
    hist.assign(W*H,{});
    n.assign(W*H,0);
    lumVar.assign(W*H,0);
    out.assign(W*H,{});
    surfaceModelHist.assign(W*H,{});
    surfaceModelN.assign(W*H,0);
    if(transportChromaModel || planarFitTransportChromaField){
      transportChromaHist.assign(W*H,{});
      transportChromaN.assign(W*H,0);
    } else {
      transportChromaHist.clear();
      transportChromaN.clear();
    }
    adaptivePixelProbCache.clear();
    adaptivePixelProbCachePpp=-1.f;
    buildLightCdfO();
    odf.build();
    probeTraversal();
    captureKeys();
  }

  void onEvent(const SceneEvent& e) override {
    buildLightCdfO();
    adaptivePixelProbCache.clear();
    adaptivePixelProbCachePpp=-1.f;
    surfaceModelHist.assign(W*H,{});
    surfaceModelN.assign(W*H,0);
    if(transportChromaModel || planarFitTransportChromaField){
      transportChromaHist.assign(W*H,{});
      transportChromaN.assign(W*H,0);
    }
    if(e.geomEdit){
      odf.build();
      probeTraversal();
    }

    if(localizedEventHistory && e.geomEdit){
      for(int i=0;i<W*H;i++){
        uint64_t k=MethodPTG::keyFor(gbuf[i]);
        if(i>=(int)pixKey.size() || k!=pixKey[i]){
          hist[i]=V3{};
          n[i]=0.f;
          lumVar[i]=0.f;
        } else {
          n[i]=std::min(n[i], geomHistoryCap);
          lumVar[i]=std::max(lumVar[i], 0.01f);
        }
      }
      captureKeys();
    } else if(resetHistoryOnEvent && e.geomEdit){
      hist.assign(W*H,{});
      n.assign(W*H,0);
      lumVar.assign(W*H,0);
      captureKeys();
    }

    if(localizedEventHistory && e.lightChange){
      for(int i=0;i<W*H;i++){
        n[i]=std::min(n[i], lightHistoryCap);
        lumVar[i]=std::max(lumVar[i], 0.01f);
      }
    } else if(resetHistoryOnEvent && e.lightChange){
      hist.assign(W*H,{});
      n.assign(W*H,0);
      lumVar.assign(W*H,0);
    }
  }

  long frame(long budget) override {
    double ppp = (double)budget / ((double)(W*H)*5.0);
    bool subPixel = ppp < 1.0;
    long basePaths = subPixel ? 1L : std::max(1L,(long)std::floor(ppp));
    double fracP = (ppp>1.0)? ppp-std::floor(ppp) : 0.0;
    uint64_t r0=tl_rays;

    std::vector<float>* pixelProb=nullptr;
    if(subPixel && adaptivePixelSampling){
      if(adaptivePixelUseVariance || adaptivePixelProbCache.empty() || std::fabs(adaptivePixelProbCachePpp-(float)ppp)>1e-6f){
        std::vector<float> weight(W*H,0.f);
        double target=0;
        float maxW=0;
        int eligibleCount=0;
        int br=std::max(1,adaptivePixelBroadRadius);
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
          int i=y*W+x; GPix& g=gbuf[i];
          if(!g.hit || emissive(g.m)) continue;
          eligibleCount++;
          target += ppp;
          float d=luma(directImg[i]);
          float low=1.f - std::min(1.f,std::max(0.f,d/std::max(1e-6f,reuseDirectMax)));
          float w=adaptivePixelHighWeight*(1.f-low) + adaptivePixelLowWeight*low;
          int same=0, tot=0;
          for(int dy=-br;dy<=br;dy++)for(int dx=-br;dx<=br;dx++){
            int xx=x+dx, yy=y+dy;
            if(xx<0||xx>=W||yy<0||yy>=H) continue;
            tot++;
            int j=yy*W+xx; GPix& q=gbuf[j];
            if(q.hit && !emissive(q.m) && q.m==g.m && q.face==g.face &&
               std::fabs(q.depth-g.depth)<=reuseDepthMax) same++;
          }
          float area=tot>0 ? (float)same/(float)tot : 0.f;
          float broad=(area-adaptivePixelAreaThreshold)/std::max(1e-6f,adaptivePixelAreaSoftness);
          broad=std::max(0.f,std::min(1.f,broad));
          broad=broad*broad*(3.f-2.f*broad);
          w*=1.f + (std::max(1.f,adaptivePixelBroadBoost)-1.f)*broad*low;
          if(adaptivePixelUseVariance && !lumVar.empty()){
            float st=std::sqrt(std::max(0.f,lumVar[i]));
            float vt=(st-adaptivePixelVarianceBase)/std::max(1e-6f,adaptivePixelVarianceScale);
            vt=std::max(0.f,std::min(1.f,vt));
            vt=vt*vt*(3.f-2.f*vt);
            float broadLow=0.25f + 0.75f*std::max(low,broad);
            w*=1.f + std::max(0.f,adaptivePixelVarianceBoost)*vt*broadLow;
          }
          weight[i]=std::max(0.001f,w);
          maxW=std::max(maxW,weight[i]);
        }
        adaptivePixelProbCache.assign(W*H,0.f);
        adaptivePixelProbCachePpp=(float)ppp;
        if(eligibleCount>0 && maxW>0){
          float minP=std::max(0.f,adaptivePixelMinProb);
          float maxP=std::min(1.f,std::max(minP,adaptivePixelMaxProb));
          if(minP*(double)eligibleCount>target) minP=0.f;
          double lo=0, hi=1;
          auto sumProb=[&](double s){
            double sum=0;
            for(int i=0;i<W*H;i++) if(weight[i]>0){
              float p=(float)std::min((double)maxP,std::max((double)minP,weight[i]*s));
              sum+=p;
            }
            return sum;
          };
          while(sumProb(hi)<target && hi<1e6) hi*=2.0;
          for(int it=0;it<32;it++){
            double mid=(lo+hi)*0.5;
            if(sumProb(mid)<target) lo=mid; else hi=mid;
          }
          for(int i=0;i<W*H;i++) if(weight[i]>0){
            adaptivePixelProbCache[i]=(float)std::min((double)maxP,std::max((double)minP,weight[i]*hi));
          }
        }
      }
      pixelProb=&adaptivePixelProbCache;
    }

    auto takePixelGate = [&](int i){
      if(!subPixel) return true;
      if(pixelProb && !pixelProb->empty()){
        RNG gate((uint64_t)i,(uint64_t)fcount,77137);
        return gate.uf() < (*pixelProb)[i];
      }
      if(lowDiscrepancyFineGate){
        static const uint8_t bayer8[64]={
           0,48,12,60, 3,51,15,63,
          32,16,44,28,35,19,47,31,
           8,56, 4,52,11,59, 7,55,
          40,24,36,20,43,27,39,23,
           2,50,14,62, 1,49,13,61,
          34,18,46,30,33,17,45,29,
          10,58, 6,54, 9,57, 5,53,
          42,26,38,22,41,25,37,21
        };
        static const uint8_t bayer4[16]={
           0, 8, 2,10,
          12, 4,14, 6,
           3,11, 1, 9,
          15, 7,13, 5
        };
        int x=i%W, y=i/W;
        int bx=(x + (fcount*3)) & 7;
        int by=(y + (fcount*5)) & 7;
        int fx=((x>>3) + fcount) & 3;
        int fy=((y>>3) + fcount*3) & 3;
        int rank=(int)bayer8[by*8+bx]*16 + (int)bayer4[fy*4+fx];
        int threshold=(int)std::floor(std::min(0.999f,(float)ppp)*1024.f);
        return rank < threshold;
      }
      if(lowDiscrepancyGate){
        static const uint8_t bayer8[64]={
           0,48,12,60, 3,51,15,63,
          32,16,44,28,35,19,47,31,
           8,56, 4,52,11,59, 7,55,
          40,24,36,20,43,27,39,23,
           2,50,14,62, 1,49,13,61,
          34,18,46,30,33,17,45,29,
          10,58, 6,54, 9,57, 5,53,
          42,26,38,22,41,25,37,21
        };
        int x=i%W, y=i/W;
        int bx=(x + (fcount*3)) & 7;
        int by=(y + (fcount*5)) & 7;
        int threshold=(int)std::floor(std::min(0.999f,(float)ppp)*64.f);
        return bayer8[by*8+bx] < threshold;
      }
      RNG gate((uint64_t)i,(uint64_t)fcount,91117);
      return gate.uf() < ppp;
    };

    auto relChromaVecForTransport=[](V3 c){
      float lum=std::max(1e-6f,luma(c));
      return V3{c.x/lum-1.f,c.y/lum-1.f,c.z/lum-1.f};
    };

    std::vector<uint8_t> stratifiedHistoryChromaMask;
    if(stratifiedHistoryChromaGuard){
      stratifiedHistoryChromaMask.assign(W*H,0);
      std::vector<uint8_t> rawMask(W*H,0);
      for(int i=0;i<W*H;i++){
        GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        if(n[i]<stratifiedHistoryMinN) continue;
        if(luma(directImg[i])>=stratifiedHistoryDirectMax) continue;
        V3 hc=relChromaVecForTransport(hist[i]);
        float sat=len(hc);
        float greenOpp=hc.y - 0.5f*(hc.x+hc.z);
        bool chromaHit=sat>=stratifiedHistoryChromaMin;
        bool greenHit=stratifiedHistoryGreenOppPositiveOnly
          ? (greenOpp>=stratifiedHistoryGreenOppMin)
          : (std::fabs(greenOpp)>=stratifiedHistoryGreenOppMin);
        if(chromaHit || greenHit) rawMask[i]=1;
      }
      if(stratifiedHistoryChromaSupport){
        int sr=std::max(1,stratifiedHistoryChromaSupportRadius);
        int minCount=std::max(1,stratifiedHistoryChromaSupportMinCount);
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
          int i=y*W+x;
          if(!rawMask[i]) continue;
          GPix& g=gbuf[i];
          int same=0, hit=0;
          for(int dy=-sr;dy<=sr;dy++)for(int dx=-sr;dx<=sr;dx++){
            int xx=x+dx, yy=y+dy;
            if(xx<0||xx>=W||yy<0||yy>=H) continue;
            int j=yy*W+xx; GPix& q=gbuf[j];
            if(!q.hit || emissive(q.m)) continue;
            if(q.m!=g.m || q.face!=g.face) continue;
            if(std::fabs(q.depth-g.depth)>stratifiedHistoryChromaSupportDepthMax) continue;
            same++;
            if(rawMask[j]) hit++;
          }
          float frac=same>0 ? (float)hit/(float)same : 0.f;
          if(hit>=minCount && frac>=stratifiedHistoryChromaSupportMinFrac){
            stratifiedHistoryChromaMask[i]=1;
          }
        }
      } else {
        stratifiedHistoryChromaMask.swap(rawMask);
      }
    }

    auto samplePaths = [&](int i, long paths, RNG& rng, V3* modelChromaOut, float* modelWeightOut){
      GPix& g=gbuf[i];
      V3 E{};
      V3 modelC{};
      float modelW=0.f;
      for(long s=0;s<paths;s++){
        V3 p=g.pos, nn=g.n, tp{1,1,1}; V3 acc{};
        for(int b=0;b<8;b++){
          bool useStrat = stratifiedDirs && (!stratifiedFirstBounceOnly || b==0);
          if(useStrat && stratifiedMaterialChromaGuard && b==0){
            float albMax=std::max(g.alb.x,std::max(g.alb.y,g.alb.z));
            float albMin=std::min(g.alb.x,std::min(g.alb.y,g.alb.z));
            float matSat=albMax-albMin;
            if(matSat>=stratifiedMaterialChromaMin &&
               luma(directImg[i])<stratifiedMaterialDirectMax){
              useStrat=false;
            }
          }
          if(useStrat && stratifiedHistoryChromaGuard && b==0 &&
             !stratifiedHistoryChromaMask.empty() && stratifiedHistoryChromaMask[i]){
            float fp=std::min(1.f,std::max(0.f,stratifiedHistoryChromaFallbackProb));
            bool takeFallback=true;
            if(fp<0.999f){
              RNG histGate((uint64_t)i,(uint64_t)fcount,424243);
              takeFallback = histGate.uf() < fp;
            }
            if(takeFallback) useStrat=false;
          }
          V3 d=useStrat ? cosDirStratO(nn,rng,fcount + i*17 + (int)s*23 + b*11) : cosDir(nn,rng);
          Hit h;
          if(!traceFast(p,d,1e9f,h)){ acc += tp*(SKY.skyL(d)*3.14159265f); break; }
          if(emissive(h.m)) break;
          V3 hp=facePoint(h.vx,h.vy,h.vz,h.face);
          V3 hn=FN[h.face];
          V3 nee=neeIrrO(hp,hn,rng);
          if(transportChromaModel && b==0){
            V3 contrib=MAT[h.m].alb*nee;
            float w=std::max(0.f,luma(contrib));
            if(w>1e-6f){
              modelC += relChromaVecForTransport(contrib)*w;
              modelW += w;
            }
          }
          tp = tp*MAT[h.m].alb;
          acc += tp*nee;
          if(b>=1){ float rr=0.7f; if(rng.uf()>=rr) break; tp=tp*(1.f/rr); }
          p=hp; nn=hn;
        }
        E += acc;
      }
      if(modelChromaOut) *modelChromaOut = (modelW>0.f) ? modelC*(1.f/modelW) : V3{};
      if(modelWeightOut) *modelWeightOut = modelW/(float)std::max(1L,paths);
      return E*(1.f/(float)paths);
    };

    auto updateHistory = [&](int i, V3 E, float obsWeight=1.0f){
      obsWeight=std::max(0.01f, obsWeight);
      if(robustClamp && n[i]>=8.f){
        float base = std::max(luma(hist[i]), 0.f);
        float le = luma(E);
        float cap = base*robustClampMul + robustClampAdd;
        if(le>cap && le>1e-6f) E = E*(cap/le);
      }
      float a = (n[i]<1)?1.f:(progressive ? obsWeight/(n[i]+obsWeight) : std::min(1.f, alpha*obsWeight));
      if(varianceFilter || adaptivePixelUseVariance || trackLumaVariance){
        float pred = luma(hist[i]);
        float obs = luma(E);
        float r = obs - pred;
        lumVar[i] = (n[i]<1.f) ? 0.f : lumVar[i]*(1.f-a) + r*r*a;
      }
      hist[i] = hist[i]*(1-a) + E*a; n[i]+=obsWeight;
    };

    auto updateTransportChroma = [&](int i, V3 c, float w, float obsWeight=1.0f){
      if(!transportChromaModel || transportChromaHist.empty() || transportChromaN.empty()) return;
      if(i<0 || i>=(int)transportChromaHist.size() || i>=(int)transportChromaN.size()) return;
      if(w<=1e-6f) return;
      float sat=len(c);
      if(sat<0.004f) return;
      float ow=std::max(0.01f, obsWeight);
      float a=(transportChromaN[i]<1.f) ? 1.f : ow/(transportChromaN[i]+ow);
      transportChromaHist[i]=transportChromaHist[i]*(1.f-a)+c*a;
      transportChromaN[i]+=ow;
    };

    std::vector<V3> frameHint;
    std::vector<uint8_t> frameHintValid;
    std::vector<V3> acceptHist;
    std::vector<float> acceptN;
    if(historyPlaneAcceptance){
      acceptHist=hist;
      acceptN=n;
    }

    if(spatialSampleReuse){
      std::vector<V3> cur(W*H);
      std::vector<uint8_t> valid(W*H,0);
      if(reconstructionOnlyReuse){
        frameHint.assign(W*H,{});
        frameHintValid.assign(W*H,0);
      }
      for(int i=0;i<W*H;i++){
        GPix& g=gbuf[i]; if(!g.hit||emissive(g.m)) continue;
        if(!takePixelGate(i)) continue;
        RNG rng((uint64_t)i,(uint64_t)fcount,101);
        long paths = basePaths + ((rng.uf()<fracP)?1:0);
        V3 modelC{};
        float modelW=0.f;
        cur[i]=samplePaths(i,paths,rng,&modelC,&modelW);
        updateTransportChroma(i,modelC,modelW,1.f);
        valid[i]=1;
      }
      std::vector<V3> basisHint;
      std::vector<uint8_t> basisValid;
      if(surfaceBasisSampleReuse || persistentSurfaceModel){
        basisHint.assign(W*H,{});
        basisValid.assign(W*H,0);
        std::vector<int> seen(W*H,-1), stack, pix, vpix;
        auto eligibleBasis=[&](int i){
          GPix& g=gbuf[i];
          return g.hit && !emissive(g.m) && (!planarFitLowDirectOnly || luma(directImg[i])<reuseDirectMax);
        };
        auto sameBasisNeighbor=[&](int a,int b){
          GPix& ga=gbuf[a]; GPix& gb=gbuf[b];
          return eligibleBasis(b) && gb.m==ga.m && gb.face==ga.face &&
            std::fabs(gb.depth-ga.depth)<=reuseDepthMax;
        };
        auto solveBasis=[](int n,double AIn[6][6],double bIn[6],double out[6]){
          double m[6][7]{};
          for(int r=0;r<n;r++){
            for(int c=0;c<n;c++) m[r][c]=AIn[r][c];
            m[r][n]=bIn[r];
          }
          for(int col=0;col<n;col++){
            int piv=col;
            double best=std::fabs(m[col][col]);
            for(int r=col+1;r<n;r++){
              double v=std::fabs(m[r][col]);
              if(v>best){ best=v; piv=r; }
            }
            if(best<1e-9) return false;
            if(piv!=col){
              for(int c=col;c<=n;c++){
                double tmp=m[col][c]; m[col][c]=m[piv][c]; m[piv][c]=tmp;
              }
            }
            double inv=1.0/m[col][col];
            for(int c=col;c<=n;c++) m[col][c]*=inv;
            for(int r=0;r<n;r++){
              if(r==col) continue;
              double k=m[r][col];
              if(std::fabs(k)<1e-12) continue;
              for(int c=col;c<=n;c++) m[r][c]-=k*m[col][c];
            }
          }
          for(int r=0;r<n;r++) out[r]=m[r][n];
          return true;
        };
        int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        int cid=0;
        for(int sy=0;sy<H;sy++)for(int sx=0;sx<W;sx++){
          int seed=sy*W+sx;
          if(seen[seed]>=0 || !eligibleBasis(seed)) continue;
          pix.clear(); vpix.clear(); stack.clear();
          seen[seed]=cid; stack.push_back(seed);
          int minX=sx,maxX=sx,minY=sy,maxY=sy;
          while(!stack.empty()){
            int i=stack.back(); stack.pop_back(); pix.push_back(i);
            if(valid[i]) vpix.push_back(i);
            int x=i%W, y=i/W;
            minX=std::min(minX,x); maxX=std::max(maxX,x);
            minY=std::min(minY,y); maxY=std::max(maxY,y);
            for(auto& d:dirs){
              int xx=x+d[0], yy=y+d[1];
              if(xx<0||xx>=W||yy<0||yy>=H) continue;
              int j=yy*W+xx;
              if(seen[j]>=0 || !sameBasisNeighbor(i,j)) continue;
              seen[j]=cid; stack.push_back(j);
            }
          }
          cid++;
          int bw=maxX-minX+1, bh=maxY-minY+1;
          int bbox=std::max(1,bw*bh);
          float fill=(float)pix.size()/(float)bbox;
          if((int)pix.size()<surfaceBasisMinPixels || (int)vpix.size()<surfaceBasisMinValid ||
             bw<surfaceBasisMinSpan || bh<surfaceBasisMinSpan || fill<surfaceBasisMinFill) continue;
          double mean=0, var=0;
          for(int i:vpix) mean+=luma(cur[i]);
          mean/=std::max(1,(int)vpix.size());
          for(int i:vpix){
            double d=luma(cur[i])-mean;
            var+=d*d;
          }
          var/=std::max(1,(int)vpix.size());
          double cap=mean + surfaceBasisRejectSigma*std::sqrt(std::max(0.0,var)) + surfaceBasisRejectAdd;
          int ncoef=surfaceBasisQuadraticFit ? 6 : 3;
          double A[6][6]{}, br[6]{}, bg[6]{}, bb[6]{};
          double cx=0.5*(minX+maxX), cy=0.5*(minY+maxY);
          double sxn=std::max(1.0,0.5*(double)(bw-1));
          double syn=std::max(1.0,0.5*(double)(bh-1));
          int kept=0;
          for(int i:vpix){
            if(luma(cur[i])>cap) continue;
            int x=i%W, y=i/W;
            double nx=((double)x-cx)/sxn, ny=((double)y-cy)/syn;
            double p[6]={1.0,nx,ny,nx*nx,nx*ny,ny*ny};
            for(int a=0;a<ncoef;a++){
              for(int b=0;b<ncoef;b++) A[a][b]+=p[a]*p[b];
              br[a]+=p[a]*cur[i].x; bg[a]+=p[a]*cur[i].y; bb[a]+=p[a]*cur[i].z;
            }
            kept++;
          }
          if(kept<surfaceBasisMinValid) continue;
          double ridge=std::max(0.f,surfaceBasisRidge)*(double)kept;
          for(int a=0;a<ncoef;a++) A[a][a]+=ridge;
          double cr[6]{}, cg[6]{}, cb[6]{};
          if(!solveBasis(ncoef,A,br,cr) || !solveBasis(ncoef,A,bg,cg) || !solveBasis(ncoef,A,bb,cb)) continue;
          for(int i:pix){
            int x=i%W, y=i/W;
            double nx=((double)x-cx)/sxn, ny=((double)y-cy)/syn;
            double p[6]={1.0,nx,ny,nx*nx,nx*ny,ny*ny};
            V3 fit{};
            for(int a=0;a<ncoef;a++){
              fit.x+=(float)(cr[a]*p[a]);
              fit.y+=(float)(cg[a]*p[a]);
              fit.z+=(float)(cb[a]*p[a]);
            }
            fit.x=std::max(0.f,fit.x);
            fit.y=std::max(0.f,fit.y);
            fit.z=std::max(0.f,fit.z);
            basisHint[i]=fit;
            basisValid[i]=1;
          }
        }
        if(persistentSurfaceModel){
          float a=std::min(1.f,std::max(0.001f,persistentSurfaceModelAlpha));
          for(int i=0;i<W*H;i++) if(basisValid[i]){
            if(surfaceModelN[i]<1.f) surfaceModelHist[i]=basisHint[i];
            else surfaceModelHist[i]=surfaceModelHist[i]*(1.f-a) + basisHint[i]*a;
            surfaceModelN[i]+=1.f;
          }
        }
      }
      auto solveAcceptPlane=[](float s00,float sx,float sy,float sxx,float sxy,float syy,
                               float b0,float bx,float by,float& a,float& b,float& c){
        float det =
          s00*(sxx*syy - sxy*sxy) -
          sx *(sx *syy - sxy*sy ) +
          sy *(sx *sxy - sxx*sy );
        if(std::fabs(det)<1e-6f) return false;
        float detA =
          b0*(sxx*syy - sxy*sxy) -
          sx*(bx*syy - sxy*by) +
          sy*(bx*sxy - sxx*by);
        float detB =
          s00*(bx*syy - sxy*by) -
          b0 *(sx*syy - sxy*sy) +
          sy *(sx*by - bx*sy);
        float detC =
          s00*(sxx*by - bx*sxy) -
          sx *(sx*by - bx*sy) +
          b0 *(sx*sxy - sxx*sy);
        a=detA/det; b=detB/det; c=detC/det;
        return true;
      };
      auto acceptObservation = [&](int i, V3 obs, bool hasCenter, float& obsWeight){
        if(!historyPlaneAcceptance || acceptHist.empty() || acceptN.empty()) return obs;
        GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) return obs;
        if(luma(directImg[i])>=reuseDirectMax) return obs;
        if(acceptN[i]<historyPlaneMinN) return obs;
        int x=i%W, y=i/W;
        int r=std::max(1,historyPlaneRadius);
        int x0=std::max(0,x-r), y0=std::max(0,y-r);
        int x1=std::min(W,x+r+1), y1=std::min(H,y+r+1);
        int total=std::max(1,(x1-x0)*(y1-y0));
        int sameGeom=0, fitCount=0;
        float s00=0,sx=0,sy=0,sxx=0,sxy=0,syy=0;
        V3 b0{}, bxv{}, byv{};
        float l0=0,lxv=0,lyv=0,l2=0;
        for(int yy=y0;yy<y1;yy++)for(int xx=x0;xx<x1;xx++){
          int j=yy*W+xx; GPix& q=gbuf[j];
          if(!q.hit || emissive(q.m)) continue;
          if(q.m!=g.m || q.face!=g.face) continue;
          if(dot(q.n,g.n)<reuseNormalMin) continue;
          if(std::fabs(q.depth-g.depth)>reuseDepthMax) continue;
          sameGeom++;
          if(acceptN[j]<historyPlaneMinN) continue;
          float fx=(float)(xx-x), fy=(float)(yy-y);
          V3 hj=acceptHist[j];
          float lj=luma(hj);
          float w=1.f;
          s00+=w; sx+=w*fx; sy+=w*fy; sxx+=w*fx*fx; sxy+=w*fx*fy; syy+=w*fy*fy;
          b0+=hj*w; bxv+=hj*(w*fx); byv+=hj*(w*fy);
          l0+=w*lj; lxv+=w*lj*fx; lyv+=w*lj*fy; l2+=w*lj*lj;
          fitCount++;
        }
        float area=(float)sameGeom/(float)total;
        float areaT=(area-historyPlaneAreaThreshold)/std::max(1e-6f,historyPlaneAreaSoftness);
        areaT=std::max(0.f,std::min(1.f,areaT));
        areaT=areaT*areaT*(3.f-2.f*areaT);
        if(areaT<=0.f || fitCount<historyPlaneMinSamples) return obs;
        V3 fit{};
        float rr=0,rg=0,gr=0,gg=0,br=0,bg=0;
        float la=0,lb=0,lc=0;
        bool ok =
          solveAcceptPlane(s00,sx,sy,sxx,sxy,syy,b0.x,bxv.x,byv.x,fit.x,rr,rg) &&
          solveAcceptPlane(s00,sx,sy,sxx,sxy,syy,b0.y,bxv.y,byv.y,fit.y,gr,gg) &&
          solveAcceptPlane(s00,sx,sy,sxx,sxy,syy,b0.z,bxv.z,byv.z,fit.z,br,bg) &&
          solveAcceptPlane(s00,sx,sy,sxx,sxy,syy,l0,lxv,lyv,la,lb,lc);
        if(!ok) return obs;
        fit.x=std::max(0.f,fit.x);
        fit.y=std::max(0.f,fit.y);
        fit.z=std::max(0.f,fit.z);
        float sse=std::max(0.f, l2 - (la*l0 + lb*lxv + lc*lyv));
        float rms=std::sqrt(sse/std::max(s00,1e-6f));
        float mean=std::fabs(l0/std::max(s00,1e-6f));
        float pred=std::max(0.f,luma(fit));
        float diff=std::fabs(luma(obs)-pred);
        float range=historyPlaneBase + historyPlaneScale*std::max(mean,pred) + historyPlaneRmsScale*rms;
        if(diff<=range || diff<1e-6f) return obs;
        float scale=std::max(0.f,std::min(1.f,range/diff));
        V3 clamped=fit + (obs-fit)*scale;
        clamped.x=std::max(0.f,clamped.x);
        clamped.y=std::max(0.f,clamped.y);
        clamped.z=std::max(0.f,clamped.z);
        float excess=std::max(0.f,std::min(1.f,(diff-range)/std::max(diff,1e-6f)));
        float blend=(hasCenter?historyPlaneCenterBlend:historyPlaneBorrowBlend)*areaT*excess;
        obsWeight*=std::max(0.05f, 1.f - historyPlaneWeightCut*blend);
        return obs*(1.f-blend) + clamped*blend;
      };
      auto updateAccepted = [&](int i, V3 obs, float obsWeight, bool hasCenter){
        obs=acceptObservation(i,obs,hasCenter,obsWeight);
        updateHistory(i,obs,obsWeight);
      };
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i]; if(!g.hit||emissive(g.m)) continue;
        bool allowReuse = !adaptiveSampleReuse || luma(directImg[i]) < reuseDirectMax;
        if(warmupOnlyReuse && n[i]>=reuseWarmupMaxN) allowReuse=false;
        if(reconstructionOnlyReuse){
          if(valid[i]) updateAccepted(i,cur[i],1.0f,true);
          if(!allowReuse) continue;
        } else if(!allowReuse || (preserveCenterSamples && valid[i])){
          if(valid[i]) updateAccepted(i,cur[i],1.0f,true);
          continue;
        }
        float reuseLumaCap=1e30f;
        if(winsorizedSampleReuse){
          int nCand=0;
          float mean=0.f, m2=0.f;
          for(int dy=-reuseRadius;dy<=reuseRadius;dy++)for(int dx=-reuseRadius;dx<=reuseRadius;dx++){
            int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H)continue;
            int j=yy*W+xx; if(!valid[j]) continue;
            GPix& q=gbuf[j]; if(!q.hit||emissive(q.m))continue;
            if(q.m!=g.m || q.face!=g.face) continue;
            if(dot(q.n,g.n)<reuseNormalMin) continue;
            if(std::fabs(q.depth-g.depth)>reuseDepthMax) continue;
            float lj=luma(cur[j]);
            nCand++;
            float delta=lj-mean;
            mean += delta/(float)nCand;
            m2 += delta*(lj-mean);
          }
          if(nCand>=reuseWinsorMinSamples){
            float var=m2/std::max(1,nCand-1);
            reuseLumaCap=mean + reuseWinsorSigma*std::sqrt(std::max(0.f,var)) + reuseWinsorAdd;
          }
        }
        V3 acc{}; float ws=0;
        int sampleCount=0;
        for(int dy=-reuseRadius;dy<=reuseRadius;dy++)for(int dx=-reuseRadius;dx<=reuseRadius;dx++){
          int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H)continue;
          int j=yy*W+xx; if(!valid[j]) continue;
          GPix& q=gbuf[j]; if(!q.hit||emissive(q.m))continue;
          if(q.m!=g.m || q.face!=g.face) continue;
          if(dot(q.n,g.n)<reuseNormalMin) continue;
          if(std::fabs(q.depth-g.depth)>reuseDepthMax) continue;
          if(stochasticSampleReuse && j!=i){
            RNG keepRng((uint64_t)i,(uint64_t)(j + fcount*65537),44381);
            if(keepRng.uf()>reuseStochasticKeep) continue;
          }
          float w=std::exp(-(dx*dx+dy*dy)/reuseSpatialSigma);
          if(historyGuidedSampleReuse && n[i]>=2.f && n[j]>=2.f){
            float ci=luma(hist[i]);
            float cj=luma(hist[j]);
            float dl=std::fabs(cj-ci);
            float range=reuseHistoryBase + reuseHistoryScale*std::max(ci,cj);
            if(dl>range*4.0f) continue;
            float r2=range*range + 1e-6f;
            w *= r2/(r2 + dl*dl);
          }
          if(j==i) w*=reuseCenterWeight;
          V3 sample=cur[j];
          float lj=luma(sample);
          if(lj>reuseLumaCap && lj>1e-6f) sample=sample*(reuseLumaCap/lj);
          acc+=sample*w; ws+=w;
          sampleCount++;
        }
        bool haveSpatialObs = ws>0 && (valid[i] || sampleCount>=reuseMinSamples);
        if(!reconstructionOnlyReuse && surfaceBasisSampleReuse && !basisValid.empty() && basisValid[i]){
          V3 obs{};
          if(haveSpatialObs){
            obs=acc*(1.f/ws);
            float b=std::min(1.f,std::max(0.f,surfaceBasisBlend));
            obs=obs*(1.f-b) + basisHint[i]*b;
          } else {
            obs=basisHint[i];
          }
          float hw = valid[i] ? 1.0f : surfaceBasisBorrowHistoryWeight;
          updateAccepted(i,obs,hw,valid[i]!=0);
        } else if(reconstructionOnlyReuse){
          if(ws>0 && (valid[i] || sampleCount>=reuseMinSamples)){
            if(!reconstructionHintSkippedOnly || !valid[i]){
              frameHint[i]=acc*(1.f/ws);
              frameHintValid[i]=1;
            }
          }
        } else if(haveSpatialObs){
          V3 obs=acc*(1.f/ws);
          if(!reconstructionOnlyReuse && persistentSurfaceModel && surfaceModelN[i]>=persistentSurfaceModelWarmup){
            float b=std::min(1.f,std::max(0.f,persistentSurfaceModelBlend));
            float mo=luma(surfaceModelHist[i]);
            float oo=luma(obs);
            float range=persistentSurfaceModelBiasBase + persistentSurfaceModelBiasScale*std::max(std::fabs(mo), std::fabs(oo));
            float diff=std::fabs(mo-oo);
            float conf=1.f - diff/std::max(range,1e-6f);
            conf=std::max(0.f,std::min(1.f,conf));
            if(n[i]>=2.f){
              float ho=luma(hist[i]);
              float hRange=persistentSurfaceModelBiasBase + persistentSurfaceModelBiasScale*std::max(std::fabs(mo), std::fabs(ho));
              float hConf=1.f - std::fabs(mo-ho)/std::max(hRange,1e-6f);
              conf*=std::max(0.f,std::min(1.f,hConf));
            }
            b*=conf;
            if(b>0.001f) obs=obs*(1.f-b) + surfaceModelHist[i]*b;
          }
          float hw = valid[i] ? 1.0f : reuseBorrowHistoryWeight;
          updateAccepted(i,obs,hw,valid[i]!=0);
        }
      }
    } else {
      for(int i=0;i<W*H;i++){
        GPix& g=gbuf[i]; if(!g.hit||emissive(g.m)) continue;
        if(!takePixelGate(i)) continue;
        RNG rng((uint64_t)i,(uint64_t)fcount,101);
        long paths = basePaths + ((rng.uf()<fracP)?1:0);
        V3 modelC{};
        float modelW=0.f;
        V3 obs=samplePaths(i,paths,rng,&modelC,&modelW);
        updateTransportChroma(i,modelC,modelW,1.f);
        updateHistory(i,obs);
      }
    }
    fcount++;
    std::vector<V3> f(W*H);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
      int i=y*W+x; GPix& g=gbuf[i];
      if(!g.hit){f[i]=hist[i];continue;}
      V3 acc{}; float ws=0;
      bool enforceStrict = strictFinalFilter;
      if(finalFilterMaterialEdgeOnly){
        int er=std::max(1,finalFilterEdgeRadius);
        int same=0,total=0;
        for(int ey=-er;ey<=er;ey++)for(int ex=-er;ex<=er;ex++){
          int xx=x+ex, yy=y+ey; if(xx<0||xx>=W||yy<0||yy>=H) continue;
          total++;
          int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit || emissive(q.m)) continue;
          if(q.m==g.m && q.face==g.face && std::fabs(q.depth-g.depth)<=finalFilterDepthMax) same++;
        }
        float frac=total>0 ? (float)same/(float)total : 0.f;
        enforceStrict = frac < finalFilterEdgeMinSame;
      }
      for(int dy=-finalFilterRadius;dy<=finalFilterRadius;dy++)for(int dx=-finalFilterRadius;dx<=finalFilterRadius;dx++){
        int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H)continue;
        int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit)continue;
        if(enforceStrict && (q.m!=g.m || q.face!=g.face)) continue;
        if(dot(q.n,g.n)<finalFilterNormalMin) continue;
        if(std::fabs(q.depth-g.depth)>finalFilterDepthMax) continue;
        float w=std::exp(-(dx*dx+dy*dy)/finalFilterSigma);
        if(rangeFinalFilter && n[i]>=2.f && n[j]>=2.f){
          float ci=luma(hist[i]);
          float cj=luma(hist[j]);
          float dl=std::fabs(cj-ci);
          float range=finalRangeBase + finalRangeScale*std::max(ci,cj);
          if(dl>range*4.0f) continue;
          float r2=range*range + 1e-6f;
          w *= r2/(r2 + dl*dl);
        }
        V3 src=hist[j];
        if(reconstructionOnlyReuse && !frameHintValid.empty() && frameHintValid[j]){
          float hb=std::min(1.f,std::max(0.f,reconstructionHintBlend));
          src=src*(1.f-hb) + frameHint[j]*hb;
        }
        acc+=src*w; ws+=w;
      }
      V3 base = (ws>0)? acc*(1.f/ws) : hist[i];

      if(!varianceFilter){
        f[i]=base;
        continue;
      }

      float centerStd=std::sqrt(std::max(lumVar[i],0.f));
      float blend=std::min(0.82f, std::max(0.f, (centerStd-0.035f)/0.13f));
      if(n[i]<10.f) blend=std::max(blend, 0.35f);
      if(blend<=0.001f){
        f[i]=base;
        continue;
      }

      V3 wide{}; float wsw=0;
      for(int dy=-4;dy<=4;dy+=2)for(int dx=-4;dx<=4;dx+=2){
        int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H)continue;
        int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit)continue;
        if(dot(q.n,g.n)<0.92f) continue;
        if(std::fabs(q.depth-g.depth)>2.8f) continue;
        if((std::abs(dx)>2 || std::abs(dy)>2) && (q.m!=g.m || q.face!=g.face)) continue;
        float dl=std::fabs(luma(hist[j])-luma(hist[i]));
        float range=0.08f + 2.6f*std::sqrt(std::max(0.f, lumVar[i]+lumVar[j]));
        if(dl>range*3.0f) continue;
        float range2=range*range + 1e-6f;
        float w=std::exp(-(dx*dx+dy*dy)/18.0f) * (range2/(range2 + dl*dl));
        wide+=hist[j]*w; wsw+=w;
      }
      if(wsw>0){
        wide=wide*(1.f/wsw);
        f[i]=base*(1.f-blend) + wide*blend;
      } else {
        f[i]=base;
      }
    }
    if(finalFireflyFilter){
      std::vector<V3> clean=f;
      auto maxc=[](V3 v){ return std::max(v.x, std::max(v.y, v.z)); };
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit) continue;
        V3 mean{}; float meanL=0, meanM=0; int cnt=0;
        for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){
          if(dx==0 && dy==0) continue;
          int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H)continue;
          int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit)continue;
          if(dot(q.n,g.n)<0.92f) continue;
          if(std::fabs(q.depth-g.depth)>2.2f) continue;
          mean+=f[j]; meanL+=luma(f[j]); meanM+=maxc(f[j]); cnt++;
        }
        if(cnt<5) continue;
        mean=mean*(1.f/(float)cnt); meanL/=(float)cnt; meanM/=(float)cnt;
        float varL=0, varM=0;
        for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){
          if(dx==0 && dy==0) continue;
          int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H)continue;
          int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit)continue;
          if(dot(q.n,g.n)<0.92f) continue;
          if(std::fabs(q.depth-g.depth)>2.2f) continue;
          float dl=luma(f[j])-meanL;
          float dm=maxc(f[j])-meanM;
          varL+=dl*dl; varM+=dm*dm;
        }
        varL/=(float)cnt; varM/=(float)cnt;
        float li=luma(f[i]), mi=maxc(f[i]);
        float capL=meanL + 2.2f*std::sqrt(varL) + 0.10f;
        float capM=meanM + 2.2f*std::sqrt(varM) + 0.14f;
        if(li>capL && mi>capM){
          float over=std::min(1.f, std::max(0.f, (li-capL)/(li+1e-6f)));
          float t=0.85f*over;
          clean[i]=f[i]*(1.f-t) + mean*t;
        }
      }
      f.swap(clean);
    }

    std::vector<V3> detailRestoreSrc;
    if(finalDetailRestore || broadPlaneTextureRestore || broadPlaneChromaRestore || broadPlaneLowFreqRestore) detailRestoreSrc=f;

    if(planarFinalFit){
      std::vector<V3> clean=f;
      auto solvePlane=[](float s00,float sx,float sy,float sxx,float sxy,float syy,
                         float b0,float bx,float by,float& a,float& b,float& c){
        float det =
          s00*(sxx*syy - sxy*sxy) -
          sx *(sx *syy - sxy*sy ) +
          sy *(sx *sxy - sxx*sy );
        if(std::fabs(det)<1e-6f) return false;
        float detA =
          b0*(sxx*syy - sxy*sxy) -
          sx*(bx*syy - sxy*by) +
          sy*(bx*sxy - sxx*by);
        float detB =
          s00*(bx*syy - sxy*by) -
          b0 *(sx*syy - sxy*sy) +
          sy *(sx*by - bx*sy);
        float detC =
          s00*(sxx*by - bx*sxy) -
          sx *(sx*by - bx*sy) +
          b0 *(sx*sxy - sxx*sy);
        a=detA/det;
        b=detB/det;
        c=detC/det;
        return true;
      };
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        if(planarFitLowDirectOnly && luma(directImg[i])>=reuseDirectMax) continue;
        float s00=0,sx=0,sy=0,sxx=0,sxy=0,syy=0;
        V3 b0{}, bxv{}, byv{};
        float l0=0,lxv=0,lyv=0,l2=0;
        int cnt=0;
        for(int dy=-planarFitRadius;dy<=planarFitRadius;dy++)for(int dx=-planarFitRadius;dx<=planarFitRadius;dx++){
          int step=std::max(1,planarFitStep);
          if(step>1 && ((std::abs(dx)%step)!=0 || (std::abs(dy)%step)!=0)) continue;
          int xx=x+dx,yy=y+dy; if(xx<0||xx>=W||yy<0||yy>=H) continue;
          int j=yy*W+xx; GPix& q=gbuf[j]; if(!q.hit||emissive(q.m)) continue;
          if(q.m!=g.m || q.face!=g.face) continue;
          if(dot(q.n,g.n)<planarFitNormalMin) continue;
          if(std::fabs(q.depth-g.depth)>planarFitDepthMax) continue;
          float w=std::exp(-(dx*dx+dy*dy)/(float)(planarFitRadius*planarFitRadius));
          float fx=(float)dx, fy=(float)dy;
          float lj=luma(f[j]);
          s00+=w; sx+=w*fx; sy+=w*fy; sxx+=w*fx*fx; sxy+=w*fx*fy; syy+=w*fy*fy;
          b0+=f[j]*w; bxv+=f[j]*(w*fx); byv+=f[j]*(w*fy);
          l0+=w*lj; lxv+=w*lj*fx; lyv+=w*lj*fy; l2+=w*lj*lj;
          cnt++;
        }
        if(cnt<planarFitMinSamples) continue;
        V3 fit{};
        float rx=0,ry=0,gx=0,gy=0,bx=0,by=0;
        bool ok =
          solvePlane(s00,sx,sy,sxx,sxy,syy,b0.x,bxv.x,byv.x,fit.x,rx,ry) &&
          solvePlane(s00,sx,sy,sxx,sxy,syy,b0.y,bxv.y,byv.y,fit.y,gx,gy) &&
          solvePlane(s00,sx,sy,sxx,sxy,syy,b0.z,bxv.z,byv.z,fit.z,bx,by);
        if(!ok) continue;
        fit.x=std::max(0.f,fit.x);
        fit.y=std::max(0.f,fit.y);
        fit.z=std::max(0.f,fit.z);
        float b=std::min(1.f,std::max(0.f,planarFitBlend));
        if(planarFitResidualGate){
          float la=0,lb=0,lc=0;
          if(!solvePlane(s00,sx,sy,sxx,sxy,syy,l0,lxv,lyv,la,lb,lc)) continue;
          float sse=std::max(0.f, l2 - (la*l0 + lb*lxv + lc*lyv));
          float rms=std::sqrt(sse/std::max(s00,1e-6f));
          float mean=std::fabs(l0/std::max(s00,1e-6f));
          float range=planarFitResidualBase + planarFitResidualScale*mean;
          if(rms>range*2.0f) continue;
          float conf=1.f - rms/std::max(range*2.0f,1e-6f);
          b*=std::max(0.f,std::min(1.f,conf));
          if(b<0.02f) continue;
        }
        clean[i]=f[i]*(1.f-b) + fit*b;
      }
      f.swap(clean);
    }

    auto localSurfaceConfidence=[&](int x,int y,int radius,float minSame,float softness){
      int i=y*W+x; GPix& g=gbuf[i];
      int r=std::max(0,radius);
      int same=0,total=0;
      for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++){
        int xx=x+dx, yy=y+dy;
        if(xx<0||xx>=W||yy<0||yy>=H) continue;
        total++;
        int j=yy*W+xx; GPix& q=gbuf[j];
        if(!q.hit || emissive(q.m)) continue;
        if(q.m!=g.m || q.face!=g.face) continue;
        if(std::fabs(q.depth-g.depth)>reuseDepthMax) continue;
        same++;
      }
      float area = total>0 ? (float)same/(float)total : 0.f;
      float t=(area-minSame)/std::max(1e-6f,softness);
      t=std::max(0.f,std::min(1.f,t));
      return t*t*(3.f-2.f*t);
    };

    auto voxelContactAmount=[&](const GPix& g){
      if(!g.hit) return 0.f;
      int nx=OXf[g.face], ny=OYf[g.face], nz=OZf[g.face];
      int dirs[4][3]{};
      if(nx!=0){
        dirs[0][1]=1; dirs[1][1]=-1; dirs[2][2]=1; dirs[3][2]=-1;
      } else if(ny!=0){
        dirs[0][0]=1; dirs[1][0]=-1; dirs[2][2]=1; dirs[3][2]=-1;
      } else {
        dirs[0][0]=1; dirs[1][0]=-1; dirs[2][1]=1; dirs[3][1]=-1;
      }
      int contact=0,total=0;
      for(int k=0;k<4;k++){
        int tx=dirs[k][0], ty=dirs[k][1], tz=dirs[k][2];
        int sx=g.vx+tx, sy=g.vy+ty, sz=g.vz+tz;
        total++;
        bool tangentSolid=at(sx,sy,sz)!=0;
        bool tangentExposed=at(sx+nx,sy+ny,sz+nz)==0;
        if(!tangentSolid || !tangentExposed) contact++;
      }
      return total>0 ? (float)contact/(float)total : 0.f;
    };

    if(fastPlanarFinalFit){
      struct PAcc {
        float s00=0,sx=0,sy=0,sxx=0,sxy=0,syy=0;
        float rx=0,ry=0,gx=0,gy=0,bx=0,by=0;
        float r=0,g=0,b=0,l=0,lx=0,ly=0,l2=0;
        float dl=0,dl2=0;
        PAcc operator+(const PAcc& o) const {
          PAcc a=*this;
          a.s00+=o.s00; a.sx+=o.sx; a.sy+=o.sy; a.sxx+=o.sxx; a.sxy+=o.sxy; a.syy+=o.syy;
          a.rx+=o.rx; a.ry+=o.ry; a.gx+=o.gx; a.gy+=o.gy; a.bx+=o.bx; a.by+=o.by;
          a.r+=o.r; a.g+=o.g; a.b+=o.b; a.l+=o.l; a.lx+=o.lx; a.ly+=o.ly; a.l2+=o.l2;
          a.dl+=o.dl; a.dl2+=o.dl2;
          return a;
        }
        PAcc operator-(const PAcc& o) const {
          PAcc a=*this;
          a.s00-=o.s00; a.sx-=o.sx; a.sy-=o.sy; a.sxx-=o.sxx; a.sxy-=o.sxy; a.syy-=o.syy;
          a.rx-=o.rx; a.ry-=o.ry; a.gx-=o.gx; a.gy-=o.gy; a.bx-=o.bx; a.by-=o.by;
          a.r-=o.r; a.g-=o.g; a.b-=o.b; a.l-=o.l; a.lx-=o.lx; a.ly-=o.ly; a.l2-=o.l2;
          a.dl-=o.dl; a.dl2-=o.dl2;
          return a;
        }
        PAcc& operator+=(const PAcc& o){
          *this = *this + o;
          return *this;
        }
      };
      struct CAcc {
        float w=0,x=0,y=0,z=0;
        CAcc operator+(const CAcc& o) const {
          CAcc a=*this;
          a.w+=o.w; a.x+=o.x; a.y+=o.y; a.z+=o.z;
          return a;
        }
        CAcc operator-(const CAcc& o) const {
          CAcc a=*this;
          a.w-=o.w; a.x-=o.x; a.y-=o.y; a.z-=o.z;
          return a;
        }
        CAcc& operator+=(const CAcc& o){
          *this = *this + o;
          return *this;
        }
      };
      auto solvePlane=[](float s00,float sx,float sy,float sxx,float sxy,float syy,
                         float b0,float bx,float by,float& a,float& b,float& c){
        float det =
          s00*(sxx*syy - sxy*sxy) -
          sx *(sx *syy - sxy*sy ) +
          sy *(sx *sxy - sxx*sy );
        if(std::fabs(det)<1e-6f) return false;
        float detA =
          b0*(sxx*syy - sxy*sxy) -
          sx*(bx*syy - sxy*by) +
          sy*(bx*sxy - sxx*by);
        float detB =
          s00*(bx*syy - sxy*by) -
          b0 *(sx*syy - sxy*sy) +
          sy *(sx*by - bx*sy);
        float detC =
          s00*(sxx*by - bx*sxy) -
          sx *(sx*by - bx*sy) +
          b0 *(sx*sxy - sxx*sy);
        a=detA/det; b=detB/det; c=detC/det;
        return true;
      };
      std::vector<uint8_t> seen(256,0);
      std::vector<int> keys;
      auto fitKey=[&](const GPix& g){
        int key=g.m*6+g.face;
        if(!fastPlanarDepthBins) return key;
        int bin=(int)std::floor(g.depth/std::max(0.25f,planarFitDepthBinSize));
        bin=std::max(0,std::min(4095,bin));
        return key*4096 + bin;
      };
      for(int i=0;i<W*H;i++){
        GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        int key=fitKey(g);
        if(key>=(int)seen.size()) seen.resize(key+1,0);
        if(!seen[key]){ seen[key]=1; keys.push_back(key); }
      }
      std::vector<V3> clean=f;
      int iw=W+1, ih=H+1;
      std::vector<PAcc> integ(iw*ih);
      std::vector<CAcc> chromaInteg;
      if((planarFitLowPassChromaField || planarFitTransportChromaField) &&
         !planarFitLumaOnly) chromaInteg.resize(iw*ih);
      int rfit=std::max(1,planarFitRadius);
      std::vector<V3> connectedChromaDir;
      std::vector<float> connectedChromaWeight;
      std::vector<float> connectedChromaSat;
      auto relChromaVecForConn=[](V3 c){
        float lum=std::max(1e-6f,luma(c));
        return V3{c.x/lum-1.f,c.y/lum-1.f,c.z/lum-1.f};
      };
      if(planarFitConnectedChromaLossGate && !planarFitLumaOnly){
        connectedChromaDir.assign(W*H,V3{});
        connectedChromaWeight.assign(W*H,0.f);
        connectedChromaSat.assign(W*H,0.f);
        std::vector<uint8_t> cseen(W*H,0);
        std::vector<int> cstack, cpix;
        int dirs4[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
        auto eligibleConn=[&](int i){
          GPix& g=gbuf[i];
          return g.hit && !emissive(g.m) &&
            (!planarFitLowDirectOnly || luma(directImg[i])<reuseDirectMax);
        };
        auto sameConn=[&](int a,int b){
          GPix& ga=gbuf[a]; GPix& gb=gbuf[b];
          return eligibleConn(b) && fitKey(gb)==fitKey(ga) &&
            std::fabs(gb.depth-ga.depth)<=planarFitDepthMax;
        };
        int minPixels=std::max(1,planarFitConnectedChromaMinPixels);
        int minSpan=std::max(1,planarFitConnectedChromaMinSpan);
        for(int sy=0;sy<H;sy++)for(int sx=0;sx<W;sx++){
          int seed=sy*W+sx;
          if(cseen[seed] || !eligibleConn(seed)) continue;
          cseen[seed]=1;
          cstack.clear();
          cpix.clear();
          cstack.push_back(seed);
          int minX=sx,maxX=sx,minY=sy,maxY=sy;
          while(!cstack.empty()){
            int i=cstack.back();
            cstack.pop_back();
            cpix.push_back(i);
            int x=i%W, y=i/W;
            minX=std::min(minX,x); maxX=std::max(maxX,x);
            minY=std::min(minY,y); maxY=std::max(maxY,y);
            for(auto& d:dirs4){
              int xx=x+d[0], yy=y+d[1];
              if(xx<0||xx>=W||yy<0||yy>=H) continue;
              int j=yy*W+xx;
              if(cseen[j] || !sameConn(i,j)) continue;
              cseen[j]=1;
              cstack.push_back(j);
            }
          }
          int bw=maxX-minX+1, bh=maxY-minY+1;
          int bbox=std::max(1,bw*bh);
          float fill=(float)cpix.size()/(float)bbox;
          if((int)cpix.size()<minPixels || bw<minSpan || bh<minSpan ||
             fill<planarFitConnectedChromaMinFill) continue;
          V3 meanC{};
          for(int i:cpix) meanC+=relChromaVecForConn(f[i]);
          meanC=meanC*(1.f/(float)cpix.size());
          float sat=len(meanC);
          if(sat<=planarFitConnectedChromaSatMin) continue;
          V3 dir=meanC*(1.f/std::max(sat,1e-6f));
          int agree=0;
          float agreeMin=0.35f*sat;
          for(int i:cpix){
            float proj=dot(relChromaVecForConn(f[i]),dir);
            if(proj>agreeMin) agree++;
          }
          float support=(float)agree/(float)std::max(1,(int)cpix.size());
          if(support<=planarFitConnectedChromaSupportMin) continue;
          float satT=(sat-planarFitConnectedChromaSatMin)/
            std::max(1e-6f,planarFitConnectedChromaSatSoftness);
          satT=std::max(0.f,std::min(1.f,satT));
          satT=satT*satT*(3.f-2.f*satT);
          float supportT=(support-planarFitConnectedChromaSupportMin)/
            std::max(1e-6f,1.f-planarFitConnectedChromaSupportMin);
          supportT=std::max(0.f,std::min(1.f,supportT));
          supportT=supportT*supportT*(3.f-2.f*supportT);
          float weight=satT*supportT;
          if(weight<=0.f) continue;
          for(int i:cpix){
            connectedChromaDir[i]=dir;
            connectedChromaWeight[i]=weight;
            connectedChromaSat[i]=sat;
          }
        }
      }
      std::vector<V3> temporalChromaDir;
      std::vector<float> temporalChromaWeight;
      std::vector<float> temporalChromaSat;
      if(planarFitTemporalChromaLossGate && !planarFitLumaOnly){
        temporalChromaDir.assign(W*H,V3{});
        temporalChromaWeight.assign(W*H,0.f);
        temporalChromaSat.assign(W*H,0.f);
        int tr=std::max(0,planarFitTemporalChromaRadius);
        int minSamples=std::max(1,planarFitTemporalChromaMinSamples);
        float minN=std::max(1.f,planarFitTemporalChromaMinN);
        auto histStableWeight=[&](int j){
          if(j<0 || j>=W*H) return 0.f;
          if(n[j]<minN) return 0.f;
          float nw=std::min(1.f,n[j]/std::max(minN,1e-6f));
          float st=std::sqrt(std::max(0.f,lumVar[j]));
          float sw=(planarFitTemporalChromaStdMax-st)/
            std::max(1e-6f,planarFitTemporalChromaStdSoftness);
          sw=std::max(0.f,std::min(1.f,sw));
          sw=sw*sw*(3.f-2.f*sw);
          return nw*sw;
        };
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
          int i=y*W+x; GPix& g=gbuf[i];
          if(!g.hit || emissive(g.m)) continue;
          if(planarFitLowDirectOnly && luma(directImg[i])>=reuseDirectMax) continue;
          V3 meanC{};
          float wsum=0.f;
          int samples=0;
          for(int dy=-tr;dy<=tr;dy++)for(int dx=-tr;dx<=tr;dx++){
            int xx=x+dx, yy=y+dy;
            if(xx<0||xx>=W||yy<0||yy>=H) continue;
            int j=yy*W+xx; GPix& q=gbuf[j];
            if(!q.hit || emissive(q.m)) continue;
            if(fitKey(q)!=fitKey(g)) continue;
            if(std::fabs(q.depth-g.depth)>planarFitDepthMax) continue;
            if(planarFitLowDirectOnly && luma(directImg[j])>=reuseDirectMax) continue;
            float w=histStableWeight(j);
            if(w<=0.f) continue;
            meanC+=relChromaVecForConn(hist[j])*w;
            wsum+=w;
            samples++;
          }
          if(samples<minSamples || wsum<=0.f) continue;
          meanC=meanC*(1.f/wsum);
          float sat=len(meanC);
          if(sat<=planarFitTemporalChromaSatMin) continue;
          V3 dir=meanC*(1.f/std::max(sat,1e-6f));
          float agreeW=0.f;
          float agreeMin=0.30f*sat;
          for(int dy=-tr;dy<=tr;dy++)for(int dx=-tr;dx<=tr;dx++){
            int xx=x+dx, yy=y+dy;
            if(xx<0||xx>=W||yy<0||yy>=H) continue;
            int j=yy*W+xx; GPix& q=gbuf[j];
            if(!q.hit || emissive(q.m)) continue;
            if(fitKey(q)!=fitKey(g)) continue;
            if(std::fabs(q.depth-g.depth)>planarFitDepthMax) continue;
            if(planarFitLowDirectOnly && luma(directImg[j])>=reuseDirectMax) continue;
            float w=histStableWeight(j);
            if(w<=0.f) continue;
            float proj=dot(relChromaVecForConn(hist[j]),dir);
            if(proj>agreeMin) agreeW+=w;
          }
          float support=agreeW/std::max(wsum,1e-6f);
          if(support<=planarFitTemporalChromaSupportMin) continue;
          float satT=(sat-planarFitTemporalChromaSatMin)/
            std::max(1e-6f,planarFitTemporalChromaSatSoftness);
          satT=std::max(0.f,std::min(1.f,satT));
          satT=satT*satT*(3.f-2.f*satT);
          float supportT=(support-planarFitTemporalChromaSupportMin)/
            std::max(1e-6f,1.f-planarFitTemporalChromaSupportMin);
          supportT=std::max(0.f,std::min(1.f,supportT));
          supportT=supportT*supportT*(3.f-2.f*supportT);
          float sampleT=std::min(1.f,wsum/(float)minSamples);
          float weight=satT*supportT*sampleT;
          if(weight<=0.f) continue;
          temporalChromaDir[i]=dir;
          temporalChromaWeight[i]=weight;
          temporalChromaSat[i]=sat;
        }
      }
      for(int key: keys){
        std::fill(integ.begin(), integ.end(), PAcc{});
        if(!chromaInteg.empty()) std::fill(chromaInteg.begin(), chromaInteg.end(), CAcc{});
        for(int y=0;y<H;y++){
          PAcc row{};
          CAcc chromaRow{};
          for(int x=0;x<W;x++){
            int i=y*W+x; GPix& g=gbuf[i];
            if(g.hit && !emissive(g.m) && fitKey(g)==key){
              float fx=(float)x, fy=(float)y;
              float lj=luma(f[i]);
              float dj=luma(directImg[i]);
              PAcc v;
              v.s00=1.f; v.sx=fx; v.sy=fy; v.sxx=fx*fx; v.sxy=fx*fy; v.syy=fy*fy;
              v.r=f[i].x; v.g=f[i].y; v.b=f[i].z;
              v.rx=f[i].x*fx; v.ry=f[i].x*fy;
              v.gx=f[i].y*fx; v.gy=f[i].y*fy;
              v.bx=f[i].z*fx; v.by=f[i].z*fy;
              v.l=lj; v.lx=lj*fx; v.ly=lj*fy; v.l2=lj*lj;
              v.dl=dj; v.dl2=dj*dj;
              row+=v;
              if(!chromaInteg.empty() &&
                 (!planarFitLowDirectOnly || luma(directImg[i])<reuseDirectMax)){
                float cw=1.f;
                V3 hc{};
                if(planarFitTransportChromaField){
                  if(i>=(int)transportChromaN.size() || i>=(int)transportChromaHist.size() ||
                     transportChromaN[i]<planarFitLowPassChromaMinN){
                    cw=0.f;
                  } else {
                    cw=std::min(1.f,transportChromaN[i]/
                      std::max(1e-6f,planarFitLowPassChromaMinN));
                    hc=transportChromaHist[i];
                  }
                } else {
                  if(n[i]<planarFitLowPassChromaMinN) cw=0.f;
                  else {
                    cw=std::min(1.f,n[i]/
                      std::max(1e-6f,planarFitLowPassChromaMinN));
                    hc=relChromaVecForConn(hist[i]);
                  }
                }
                if(cw>0.f){
                  if(planarFitLowPassChromaSatWeight){
                    float sat=len(hc);
                    float st=(sat-planarFitLowPassChromaWeightSatMin)/
                      std::max(1e-6f,planarFitLowPassChromaWeightSatSoftness);
                    st=std::max(0.f,std::min(1.f,st));
                    st=st*st*(3.f-2.f*st);
                    cw*=st;
                  }
                  if(cw<=0.f) continue;
                  CAcc cv;
                  cv.w=cw; cv.x=hc.x*cw; cv.y=hc.y*cw; cv.z=hc.z*cw;
                  chromaRow+=cv;
                }
              }
            }
            integ[(y+1)*iw + (x+1)] = integ[y*iw + (x+1)] + row;
            if(!chromaInteg.empty()){
              chromaInteg[(y+1)*iw + (x+1)] =
                chromaInteg[y*iw + (x+1)] + chromaRow;
            }
          }
        }
        auto rect=[&](int x0,int y0,int x1,int y1){
          return integ[y1*iw+x1] - integ[y0*iw+x1] - integ[y1*iw+x0] + integ[y0*iw+x0];
        };
        auto chromaRect=[&](int x0,int y0,int x1,int y1){
          return chromaInteg[y1*iw+x1] - chromaInteg[y0*iw+x1] -
            chromaInteg[y1*iw+x0] + chromaInteg[y0*iw+x0];
        };
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
          int i=y*W+x; GPix& g=gbuf[i];
          if(!g.hit || emissive(g.m) || fitKey(g)!=key) continue;
          if(planarFitLowDirectOnly && luma(directImg[i])>=reuseDirectMax) continue;
          int x0=std::max(0,x-rfit), y0=std::max(0,y-rfit);
          int x1=std::min(W,x+rfit+1), y1=std::min(H,y+rfit+1);
          PAcc s=rect(x0,y0,x1,y1);
          if(s.s00<planarFitMinSamples) continue;
          V3 fit{};
          V3 fitDx{}, fitDy{};
          float fitLumDx=0.f, fitLumDy=0.f;
          if(planarFitLumaOnly){
            float al=0,bl=0,cl=0;
            if(!solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.l,s.lx,s.ly,al,bl,cl)) continue;
            float targetLum=std::max(0.f, al+bl*(float)x+cl*(float)y);
            float curLum=std::max(1e-6f,luma(f[i]));
            float scale=targetLum/curLum;
            scale=std::max(planarFitLumaScaleMin,std::min(planarFitLumaScaleMax,scale));
            fit=f[i]*scale;
            fitDx=f[i]*(bl/std::max(1e-6f,curLum));
            fitDy=f[i]*(cl/std::max(1e-6f,curLum));
            fitLumDx=bl;
            fitLumDy=cl;
          } else {
            float ar=0,br=0,cr=0, ag=0,bg=0,cg=0, ab=0,bb=0,cb=0;
            bool ok =
              solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.r,s.rx,s.ry,ar,br,cr) &&
              solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.g,s.gx,s.gy,ag,bg,cg) &&
              solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.b,s.bx,s.by,ab,bb,cb);
            if(!ok) continue;
            fit={ar+br*(float)x+cr*(float)y, ag+bg*(float)x+cg*(float)y, ab+bb*(float)x+cb*(float)y};
            fitDx={br,bg,bb};
            fitDy={cr,cg,cb};
            fitLumDx=0.2126f*br + 0.7152f*bg + 0.0722f*bb;
            fitLumDy=0.2126f*cr + 0.7152f*cg + 0.0722f*cb;
          }
          fit.x=std::max(0.f,fit.x);
          fit.y=std::max(0.f,fit.y);
          fit.z=std::max(0.f,fit.z);
          if((planarFitLowPassChromaField || planarFitTransportChromaField) && !planarFitLumaOnly &&
             !chromaInteg.empty()){
            CAcc cs=chromaRect(x0,y0,x1,y1);
            if(cs.w>=planarFitLowPassChromaMinWeight){
              V3 meanC{cs.x/std::max(cs.w,1e-6f),
                       cs.y/std::max(cs.w,1e-6f),
                       cs.z/std::max(cs.w,1e-6f)};
              float sat=len(meanC);
              if(sat>planarFitLowPassChromaSatMin){
                float satT=(sat-planarFitLowPassChromaSatMin)/
                  std::max(1e-6f,planarFitLowPassChromaSatSoftness);
                satT=std::max(0.f,std::min(1.f,satT));
                satT=satT*satT*(3.f-2.f*satT);
                float targetLum=std::max(0.f,luma(fit));
                V3 chromaFit{targetLum*(1.f+meanC.x),
                             targetLum*(1.f+meanC.y),
                             targetLum*(1.f+meanC.z)};
                chromaFit.x=std::max(0.f,chromaFit.x);
                chromaFit.y=std::max(0.f,chromaFit.y);
                chromaFit.z=std::max(0.f,chromaFit.z);
                float maxDelta=planarFitLowPassChromaClampBase +
                  planarFitLowPassChromaClampScale*targetLum;
                V3 d=chromaFit-fit;
                d.x=std::max(-maxDelta,std::min(maxDelta,d.x));
                d.y=std::max(-maxDelta,std::min(maxDelta,d.y));
                d.z=std::max(-maxDelta,std::min(maxDelta,d.z));
                float a=std::max(0.f,std::min(1.f,planarFitLowPassChromaAmount))*satT;
                fit=fit + d*a;
                fit.x=std::max(0.f,fit.x);
                fit.y=std::max(0.f,fit.y);
                fit.z=std::max(0.f,fit.z);
              }
            }
          }
          if(planarFitChromaAnchor && !planarFitLumaOnly){
            float targetLum=std::max(0.f,luma(fit));
            float srcLum=std::max(1e-6f,luma(f[i]));
            if(targetLum>1e-6f){
              float scale=targetLum/srcLum;
              scale=std::max(planarFitChromaAnchorScaleMin,
                std::min(planarFitChromaAnchorScaleMax,scale));
              V3 chromaFit=f[i]*scale;
              float maxDelta=planarFitChromaAnchorClampBase +
                planarFitChromaAnchorClampScale*targetLum;
              V3 d=chromaFit-fit;
              d.x=std::max(-maxDelta,std::min(maxDelta,d.x));
              d.y=std::max(-maxDelta,std::min(maxDelta,d.y));
              d.z=std::max(-maxDelta,std::min(maxDelta,d.z));
              float a=std::max(0.f,std::min(1.f,planarFitChromaAnchorAmount));
              fit=fit + d*a;
              fit.x=std::max(0.f,fit.x);
              fit.y=std::max(0.f,fit.y);
              fit.z=std::max(0.f,fit.z);
            }
          }
          if(planarFitMaterialChromaAnchor && !planarFitLumaOnly){
            float albMax=std::max(g.alb.x,std::max(g.alb.y,g.alb.z));
            float albMin=std::min(g.alb.x,std::min(g.alb.y,g.alb.z));
            float matSat=albMax-albMin;
            float mt=(matSat-planarFitMaterialChromaMin)/
              std::max(1e-6f,planarFitMaterialChromaSoftness);
            mt=std::max(0.f,std::min(1.f,mt));
            mt=mt*mt*(3.f-2.f*mt);
            if(mt>0.f){
              float surf=localSurfaceConfidence(
                x,y,planarFitMaterialChromaSurfaceRadius,
                planarFitMaterialChromaSurfaceMinSame,
                planarFitMaterialChromaSurfaceSoftness);
              float a=std::max(0.f,std::min(1.f,planarFitMaterialChromaAmount))*mt*surf;
              if(a>0.f){
                float targetLum=std::max(0.f,luma(fit));
                float srcLum=std::max(1e-6f,luma(f[i]));
                if(targetLum>1e-6f){
                  V3 chromaFit=f[i]*(targetLum/srcLum);
                  float maxDelta=planarFitMaterialChromaClampBase +
                    planarFitMaterialChromaClampScale*targetLum;
                  V3 d=chromaFit-fit;
                  d.x=std::max(-maxDelta,std::min(maxDelta,d.x));
                  d.y=std::max(-maxDelta,std::min(maxDelta,d.y));
                  d.z=std::max(-maxDelta,std::min(maxDelta,d.z));
                  fit=fit + d*a;
                  fit.x=std::max(0.f,fit.x);
                  fit.y=std::max(0.f,fit.y);
                  fit.z=std::max(0.f,fit.z);
                }
              }
            }
          }
          if(planarFitMultiscaleResidual && !planarFitLumaOnly){
            int rs=std::max(1,planarFitMultiscaleRadius);
            int sx0=std::max(0,x-rs), sy0=std::max(0,y-rs);
            int sx1=std::min(W,x+rs+1), sy1=std::min(H,y+rs+1);
            PAcc ss=rect(sx0,sy0,sx1,sy1);
            if(ss.s00>=std::max(3,planarFitMultiscaleMinSamples)){
              float sr=0,sbr=0,scr=0, sg=0,sbg=0,scg=0, sb=0,sbb=0,scb=0;
              bool okSmall =
                solvePlane(ss.s00,ss.sx,ss.sy,ss.sxx,ss.sxy,ss.syy,ss.r,ss.rx,ss.ry,sr,sbr,scr) &&
                solvePlane(ss.s00,ss.sx,ss.sy,ss.sxx,ss.sxy,ss.syy,ss.g,ss.gx,ss.gy,sg,sbg,scg) &&
                solvePlane(ss.s00,ss.sx,ss.sy,ss.sxx,ss.sxy,ss.syy,ss.b,ss.bx,ss.by,sb,sbb,scb);
              if(okSmall){
                V3 smallFit{sr+sbr*(float)x+scr*(float)y,
                            sg+sbg*(float)x+scg*(float)y,
                            sb+sbb*(float)x+scb*(float)y};
                smallFit.x=std::max(0.f,smallFit.x);
                smallFit.y=std::max(0.f,smallFit.y);
                smallFit.z=std::max(0.f,smallFit.z);
                V3 delta=smallFit-fit;
                float maxDelta=planarFitMultiscaleClampBase +
                  planarFitMultiscaleClampScale*std::max(0.f,luma(fit));
                delta.x=std::max(-maxDelta,std::min(maxDelta,delta.x));
                delta.y=std::max(-maxDelta,std::min(maxDelta,delta.y));
                delta.z=std::max(-maxDelta,std::min(maxDelta,delta.z));
                float a=std::max(0.f,std::min(1.f,planarFitMultiscaleAmount));
                if(planarFitMultiscaleMaterialGate){
                  float albMax=std::max(g.alb.x,std::max(g.alb.y,g.alb.z));
                  float albMin=std::min(g.alb.x,std::min(g.alb.y,g.alb.z));
                  float matSat=albMax-albMin;
                  float mt=(matSat-planarFitMultiscaleMaterialMin)/
                    std::max(1e-6f,planarFitMultiscaleMaterialSoftness);
                  mt=std::max(0.f,std::min(1.f,mt));
                  a*=mt*mt*(3.f-2.f*mt);
                }
                if(planarFitMultiscaleDarkGate){
                  float surfL=luma(g.alb*(fit*(1.f/3.14159265f)));
                  float dark=(planarFitMultiscaleLumaMax-surfL)/
                    std::max(1e-6f,planarFitMultiscaleLumaMax-planarFitMultiscaleLumaMin);
                  dark=std::max(0.f,std::min(1.f,dark));
                  a*=dark*dark*(3.f-2.f*dark);
                }
                fit=fit + delta*a;
                fit.x=std::max(0.f,fit.x);
                fit.y=std::max(0.f,fit.y);
                fit.z=std::max(0.f,fit.z);
              }
            }
          }
          float b=std::min(1.f,std::max(0.f,planarFitBlend));
          if(planarFitAreaBlend){
            float denom=(float)std::max(1,(x1-x0)*(y1-y0));
            float area=s.s00/denom;
            float t=(area-planarFitAreaThreshold)/std::max(1e-6f,planarFitAreaSoftness);
            t=std::max(0.f,std::min(1.f,t));
            t=t*t*(3.f-2.f*t);
            float bh=std::min(1.f,std::max(0.f,planarFitBlendHigh));
            b=b*(1.f-t)+bh*t;
          }
          if(planarFitMaterialDarkBlendCap){
            float albMax=std::max(g.alb.x,std::max(g.alb.y,g.alb.z));
            float albMin=std::min(g.alb.x,std::min(g.alb.y,g.alb.z));
            float matSat=albMax-albMin;
            float mt=(matSat-planarFitMaterialDarkSatMin)/
              std::max(1e-6f,planarFitMaterialDarkSatSoftness);
            mt=std::max(0.f,std::min(1.f,mt));
            mt=mt*mt*(3.f-2.f*mt);
            float surfL=luma(g.alb*(fit*(1.f/3.14159265f)));
            float dt=(planarFitMaterialDarkLumaMax-surfL)/
              std::max(1e-6f,planarFitMaterialDarkLumaMax-planarFitMaterialDarkLumaMin);
            dt=std::max(0.f,std::min(1.f,dt));
            dt=dt*dt*(3.f-2.f*dt);
            float t=mt*dt;
            float cap=std::min(1.f,std::max(0.f,planarFitMaterialDarkBlendCapValue));
            if(t>0.f && b>cap) b=b*(1.f-t)+cap*t;
          }
          if(planarFitGreenOppLossGate && !planarFitLumaOnly){
            auto greenOppRel=[](V3 c){
              float lum=std::max(1e-6f,luma(c));
              V3 rc{c.x/lum-1.f,c.y/lum-1.f,c.z/lum-1.f};
              return rc.y - 0.5f*(rc.x+rc.z);
            };
            float invS=1.f/std::max(s.s00,1e-6f);
            V3 meanSrc{s.r*invS,s.g*invS,s.b*invS};
            float meanX=s.sx*invS;
            float meanY=s.sy*invS;
            V3 meanFit=fit + fitDx*(meanX-(float)x) + fitDy*(meanY-(float)y);
            meanFit.x=std::max(0.f,meanFit.x);
            meanFit.y=std::max(0.f,meanFit.y);
            meanFit.z=std::max(0.f,meanFit.z);
            float srcOpp=greenOppRel(meanSrc);
            float fitOpp=greenOppRel(meanFit);
            float loss=srcOpp-fitOpp;
            if(srcOpp>planarFitGreenOppMin && loss>planarFitGreenOppLossMin){
              float srcT=(srcOpp-planarFitGreenOppMin)/
                std::max(1e-6f,planarFitGreenOppSoftness);
              srcT=std::max(0.f,std::min(1.f,srcT));
              srcT=srcT*srcT*(3.f-2.f*srcT);
              float lossT=(loss-planarFitGreenOppLossMin)/
                std::max(1e-6f,planarFitGreenOppLossSoftness);
              lossT=std::max(0.f,std::min(1.f,lossT));
              lossT=lossT*lossT*(3.f-2.f*lossT);
              float t=srcT*lossT;
              float cap=std::min(1.f,std::max(0.f,planarFitGreenOppBlendCap));
              if(t>0.f && b>cap) b=b*(1.f-t)+cap*t;
            }
          }
          if(planarFitConnectedChromaLossGate && !planarFitLumaOnly &&
             !connectedChromaWeight.empty()){
            float conf=connectedChromaWeight[i];
            if(conf>0.f){
              V3 dir=connectedChromaDir[i];
              float compSat=std::max(connectedChromaSat[i],1e-6f);
              V3 srcC=relChromaVecForConn(f[i]);
              V3 fitC=relChromaVecForConn(fit);
              float srcProj=dot(srcC,dir);
              float fitProj=dot(fitC,dir);
              float loss=srcProj-fitProj;
              if(srcProj>0.15f*compSat && loss>0.f){
                float lossRel=loss/compSat;
                float lossT=(lossRel-planarFitConnectedChromaLossMin)/
                  std::max(1e-6f,planarFitConnectedChromaLossSoftness);
                lossT=std::max(0.f,std::min(1.f,lossT));
                lossT=lossT*lossT*(3.f-2.f*lossT);
                float t=conf*lossT;
                float cap=std::min(1.f,std::max(0.f,planarFitConnectedChromaBlendCap));
                if(t>0.f && b>cap) b=b*(1.f-t)+cap*t;
              }
            }
          }
          if(planarFitTemporalChromaLossGate && !planarFitLumaOnly &&
             !temporalChromaWeight.empty()){
            float conf=temporalChromaWeight[i];
            if(conf>0.f){
              V3 dir=temporalChromaDir[i];
              float tempSat=std::max(temporalChromaSat[i],1e-6f);
              V3 srcC=relChromaVecForConn(f[i]);
              V3 fitC=relChromaVecForConn(fit);
              float srcProj=dot(srcC,dir);
              float fitProj=dot(fitC,dir);
              float loss=srcProj-fitProj;
              if(srcProj>0.12f*tempSat && loss>0.f){
                float lossRel=loss/tempSat;
                float lossT=(lossRel-planarFitTemporalChromaLossMin)/
                  std::max(1e-6f,planarFitTemporalChromaLossSoftness);
                lossT=std::max(0.f,std::min(1.f,lossT));
                lossT=lossT*lossT*(3.f-2.f*lossT);
                float t=conf*lossT;
                float cap=std::min(1.f,std::max(0.f,planarFitTemporalChromaBlendCap));
                if(t>0.f && b>cap) b=b*(1.f-t)+cap*t;
              }
            }
          }
          if(planarFitCoherentChromaLossGate && !planarFitLumaOnly){
            auto relChromaVec=[](V3 c){
              float lum=std::max(1e-6f,luma(c));
              return V3{c.x/lum-1.f,c.y/lum-1.f,c.z/lum-1.f};
            };
            V3 srcC=relChromaVec(f[i]);
            V3 fitC=relChromaVec(fit);
            V3 lossC=srcC-fitC;
            float srcSat=len(srcC);
            float lossMag=len(lossC);
            float lossRel=lossMag/std::max(srcSat,1e-6f);
            if(srcSat>planarFitCoherentChromaSatMin &&
               lossRel>planarFitCoherentChromaLossMin){
              int crad=std::max(1,planarFitCoherentChromaRadius);
              int total=0, strong=0;
              for(int dy=-crad;dy<=crad;dy++)for(int dx=-crad;dx<=crad;dx++){
                if(dx==0 && dy==0) continue;
                int xx=x+dx, yy=y+dy;
                if(xx<0||xx>=W||yy<0||yy>=H) continue;
                int j=yy*W+xx; GPix& q=gbuf[j];
                if(!q.hit || emissive(q.m)) continue;
                if(q.m!=g.m || q.face!=g.face) continue;
                if(std::fabs(q.depth-g.depth)>planarFitDepthMax) continue;
                total++;
                V3 pred=fit + fitDx*(float)dx + fitDy*(float)dy;
                pred.x=std::max(0.f,pred.x);
                pred.y=std::max(0.f,pred.y);
                pred.z=std::max(0.f,pred.z);
                V3 nSrcC=relChromaVec(f[j]);
                V3 nFitC=relChromaVec(pred);
                V3 nLoss=nSrcC-nFitC;
                float nSrcSat=len(nSrcC);
                float nLossMag=len(nLoss);
                if(nSrcSat<=planarFitCoherentChromaSatMin) continue;
                float nLossRel=nLossMag/std::max(nSrcSat,1e-6f);
                if(nLossRel<=planarFitCoherentChromaLossMin) continue;
                float dir=dot(lossC,nLoss)/std::max(1e-6f,lossMag*nLossMag);
                if(dir>=planarFitCoherentChromaDirMin) strong++;
              }
              if(total>=3 && strong>=2){
                float support=(float)strong/(float)total;
                float supportT=(support-planarFitCoherentChromaSupportMin)/
                  std::max(1e-6f,1.f-planarFitCoherentChromaSupportMin);
                supportT=std::max(0.f,std::min(1.f,supportT));
                supportT=supportT*supportT*(3.f-2.f*supportT);
                float lossT=(lossRel-planarFitCoherentChromaLossMin)/
                  std::max(1e-6f,planarFitCoherentChromaLossSoftness);
                lossT=std::max(0.f,std::min(1.f,lossT));
                lossT=lossT*lossT*(3.f-2.f*lossT);
                float t=supportT*lossT;
                float cap=std::min(1.f,std::max(0.f,planarFitCoherentChromaBlendCap));
                if(t>0.f && b>cap) b=b*(1.f-t)+cap*t;
              }
            }
          }
          if(planarFitMeanChromaLossGate && !planarFitLumaOnly){
            auto relChromaVec=[](V3 c){
              float lum=std::max(1e-6f,luma(c));
              return V3{c.x/lum-1.f,c.y/lum-1.f,c.z/lum-1.f};
            };
            float invS=1.f/std::max(s.s00,1e-6f);
            V3 meanSrc{s.r*invS,s.g*invS,s.b*invS};
            float meanX=s.sx*invS;
            float meanY=s.sy*invS;
            V3 meanFit=fit + fitDx*(meanX-(float)x) + fitDy*(meanY-(float)y);
            meanFit.x=std::max(0.f,meanFit.x);
            meanFit.y=std::max(0.f,meanFit.y);
            meanFit.z=std::max(0.f,meanFit.z);
            V3 srcC=relChromaVec(meanSrc);
            V3 fitC=relChromaVec(meanFit);
            V3 lossC=srcC-fitC;
            float srcSat=len(srcC);
            float lossRel=len(lossC)/std::max(srcSat,1e-6f);
            if(srcSat>planarFitMeanChromaSatMin &&
               lossRel>planarFitMeanChromaLossMin){
              float lossT=(lossRel-planarFitMeanChromaLossMin)/
                std::max(1e-6f,planarFitMeanChromaLossSoftness);
              lossT=std::max(0.f,std::min(1.f,lossT));
              lossT=lossT*lossT*(3.f-2.f*lossT);
              float cap=std::min(1.f,std::max(0.f,planarFitMeanChromaBlendCap));
              if(lossT>0.f && b>cap) b=b*(1.f-lossT)+cap*lossT;
            }
          }
          if(planarFitChromaLossGate && !planarFitLumaOnly){
            auto relChroma=[](V3 c){
              float lum=std::max(1e-6f,luma(c));
              float dr=c.x-lum, dg=c.y-lum, db=c.z-lum;
              return std::sqrt(std::max(0.f,(dr*dr+dg*dg+db*db)/3.f))/lum;
            };
            float srcSat=relChroma(f[i]);
            float fitSat=relChroma(fit);
            if(srcSat>fitSat){
              float satT=(srcSat-planarFitChromaLossSatMin)/
                std::max(1e-6f,planarFitChromaLossSatSoftness);
              satT=std::max(0.f,std::min(1.f,satT));
              satT=satT*satT*(3.f-2.f*satT);
              float loss=(srcSat-fitSat)/std::max(srcSat,1e-6f);
              float lossT=(loss-planarFitChromaLossMin)/
                std::max(1e-6f,planarFitChromaLossSoftness);
              lossT=std::max(0.f,std::min(1.f,lossT));
              lossT=lossT*lossT*(3.f-2.f*lossT);
              float t=satT*lossT;
              float cap=std::min(1.f,std::max(0.f,planarFitChromaLossBlendCap));
              if(t>0.f && b>cap) b=b*(1.f-t)+cap*t;
            }
          }
          if(planarFitStableHistoryGate && n[i]>=planarFitStableHistoryMinN){
            float st=std::sqrt(std::max(0.f,lumVar[i]));
            float stable=(planarFitStableHistoryStdMax-st)/
              std::max(1e-6f,planarFitStableHistoryStdSoftness);
            stable=std::max(0.f,std::min(1.f,stable));
            stable=stable*stable*(3.f-2.f*stable);
            float cap=std::min(1.f,std::max(0.f,planarFitStableHistoryBlendCap));
            if(stable>0.f && b>cap) b=b*(1.f-stable)+cap*stable;
          }
          if(planarFitDirectStableBlend || planarFitDirectGradientGate){
            float meanD=s.dl/std::max(s.s00,1e-6f);
            float varD=std::max(0.f, s.dl2/std::max(s.s00,1e-6f) - meanD*meanD);
            float stdD=std::sqrt(varD);
            if(planarFitDirectStableBlend){
              if(meanD>planarFitDirectMeanMax || stdD>planarFitDirectStdMax){
                b=std::min(b, std::min(1.f,std::max(0.f,planarFitBlend)));
              }
            }
            if(planarFitDirectGradientGate){
              float gt=(stdD-planarFitDirectGradientStdMin)/
                std::max(1e-6f,planarFitDirectGradientStdMax-planarFitDirectGradientStdMin);
              gt=std::max(0.f,std::min(1.f,gt));
              gt=gt*gt*(3.f-2.f*gt);
              float cap=std::min(1.f,std::max(0.f,planarFitDirectGradientBlendCap));
              if(gt>0.f && b>cap){
                b=b*(1.f-gt)+cap*gt;
              }
            }
          }
          if(planarFitVoxelContactGate){
            float c=voxelContactAmount(g);
            float a=std::max(0.f,std::min(1.f,planarFitVoxelContactAmount));
            b*=1.f-a*c;
            if(b<0.02f) continue;
          }
          if(planarFitResidualGate){
            float al=0,bl=0,cl=0;
            if(!solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.l,s.lx,s.ly,al,bl,cl)) continue;
            float sse=std::max(0.f, s.l2 - (al*s.l + bl*s.lx + cl*s.ly));
            float rms=std::sqrt(sse/std::max(s.s00,1e-6f));
            float mean=std::fabs(s.l/std::max(s.s00,1e-6f));
            float range=planarFitResidualBase + planarFitResidualScale*mean;
            if(rms>range*2.0f) continue;
            float conf=1.f - rms/std::max(range*2.0f,1e-6f);
            b*=std::max(0.f,std::min(1.f,conf));
            if(b<0.02f) continue;
          }
          if(planarFitEdgeConfidence){
            b*=localSurfaceConfidence(x,y,planarFitEdgeRadius,planarFitEdgeMinSame,planarFitEdgeSoftness);
            if(b<0.02f) continue;
          }
          if(planarFitGradientLossGate){
            float srcG2=0.f, fitG2=0.f;
            int gcnt=0;
            float li=luma(f[i]);
            auto addLocalGradient=[&](int dx,int dy,float fitDelta){
              int xx=x+dx, yy=y+dy;
              if(xx<0||xx>=W||yy<0||yy>=H) return;
              int j=yy*W+xx; GPix& q=gbuf[j];
              if(!q.hit || emissive(q.m)) return;
              if(q.m!=g.m || q.face!=g.face) return;
              if(std::fabs(q.depth-g.depth)>reuseDepthMax) return;
              float d=luma(f[j])-li;
              srcG2+=d*d;
              fitG2+=fitDelta*fitDelta;
              gcnt++;
            };
            addLocalGradient( 1, 0,  fitLumDx);
            addLocalGradient(-1, 0, -fitLumDx);
            addLocalGradient( 0, 1,  fitLumDy);
            addLocalGradient( 0,-1, -fitLumDy);
            if(gcnt>0){
              float srcGrad=std::sqrt(srcG2/(float)gcnt);
              float fitGrad=std::sqrt(fitG2/(float)gcnt);
              if(srcGrad>planarFitGradientLossMin && fitGrad<srcGrad){
                float loss=(srcGrad-fitGrad)/std::max(srcGrad,1e-6f);
                float t=(loss-planarFitGradientLossStart)/
                  std::max(1e-6f,planarFitGradientLossEnd-planarFitGradientLossStart);
                t=std::max(0.f,std::min(1.f,t));
                t=t*t*(3.f-2.f*t);
                float cap=std::min(1.f,std::max(0.f,planarFitGradientLossBlendCap));
                if(t>0.f && b>cap) b=b*(1.f-t)+cap*t;
                if(b<0.02f) continue;
              }
            }
          }
          if(planarFitCoherentResidualGate){
            float fitLi=luma(fit);
            float r0=luma(f[i])-fitLi;
            float ar0=std::fabs(r0);
            if(ar0>planarFitCoherentResidualMin){
              int cr=std::max(1,planarFitCoherentRadius);
              int total=0, strong=0, sameSign=0;
              float sgn=(r0>=0.f)?1.f:-1.f;
              for(int dy=-cr;dy<=cr;dy++)for(int dx=-cr;dx<=cr;dx++){
                if(dx==0 && dy==0) continue;
                int xx=x+dx, yy=y+dy;
                if(xx<0||xx>=W||yy<0||yy>=H) continue;
                int j=yy*W+xx; GPix& q=gbuf[j];
                if(!q.hit || emissive(q.m)) continue;
                if(q.m!=g.m || q.face!=g.face) continue;
                if(std::fabs(q.depth-g.depth)>planarFitDepthMax) continue;
                total++;
                float pred=fitLi + fitLumDx*(float)dx + fitLumDy*(float)dy;
                float rj=luma(f[j])-pred;
                if(std::fabs(rj)<planarFitCoherentResidualMin) continue;
                strong++;
                if(rj*sgn>0.f) sameSign++;
              }
              if(total>=3 && strong>=2){
                float frac=(float)sameSign/(float)std::max(1,strong);
                float support=(float)strong/(float)std::max(1,total);
                float ct=(frac-planarFitCoherentSignMin)/
                  std::max(1e-6f,planarFitCoherentSignSoftness);
                ct=std::max(0.f,std::min(1.f,ct));
                ct=ct*ct*(3.f-2.f*ct);
                float mag=(ar0-planarFitCoherentResidualMin)/
                  std::max(1e-6f,planarFitCoherentResidualScale);
                mag=std::max(0.f,std::min(1.f,mag));
                mag=mag*mag*(3.f-2.f*mag);
                float t=ct*mag*support;
                if(planarFitCoherentPatchDamp){
                  float fill=(float)sameSign/(float)std::max(1,total);
                  float pt=(fill-planarFitCoherentPatchFillStart)/
                    std::max(1e-6f,planarFitCoherentPatchFillEnd-planarFitCoherentPatchFillStart);
                  pt=std::max(0.f,std::min(1.f,pt));
                  pt=pt*pt*(3.f-2.f*pt);
                  float minScale=std::min(1.f,std::max(0.f,planarFitCoherentPatchMinScale));
                  t*=1.f-pt*(1.f-minScale);
                }
                float cap=std::min(1.f,std::max(0.f,planarFitCoherentBlendCap));
                if(t>0.f && b>cap) b=b*(1.f-t)+cap*t;
                if(b<0.02f) continue;
              }
            }
          }
          clean[i]=f[i]*(1.f-b) + fit*b;
        }
      }
      f.swap(clean);
    }

    if(separablePlanarFinalFit){
      struct GAcc {
        float s00=0,sx=0,sy=0,sxx=0,sxy=0,syy=0;
        float rx=0,ry=0,gx=0,gy=0,bx=0,by=0;
        float r=0,g=0,b=0,l=0,lx=0,ly=0,l2=0;
      };
      auto addScaled=[](GAcc& a,const GAcc& o,float w){
        a.s00+=o.s00*w; a.sx+=o.sx*w; a.sy+=o.sy*w; a.sxx+=o.sxx*w; a.sxy+=o.sxy*w; a.syy+=o.syy*w;
        a.rx+=o.rx*w; a.ry+=o.ry*w; a.gx+=o.gx*w; a.gy+=o.gy*w; a.bx+=o.bx*w; a.by+=o.by*w;
        a.r+=o.r*w; a.g+=o.g*w; a.b+=o.b*w; a.l+=o.l*w; a.lx+=o.lx*w; a.ly+=o.ly*w; a.l2+=o.l2*w;
      };
      auto solvePlane=[](float s00,float sx,float sy,float sxx,float sxy,float syy,
                         float b0,float bx,float by,float& a,float& b,float& c){
        float det =
          s00*(sxx*syy - sxy*sxy) -
          sx *(sx *syy - sxy*sy ) +
          sy *(sx *sxy - sxx*sy );
        if(std::fabs(det)<1e-6f) return false;
        float detA =
          b0*(sxx*syy - sxy*sxy) -
          sx*(bx*syy - sxy*by) +
          sy*(bx*sxy - sxx*by);
        float detB =
          s00*(bx*syy - sxy*by) -
          b0 *(sx*syy - sxy*sy) +
          sy *(sx*by - bx*sy);
        float detC =
          s00*(sxx*by - bx*sxy) -
          sx *(sx*by - bx*sy) +
          b0 *(sx*sxy - sxx*sy);
        a=detA/det; b=detB/det; c=detC/det;
        return true;
      };
      int rfit=std::max(1,planarFitRadius);
      std::vector<float> kw(2*rfit+1);
      for(int d=-rfit; d<=rfit; d++) kw[d+rfit]=std::exp(-(d*d)/(float)(rfit*rfit));
      std::vector<uint8_t> seen(256,0);
      std::vector<int> keys;
      for(int i=0;i<W*H;i++){
        GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        int key=g.m*6+g.face;
        if(key>=(int)seen.size()) seen.resize(key+1,0);
        if(!seen[key]){ seen[key]=1; keys.push_back(key); }
      }
      std::vector<V3> clean=f;
      std::vector<GAcc> src(W*H), tmp(W*H), sum(W*H);
      for(int key: keys){
        std::fill(src.begin(), src.end(), GAcc{});
        std::fill(tmp.begin(), tmp.end(), GAcc{});
        std::fill(sum.begin(), sum.end(), GAcc{});
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
          int i=y*W+x; GPix& g=gbuf[i];
          if(!g.hit || emissive(g.m) || g.m*6+g.face!=key) continue;
          float fx=(float)x, fy=(float)y;
          float lj=luma(f[i]);
          GAcc v;
          v.s00=1.f; v.sx=fx; v.sy=fy; v.sxx=fx*fx; v.sxy=fx*fy; v.syy=fy*fy;
          v.r=f[i].x; v.g=f[i].y; v.b=f[i].z;
          v.rx=f[i].x*fx; v.ry=f[i].x*fy;
          v.gx=f[i].y*fx; v.gy=f[i].y*fy;
          v.bx=f[i].z*fx; v.by=f[i].z*fy;
          v.l=lj; v.lx=lj*fx; v.ly=lj*fy; v.l2=lj*lj;
          src[i]=v;
        }
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
          GAcc a;
          for(int dx=-rfit; dx<=rfit; dx++){
            int xx=x+dx; if(xx<0||xx>=W) continue;
            addScaled(a, src[y*W+xx], kw[dx+rfit]);
          }
          tmp[y*W+x]=a;
        }
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
          GAcc a;
          for(int dy=-rfit; dy<=rfit; dy++){
            int yy=y+dy; if(yy<0||yy>=H) continue;
            addScaled(a, tmp[yy*W+x], kw[dy+rfit]);
          }
          sum[y*W+x]=a;
        }
        for(int y=0;y<H;y++)for(int x=0;x<W;x++){
          int i=y*W+x; GPix& g=gbuf[i];
          if(!g.hit || emissive(g.m) || g.m*6+g.face!=key) continue;
          if(planarFitLowDirectOnly && luma(directImg[i])>=reuseDirectMax) continue;
          GAcc s=sum[i];
          if(s.s00<planarFitMinSamples) continue;
          float ar=0,br=0,cr=0, ag=0,bg=0,cg=0, ab=0,bb=0,cb=0;
          bool ok =
            solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.r,s.rx,s.ry,ar,br,cr) &&
            solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.g,s.gx,s.gy,ag,bg,cg) &&
            solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.b,s.bx,s.by,ab,bb,cb);
          if(!ok) continue;
          V3 fit{ar+br*(float)x+cr*(float)y, ag+bg*(float)x+cg*(float)y, ab+bb*(float)x+cb*(float)y};
          fit.x=std::max(0.f,fit.x);
          fit.y=std::max(0.f,fit.y);
          fit.z=std::max(0.f,fit.z);
          float b=std::min(1.f,std::max(0.f,planarFitBlend));
          if(planarFitResidualGate){
            float al=0,bl=0,cl=0;
            if(!solvePlane(s.s00,s.sx,s.sy,s.sxx,s.sxy,s.syy,s.l,s.lx,s.ly,al,bl,cl)) continue;
            float sse=std::max(0.f, s.l2 - (al*s.l + bl*s.lx + cl*s.ly));
            float rms=std::sqrt(sse/std::max(s.s00,1e-6f));
            float mean=std::fabs(s.l/std::max(s.s00,1e-6f));
            float range=planarFitResidualBase + planarFitResidualScale*mean;
            if(rms>range*2.0f) continue;
            float conf=1.f - rms/std::max(range*2.0f,1e-6f);
            b*=std::max(0.f,std::min(1.f,conf));
            if(b<0.02f) continue;
          }
          clean[i]=f[i]*(1.f-b) + fit*b;
        }
      }
      f.swap(clean);
    }

    if(connectedPlaneFinalFit){
      std::vector<V3> clean=f;
      std::vector<int> seen(W*H,-1), stack, pix;
      auto eligible=[&](int i){
        GPix& g=gbuf[i];
        return g.hit && !emissive(g.m) && (!planarFitLowDirectOnly || luma(directImg[i])<reuseDirectMax);
      };
      auto sameNeighbor=[&](int a,int b){
        GPix& ga=gbuf[a]; GPix& gb=gbuf[b];
        return eligible(b) && gb.m==ga.m && gb.face==ga.face &&
          std::fabs(gb.depth-ga.depth)<=planarFitDepthMax;
      };
      auto solveSmall=[](int n,double AIn[6][6],double bIn[6],double out[6]){
        double m[6][7]{};
        for(int r=0;r<n;r++){
          for(int c=0;c<n;c++) m[r][c]=AIn[r][c];
          m[r][n]=bIn[r];
        }
        for(int col=0;col<n;col++){
          int piv=col;
          double best=std::fabs(m[col][col]);
          for(int r=col+1;r<n;r++){
            double v=std::fabs(m[r][col]);
            if(v>best){ best=v; piv=r; }
          }
          if(best<1e-9) return false;
          if(piv!=col){
            for(int c=col;c<=n;c++){
              double tmp=m[col][c]; m[col][c]=m[piv][c]; m[piv][c]=tmp;
            }
          }
          double inv=1.0/m[col][col];
          for(int c=col;c<=n;c++) m[col][c]*=inv;
          for(int r=0;r<n;r++){
            if(r==col) continue;
            double k=m[r][col];
            if(std::fabs(k)<1e-12) continue;
            for(int c=col;c<=n;c++) m[r][c]-=k*m[col][c];
          }
        }
        for(int r=0;r<n;r++) out[r]=m[r][n];
        return true;
      };
      int cid=0;
      int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
      int minPixels=std::max(1,connectedPlaneMinPixels);
      int minSpan=std::max(1,connectedPlaneMinSpan);
      int interiorR=std::max(1,connectedPlaneInteriorRadius);
      for(int sy=0;sy<H;sy++)for(int sx=0;sx<W;sx++){
        int seed=sy*W+sx;
        if(seen[seed]>=0 || !eligible(seed)) continue;
        pix.clear(); stack.clear();
        seen[seed]=cid; stack.push_back(seed);
        int minX=sx,maxX=sx,minY=sy,maxY=sy;
        while(!stack.empty()){
          int i=stack.back(); stack.pop_back(); pix.push_back(i);
          int x=i%W, y=i/W;
          minX=std::min(minX,x); maxX=std::max(maxX,x);
          minY=std::min(minY,y); maxY=std::max(maxY,y);
          for(auto& d:dirs){
            int xx=x+d[0], yy=y+d[1];
            if(xx<0||xx>=W||yy<0||yy>=H) continue;
            int j=yy*W+xx;
            if(seen[j]>=0 || !sameNeighbor(i,j)) continue;
            seen[j]=cid; stack.push_back(j);
          }
        }
        cid++;
        int bw=maxX-minX+1, bh=maxY-minY+1;
        int bbox=std::max(1,bw*bh);
        float fill=(float)pix.size()/(float)bbox;
        if((int)pix.size()<minPixels || bw<minSpan || bh<minSpan || fill<connectedPlaneMinFill) continue;
        std::vector<int> fitPix;
        fitPix.reserve(pix.size());
        auto interiorFrac=[&](int i){
          if(!connectedPlaneInteriorOnly) return 1.f;
          int x=i%W, y=i/W;
          int same=0, total=0;
          for(int dy=-interiorR;dy<=interiorR;dy++)for(int dx=-interiorR;dx<=interiorR;dx++){
            int xx=x+dx, yy=y+dy;
            if(xx<0||xx>=W||yy<0||yy>=H) continue;
            total++;
            int j=yy*W+xx;
            if(j<(int)seen.size() && seen[j]==seen[i]) same++;
          }
          return total>0 ? (float)same/(float)total : 0.f;
        };
        for(int i:pix){
          if(interiorFrac(i)>=connectedPlaneInteriorMinSame) fitPix.push_back(i);
        }
        if(connectedPlaneInteriorOnly && (int)fitPix.size()<std::max(32,minPixels/3)) continue;
        int ncoef=connectedPlaneQuadraticFit ? 6 : 3;
        double A[6][6]{}, br[6]{}, bg[6]{}, bb[6]{};
        double cx=0.5*(minX+maxX), cy=0.5*(minY+maxY);
        double sxn=std::max(1.0,0.5*(double)(bw-1));
        double syn=std::max(1.0,0.5*(double)(bh-1));
        for(int i:fitPix){
          int x=i%W, y=i/W;
          double nx=((double)x-cx)/sxn, ny=((double)y-cy)/syn;
          double p[6]={1.0,nx,ny,nx*nx,nx*ny,ny*ny};
          for(int a=0;a<ncoef;a++){
            for(int b=0;b<ncoef;b++) A[a][b]+=p[a]*p[b];
            br[a]+=p[a]*f[i].x; bg[a]+=p[a]*f[i].y; bb[a]+=p[a]*f[i].z;
          }
        }
        double cr[6]{}, cg[6]{}, cb[6]{};
        if(!solveSmall(ncoef,A,br,cr) || !solveSmall(ncoef,A,bg,cg) || !solveSmall(ncoef,A,bb,cb)) continue;
        float base=std::min(1.f,std::max(0.f,connectedPlaneBlend));
        for(int i:pix){
          float interior=interiorFrac(i);
          if(connectedPlaneInteriorOnly && interior<connectedPlaneInteriorMinSame) continue;
          int x=i%W, y=i/W;
          double nx=((double)x-cx)/sxn, ny=((double)y-cy)/syn;
          double p[6]={1.0,nx,ny,nx*nx,nx*ny,ny*ny};
          V3 fit{};
          for(int a=0;a<ncoef;a++){
            fit.x+=(float)(cr[a]*p[a]);
            fit.y+=(float)(cg[a]*p[a]);
            fit.z+=(float)(cb[a]*p[a]);
          }
          fit.x=std::max(0.f,fit.x);
          fit.y=std::max(0.f,fit.y);
          fit.z=std::max(0.f,fit.z);
          int same=0;
          for(auto& d:dirs){
            int xx=x+d[0], yy=y+d[1];
            if(xx<0||xx>=W||yy<0||yy>=H) continue;
            int j=yy*W+xx;
            if(seen[j]==seen[i]) same++;
          }
          float edge=std::min(1.f,std::max(0.f,(float)same*0.25f));
          float b=base*(0.35f+0.65f*edge);
          if(connectedPlaneInteriorOnly) b*=std::min(1.f,std::max(0.f,interior));
          clean[i]=f[i]*(1.f-b) + fit*b;
        }
      }
      f.swap(clean);
    }

    if(broadPlaneEnergyLift && (!broadPlaneLiftSkyDisabledOnly || !SKY.enabled)){
      int r=std::max(1,broadPlaneLiftRadius);
      float amount=std::max(0.f,broadPlaneLiftAmount);
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        float d=luma(directImg[i]);
        if(d>=reuseDirectMax) continue;
        int same=0,total=0;
        for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++){
          int xx=x+dx, yy=y+dy;
          if(xx<0||xx>=W||yy<0||yy>=H) continue;
          total++;
          int j=yy*W+xx; GPix& q=gbuf[j];
          if(!q.hit || emissive(q.m)) continue;
          if(q.m!=g.m || q.face!=g.face) continue;
          if(std::fabs(q.depth-g.depth)>reuseDepthMax) continue;
          same++;
        }
        float area = total>0 ? (float)same/(float)total : 0.f;
        float t=(area-broadPlaneLiftAreaThreshold)/std::max(1e-6f,broadPlaneLiftAreaSoftness);
        t=std::max(0.f,std::min(1.f,t));
        t=t*t*(3.f-2.f*t);
        if(t<=0.f) continue;
        if(broadPlaneLiftEdgeConfidence){
          t*=localSurfaceConfidence(x,y,broadPlaneLiftEdgeRadius,broadPlaneLiftEdgeMinSame,broadPlaneLiftEdgeSoftness);
          if(t<=0.f) continue;
        }
        if(broadPlaneLiftLumaGate){
          float li=luma(g.alb*(f[i]*(1.f/3.14159265f)));
          float lg=(li-broadPlaneLiftLumaMin)/std::max(1e-6f,broadPlaneLiftLumaMax-broadPlaneLiftLumaMin);
          lg=std::max(0.f,std::min(1.f,lg));
          lg=lg*lg*(3.f-2.f*lg);
          float dark=std::max(0.f,std::min(1.f,broadPlaneLiftDarkScale));
          t*=dark + (1.f-dark)*lg;
          if(t<=0.f) continue;
        }
        if(broadPlaneLiftVoxelContactGate){
          float c=voxelContactAmount(g);
          float a=std::max(0.f,std::min(1.f,broadPlaneLiftVoxelContactAmount));
          t*=1.f-a*c;
          if(t<=0.f) continue;
        }
        float low=1.f-std::min(1.f,std::max(0.f,d/std::max(1e-6f,reuseDirectMax)));
        f[i]=f[i]*(1.f + amount*t*low);
      }
    }

    if(broadPlaneChromaRestore && !detailRestoreSrc.empty()){
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        float d=luma(directImg[i]);
        if(d>=reuseDirectMax) continue;
        float interior=localSurfaceConfidence(
          x,y,chromaRestoreSurfaceRadius,chromaRestoreSurfaceMinSame,
          chromaRestoreSurfaceSoftness);
        if(interior<=0.f) continue;
        float surfL=luma(g.alb*(f[i]*(1.f/3.14159265f)));
        float dark=(chromaRestoreLumaMax-surfL)/
          std::max(1e-6f,chromaRestoreLumaMax-chromaRestoreLumaMin);
        dark=std::max(0.f,std::min(1.f,dark));
        dark=dark*dark*(3.f-2.f*dark);
        if(dark<=0.f) continue;
        float targetLum=std::max(0.f,luma(f[i]));
        float srcLum=std::max(1e-6f,luma(detailRestoreSrc[i]));
        V3 chroma=detailRestoreSrc[i]*(targetLum/srcLum);
        V3 delta=chroma-f[i];
        float cap=chromaRestoreClampBase +
          chromaRestoreClampScale*std::max(0.f,targetLum);
        delta.x=std::max(-cap,std::min(cap,delta.x));
        delta.y=std::max(-cap,std::min(cap,delta.y));
        delta.z=std::max(-cap,std::min(cap,delta.z));
        float low=1.f-std::min(1.f,std::max(0.f,d/std::max(1e-6f,reuseDirectMax)));
        float a=std::max(0.f,chromaRestoreAmount)*interior*dark*low;
        f[i].x=std::max(0.f,f[i].x+delta.x*a);
        f[i].y=std::max(0.f,f[i].y+delta.y*a);
        f[i].z=std::max(0.f,f[i].z+delta.z*a);
      }
    }

    if(broadPlaneLowFreqRestore && !detailRestoreSrc.empty()){
      int r=std::max(1,lowFreqRestoreRadius);
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        float d=luma(directImg[i]);
        if(d>=reuseDirectMax) continue;
        float interior=localSurfaceConfidence(
          x,y,lowFreqRestoreSurfaceRadius,lowFreqRestoreSurfaceMinSame,
          lowFreqRestoreSurfaceSoftness);
        if(interior<=0.f) continue;
        float surfL=luma(g.alb*(f[i]*(1.f/3.14159265f)));
        float dark=(lowFreqRestoreLumaMax-surfL)/
          std::max(1e-6f,lowFreqRestoreLumaMax-lowFreqRestoreLumaMin);
        dark=std::max(0.f,std::min(1.f,dark));
        dark=dark*dark*(3.f-2.f*dark);
        if(dark<=0.f) continue;
        V3 srcMean{}, curMean{};
        int cnt=0;
        for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++){
          int xx=x+dx, yy=y+dy;
          if(xx<0||xx>=W||yy<0||yy>=H) continue;
          int j=yy*W+xx; GPix& q=gbuf[j];
          if(!q.hit || emissive(q.m)) continue;
          if(q.m!=g.m || q.face!=g.face) continue;
          if(std::fabs(q.depth-g.depth)>reuseDepthMax) continue;
          srcMean+=detailRestoreSrc[j];
          curMean+=f[j];
          cnt++;
        }
        if(cnt<6) continue;
        srcMean=srcMean*(1.f/(float)cnt);
        curMean=curMean*(1.f/(float)cnt);
        V3 delta=srcMean-curMean;
        float cap=lowFreqRestoreClampBase +
          lowFreqRestoreClampScale*std::max(0.f,luma(curMean));
        delta.x=std::max(-cap,std::min(cap,delta.x));
        delta.y=std::max(-cap,std::min(cap,delta.y));
        delta.z=std::max(-cap,std::min(cap,delta.z));
        float low=1.f-std::min(1.f,std::max(0.f,d/std::max(1e-6f,reuseDirectMax)));
        float a=std::max(0.f,lowFreqRestoreAmount)*interior*dark*low;
        f[i].x=std::max(0.f,f[i].x+delta.x*a);
        f[i].y=std::max(0.f,f[i].y+delta.y*a);
        f[i].z=std::max(0.f,f[i].z+delta.z*a);
      }
    }

    if(broadPlaneTextureRestore && !detailRestoreSrc.empty()){
      int r=std::max(1,textureRestoreRadius);
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        float d=luma(directImg[i]);
        if(d>=reuseDirectMax) continue;
        float interior=localSurfaceConfidence(
          x,y,textureRestoreSurfaceRadius,textureRestoreSurfaceMinSame,textureRestoreSurfaceSoftness);
        if(interior<=0.f) continue;
        float surfL=luma(g.alb*(f[i]*(1.f/3.14159265f)));
        float dark=(textureRestoreLumaMax-surfL)/std::max(1e-6f,textureRestoreLumaMax-textureRestoreLumaMin);
        dark=std::max(0.f,std::min(1.f,dark));
        dark=dark*dark*(3.f-2.f*dark);
        if(dark<=0.f) continue;
        V3 mean{}; int cnt=0;
        for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++){
          int xx=x+dx, yy=y+dy;
          if(xx<0||xx>=W||yy<0||yy>=H) continue;
          int j=yy*W+xx; GPix& q=gbuf[j];
          if(!q.hit || emissive(q.m)) continue;
          if(q.m!=g.m || q.face!=g.face) continue;
          if(std::fabs(q.depth-g.depth)>reuseDepthMax) continue;
          mean+=detailRestoreSrc[j];
          cnt++;
        }
        if(cnt<3) continue;
        mean=mean*(1.f/(float)cnt);
        V3 hi=detailRestoreSrc[i]-mean;
        float cap=textureRestoreClampBase + textureRestoreClampScale*std::max(0.f,luma(mean));
        hi.x=std::max(-cap,std::min(cap,hi.x));
        hi.y=std::max(-cap,std::min(cap,hi.y));
        hi.z=std::max(-cap,std::min(cap,hi.z));
        float low=1.f-std::min(1.f,std::max(0.f,d/std::max(1e-6f,reuseDirectMax)));
        float a=std::max(0.f,textureRestoreAmount)*interior*dark*low;
        f[i].x=std::max(0.f,f[i].x+hi.x*a);
        f[i].y=std::max(0.f,f[i].y+hi.y*a);
        f[i].z=std::max(0.f,f[i].z+hi.z*a);
      }
    }

    if(finalDetailRestore && !detailRestoreSrc.empty()){
      int r=std::max(1,detailRestoreRadius);
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        float d=luma(directImg[i]);
        if(d>=reuseDirectMax) continue;
        float edge=1.f-localSurfaceConfidence(x,y,detailRestoreEdgeRadius,detailRestoreEdgeMinSame,detailRestoreEdgeSoftness);
        edge=std::max(0.f,std::min(1.f,edge));
        if(edge<=0.f) continue;
        V3 mean{}; int cnt=0;
        for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++){
          int xx=x+dx, yy=y+dy;
          if(xx<0||xx>=W||yy<0||yy>=H) continue;
          int j=yy*W+xx; GPix& q=gbuf[j];
          if(!q.hit || emissive(q.m)) continue;
          if(q.m!=g.m || q.face!=g.face) continue;
          if(std::fabs(q.depth-g.depth)>reuseDepthMax) continue;
          mean+=detailRestoreSrc[j];
          cnt++;
        }
        if(cnt<3) continue;
        mean=mean*(1.f/(float)cnt);
        V3 hi=detailRestoreSrc[i]-mean;
        float cap=detailRestoreClampBase + detailRestoreClampScale*std::max(0.f,luma(mean));
        hi.x=std::max(-cap,std::min(cap,hi.x));
        hi.y=std::max(-cap,std::min(cap,hi.y));
        hi.z=std::max(-cap,std::min(cap,hi.z));
        float low=1.f-std::min(1.f,std::max(0.f,d/std::max(1e-6f,reuseDirectMax)));
        float a=std::max(0.f,detailRestoreAmount)*edge*low;
        f[i].x=std::max(0.f,f[i].x+hi.x*a);
        f[i].y=std::max(0.f,f[i].y+hi.y*a);
        f[i].z=std::max(0.f,f[i].z+hi.z*a);
      }
    }

    if(materialChromaFloor){
      auto satProxy=[&](V3 c,float lum){
        float dr=c.x-lum, dg=c.y-lum, db=c.z-lum;
        return std::sqrt(std::max(0.f,(dr*dr+dg*dg+db*db)/3.f));
      };
      const float invPi=1.f/3.14159265f;
      for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        int i=y*W+x; GPix& g=gbuf[i];
        if(!g.hit || emissive(g.m)) continue;
        float d=luma(directImg[i]);
        if(d>=reuseDirectMax) continue;
        float interior=localSurfaceConfidence(
          x,y,materialChromaSurfaceRadius,materialChromaSurfaceMinSame,
          materialChromaSurfaceSoftness);
        if(interior<=0.f) continue;
        V3 cur=g.alb*(f[i]*invPi);
        float curLum=std::max(0.f,luma(cur));
        float dark=(materialChromaLumaMax-curLum)/
          std::max(1e-6f,materialChromaLumaMax-materialChromaLumaMin);
        dark=std::max(0.f,std::min(1.f,dark));
        dark=dark*dark*(3.f-2.f*dark);
        if(dark<=0.f) continue;
        float albLum=luma(g.alb);
        if(albLum<=1e-5f || curLum<=1e-6f) continue;
        V3 matHue=g.alb*(curLum/albLum);
        float curSat=satProxy(cur,curLum);
        float matSat=satProxy(matHue,curLum);
        if(matSat<=curSat+1e-6f) continue;
        float need=std::max(0.f,std::min(1.f,(matSat-curSat)/std::max(matSat,1e-6f)));
        float low=1.f-std::min(1.f,std::max(0.f,d/std::max(1e-6f,reuseDirectMax)));
        float a=std::max(0.f,materialChromaAmount)*interior*dark*low*need;
        if(a<=0.f) continue;
        V3 delta=matHue-cur;
        float cap=materialChromaClampBase + materialChromaClampScale*curLum;
        delta.x=std::max(-cap,std::min(cap,delta.x));
        delta.y=std::max(-cap,std::min(cap,delta.y));
        delta.z=std::max(-cap,std::min(cap,delta.z));
        V3 outc;
        outc.x=std::max(0.f,cur.x+delta.x*a);
        outc.y=std::max(0.f,cur.y+delta.y*a);
        outc.z=std::max(0.f,cur.z+delta.z*a);
        if(g.alb.x>1e-5f) f[i].x=outc.x*3.14159265f/g.alb.x;
        if(g.alb.y>1e-5f) f[i].y=outc.y*3.14159265f/g.alb.y;
        if(g.alb.z>1e-5f) f[i].z=outc.z*3.14159265f/g.alb.z;
      }
    }

    for(int i=0;i<W*H;i++){ GPix& g=gbuf[i]; out[i]= g.hit? g.alb*(f[i]*(1.f/3.14159265f)) : V3{}; }
    long used=(long)(tl_rays-r0);
    flushCounters();
    return used;
  }
};

struct MethodPTP : MethodPTO {
  MethodPTP(){
    progressive=true;
    resetHistoryOnEvent=true;
    methodName="PTP";
  }
};

struct MethodPTC : MethodPTO {
  MethodPTC(){
    progressive=true;
    localizedEventHistory=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTC";
  }
};

struct MethodPTR : MethodPTO {
  MethodPTR(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTR";
  }
};

struct MethodPTV : MethodPTO {
  MethodPTV(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    varianceFilter=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTV";
  }
};

struct MethodPTS : MethodPTO {
  MethodPTS(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    stratifiedDirs=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTS";
  }
};

struct MethodPTL : MethodPTO {
  MethodPTL(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    stratifiedDirs=true;
    weightedNee=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTL";
  }
};

struct MethodPTM : MethodPTO {
  MethodPTM(){
    progressive=true;
    localizedEventHistory=true;
    finalFireflyFilter=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTM";
  }
};

struct MethodPTU : MethodPTO {
  MethodPTU(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTU";
  }
};

struct MethodPTA : MethodPTO {
  MethodPTA(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    reuseDirectMax=0.20f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTA";
  }
};

struct MethodPTD : MethodPTO {
  MethodPTD(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTD";
  }
};

struct MethodPTE : MethodPTO {
  MethodPTE(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    strictFinalFilter=true;
    finalFilterRadius=2;
    finalFilterNormalMin=0.94f;
    finalFilterDepthMax=1.75f;
    finalFilterSigma=3.8f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTE";
  }
};

struct MethodPTB : MethodPTO {
  MethodPTB(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    lowDiscrepancyGate=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTB";
  }
};

struct MethodPTF : MethodPTO {
  MethodPTF(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    preserveCenterSamples=true;
    reuseDirectMax=0.20f;
    reuseMinSamples=4;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTF";
  }
};

struct MethodPTH : MethodPTO {
  MethodPTH(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    historyGuidedSampleReuse=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseHistoryBase=0.08f;
    reuseHistoryScale=0.08f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTH";
  }
};

struct MethodPTK : MethodPTO {
  MethodPTK(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    rangeFinalFilter=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    finalRangeBase=0.08f;
    finalRangeScale=0.08f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTK";
  }
};

struct MethodPTN : MethodPTO {
  MethodPTN(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=5.0f;
    reuseMinSamples=2;
    reuseRadius=1;
    reuseSpatialSigma=1.6f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTN";
  }
};

struct MethodPTW : MethodPTO {
  MethodPTW(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    lowDiscrepancyGate=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTW";
  }
};

struct MethodPTJ : MethodPTO {
  MethodPTJ(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseBorrowHistoryWeight=0.35f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTJ";
  }
};

struct MethodPTI : MethodPTO {
  MethodPTI(){
    progressive=true;
    resetHistoryOnEvent=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    methodName="PTI";
  }
};

struct MethodPTQ : MethodPTO {
  MethodPTQ(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    reconstructionOnlyReuse=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reconstructionHintBlend=0.35f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTQ";
  }
};

struct MethodPTY : MethodPTO {
  MethodPTY(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    reconstructionOnlyReuse=true;
    reconstructionHintSkippedOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reconstructionHintBlend=0.08f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTY";
  }
};

struct MethodPTZ : MethodPTO {
  MethodPTZ(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    warmupOnlyReuse=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseWarmupMaxN=16.0f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTZ";
  }
};

struct MethodPTAA : MethodPTO {
  MethodPTAA(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    planarFinalFit=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=5;
    planarFitMinSamples=18;
    planarFitBlend=0.50f;
    planarFitNormalMin=0.96f;
    planarFitDepthMax=1.5f;
    planarFitLowDirectOnly=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAA";
  }
};

struct MethodPTAB : MethodPTO {
  MethodPTAB(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    planarFinalFit=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=2;
    planarFitMinSamples=8;
    planarFitBlend=0.50f;
    planarFitNormalMin=0.96f;
    planarFitDepthMax=1.5f;
    planarFitLowDirectOnly=true;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAB";
  }
};

struct MethodPTAC : MethodPTO {
  MethodPTAC(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    planarFinalFit=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=5;
    planarFitStep=2;
    planarFitMinSamples=8;
    planarFitBlend=0.70f;
    planarFitNormalMin=0.96f;
    planarFitDepthMax=1.5f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=true;
    planarFitResidualBase=0.06f;
    planarFitResidualScale=0.12f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAC";
  }
};

struct MethodPTAD : MethodPTO {
  MethodPTAD(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=5;
    planarFitMinSamples=18;
    planarFitBlend=0.55f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=true;
    planarFitResidualBase=0.06f;
    planarFitResidualScale=0.12f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAD";
  }
};

struct MethodPTAE : MethodPTO {
  MethodPTAE(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    separablePlanarFinalFit=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=5;
    planarFitMinSamples=18;
    planarFitBlend=0.50f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAE";
  }
};

struct MethodPTAF : MethodPTO {
  MethodPTAF(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=5;
    planarFitMinSamples=18;
    planarFitBlend=0.50f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAF";
  }
};

struct MethodPTAG : MethodPTO {
  MethodPTAG(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    fastPlanarDepthBins=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=5;
    planarFitMinSamples=8;
    planarFitBlend=0.55f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=true;
    planarFitResidualBase=0.06f;
    planarFitResidualScale=0.12f;
    planarFitDepthBinSize=1.5f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAG";
  }
};

struct MethodPTAH : MethodPTO {
  MethodPTAH(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=5;
    planarFitMinSamples=18;
    planarFitBlend=0.42f;
    planarFitBlendHigh=0.82f;
    planarFitAreaThreshold=0.70f;
    planarFitAreaSoftness=0.22f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAH";
  }
};

struct MethodPTAI : MethodPTO {
  MethodPTAI(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAI";
  }
};

struct MethodPTAJ : MethodPTO {
  MethodPTAJ(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    connectedPlaneFinalFit=true;
    connectedPlaneQuadraticFit=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    connectedPlaneMinPixels=180;
    connectedPlaneMinSpan=18;
    connectedPlaneMinFill=0.42f;
    connectedPlaneBlend=0.38f;
    planarFitDepthMax=1.5f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAJ";
  }
};

struct MethodPTAK : MethodPTO {
  MethodPTAK(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    surfaceBasisSampleReuse=true;
    surfaceBasisQuadraticFit=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    surfaceBasisMinPixels=180;
    surfaceBasisMinValid=28;
    surfaceBasisMinSpan=18;
    surfaceBasisMinFill=0.36f;
    surfaceBasisBlend=0.82f;
    surfaceBasisBorrowHistoryWeight=0.80f;
    surfaceBasisRejectSigma=2.8f;
    surfaceBasisRejectAdd=0.25f;
    surfaceBasisRidge=1e-3f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAK";
  }
};

struct MethodPTAL : MethodPTO {
  MethodPTAL(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    surfaceBasisSampleReuse=true;
    surfaceBasisQuadraticFit=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    surfaceBasisMinPixels=180;
    surfaceBasisMinValid=36;
    surfaceBasisMinSpan=18;
    surfaceBasisMinFill=0.36f;
    surfaceBasisBlend=0.28f;
    surfaceBasisBorrowHistoryWeight=0.22f;
    surfaceBasisRejectSigma=2.4f;
    surfaceBasisRejectAdd=0.18f;
    surfaceBasisRidge=2e-3f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAL";
  }
};

struct MethodPTAM : MethodPTO {
  MethodPTAM(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    adaptivePixelSampling=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    adaptivePixelHighWeight=0.40f;
    adaptivePixelLowWeight=2.65f;
    adaptivePixelBroadBoost=1.65f;
    adaptivePixelAreaThreshold=0.62f;
    adaptivePixelAreaSoftness=0.22f;
    adaptivePixelMinProb=0.025f;
    adaptivePixelMaxProb=0.94f;
    adaptivePixelBroadRadius=2;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAM";
  }
};

struct MethodPTAN : MethodPTO {
  MethodPTAN(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    adaptivePixelSampling=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    adaptivePixelHighWeight=0.72f;
    adaptivePixelLowWeight=1.55f;
    adaptivePixelBroadBoost=1.18f;
    adaptivePixelAreaThreshold=0.62f;
    adaptivePixelAreaSoftness=0.25f;
    adaptivePixelMinProb=0.08f;
    adaptivePixelMaxProb=0.78f;
    adaptivePixelBroadRadius=2;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAN";
  }
};

struct MethodPTAO : MethodPTO {
  MethodPTAO(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    persistentSurfaceModel=true;
    surfaceBasisQuadraticFit=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    surfaceBasisMinPixels=180;
    surfaceBasisMinValid=36;
    surfaceBasisMinSpan=18;
    surfaceBasisMinFill=0.36f;
    surfaceBasisRejectSigma=2.6f;
    surfaceBasisRejectAdd=0.20f;
    surfaceBasisRidge=2e-3f;
    persistentSurfaceModelAlpha=0.08f;
    persistentSurfaceModelWarmup=7.0f;
    persistentSurfaceModelBlend=0.24f;
    persistentSurfaceModelBiasBase=0.06f;
    persistentSurfaceModelBiasScale=0.28f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAO";
  }
};

struct MethodPTAP : MethodPTO {
  MethodPTAP(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    planarFitDirectStableBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.22f;
    planarFitBlendHigh=0.98f;
    planarFitAreaThreshold=0.72f;
    planarFitAreaSoftness=0.18f;
    planarFitDirectMeanMax=0.12f;
    planarFitDirectStdMax=0.028f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAP";
  }
};

struct MethodPTAQ : MethodPTO {
  MethodPTAQ(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=14;
    planarFitMinSamples=90;
    planarFitBlend=0.26f;
    planarFitBlendHigh=0.96f;
    planarFitAreaThreshold=0.74f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAQ";
  }
};

struct MethodPTAR : MethodPTO {
  MethodPTAR(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    lowDiscrepancyGate=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAR";
  }
};

struct MethodPTAS : MethodPTO {
  MethodPTAS(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    connectedPlaneFinalFit=true;
    connectedPlaneQuadraticFit=true;
    connectedPlaneInteriorOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    connectedPlaneMinPixels=180;
    connectedPlaneMinSpan=18;
    connectedPlaneMinFill=0.38f;
    connectedPlaneBlend=0.30f;
    connectedPlaneInteriorRadius=2;
    connectedPlaneInteriorMinSame=0.84f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAS";
  }
};

struct MethodPTAT : MethodPTO {
  MethodPTAT(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    weightedNee=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAT";
  }
};

struct MethodPTAU : MethodPTO {
  MethodPTAU(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    historyPlaneAcceptance=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    historyPlaneRadius=5;
    historyPlaneMinSamples=20;
    historyPlaneMinN=3.0f;
    historyPlaneAreaThreshold=0.64f;
    historyPlaneAreaSoftness=0.22f;
    historyPlaneBase=0.040f;
    historyPlaneScale=0.14f;
    historyPlaneRmsScale=1.35f;
    historyPlaneCenterBlend=0.24f;
    historyPlaneBorrowBlend=0.68f;
    historyPlaneWeightCut=0.40f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAU";
  }
};

struct MethodPTAV : MethodPTO {
  MethodPTAV(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    winsorizedSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    reuseWinsorMinSamples=5;
    reuseWinsorSigma=1.25f;
    reuseWinsorAdd=0.08f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAV";
  }
};

struct MethodPTAW : MethodPTO {
  MethodPTAW(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    adaptivePixelSampling=true;
    adaptivePixelUseVariance=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    adaptivePixelHighWeight=0.78f;
    adaptivePixelLowWeight=1.42f;
    adaptivePixelBroadBoost=1.22f;
    adaptivePixelAreaThreshold=0.62f;
    adaptivePixelAreaSoftness=0.25f;
    adaptivePixelMinProb=0.06f;
    adaptivePixelMaxProb=0.86f;
    adaptivePixelBroadRadius=2;
    adaptivePixelVarianceBase=0.025f;
    adaptivePixelVarianceScale=0.13f;
    adaptivePixelVarianceBoost=1.85f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAW";
  }
};

struct MethodPTAX : MethodPTO {
  MethodPTAX(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    preserveCenterSamples=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAX";
  }
};

struct MethodPTAY : MethodPTO {
  MethodPTAY(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    stochasticSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=3;
    reuseDepthMax=1.75f;
    reuseStochasticKeep=0.64f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAY";
  }
};

struct MethodPTAZ : MethodPTO {
  MethodPTAZ(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    reuseBorrowHistoryWeight=0.35f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTAZ";
  }
};

struct MethodPTBA : MethodPTO {
  MethodPTBA(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    historyGuidedSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    reuseHistoryBase=0.035f;
    reuseHistoryScale=0.075f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBA";
  }
};

struct MethodPTBB : MethodPTO {
  MethodPTBB(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    stratifiedDirs=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBB";
  }
};

struct MethodPTBC : MethodPTO {
  MethodPTBC(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    stratifiedDirs=true;
    stratifiedFirstBounceOnly=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBC";
  }
};

struct MethodPTBD : MethodPTO {
  MethodPTBD(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    stratifiedDirs=true;
    stratifiedFirstBounceOnly=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=14;
    planarFitMinSamples=90;
    planarFitBlend=0.26f;
    planarFitBlendHigh=0.96f;
    planarFitAreaThreshold=0.74f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBD";
  }
};

struct MethodPTBE : MethodPTO {
  MethodPTBE(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=5;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBE";
  }
};

struct MethodPTBF : MethodPTO {
  MethodPTBF(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBF";
  }
};

struct MethodPTBG : MethodPTO {
  MethodPTBG(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBG";
  }
};

struct MethodPTBH : MethodPTO {
  MethodPTBH(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    planarFitEdgeConfidence=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    broadPlaneLiftEdgeConfidence=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    planarFitEdgeRadius=1;
    planarFitEdgeMinSame=0.78f;
    planarFitEdgeSoftness=0.18f;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    broadPlaneLiftEdgeRadius=1;
    broadPlaneLiftEdgeMinSame=0.78f;
    broadPlaneLiftEdgeSoftness=0.18f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBH";
  }
};

struct MethodPTBI : MethodPTO {
  MethodPTBI(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    finalDetailRestore=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    detailRestoreRadius=2;
    detailRestoreEdgeRadius=1;
    detailRestoreAmount=0.32f;
    detailRestoreEdgeMinSame=0.92f;
    detailRestoreEdgeSoftness=0.12f;
    detailRestoreClampBase=0.010f;
    detailRestoreClampScale=0.16f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBI";
  }
};

struct MethodPTBJ : MethodPTO {
  MethodPTBJ(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    planarFitLumaOnly=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    planarFitLumaScaleMin=0.78f;
    planarFitLumaScaleMax=1.28f;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBJ";
  }
};

struct MethodPTBK : MethodPTO {
  MethodPTBK(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    strictFinalFilter=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    finalFilterRadius=2;
    finalFilterNormalMin=0.92f;
    finalFilterDepthMax=1.75f;
    finalFilterSigma=4.5f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBK";
  }
};

struct MethodPTBL : MethodPTO {
  MethodPTBL(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    finalFilterMaterialEdgeOnly=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    finalFilterRadius=2;
    finalFilterNormalMin=0.92f;
    finalFilterDepthMax=1.75f;
    finalFilterSigma=4.5f;
    finalFilterEdgeRadius=1;
    finalFilterEdgeMinSame=0.92f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBL";
  }
};

struct MethodPTBM : MethodPTO {
  MethodPTBM(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    finalFilterMaterialEdgeOnly=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    finalFilterRadius=2;
    finalFilterNormalMin=0.90f;
    finalFilterDepthMax=2.5f;
    finalFilterSigma=4.5f;
    finalFilterEdgeRadius=1;
    finalFilterEdgeMinSame=0.78f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBM";
  }
};

struct MethodPTBN : MethodPTO {
  MethodPTBN(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    finalFilterRadius=2;
    finalFilterNormalMin=0.92f;
    finalFilterDepthMax=1.75f;
    finalFilterSigma=4.5f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.055f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBN";
  }
};

struct MethodPTBO : MethodPTO {
  MethodPTBO(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    finalFilterRadius=2;
    finalFilterNormalMin=0.92f;
    finalFilterDepthMax=1.75f;
    finalFilterSigma=4.5f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.060f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBO";
  }
};

struct MethodPTBP : MethodPTO {
  MethodPTBP(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    finalFilterRadius=2;
    finalFilterNormalMin=0.92f;
    finalFilterDepthMax=1.75f;
    finalFilterSigma=4.5f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.065f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBP";
  }
};

struct MethodPTBQ : MethodPTO {
  MethodPTBQ(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    finalFilterRadius=2;
    finalFilterNormalMin=0.92f;
    finalFilterDepthMax=1.75f;
    finalFilterSigma=4.5f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.070f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBQ";
  }
};

struct MethodPTBR : MethodPTO {
  MethodPTBR(){
    progressive=true;
    localizedEventHistory=true;
    robustClamp=true;
    spatialSampleReuse=true;
    adaptiveSampleReuse=true;
    skyOnlyFallback=true;
    fastPlanarFinalFit=true;
    planarFitAreaBlend=true;
    broadPlaneEnergyLift=true;
    broadPlaneLiftSkyDisabledOnly=true;
    reuseDirectMax=0.20f;
    reuseCenterWeight=9.0f;
    reuseMinSamples=4;
    reuseDepthMax=1.75f;
    finalFilterRadius=2;
    finalFilterNormalMin=0.92f;
    finalFilterDepthMax=1.75f;
    finalFilterSigma=4.5f;
    planarFitRadius=9;
    planarFitMinSamples=42;
    planarFitBlend=0.30f;
    planarFitBlendHigh=0.88f;
    planarFitAreaThreshold=0.76f;
    planarFitAreaSoftness=0.16f;
    planarFitLowDirectOnly=true;
    planarFitResidualGate=false;
    broadPlaneLiftRadius=3;
    broadPlaneLiftAmount=0.080f;
    broadPlaneLiftAreaThreshold=0.68f;
    broadPlaneLiftAreaSoftness=0.22f;
    geomHistoryCap=4.f;
    lightHistoryCap=2.f;
    methodName="PTBR";
  }
};

struct MethodPTBS : MethodPTBR {
  MethodPTBS(){
    broadPlaneLiftLumaGate=true;
    broadPlaneLiftLumaMin=0.035f;
    broadPlaneLiftLumaMax=0.18f;
    broadPlaneLiftDarkScale=0.35f;
    methodName="PTBS";
  }
};

struct MethodPTBT : MethodPTBR {
  MethodPTBT(){
    broadPlaneLiftEdgeConfidence=true;
    broadPlaneLiftEdgeRadius=1;
    broadPlaneLiftEdgeMinSame=0.82f;
    broadPlaneLiftEdgeSoftness=0.16f;
    methodName="PTBT";
  }
};

struct MethodPTBU : MethodPTBR {
  MethodPTBU(){
    broadPlaneLiftLumaGate=true;
    broadPlaneLiftLumaMin=0.035f;
    broadPlaneLiftLumaMax=0.18f;
    broadPlaneLiftDarkScale=0.35f;
    broadPlaneLiftEdgeConfidence=true;
    broadPlaneLiftEdgeRadius=1;
    broadPlaneLiftEdgeMinSame=0.82f;
    broadPlaneLiftEdgeSoftness=0.16f;
    methodName="PTBU";
  }
};

struct MethodPTBV : MethodPTBR {
  MethodPTBV(){
    broadPlaneLiftLumaGate=true;
    broadPlaneLiftLumaMin=0.035f;
    broadPlaneLiftLumaMax=0.18f;
    broadPlaneLiftDarkScale=0.75f;
    methodName="PTBV";
  }
};

struct MethodPTBW : MethodPTBR {
  MethodPTBW(){
    broadPlaneLiftLumaGate=true;
    broadPlaneLiftLumaMin=0.035f;
    broadPlaneLiftLumaMax=0.18f;
    broadPlaneLiftDarkScale=0.85f;
    methodName="PTBW";
  }
};

struct MethodPTBX : MethodPTBR {
  MethodPTBX(){
    broadPlaneTextureRestore=true;
    textureRestoreRadius=2;
    textureRestoreSurfaceRadius=1;
    textureRestoreAmount=0.12f;
    textureRestoreSurfaceMinSame=0.82f;
    textureRestoreSurfaceSoftness=0.14f;
    textureRestoreLumaMin=0.05f;
    textureRestoreLumaMax=0.30f;
    textureRestoreClampBase=0.006f;
    textureRestoreClampScale=0.08f;
    methodName="PTBX";
  }
};

struct MethodPTBY : MethodPTBR {
  MethodPTBY(){
    broadPlaneTextureRestore=true;
    textureRestoreRadius=2;
    textureRestoreSurfaceRadius=1;
    textureRestoreAmount=0.22f;
    textureRestoreSurfaceMinSame=0.82f;
    textureRestoreSurfaceSoftness=0.14f;
    textureRestoreLumaMin=0.05f;
    textureRestoreLumaMax=0.30f;
    textureRestoreClampBase=0.006f;
    textureRestoreClampScale=0.08f;
    methodName="PTBY";
  }
};

struct MethodPTBZ : MethodPTBR {
  MethodPTBZ(){
    planarFitDirectGradientGate=true;
    planarFitDirectGradientStdMin=0.012f;
    planarFitDirectGradientStdMax=0.040f;
    planarFitDirectGradientBlendCap=0.48f;
    methodName="PTBZ";
  }
};

struct MethodPTCA : MethodPTBR {
  MethodPTCA(){
    planarFitDirectGradientGate=true;
    planarFitDirectGradientStdMin=0.018f;
    planarFitDirectGradientStdMax=0.050f;
    planarFitDirectGradientBlendCap=0.62f;
    methodName="PTCA";
  }
};

struct MethodPTCB : MethodPTBR {
  MethodPTCB(){
    planarFitBlend=0.24f;
    planarFitBlendHigh=0.70f;
    broadPlaneLiftAmount=0.095f;
    methodName="PTCB";
  }
};

struct MethodPTCC : MethodPTBR {
  MethodPTCC(){
    planarFitBlend=0.20f;
    planarFitBlendHigh=0.62f;
    broadPlaneLiftAmount=0.105f;
    methodName="PTCC";
  }
};

struct MethodPTCD : MethodPTBR {
  MethodPTCD(){
    planarFitVoxelContactGate=true;
    planarFitVoxelContactAmount=0.55f;
    methodName="PTCD";
  }
};

struct MethodPTCE : MethodPTBR {
  MethodPTCE(){
    planarFitVoxelContactGate=true;
    planarFitVoxelContactAmount=0.55f;
    broadPlaneLiftVoxelContactGate=true;
    broadPlaneLiftVoxelContactAmount=0.55f;
    methodName="PTCE";
  }
};

struct MethodPTCF : MethodPTBR {
  MethodPTCF(){
    planarFitVoxelContactGate=true;
    planarFitVoxelContactAmount=0.32f;
    broadPlaneLiftVoxelContactGate=true;
    broadPlaneLiftVoxelContactAmount=0.32f;
    methodName="PTCF";
  }
};

struct MethodPTCG : MethodPTBR {
  MethodPTCG(){
    lowDiscrepancyGate=true;
    methodName="PTCG";
  }
};

struct MethodPTCH : MethodPTBR {
  MethodPTCH(){
    lowDiscrepancyFineGate=true;
    methodName="PTCH";
  }
};

struct MethodPTCI : MethodPTBR {
  MethodPTCI(){
    planarFitGradientLossGate=true;
    planarFitGradientLossMin=0.010f;
    planarFitGradientLossStart=0.25f;
    planarFitGradientLossEnd=0.65f;
    planarFitGradientLossBlendCap=0.58f;
    methodName="PTCI";
  }
};

struct MethodPTCJ : MethodPTBR {
  MethodPTCJ(){
    planarFitGradientLossGate=true;
    planarFitGradientLossMin=0.006f;
    planarFitGradientLossStart=0.15f;
    planarFitGradientLossEnd=0.55f;
    planarFitGradientLossBlendCap=0.45f;
    methodName="PTCJ";
  }
};

struct MethodPTCK : MethodPTBR {
  MethodPTCK(){
    planarFitLumaOnly=true;
    methodName="PTCK";
  }
};

struct MethodPTCL : MethodPTBR {
  MethodPTCL(){
    planarFitLumaOnly=true;
    planarFitLumaScaleMin=0.86f;
    planarFitLumaScaleMax=1.18f;
    methodName="PTCL";
  }
};

struct MethodPTCM : MethodPTBR {
  MethodPTCM(){
    planarFitChromaAnchor=true;
    planarFitChromaAnchorAmount=0.25f;
    planarFitChromaAnchorClampBase=0.006f;
    planarFitChromaAnchorClampScale=0.08f;
    methodName="PTCM";
  }
};

struct MethodPTCN : MethodPTBR {
  MethodPTCN(){
    planarFitChromaAnchor=true;
    planarFitChromaAnchorAmount=0.42f;
    planarFitChromaAnchorClampBase=0.008f;
    planarFitChromaAnchorClampScale=0.12f;
    methodName="PTCN";
  }
};

struct MethodPTCO : MethodPTBR {
  MethodPTCO(){
    trackLumaVariance=true;
    planarFitStableHistoryGate=true;
    planarFitStableHistoryMinN=18.0f;
    planarFitStableHistoryStdMax=0.035f;
    planarFitStableHistoryStdSoftness=0.045f;
    planarFitStableHistoryBlendCap=0.58f;
    methodName="PTCO";
  }
};

struct MethodPTCP : MethodPTBR {
  MethodPTCP(){
    trackLumaVariance=true;
    planarFitStableHistoryGate=true;
    planarFitStableHistoryMinN=12.0f;
    planarFitStableHistoryStdMax=0.050f;
    planarFitStableHistoryStdSoftness=0.055f;
    planarFitStableHistoryBlendCap=0.46f;
    methodName="PTCP";
  }
};

struct MethodPTCQ : MethodPTBR {
  MethodPTCQ(){
    trackLumaVariance=true;
    planarFitStableHistoryGate=true;
    planarFitStableHistoryMinN=8.0f;
    planarFitStableHistoryStdMax=0.18f;
    planarFitStableHistoryStdSoftness=0.16f;
    planarFitStableHistoryBlendCap=0.36f;
    methodName="PTCQ";
  }
};

struct MethodPTCR : MethodPTBR {
  MethodPTCR(){
    planarFitCoherentResidualGate=true;
    planarFitCoherentRadius=1;
    planarFitCoherentResidualMin=0.006f;
    planarFitCoherentResidualScale=0.024f;
    planarFitCoherentSignMin=0.60f;
    planarFitCoherentSignSoftness=0.20f;
    planarFitCoherentBlendCap=0.58f;
    methodName="PTCR";
  }
};

struct MethodPTCS : MethodPTBR {
  MethodPTCS(){
    planarFitCoherentResidualGate=true;
    planarFitCoherentRadius=1;
    planarFitCoherentResidualMin=0.0045f;
    planarFitCoherentResidualScale=0.020f;
    planarFitCoherentSignMin=0.55f;
    planarFitCoherentSignSoftness=0.22f;
    planarFitCoherentBlendCap=0.46f;
    methodName="PTCS";
  }
};

struct MethodPTCT : MethodPTCR {
  MethodPTCT(){
    planarFitChromaAnchor=true;
    planarFitChromaAnchorAmount=0.18f;
    planarFitChromaAnchorClampBase=0.004f;
    planarFitChromaAnchorClampScale=0.055f;
    methodName="PTCT";
  }
};

struct MethodPTCU : MethodPTCR {
  MethodPTCU(){
    planarFitChromaAnchor=true;
    planarFitChromaAnchorAmount=0.30f;
    planarFitChromaAnchorClampBase=0.006f;
    planarFitChromaAnchorClampScale=0.080f;
    methodName="PTCU";
  }
};

struct MethodPTCV : MethodPTCR {
  MethodPTCV(){
    planarFitChromaAnchor=true;
    planarFitChromaAnchorAmount=0.55f;
    planarFitChromaAnchorClampBase=0.010f;
    planarFitChromaAnchorClampScale=0.130f;
    methodName="PTCV";
  }
};

struct MethodPTCW : MethodPTCR {
  MethodPTCW(){
    planarFitCoherentPatchDamp=true;
    planarFitCoherentPatchFillStart=0.82f;
    planarFitCoherentPatchFillEnd=1.00f;
    planarFitCoherentPatchMinScale=0.45f;
    methodName="PTCW";
  }
};

struct MethodPTCX : MethodPTCR {
  MethodPTCX(){
    planarFitCoherentPatchDamp=true;
    planarFitCoherentPatchFillStart=0.70f;
    planarFitCoherentPatchFillEnd=0.94f;
    planarFitCoherentPatchMinScale=0.18f;
    methodName="PTCX";
  }
};

struct MethodPTCY : MethodPTCX {
  MethodPTCY(){
    broadPlaneChromaRestore=true;
    chromaRestoreAmount=0.22f;
    chromaRestoreClampBase=0.006f;
    chromaRestoreClampScale=0.080f;
    methodName="PTCY";
  }
};

struct MethodPTCZ : MethodPTCX {
  MethodPTCZ(){
    broadPlaneChromaRestore=true;
    chromaRestoreAmount=0.42f;
    chromaRestoreClampBase=0.010f;
    chromaRestoreClampScale=0.130f;
    methodName="PTCZ";
  }
};

struct MethodPTDA : MethodPTCX {
  MethodPTDA(){
    materialChromaFloor=true;
    materialChromaAmount=0.24f;
    materialChromaClampBase=0.006f;
    materialChromaClampScale=0.10f;
    methodName="PTDA";
  }
};

struct MethodPTDB : MethodPTCX {
  MethodPTDB(){
    materialChromaFloor=true;
    materialChromaAmount=0.42f;
    materialChromaClampBase=0.010f;
    materialChromaClampScale=0.16f;
    methodName="PTDB";
  }
};

struct MethodPTDC : MethodPTCX {
  MethodPTDC(){
    planarFitMaterialChromaAnchor=true;
    planarFitMaterialChromaAmount=0.18f;
    planarFitMaterialChromaMin=0.14f;
    planarFitMaterialChromaSoftness=0.18f;
    planarFitMaterialChromaClampBase=0.004f;
    planarFitMaterialChromaClampScale=0.055f;
    methodName="PTDC";
  }
};

struct MethodPTDD : MethodPTCX {
  MethodPTDD(){
    planarFitMaterialChromaAnchor=true;
    planarFitMaterialChromaAmount=0.34f;
    planarFitMaterialChromaMin=0.12f;
    planarFitMaterialChromaSoftness=0.18f;
    planarFitMaterialChromaClampBase=0.006f;
    planarFitMaterialChromaClampScale=0.080f;
    methodName="PTDD";
  }
};

struct MethodPTDE : MethodPTCX {
  MethodPTDE(){
    broadPlaneLowFreqRestore=true;
    lowFreqRestoreRadius=4;
    lowFreqRestoreAmount=0.16f;
    lowFreqRestoreClampBase=0.004f;
    lowFreqRestoreClampScale=0.055f;
    methodName="PTDE";
  }
};

struct MethodPTDF : MethodPTCX {
  MethodPTDF(){
    broadPlaneLowFreqRestore=true;
    lowFreqRestoreRadius=4;
    lowFreqRestoreAmount=0.30f;
    lowFreqRestoreClampBase=0.006f;
    lowFreqRestoreClampScale=0.080f;
    methodName="PTDF";
  }
};

struct MethodPTDG : MethodPTCR {
  MethodPTDG(){
    planarFitCoherentPatchDamp=true;
    planarFitCoherentPatchFillStart=0.72f;
    planarFitCoherentPatchFillEnd=0.96f;
    planarFitCoherentPatchMinScale=0.28f;
    methodName="PTDG";
  }
};

struct MethodPTDH : MethodPTCR {
  MethodPTDH(){
    planarFitCoherentPatchDamp=true;
    planarFitCoherentPatchFillStart=0.76f;
    planarFitCoherentPatchFillEnd=0.98f;
    planarFitCoherentPatchMinScale=0.36f;
    methodName="PTDH";
  }
};

struct MethodPTDI : MethodPTCX {
  MethodPTDI(){
    planarFitMultiscaleResidual=true;
    planarFitMultiscaleRadius=4;
    planarFitMultiscaleMinSamples=12;
    planarFitMultiscaleAmount=0.18f;
    planarFitMultiscaleClampBase=0.004f;
    planarFitMultiscaleClampScale=0.055f;
    methodName="PTDI";
  }
};

struct MethodPTDJ : MethodPTCX {
  MethodPTDJ(){
    planarFitMultiscaleResidual=true;
    planarFitMultiscaleRadius=4;
    planarFitMultiscaleMinSamples=12;
    planarFitMultiscaleAmount=0.34f;
    planarFitMultiscaleClampBase=0.006f;
    planarFitMultiscaleClampScale=0.080f;
    methodName="PTDJ";
  }
};

struct MethodPTDK : MethodPTCX {
  MethodPTDK(){
    planarFitMultiscaleResidual=true;
    planarFitMultiscaleMaterialGate=true;
    planarFitMultiscaleDarkGate=true;
    planarFitMultiscaleRadius=4;
    planarFitMultiscaleMinSamples=12;
    planarFitMultiscaleAmount=0.30f;
    planarFitMultiscaleClampBase=0.006f;
    planarFitMultiscaleClampScale=0.080f;
    methodName="PTDK";
  }
};

struct MethodPTDL : MethodPTCX {
  MethodPTDL(){
    planarFitMultiscaleResidual=true;
    planarFitMultiscaleMaterialGate=true;
    planarFitMultiscaleDarkGate=true;
    planarFitMultiscaleRadius=5;
    planarFitMultiscaleMinSamples=16;
    planarFitMultiscaleAmount=0.45f;
    planarFitMultiscaleClampBase=0.008f;
    planarFitMultiscaleClampScale=0.110f;
    planarFitMultiscaleMaterialMin=0.10f;
    methodName="PTDL";
  }
};

struct MethodPTDM : MethodPTCX {
  MethodPTDM(){
    stratifiedDirs=true;
    stratifiedFirstBounceOnly=true;
    methodName="PTDM";
  }
};

struct MethodPTDN : MethodPTCX {
  MethodPTDN(){
    stratifiedDirs=true;
    stratifiedFirstBounceOnly=false;
    methodName="PTDN";
  }
};

struct MethodPTDO : MethodPTDM {
  MethodPTDO(){
    broadPlaneChromaRestore=true;
    chromaRestoreAmount=0.22f;
    chromaRestoreClampBase=0.006f;
    chromaRestoreClampScale=0.080f;
    methodName="PTDO";
  }
};

struct MethodPTDP : MethodPTDM {
  MethodPTDP(){
    broadPlaneChromaRestore=true;
    chromaRestoreAmount=0.42f;
    chromaRestoreClampBase=0.010f;
    chromaRestoreClampScale=0.130f;
    methodName="PTDP";
  }
};

struct MethodPTDQ : MethodPTDM {
  MethodPTDQ(){
    broadPlaneChromaRestore=true;
    chromaRestoreAmount=0.78f;
    chromaRestoreClampBase=0.016f;
    chromaRestoreClampScale=0.220f;
    methodName="PTDQ";
  }
};

struct MethodPTDR : MethodPTDM {
  MethodPTDR(){
    stratifiedMaterialChromaGuard=true;
    stratifiedMaterialChromaMin=0.30f;
    stratifiedMaterialDirectMax=0.20f;
    methodName="PTDR";
  }
};

struct MethodPTDS : MethodPTDM {
  MethodPTDS(){
    planarFitChromaLossGate=true;
    planarFitChromaLossSatMin=0.025f;
    planarFitChromaLossSatSoftness=0.070f;
    planarFitChromaLossMin=0.08f;
    planarFitChromaLossSoftness=0.22f;
    planarFitChromaLossBlendCap=0.38f;
    methodName="PTDS";
  }
};

struct MethodPTDT : MethodPTDM {
  MethodPTDT(){
    planarFitChromaLossGate=true;
    planarFitChromaLossSatMin=0.055f;
    planarFitChromaLossSatSoftness=0.100f;
    planarFitChromaLossMin=0.18f;
    planarFitChromaLossSoftness=0.32f;
    planarFitChromaLossBlendCap=0.58f;
    methodName="PTDT";
  }
};

struct MethodPTDU : MethodPTDM {
  MethodPTDU(){
    planarFitCoherentChromaLossGate=true;
    planarFitCoherentChromaRadius=1;
    planarFitCoherentChromaSatMin=0.030f;
    planarFitCoherentChromaLossMin=0.085f;
    planarFitCoherentChromaLossSoftness=0.26f;
    planarFitCoherentChromaDirMin=0.35f;
    planarFitCoherentChromaSupportMin=0.45f;
    planarFitCoherentChromaBlendCap=0.42f;
    methodName="PTDU";
  }
};

struct MethodPTDV : MethodPTDM {
  MethodPTDV(){
    planarFitMeanChromaLossGate=true;
    planarFitMeanChromaSatMin=0.022f;
    planarFitMeanChromaLossMin=0.070f;
    planarFitMeanChromaLossSoftness=0.22f;
    planarFitMeanChromaBlendCap=0.48f;
    methodName="PTDV";
  }
};

struct MethodPTDW : MethodPTDM {
  MethodPTDW(){
    planarFitConnectedChromaLossGate=true;
    planarFitConnectedChromaMinPixels=140;
    planarFitConnectedChromaMinSpan=10;
    planarFitConnectedChromaMinFill=0.32f;
    planarFitConnectedChromaSatMin=0.024f;
    planarFitConnectedChromaSatSoftness=0.050f;
    planarFitConnectedChromaSupportMin=0.36f;
    planarFitConnectedChromaLossMin=0.085f;
    planarFitConnectedChromaLossSoftness=0.20f;
    planarFitConnectedChromaBlendCap=0.50f;
    methodName="PTDW";
  }
};

struct MethodPTDX : MethodPTDM {
  MethodPTDX(){
    planarFitConnectedChromaLossGate=true;
    planarFitConnectedChromaMinPixels=80;
    planarFitConnectedChromaMinSpan=8;
    planarFitConnectedChromaMinFill=0.28f;
    planarFitConnectedChromaSatMin=0.016f;
    planarFitConnectedChromaSatSoftness=0.040f;
    planarFitConnectedChromaSupportMin=0.25f;
    planarFitConnectedChromaLossMin=0.045f;
    planarFitConnectedChromaLossSoftness=0.14f;
    planarFitConnectedChromaBlendCap=0.34f;
    methodName="PTDX";
  }
};

struct MethodPTDY : MethodPTDM {
  MethodPTDY(){
    trackLumaVariance=true;
    planarFitTemporalChromaLossGate=true;
    planarFitTemporalChromaRadius=1;
    planarFitTemporalChromaMinSamples=3;
    planarFitTemporalChromaMinN=8.0f;
    planarFitTemporalChromaStdMax=0.090f;
    planarFitTemporalChromaStdSoftness=0.085f;
    planarFitTemporalChromaSatMin=0.020f;
    planarFitTemporalChromaSatSoftness=0.050f;
    planarFitTemporalChromaSupportMin=0.34f;
    planarFitTemporalChromaLossMin=0.065f;
    planarFitTemporalChromaLossSoftness=0.18f;
    planarFitTemporalChromaBlendCap=0.48f;
    methodName="PTDY";
  }
};

struct MethodPTDZ : MethodPTDM {
  MethodPTDZ(){
    trackLumaVariance=true;
    planarFitTemporalChromaLossGate=true;
    planarFitTemporalChromaRadius=2;
    planarFitTemporalChromaMinSamples=6;
    planarFitTemporalChromaMinN=6.0f;
    planarFitTemporalChromaStdMax=0.120f;
    planarFitTemporalChromaStdSoftness=0.110f;
    planarFitTemporalChromaSatMin=0.014f;
    planarFitTemporalChromaSatSoftness=0.040f;
    planarFitTemporalChromaSupportMin=0.26f;
    planarFitTemporalChromaLossMin=0.040f;
    planarFitTemporalChromaLossSoftness=0.13f;
    planarFitTemporalChromaBlendCap=0.36f;
    methodName="PTDZ";
  }
};

struct MethodPTDZA : MethodPTDM {
  MethodPTDZA(){
    planarFitTemporalChromaLossGate=true;
    planarFitTemporalChromaRadius=2;
    planarFitTemporalChromaMinSamples=3;
    planarFitTemporalChromaMinN=2.0f;
    planarFitTemporalChromaStdMax=10.0f;
    planarFitTemporalChromaStdSoftness=1.0f;
    planarFitTemporalChromaSatMin=0.004f;
    planarFitTemporalChromaSatSoftness=0.025f;
    planarFitTemporalChromaSupportMin=0.10f;
    planarFitTemporalChromaLossMin=0.010f;
    planarFitTemporalChromaLossSoftness=0.08f;
    planarFitTemporalChromaBlendCap=0.28f;
    methodName="PTDZA";
  }
};

struct MethodPTDZB : MethodPTDM {
  MethodPTDZB(){
    planarFitTemporalChromaLossGate=true;
    planarFitTemporalChromaRadius=0;
    planarFitTemporalChromaMinSamples=1;
    planarFitTemporalChromaMinN=2.0f;
    planarFitTemporalChromaStdMax=10.0f;
    planarFitTemporalChromaStdSoftness=1.0f;
    planarFitTemporalChromaSatMin=0.004f;
    planarFitTemporalChromaSatSoftness=0.025f;
    planarFitTemporalChromaSupportMin=0.0f;
    planarFitTemporalChromaLossMin=0.010f;
    planarFitTemporalChromaLossSoftness=0.08f;
    planarFitTemporalChromaBlendCap=0.32f;
    methodName="PTDZB";
  }
};

struct MethodPTDZC : MethodPTDM {
  MethodPTDZC(){
    planarFitLowPassChromaField=true;
    planarFitLowPassChromaMinN=2.0f;
    planarFitLowPassChromaMinWeight=24.0f;
    planarFitLowPassChromaSatMin=0.010f;
    planarFitLowPassChromaSatSoftness=0.040f;
    planarFitLowPassChromaAmount=0.28f;
    planarFitLowPassChromaClampBase=0.0025f;
    planarFitLowPassChromaClampScale=0.040f;
    methodName="PTDZC";
  }
};

struct MethodPTDZD : MethodPTDM {
  MethodPTDZD(){
    planarFitLowPassChromaField=true;
    planarFitLowPassChromaMinN=2.0f;
    planarFitLowPassChromaMinWeight=12.0f;
    planarFitLowPassChromaSatMin=0.006f;
    planarFitLowPassChromaSatSoftness=0.032f;
    planarFitLowPassChromaAmount=0.48f;
    planarFitLowPassChromaClampBase=0.004f;
    planarFitLowPassChromaClampScale=0.070f;
    methodName="PTDZD";
  }
};

struct MethodPTDZE : MethodPTDM {
  MethodPTDZE(){
    planarFitLowPassChromaField=true;
    planarFitLowPassChromaSatWeight=true;
    planarFitLowPassChromaMinN=2.0f;
    planarFitLowPassChromaMinWeight=8.0f;
    planarFitLowPassChromaWeightSatMin=0.008f;
    planarFitLowPassChromaWeightSatSoftness=0.050f;
    planarFitLowPassChromaSatMin=0.012f;
    planarFitLowPassChromaSatSoftness=0.040f;
    planarFitLowPassChromaAmount=0.35f;
    planarFitLowPassChromaClampBase=0.003f;
    planarFitLowPassChromaClampScale=0.055f;
    methodName="PTDZE";
  }
};

struct MethodPTDZF : MethodPTDM {
  MethodPTDZF(){
    planarFitLowPassChromaField=true;
    planarFitLowPassChromaSatWeight=true;
    planarFitLowPassChromaMinN=2.0f;
    planarFitLowPassChromaMinWeight=5.0f;
    planarFitLowPassChromaWeightSatMin=0.004f;
    planarFitLowPassChromaWeightSatSoftness=0.040f;
    planarFitLowPassChromaSatMin=0.006f;
    planarFitLowPassChromaSatSoftness=0.032f;
    planarFitLowPassChromaAmount=0.55f;
    planarFitLowPassChromaClampBase=0.004f;
    planarFitLowPassChromaClampScale=0.075f;
    methodName="PTDZF";
  }
};

struct MethodPTDZG : MethodPTDM {
  MethodPTDZG(){
    transportChromaModel=true;
    planarFitTransportChromaField=true;
    planarFitLowPassChromaMinN=2.0f;
    planarFitLowPassChromaMinWeight=8.0f;
    planarFitLowPassChromaSatMin=0.006f;
    planarFitLowPassChromaSatSoftness=0.032f;
    planarFitLowPassChromaAmount=0.35f;
    planarFitLowPassChromaClampBase=0.003f;
    planarFitLowPassChromaClampScale=0.055f;
    methodName="PTDZG";
  }
};

struct MethodPTDZH : MethodPTDM {
  MethodPTDZH(){
    transportChromaModel=true;
    planarFitTransportChromaField=true;
    planarFitLowPassChromaMinN=1.0f;
    planarFitLowPassChromaMinWeight=4.0f;
    planarFitLowPassChromaSatMin=0.004f;
    planarFitLowPassChromaSatSoftness=0.028f;
    planarFitLowPassChromaAmount=0.55f;
    planarFitLowPassChromaClampBase=0.004f;
    planarFitLowPassChromaClampScale=0.075f;
    methodName="PTDZH";
  }
};

struct MethodPTDZI : MethodPTDM {
  MethodPTDZI(){
    planarFitMaterialDarkBlendCap=true;
    planarFitMaterialDarkSatMin=0.10f;
    planarFitMaterialDarkSatSoftness=0.18f;
    planarFitMaterialDarkLumaMin=0.035f;
    planarFitMaterialDarkLumaMax=0.26f;
    planarFitMaterialDarkBlendCapValue=0.58f;
    methodName="PTDZI";
  }
};

struct MethodPTDZJ : MethodPTDM {
  MethodPTDZJ(){
    planarFitMaterialDarkBlendCap=true;
    planarFitMaterialDarkSatMin=0.08f;
    planarFitMaterialDarkSatSoftness=0.16f;
    planarFitMaterialDarkLumaMin=0.035f;
    planarFitMaterialDarkLumaMax=0.30f;
    planarFitMaterialDarkBlendCapValue=0.42f;
    methodName="PTDZJ";
  }
};

struct MethodPTDZK : MethodPTDM {
  MethodPTDZK(){
    planarFitGreenOppLossGate=true;
    planarFitGreenOppMin=0.004f;
    planarFitGreenOppSoftness=0.018f;
    planarFitGreenOppLossMin=0.0025f;
    planarFitGreenOppLossSoftness=0.014f;
    planarFitGreenOppBlendCap=0.58f;
    methodName="PTDZK";
  }
};

struct MethodPTDZL : MethodPTDM {
  MethodPTDZL(){
    planarFitGreenOppLossGate=true;
    planarFitGreenOppMin=0.0025f;
    planarFitGreenOppSoftness=0.014f;
    planarFitGreenOppLossMin=0.0015f;
    planarFitGreenOppLossSoftness=0.010f;
    planarFitGreenOppBlendCap=0.42f;
    methodName="PTDZL";
  }
};

struct MethodPTDZM : MethodPTDM {
  MethodPTDZM(){
    stratifiedHistoryChromaGuard=true;
    stratifiedHistoryChromaMin=0.030f;
    stratifiedHistoryGreenOppMin=0.006f;
    stratifiedHistoryMinN=3.0f;
    stratifiedHistoryDirectMax=0.18f;
    methodName="PTDZM";
  }
};

struct MethodPTDZN : MethodPTDM {
  MethodPTDZN(){
    stratifiedHistoryChromaGuard=true;
    stratifiedHistoryChromaMin=0.016f;
    stratifiedHistoryGreenOppMin=0.003f;
    stratifiedHistoryMinN=2.0f;
    stratifiedHistoryDirectMax=0.22f;
    methodName="PTDZN";
  }
};

struct MethodPTDZO : MethodPTDM {
  MethodPTDZO(){
    stratifiedHistoryChromaGuard=true;
    stratifiedHistoryGreenOppPositiveOnly=true;
    stratifiedHistoryChromaMin=10.0f;
    stratifiedHistoryGreenOppMin=0.0045f;
    stratifiedHistoryMinN=3.0f;
    stratifiedHistoryDirectMax=0.18f;
    methodName="PTDZO";
  }
};

struct MethodPTDZP : MethodPTDM {
  MethodPTDZP(){
    stratifiedHistoryChromaGuard=true;
    stratifiedHistoryGreenOppPositiveOnly=true;
    stratifiedHistoryChromaMin=10.0f;
    stratifiedHistoryGreenOppMin=0.0065f;
    stratifiedHistoryMinN=4.0f;
    stratifiedHistoryDirectMax=0.16f;
    methodName="PTDZP";
  }
};

struct MethodPTDZQ : MethodPTDM {
  MethodPTDZQ(){
    stratifiedHistoryChromaGuard=true;
    stratifiedHistoryChromaSupport=true;
    stratifiedHistoryChromaMin=0.030f;
    stratifiedHistoryGreenOppMin=0.006f;
    stratifiedHistoryMinN=3.0f;
    stratifiedHistoryDirectMax=0.18f;
    stratifiedHistoryChromaSupportRadius=1;
    stratifiedHistoryChromaSupportMinCount=3;
    stratifiedHistoryChromaSupportMinFrac=0.34f;
    methodName="PTDZQ";
  }
};

struct MethodPTDZR : MethodPTDM {
  MethodPTDZR(){
    stratifiedHistoryChromaGuard=true;
    stratifiedHistoryChromaSupport=true;
    stratifiedHistoryChromaMin=0.016f;
    stratifiedHistoryGreenOppMin=0.003f;
    stratifiedHistoryMinN=2.0f;
    stratifiedHistoryDirectMax=0.22f;
    stratifiedHistoryChromaSupportRadius=2;
    stratifiedHistoryChromaSupportMinCount=6;
    stratifiedHistoryChromaSupportMinFrac=0.26f;
    methodName="PTDZR";
  }
};

struct MethodPTDZS : MethodPTDZQ {
  MethodPTDZS(){
    stratifiedHistoryChromaFallbackProb=0.50f;
    methodName="PTDZS";
  }
};

struct MethodPTDZT : MethodPTDZQ {
  MethodPTDZT(){
    stratifiedHistoryChromaFallbackProb=0.72f;
    methodName="PTDZT";
  }
};
