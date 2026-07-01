// VoxelBench harness.
//   ./bench test                         — validate brickmap traversal vs naive DDA
//   ./bench ref  <scene> <pre|post> <spp> <seed>   — accumulate reference (raw+ppm)
//   ./bench run  <scene> <method> <mult> — 150-frame run, event @75, CSV append
//   ./bench runpost <scene> <method> <mult> [frames] — post-event cold-start diagnostic
//   ./bench still <scene> <pre|post>     — dump reference + direct-only images
// Output: results/raw/*.raw, results/images/*.ppm, results/results.csv
#include "core.h"
#include "scenes.h"
#include "technique.h"
#include "metrics.h"
#include "techniques/pt.h"
#include "techniques/ptx.h"
#include "techniques/ptg.h"
#include "techniques/ddgi.h"
#include "techniques/fcgi.h"
#include "techniques/gfc.h"
#include "techniques/gfcao.h"
#include "techniques/sfg.h"
#include "techniques/fcgi2.h"
#include "techniques/fcgix.h"
#include "techniques/fclt.h"
#include <memory>
#include <omp.h>

static const int FRAMES=150, EVENT_FRAME=75;
static const long BASE_BUDGET=4L*W*H;   // rays/frame at mult=1.0
static std::string RES="results";

static void prepState(Scene* s, bool post){
  s->build();
  if(post) s->applyEvent();
  collectEmitters();
  s->camera();
  buildGBuffer();
  buildDirect(128);
}

static Technique* makeTech(const std::string& m){
  if(m=="PT") return new MethodPT();
  if(m=="PTX") return new MethodPTX();
  if(m=="PTG") return new MethodPTG();
  if(m=="PTO") return new MethodPTO();
  if(m=="PTP") return new MethodPTP();
  if(m=="PTC") return new MethodPTC();
  if(m=="PTR") return new MethodPTR();
  if(m=="PTV") return new MethodPTV();
  if(m=="PTS") return new MethodPTS();
  if(m=="PTL") return new MethodPTL();
  if(m=="PTM") return new MethodPTM();
  if(m=="PTU") return new MethodPTU();
  if(m=="PTA") return new MethodPTA();
  if(m=="PTD") return new MethodPTD();
  if(m=="PTE") return new MethodPTE();
  if(m=="PTB") return new MethodPTB();
  if(m=="PTF") return new MethodPTF();
  if(m=="PTH") return new MethodPTH();
  if(m=="PTK") return new MethodPTK();
  if(m=="PTN") return new MethodPTN();
  if(m=="PTW") return new MethodPTW();
  if(m=="PTJ") return new MethodPTJ();
  if(m=="PTI") return new MethodPTI();
  if(m=="PTQ") return new MethodPTQ();
  if(m=="PTY") return new MethodPTY();
  if(m=="PTZ") return new MethodPTZ();
  if(m=="PTAA") return new MethodPTAA();
  if(m=="PTAB") return new MethodPTAB();
  if(m=="PTAC") return new MethodPTAC();
  if(m=="PTAD") return new MethodPTAD();
  if(m=="PTAE") return new MethodPTAE();
  if(m=="PTAF") return new MethodPTAF();
  if(m=="PTAG") return new MethodPTAG();
  if(m=="PTAH") return new MethodPTAH();
  if(m=="PTAI") return new MethodPTAI();
  if(m=="PTAJ") return new MethodPTAJ();
  if(m=="PTAK") return new MethodPTAK();
  if(m=="PTAL") return new MethodPTAL();
  if(m=="PTAM") return new MethodPTAM();
  if(m=="PTAN") return new MethodPTAN();
  if(m=="PTAO") return new MethodPTAO();
  if(m=="PTAP") return new MethodPTAP();
  if(m=="PTAQ") return new MethodPTAQ();
  if(m=="PTAR") return new MethodPTAR();
  if(m=="PTAS") return new MethodPTAS();
  if(m=="PTAT") return new MethodPTAT();
  if(m=="PTAU") return new MethodPTAU();
  if(m=="PTAV") return new MethodPTAV();
  if(m=="PTAW") return new MethodPTAW();
  if(m=="PTAX") return new MethodPTAX();
  if(m=="PTAY") return new MethodPTAY();
  if(m=="PTAZ") return new MethodPTAZ();
  if(m=="PTBA") return new MethodPTBA();
  if(m=="PTBB") return new MethodPTBB();
  if(m=="PTBC") return new MethodPTBC();
  if(m=="PTBD") return new MethodPTBD();
  if(m=="PTBE") return new MethodPTBE();
  if(m=="PTBF") return new MethodPTBF();
  if(m=="PTBG") return new MethodPTBG();
  if(m=="PTBH") return new MethodPTBH();
  if(m=="PTBI") return new MethodPTBI();
  if(m=="PTBJ") return new MethodPTBJ();
  if(m=="PTBK") return new MethodPTBK();
  if(m=="PTBL") return new MethodPTBL();
  if(m=="PTBM") return new MethodPTBM();
  if(m=="PTBN") return new MethodPTBN();
  if(m=="PTBO") return new MethodPTBO();
  if(m=="PTBP") return new MethodPTBP();
  if(m=="PTBQ") return new MethodPTBQ();
  if(m=="PTBR") return new MethodPTBR();
  if(m=="PTBS") return new MethodPTBS();
  if(m=="PTBT") return new MethodPTBT();
  if(m=="PTBU") return new MethodPTBU();
  if(m=="PTBV") return new MethodPTBV();
  if(m=="PTBW") return new MethodPTBW();
  if(m=="PTBX") return new MethodPTBX();
  if(m=="PTBY") return new MethodPTBY();
  if(m=="PTBZ") return new MethodPTBZ();
  if(m=="PTCA") return new MethodPTCA();
  if(m=="PTCB") return new MethodPTCB();
  if(m=="PTCC") return new MethodPTCC();
  if(m=="PTCD") return new MethodPTCD();
  if(m=="PTCE") return new MethodPTCE();
  if(m=="PTCF") return new MethodPTCF();
  if(m=="PTCG") return new MethodPTCG();
  if(m=="PTCH") return new MethodPTCH();
  if(m=="PTCI") return new MethodPTCI();
  if(m=="PTCJ") return new MethodPTCJ();
  if(m=="PTCK") return new MethodPTCK();
  if(m=="PTCL") return new MethodPTCL();
  if(m=="PTCM") return new MethodPTCM();
  if(m=="PTCN") return new MethodPTCN();
  if(m=="PTCO") return new MethodPTCO();
  if(m=="PTCP") return new MethodPTCP();
  if(m=="PTCQ") return new MethodPTCQ();
  if(m=="PTCR") return new MethodPTCR();
  if(m=="PTCS") return new MethodPTCS();
  if(m=="PTCT") return new MethodPTCT();
  if(m=="PTCU") return new MethodPTCU();
  if(m=="PTCV") return new MethodPTCV();
  if(m=="PTCW") return new MethodPTCW();
  if(m=="PTCX") return new MethodPTCX();
  if(m=="PTCY") return new MethodPTCY();
  if(m=="PTCZ") return new MethodPTCZ();
  if(m=="PTDA") return new MethodPTDA();
  if(m=="PTDB") return new MethodPTDB();
  if(m=="PTDC") return new MethodPTDC();
  if(m=="PTDD") return new MethodPTDD();
  if(m=="PTDE") return new MethodPTDE();
  if(m=="PTDF") return new MethodPTDF();
  if(m=="PTDG") return new MethodPTDG();
  if(m=="PTDH") return new MethodPTDH();
  if(m=="PTDI") return new MethodPTDI();
  if(m=="PTDJ") return new MethodPTDJ();
  if(m=="PTDK") return new MethodPTDK();
  if(m=="PTDL") return new MethodPTDL();
  if(m=="PTDM") return new MethodPTDM();
  if(m=="PTDN") return new MethodPTDN();
  if(m=="PTDO") return new MethodPTDO();
  if(m=="PTDP") return new MethodPTDP();
  if(m=="PTDQ") return new MethodPTDQ();
  if(m=="PTDR") return new MethodPTDR();
  if(m=="PTDS") return new MethodPTDS();
  if(m=="PTDT") return new MethodPTDT();
  if(m=="PTDU") return new MethodPTDU();
  if(m=="PTDV") return new MethodPTDV();
  if(m=="PTDW") return new MethodPTDW();
  if(m=="PTDX") return new MethodPTDX();
  if(m=="PTDY") return new MethodPTDY();
  if(m=="PTDZ") return new MethodPTDZ();
  if(m=="PTDZA") return new MethodPTDZA();
  if(m=="PTDZB") return new MethodPTDZB();
  if(m=="PTDZC") return new MethodPTDZC();
  if(m=="PTDZD") return new MethodPTDZD();
  if(m=="PTDZE") return new MethodPTDZE();
  if(m=="PTDZF") return new MethodPTDZF();
  if(m=="PTDZG") return new MethodPTDZG();
  if(m=="PTDZH") return new MethodPTDZH();
  if(m=="PTDZI") return new MethodPTDZI();
  if(m=="PTDZJ") return new MethodPTDZJ();
  if(m=="PTDZK") return new MethodPTDZK();
  if(m=="PTDZL") return new MethodPTDZL();
  if(m=="PTDZM") return new MethodPTDZM();
  if(m=="PTDZN") return new MethodPTDZN();
  if(m=="PTDZO") return new MethodPTDZO();
  if(m=="PTDZP") return new MethodPTDZP();
  if(m=="PTDZQ") return new MethodPTDZQ();
  if(m=="PTDZR") return new MethodPTDZR();
  if(m=="PTDZS") return new MethodPTDZS();
  if(m=="PTDZT") return new MethodPTDZT();
  if(m=="DDGI") return new MethodDDGI();
  if(m=="FCGI") return new MethodFC();
  if(m=="GFC") return new MethodGFC();
  if(m=="GFCAO") return new MethodGFCAO();
  if(m=="SFG") return new MethodSFG();
  if(m=="FCGI2") return new MethodFC2();
  if(m=="FCGIX") return new MethodFCX();
  if(m=="FCLT") return new MethodFCLT();
  return nullptr;
}

int main(int argc,char**argv){
  omp_set_num_threads(2);
  std::string mode = argc>1? argv[1] : "";

  if(mode=="test"){
    // traversal equivalence across all scenes/states + speed ratio
    const char* scenes[4]={"bunker","courtyard","cavern","town"};
    long bad=0, tot=0; double tN=0,tB=0;
    for(int si=0;si<4;si++)for(int post=0;post<2;post++){
      std::unique_ptr<Scene> s(makeScene(scenes[si]));
      s->build(); if(post) s->applyEvent(); collectEmitters();
      RNG rng(si*2+post,42,777);
      uint64_t r0,s0;
      for(int i=0;i<40000;i++){
        V3 o(1.f+rng.uf()*(NX-2), 1.f+rng.uf()*(NY-2), 1.f+rng.uf()*(NZ-2));
        float z=rng.uf()*2-1, ph=rng.uf()*6.2831853f, rr=std::sqrt(std::max(0.f,1-z*z));
        V3 d(rr*std::cos(ph), z, rr*std::sin(ph));
        Hit a,b;
        auto ta=std::chrono::high_resolution_clock::now();
        bool ha=traceNaive(o,d,1e9f,a);
        auto tb_=std::chrono::high_resolution_clock::now();
        bool hb=trace(o,d,1e9f,b);
        auto tc=std::chrono::high_resolution_clock::now();
        tN+=std::chrono::duration<double,std::micro>(tb_-ta).count();
        tB+=std::chrono::duration<double,std::micro>(tc-tb_).count();
        tot++;
        bool ok = (ha==hb) && (!ha || (a.vx==b.vx&&a.vy==b.vy&&a.vz==b.vz&&a.face==b.face));
        if(!ok){ bad++;
          if(bad<6) fprintf(stderr,"MISMATCH s=%s post=%d o=(%.3f,%.3f,%.3f) d=(%.3f,%.3f,%.3f) ha=%d hb=%d a=(%d,%d,%d f%d) b=(%d,%d,%d f%d)\n",
            scenes[si],post,o.x,o.y,o.z,d.x,d.y,d.z,(int)ha,(int)hb,
            ha?a.vx:-1,ha?a.vy:-1,ha?a.vz:-1,ha?a.face:-1, hb?b.vx:-1,hb?b.vy:-1,hb?b.vz:-1,hb?b.face:-1);
        }
      }
      (void)r0;(void)s0;
    }
    printf("traversal test: %ld/%ld mismatches; naive %.2fus/ray, brick %.2fus/ray (%.2fx)\n",
           bad,tot,tN/tot,tB/tot,tN/tB);
    return bad?1:0;
  }

  if(mode=="ref"){
    std::string sc=argv[2]; bool post=std::string(argv[3])=="post";
    int spp=atoi(argv[4]); uint64_t seed=atoll(argv[5]);
    std::unique_ptr<Scene> s(makeScene(sc));
    prepState(s.get(),post);
    std::vector<V3> acc(W*H), cur(W*H); double cnt=0;
    std::string raw=RES+"/raw/"+sc+(post?"_post":"_pre")+".raw";
    loadRawImg(raw,acc,cnt);
    renderReference(cur,spp,seed*7919+post*13);
    double nc=cnt+spp;
    for(int i=0;i<W*H;i++) acc[i]=acc[i]*(float)(cnt/nc)+cur[i]*(float)(spp/nc);
    saveRawImg(raw,acc,nc);
    savePPM(RES+"/images/ref_"+sc+(post?"_post":"_pre")+".ppm",acc);
    fprintf(stderr,"ref %s %s spp=%.0f\n",sc.c_str(),post?"post":"pre",nc);
    return 0;
  }

  if(mode=="still"){
    std::string sc=argv[2]; bool post=std::string(argv[3])=="post";
    std::unique_ptr<Scene> s(makeScene(sc));
    prepState(s.get(),post);
    std::vector<V3> zero(W*H);
    savePPM(RES+"/images/direct_"+sc+(post?"_post":"_pre")+".ppm",zero);
    return 0;
  }

  if(mode=="runpost"){
    std::string sc=argv[2], meth=argv[3]; float mult=atof(argv[4]);
    int frames = argc>5 ? atoi(argv[5]) : EVENT_FRAME;
    std::unique_ptr<Scene> s(makeScene(sc));
    std::vector<V3> refPost(W*H); double c=0;
    if(!loadRawImg(RES+"/raw/"+sc+"_post.raw",refPost,c)){
      fprintf(stderr,"missing post reference for %s\n",sc.c_str()); return 1; }
    prepState(s.get(),true);
    std::unique_ptr<Technique> m(makeTech(meth));
    if(!m){ fprintf(stderr,"unknown method\n"); return 1; }
    m->init();
    long budget=(long)(BASE_BUDGET*mult);
    char tag[80]; snprintf(tag,80,"%s_%s_post_b%.2fx",sc.c_str(),meth.c_str(),mult);
    FILE* fp=fopen((RES+"/coldstart.csv").c_str(),"a");
    fseek(fp,0,SEEK_END);
    if(ftell(fp)==0) fprintf(fp,"scene,method,mult,frame,psnr,ssim,gpsnr,flicker,rays,ms,steps\n");
    std::vector<V3> prev;
    for(int f=0; f<frames; f++){
      g_rays=0; g_steps=0;
      auto t0=std::chrono::high_resolution_clock::now();
      long rays=m->frame(budget);
      auto t1=std::chrono::high_resolution_clock::now();
      double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
      double psnr=psnrTM(m->out,refPost);
      double ssim=ssimTM(m->out,refPost);
      double gpsnr=gradPsnrTM(m->out,refPost);
      double flick=prev.empty()?0:flickerTM(m->out,prev);
      prev=m->out;
      fprintf(fp,"%s,%s,%.2f,%d,%.4f,%.5f,%.4f,%.6f,%ld,%.3f,%llu\n",
        sc.c_str(),meth.c_str(),mult,f,psnr,ssim,gpsnr,flick,rays,ms,(unsigned long long)g_steps.load());
      if(f==30||f==74||f==frames-1)
        savePPM(RES+"/images/"+std::string(tag)+"_f"+std::to_string(f)+".ppm",m->out);
    }
    fclose(fp);
    fprintf(stderr,"done %s frames=%d\n",tag,frames);
    return 0;
  }

  if(mode=="run"){
    std::string sc=argv[2], meth=argv[3]; float mult=atof(argv[4]);
    std::unique_ptr<Scene> s(makeScene(sc));
    // references must exist
    std::vector<V3> refPre(W*H), refPost(W*H); double c1=0,c2=0;
    if(!loadRawImg(RES+"/raw/"+sc+"_pre.raw",refPre,c1) ||
       !loadRawImg(RES+"/raw/"+sc+"_post.raw",refPost,c2)){
      fprintf(stderr,"missing references for %s\n",sc.c_str()); return 1; }
    prepState(s.get(),false);
    std::unique_ptr<Technique> m(makeTech(meth));
    if(!m){ fprintf(stderr,"unknown method\n"); return 1; }
    m->init();
    long budget=(long)(BASE_BUDGET*mult);
    char tag[64]; snprintf(tag,64,"%s_%s_b%.2fx",sc.c_str(),meth.c_str(),mult);
    FILE* fp=fopen((RES+"/results.csv").c_str(),"a");
    fseek(fp,0,SEEK_END);
    if(ftell(fp)==0) fprintf(fp,"scene,method,mult,frame,psnr,ssim,gpsnr,flicker,rays,ms,steps\n");
    std::vector<V3> prev;
    bool dump = std::fabs(mult-1.f)<1e-3 || meth=="PTR" || meth=="PTV" || meth=="PTS" || meth=="PTL" || meth=="PTM" || meth=="PTU" || meth=="PTA" || meth=="PTD" || meth=="PTE" || meth=="PTB" || meth=="PTF" || meth=="PTH" || meth=="PTK" || meth=="PTN" || meth=="PTW" || meth=="PTJ" || meth=="PTI" || meth=="PTQ" || meth=="PTY" || meth=="PTZ" || meth=="PTAA" || meth=="PTAB" || meth=="PTAC" || meth=="PTAD" || meth=="PTAE" || meth=="PTAF" || meth=="PTAG" || meth=="PTAH" || meth=="PTAI" || meth=="PTAJ" || meth=="PTAK" || meth=="PTAL" || meth=="PTAM" || meth=="PTAN" || meth=="PTAO" || meth=="PTAP" || meth=="PTAQ" || meth=="PTAR" || meth=="PTAS" || meth=="PTAT" || meth=="PTAU" || meth=="PTAV" || meth=="PTAW" || meth=="PTAX" || meth=="PTAY" || meth=="PTAZ" || meth=="PTBA" || meth=="PTBB" || meth=="PTBC" || meth=="PTBD" || meth=="PTBE" || meth=="PTBF" || meth=="PTBG" || meth=="PTBH" || meth=="PTBI" || meth=="PTBJ" || meth=="PTBK" || meth=="PTBL" || meth=="PTBM" || meth=="PTBN" || meth=="PTBO" || meth=="PTBP" || meth=="PTBQ" || meth=="PTBR" || meth=="PTBS" || meth=="PTBT" || meth=="PTBU" || meth=="PTBV" || meth=="PTBW" || meth=="PTBX" || meth=="PTBY" || meth=="PTBZ" || meth=="PTCA" || meth=="PTCB" || meth=="PTCC" || meth=="PTCD" || meth=="PTCE" || meth=="PTCF" || meth=="PTCG" || meth=="PTCH" || meth=="PTCI" || meth=="PTCJ" || meth=="PTCK" || meth=="PTCL" || meth=="PTCM" || meth=="PTCN" || meth=="PTCO" || meth=="PTCP" || meth=="PTCQ" || meth=="PTCR" || meth=="PTCS" || meth=="PTCT" || meth=="PTCU" || meth=="PTCV" || meth=="PTCW" || meth=="PTCX" || meth=="PTCY" || meth=="PTCZ" || meth=="PTDA" || meth=="PTDB" || meth=="PTDC" || meth=="PTDD" || meth=="PTDE" || meth=="PTDF" || meth=="PTDG" || meth=="PTDH" || meth=="PTDI" || meth=="PTDJ" || meth=="PTDK" || meth=="PTDL" || meth=="PTDM" || meth=="PTDN" || meth=="PTDO" || meth=="PTDP" || meth=="PTDQ" || meth=="PTDR" || meth=="PTDS" || meth=="PTDT" || meth=="PTDU" || meth=="PTDV" || meth=="PTDW" || meth=="PTDX" || meth=="PTDY" || meth=="PTDZ" || meth=="PTDZA" || meth=="PTDZB" || meth=="PTDZC" || meth=="PTDZD" || meth=="PTDZE" || meth=="PTDZF";
    for(int f=0; f<FRAMES; f++){
      if(f==EVENT_FRAME){
        if(s->event.geomEdit){ s->applyEvent(); collectEmitters(); buildGBuffer(); }
        else s->applyEvent();              // lighting-only
        buildDirect(128);
        m->onEvent(s->event);
      }
      g_rays=0; g_steps=0;
      auto t0=std::chrono::high_resolution_clock::now();
      long rays=m->frame(budget);
      auto t1=std::chrono::high_resolution_clock::now();
      double ms=std::chrono::duration<double,std::milli>(t1-t0).count();
      auto& ref=(f<EVENT_FRAME)?refPre:refPost;
      double psnr=psnrTM(m->out,ref);
      double ssim=ssimTM(m->out,ref);
      double gpsnr=gradPsnrTM(m->out,ref);
      double flick=prev.empty()?0:flickerTM(m->out,prev);
      prev=m->out;
      fprintf(fp,"%s,%s,%.2f,%d,%.4f,%.5f,%.4f,%.6f,%ld,%.3f,%llu\n",
        sc.c_str(),meth.c_str(),mult,f,psnr,ssim,gpsnr,flick,rays,ms,(unsigned long long)g_steps.load());
      if(dump && (f==30||f==74||f==76||f==80||f==90||f==149))
        savePPM(RES+"/images/"+std::string(tag)+"_f"+std::to_string(f)+".ppm",m->out);
    }
    fclose(fp);
    fprintf(stderr,"done %s\n",tag);
    return 0;
  }

  fprintf(stderr,"usage: bench test | ref <scene> <pre|post> <spp> <seed> | run <scene> <method> <mult> | runpost <scene> <method> <mult> [frames] | still <scene> <pre|post>\n");
  return 1;
}
