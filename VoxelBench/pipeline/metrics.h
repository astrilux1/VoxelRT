// Image quality metrics, computed on tonemapped output (what the player sees).
// PSNR: hit-pixels only (sky identical across methods). SSIM: gaussian 11x11,
// luma. Flicker: mean abs tonemapped-luma delta between consecutive frames.
#pragma once
#include "core.h"

static inline void tonemapImg(const std::vector<V3>& ind, std::vector<V3>& tm3){
  tm3.resize(W*H);
  for(int i=0;i<W*H;i++){
    V3 c=directImg[i]+ind[i];
    tm3[i]=V3(tonemap(c.x),tonemap(c.y),tonemap(c.z));
  }
}

static double psnrTM(const std::vector<V3>& ind, const std::vector<V3>& refInd){
  double mse=0; int n=0;
  for(int i=0;i<W*H;i++){
    if(!gbuf[i].hit) continue;
    V3 a=directImg[i]+ind[i], b=directImg[i]+refInd[i];
    float da=tonemap(a.x)-tonemap(b.x), db=tonemap(a.y)-tonemap(b.y), dc=tonemap(a.z)-tonemap(b.z);
    mse += (da*da+db*db+dc*dc)/3.0; n++;
  }
  if(!n) return 99;
  mse/=n;
  return 10.0*std::log10(1.0/std::max(mse,1e-12));
}

// SSIM on tonemapped luma, gaussian 11x11 sigma 1.5 (standard constants)
static double ssimTM(const std::vector<V3>& ind, const std::vector<V3>& refInd){
  static std::vector<float> A, B; A.resize(W*H); B.resize(W*H);
  for(int i=0;i<W*H;i++){
    V3 a=directImg[i]+ind[i], b=directImg[i]+refInd[i];
    A[i]=luma(V3(tonemap(a.x),tonemap(a.y),tonemap(a.z)));
    B[i]=luma(V3(tonemap(b.x),tonemap(b.y),tonemap(b.z)));
  }
  static float gk[11]; static bool gkInit=false;
  if(!gkInit){ float s=0; for(int i=0;i<11;i++){ float d=i-5; gk[i]=std::exp(-d*d/(2*1.5f*1.5f)); s+=gk[i]; }
               for(int i=0;i<11;i++) gk[i]/=s; gkInit=true; }
  const double C1=0.01*0.01, C2=0.03*0.03;
  double ssim=0; int n=0;
  for(int y=5;y<H-5;y++)for(int x=5;x<W-5;x++){
    double ma=0,mb=0;
    for(int dy=-5;dy<=5;dy++)for(int dx=-5;dx<=5;dx++){
      double w=gk[dy+5]*gk[dx+5]; int j=(y+dy)*W+(x+dx);
      ma+=w*A[j]; mb+=w*B[j];
    }
    double va=0,vb=0,cab=0;
    for(int dy=-5;dy<=5;dy++)for(int dx=-5;dx<=5;dx++){
      double w=gk[dy+5]*gk[dx+5]; int j=(y+dy)*W+(x+dx);
      va+=w*(A[j]-ma)*(A[j]-ma); vb+=w*(B[j]-mb)*(B[j]-mb); cab+=w*(A[j]-ma)*(B[j]-mb);
    }
    ssim += ((2*ma*mb+C1)*(2*cab+C2))/((ma*ma+mb*mb+C1)*(va+vb+C2));
    n++;
  }
  return ssim/n;
}

// Gradient PSNR: PSNR over central-difference gradients of tonemapped luma.
// Punishes BOTH blur (missing gradients the reference has) and residual noise
// (gradients the reference lacks). A flat over-filtered render can score well
// on plain PSNR but cannot score well here - this is the anti-blur-gaming
// metric. Sky/miss pixels excluded via gbuf like psnrTM.
static double gradPsnrTM(const std::vector<V3>& ind, const std::vector<V3>& refInd){
  static std::vector<float> GA, GB; GA.resize(W*H); GB.resize(W*H);
  for(int i=0;i<W*H;i++){
    V3 a=directImg[i]+ind[i], b=directImg[i]+refInd[i];
    GA[i]=luma(V3(tonemap(a.x),tonemap(a.y),tonemap(a.z)));
    GB[i]=luma(V3(tonemap(b.x),tonemap(b.y),tonemap(b.z)));
  }
  double mse=0; long n=0;
  for(int y=1;y<H-1;y++)for(int x=1;x<W-1;x++){
    int i=y*W+x;
    if(!gbuf[i].hit) continue;
    float gxa=GA[i+1]-GA[i-1], gya=GA[i+W]-GA[i-W];
    float gxb=GB[i+1]-GB[i-1], gyb=GB[i+W]-GB[i-W];
    float dx=gxa-gxb, dy=gya-gyb;
    mse += (dx*dx+dy*dy)*0.5; n++;
  }
  if(!n) return 99;
  mse/=n;
  return 10.0*std::log10(1.0/std::max(mse,1e-12));
}

// temporal flicker: mean |luma(t)-luma(t-1)| (tonemapped), hit pixels
static double flickerTM(const std::vector<V3>& ind, const std::vector<V3>& prevInd){
  double acc=0; int n=0;
  for(int i=0;i<W*H;i++){
    if(!gbuf[i].hit) continue;
    V3 a=directImg[i]+ind[i], b=directImg[i]+prevInd[i];
    acc += std::fabs(luma(V3(tonemap(a.x),tonemap(a.y),tonemap(a.z)))
                    -luma(V3(tonemap(b.x),tonemap(b.y),tonemap(b.z))));
    n++;
  }
  return n? acc/n : 0;
}
