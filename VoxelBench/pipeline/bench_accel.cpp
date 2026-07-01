// Acceleration-structure research benchmark.
//   ./bench_accel
// Builds bunker/cavern/town (pre+post), builds all accel structures, validates
// traceSuper/traceCSDF/traceBrickCol against traceNaive on several workloads,
// times them (best-of-3), and measures edit cost (rebuild + staleness recovery).
// Output: results/accel.csv, results/accel_meta.csv + stdout summary.
#include "core.h"
#include "scenes.h"
#include "accel.h"
#include "accel2.h"
#include <memory>
#include <cstdio>
#include <vector>
#include <string>

struct Ray{ V3 o,d; float tmax; };

static const int NRAYS=30000;
static float g_capT=1e9f;
static const int REPS=3;

// ----------------------------------------------------------------------------
static bool hitEq(bool ha,const Hit&a,bool hb,const Hit&b,float& tdiff){
  tdiff=0;
  if(ha!=hb) return false;
  if(!ha) return true;
  if(a.vx!=b.vx||a.vy!=b.vy||a.vz!=b.vz||a.face!=b.face) return false;
  tdiff=std::fabs(a.t-b.t);
  return tdiff<=1e-3f;
}

// generic trace dispatch
enum Struct{ S_NAIVE=0, S_BRICK, S_SUPER, S_CSDF, S_BRICKCOL, S_HX, S_ODF, S_COUNT };
static const char* structName[S_COUNT]={"naive","brick","super","csdf","brickcol","hx","odf"};

struct Structures{
  SuperGrid super; CSDF csdf; ColMap col; HX hx; ODF odf;
};

static bool doTrace(int which, Structures& S, V3 o, V3 d, float tmax, Hit& h){
  switch(which){
    case S_NAIVE: return traceNaive(o,d,tmax,h);
    case S_BRICK: return trace(o,d,tmax,h);
    case S_SUPER: return traceSuper(S.super,o,d,tmax,h);
    case S_CSDF:  return traceCSDF(S.csdf,o,d,tmax,h);
    case S_BRICKCOL: return traceBrickCol(S.col,o,d,tmax,h);
    case S_HX: return traceHX(S.hx,o,d,tmax,h);
    case S_ODF: return traceODF(S.odf,o,d,tmax,h);
  }
  return false;
}

// ----------------------------------------------------------------------------
// workload generation
struct SurfacePt{ V3 p; V3 n; int vx,vy,vz,face; uint8_t m; };

static std::vector<Ray> genPrimary(RNG& rng, int n){
  std::vector<Ray> out; out.reserve(n);
  for(int i=0;i<n;i++){
    float px=rng.uf()*W, py=rng.uf()*H;
    out.push_back({camPos, pixDir(px,py), g_capT});
  }
  return out;
}

static std::vector<SurfacePt> genSurfacePts(RNG& rng, const std::vector<Ray>& primary, int n){
  std::vector<SurfacePt> pts; pts.reserve(n);
  int tries=0;
  while((int)pts.size()<n && tries < n*50){
    tries++;
    int idx = rng.u32()%primary.size();
    Hit h;
    if(traceNaive(primary[idx].o, primary[idx].d, primary[idx].tmax, h)){
      SurfacePt sp;
      sp.p = facePoint(h.vx,h.vy,h.vz,h.face);
      sp.n = FN[h.face];
      sp.vx=h.vx; sp.vy=h.vy; sp.vz=h.vz; sp.face=h.face; sp.m=h.m;
      pts.push_back(sp);
    }
  }
  return pts;
}

static std::vector<Ray> genGather(RNG& rng, const std::vector<SurfacePt>& pts, int n){
  std::vector<Ray> out; out.reserve(n);
  for(int i=0;i<n;i++){
    const SurfacePt& sp = pts[rng.u32()%pts.size()];
    V3 d = cosDir(sp.n, rng);
    out.push_back({sp.p, d, g_capT});
  }
  return out;
}

static std::vector<Ray> genSunShadow(RNG& rng, const std::vector<SurfacePt>& pts, int n){
  std::vector<Ray> out; out.reserve(n);
  V3 sunDir = SKY.enabled ? SKY.sunDir : norm(V3(0.3f,0.9f,0.2f));
  for(int i=0;i<n;i++){
    const SurfacePt& sp = pts[rng.u32()%pts.size()];
    out.push_back({sp.p, sunDir, 1e9f});
  }
  return out;
}

static std::vector<Ray> genEmShadow(RNG& rng, const std::vector<SurfacePt>& pts, int n){
  std::vector<Ray> out; out.reserve(n);
  if(emFaces.empty()) return out;
  for(int i=0;i<n;i++){
    const SurfacePt& sp = pts[rng.u32()%pts.size()];
    const EmFace& e = emFaces[rng.u32()%emFaces.size()];
    V3 fn = FN[e.f];
    V3 q = facePoint(e.x,e.y,e.z,e.f);
    // jitter the target point across the emissive face (matches neeIrr's NEE
    // sampling); avoids exact-tie diagonal directions (dx==-dz etc.) that hit
    // the corner-voxel ambiguity inherent to trace()'s tie-breaking.
    V3 tu = (std::fabs(fn.x)>0.5f)? V3(0,1,0) : V3(1,0,0);
    V3 tv = cross(fn,tu);
    q += tu*(rng.uf()-0.5f) + tv*(rng.uf()-0.5f);
    V3 w = q - sp.p; float dlen=len(w);
    if(dlen<1e-6f) continue;
    out.push_back({sp.p, w*(1.f/dlen), dlen+1.f});
  }
  return out;
}

// ----------------------------------------------------------------------------
// validate + time a workload for one structure
struct Result{ double ns_per_ray=0; double steps_per_ray=0; long mismatches=0; };

static Result runWorkload(int which, Structures& S, const std::vector<Ray>& rays, const char* wlname, const char* scname, const char* statename){
  Result R;
  // validate
  long bad=0;
  for(size_t i=0;i<rays.size();i++){
    Hit ha,hb; bool ra=traceNaive(rays[i].o,rays[i].d,rays[i].tmax,ha);
    Hit hb2; bool rb=doTrace(which,S,rays[i].o,rays[i].d,rays[i].tmax,hb2);
    float tdiff;
    if(!hitEq(ra,ha,rb,hb2,tdiff)){
      bad++;
      if(bad<=5){
        fprintf(stderr,"MISMATCH scene=%s state=%s struct=%s workload=%s ray=%zu o=(%.4f,%.4f,%.4f) d=(%.6f,%.6f,%.6f)\n"
                       "  naive: hit=%d (%d,%d,%d) f%d t=%.6f m=%d\n"
                       "  %s : hit=%d (%d,%d,%d) f%d t=%.6f m=%d\n",
                scname,statename,structName[which],wlname,i,
                rays[i].o.x,rays[i].o.y,rays[i].o.z,rays[i].d.x,rays[i].d.y,rays[i].d.z,
                (int)ra, ra?ha.vx:-1, ra?ha.vy:-1, ra?ha.vz:-1, ra?ha.face:-1, ra?ha.t:0.f, ra?ha.m:0,
                structName[which],(int)rb, rb?hb2.vx:-1, rb?hb2.vy:-1, rb?hb2.vz:-1, rb?hb2.face:-1, rb?hb2.t:0.f, rb?hb2.m:0);
      }
    }
  }
  R.mismatches=bad;
  // time: best of REPS
  double bestNs=1e30; uint64_t bestSteps=0;
  for(int rep=0;rep<REPS;rep++){
    flushCounters(); tl_steps=0; tl_rays=0;
    auto t0=std::chrono::high_resolution_clock::now();
    for(size_t i=0;i<rays.size();i++){
      Hit h; doTrace(which,S,rays[i].o,rays[i].d,rays[i].tmax,h);
    }
    auto t1=std::chrono::high_resolution_clock::now();
    double ns=std::chrono::duration<double,std::nano>(t1-t0).count();
    if(ns<bestNs){ bestNs=ns; bestSteps=tl_steps; }
    flushCounters();
  }
  R.ns_per_ray = bestNs/(double)rays.size();
  R.steps_per_ray = (double)bestSteps/(double)rays.size();
  return R;
}

// ----------------------------------------------------------------------------
int main(int argc, char** argv){
  char csvPath[256]; snprintf(csvPath,sizeof(csvPath),"results/accel%s%s.csv",(argc>1?"_":""),(argc>1?argv[1]:""));
  FILE* fcsv=fopen(csvPath,"w");
  fprintf(fcsv,"scene,state,structure,workload,ns_per_ray,steps_per_ray,mismatches\n");
  char metaPath[256]; snprintf(metaPath,sizeof(metaPath),"results/accel_meta%s%s.csv",(argc>1?"_":""),(argc>1?argv[1]:""));
  FILE* fmeta=fopen(metaPath,"w");
  fprintf(fmeta,"scene,structure,build_ms,bytes\n");

  const char* scenes[4]={"bunker","cavern","town","city"};
  long totalMismatches=0;

  printf("%-8s %-5s %-9s %-10s %12s %10s %6s\n","scene","state","struct","workload","ns/ray","steps/ray","mism");

  for(int si=0;si<4;si++){
    if(argc>1 && std::string(argv[1])!=scenes[si]) continue;
    for(int post=0;post<2;post++){
      const char* statename = post? "post":"pre";
      std::unique_ptr<Scene> s(makeScene(scenes[si]));
      s->build();
      if(post) s->applyEvent();
      collectEmitters();
      s->camera();

      // ---- build accel structures, time + bytes ----
      Structures S;
      auto bt0=std::chrono::high_resolution_clock::now();
      S.super.build();
      auto bt1=std::chrono::high_resolution_clock::now();
      S.csdf.build();
      auto bt2=std::chrono::high_resolution_clock::now();
      S.col.build(); S.col.buildBrickCols();
      auto bt3=std::chrono::high_resolution_clock::now();
      S.hx.build();
      auto bt4=std::chrono::high_resolution_clock::now();
      S.odf.build();
      auto bt5=std::chrono::high_resolution_clock::now();

      double superMs=std::chrono::duration<double,std::milli>(bt1-bt0).count();
      double csdfMs =std::chrono::duration<double,std::milli>(bt2-bt1).count();
      double colMs  =std::chrono::duration<double,std::milli>(bt3-bt2).count();
      double hxMs   =std::chrono::duration<double,std::milli>(bt4-bt3).count();
      double odfMs  =std::chrono::duration<double,std::milli>(bt5-bt4).count();

      size_t brickBytes = brick.size()*8 + grid.size(); // brickmap masks + voxel grid
      // Only emit meta once per scene (pre state); structures are rebuilt for
      // post too but sizes are essentially identical (same dims).
      if(!post){
        fprintf(fmeta,"%s,brick,%.4f,%zu\n",scenes[si],0.0,brickBytes);
        fprintf(fmeta,"%s,super,%.4f,%zu\n",scenes[si],superMs,S.super.bytes());
        fprintf(fmeta,"%s,csdf,%.4f,%zu\n",scenes[si],csdfMs,S.csdf.bytes());
        fprintf(fmeta,"%s,brickcol,%.4f,%zu\n",scenes[si],colMs,S.col.bytes());
        fprintf(fmeta,"%s,hx,%.4f,%zu\n",scenes[si],hxMs,S.hx.bytes());
        fprintf(fmeta,"%s,odf,%.4f,%zu\n",scenes[si],odfMs,S.odf.bytes());
      }

      // ---- workloads ----
      g_capT = (std::string(scenes[si])=="city")? 384.f : 1e9f;
      RNG rng((uint64_t)(si*2+post)+1, 99, 555);
      std::vector<Ray> primary = genPrimary(rng, NRAYS);
      std::vector<SurfacePt> surf = genSurfacePts(rng, primary, 4096);
      std::vector<Ray> gather = surf.empty()? std::vector<Ray>() : genGather(rng, surf, NRAYS);
      std::vector<Ray> sunshadow = surf.empty()? std::vector<Ray>() : genSunShadow(rng, surf, NRAYS);
      std::vector<Ray> emshadow = (surf.empty()||emFaces.empty())? std::vector<Ray>() : genEmShadow(rng, surf, NRAYS);

      struct WL{ const char* name; std::vector<Ray>* rays; };
      std::vector<WL> wls;
      wls.push_back({"primary",&primary});
      if(!gather.empty()) wls.push_back({"gather",&gather});
      if(!sunshadow.empty()) wls.push_back({"sunshadow",&sunshadow});
      if(!emshadow.empty()) wls.push_back({"emshadow",&emshadow});

      for(auto& wl: wls){
        for(int which=0; which<S_COUNT; which++){
          Result R = runWorkload(which, S, *wl.rays, wl.name, scenes[si], statename);
          totalMismatches += R.mismatches;
          fprintf(fcsv,"%s,%s,%s,%s,%.3f,%.4f,%ld\n",
            scenes[si],statename,structName[which],wl.name,R.ns_per_ray,R.steps_per_ray,R.mismatches);
          printf("%-8s %-5s %-9s %-10s %12.2f %10.4f %6ld\n",
            scenes[si],statename,structName[which],wl.name,R.ns_per_ray,R.steps_per_ray,R.mismatches);
        }
      }

      // ---- EDIT benchmark (post-event states only; all three scenes geomEdit) ----
      if(post && s->event.geomEdit){
        printf("\n-- edit cost: %s --\n", scenes[si]);
        SceneEvent& ev = s->event;
        // (i) full rebuildAllBricks() vs incremental setVox over the blast bbox
        auto e0=std::chrono::high_resolution_clock::now();
        rebuildAllBricks();
        auto e1=std::chrono::high_resolution_clock::now();
        double fullRebuildMs=std::chrono::duration<double,std::milli>(e1-e0).count();

        int x0=std::max(0,(int)(ev.center.x-ev.radius)-1), x1=std::min(NX-1,(int)(ev.center.x+ev.radius)+1);
        int y0=std::max(0,(int)(ev.center.y-ev.radius)-1), y1=std::min(NY-1,(int)(ev.center.y+ev.radius)+1);
        int z0=std::max(0,(int)(ev.center.z-ev.radius)-1), z1=std::min(NZ-1,(int)(ev.center.z+ev.radius)+1);
        auto e2=std::chrono::high_resolution_clock::now();
        for(int z=z0;z<=z1;z++)for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)
          setVox(x,y,z,grid[gi(x,y,z)]); // re-set current value: simulates per-voxel incremental update cost
        auto e3=std::chrono::high_resolution_clock::now();
        double incMs=std::chrono::duration<double,std::milli>(e3-e2).count();
        long blastVoxels=(long)(x1-x0+1)*(y1-y0+1)*(z1-z0+1);

        printf("  (i) full rebuildAllBricks(): %.3f ms | incremental setVox over blast bbox (%ld vox): %.3f ms\n",
               fullRebuildMs, blastVoxels, incMs);

        // (ii) SuperGrid full rebuild ms
        auto e4=std::chrono::high_resolution_clock::now();
        S.super.build();
        auto e5=std::chrono::high_resolution_clock::now();
        double superRebuildMs=std::chrono::duration<double,std::milli>(e5-e4).count();
        printf("  (ii) SuperGrid full rebuild: %.3f ms\n", superRebuildMs);

        // (iii) CSDF staleness: validate + measure gather workload with PRE-edit
        // (stale) dist field, then relax progressively.
        {
          std::unique_ptr<Scene> spre(makeScene(scenes[si]));
          spre->build(); // pre-edit grid/bricks (this mutates global grid/brick!)
          CSDF staleCsdf; staleCsdf.build();
          // restore post-edit grid/bricks for tracing
          s->build(); s->applyEvent(); rebuildAllBricks();
          collectEmitters();

          // validate stale csdf on gather workload (post-edit world, stale dist)
          long staleBad=0; double tdiffmax=0;
          for(auto& r: gather){
            Hit ha,hb; bool ra=traceNaive(r.o,r.d,r.tmax,ha);
            bool rb=traceCSDF(staleCsdf,r.o,r.d,r.tmax,hb);
            float td; if(!hitEq(ra,ha,rb,hb,td)) staleBad++;
            tdiffmax=std::max(tdiffmax,(double)td);
          }
          // time stale csdf
          flushCounters(); tl_steps=0; tl_rays=0;
          auto s0=std::chrono::high_resolution_clock::now();
          for(auto& r: gather){ Hit h; traceCSDF(staleCsdf,r.o,r.d,r.tmax,h); }
          auto s1=std::chrono::high_resolution_clock::now();
          double staleNs=std::chrono::duration<double,std::nano>(s1-s0).count()/(double)gather.size();
          double staleSteps=(double)tl_steps/(double)gather.size();
          flushCounters();
          printf("  (iii) CSDF staleness (gather, pre-edit dist field): %.2f ns/ray, %.4f steps/ray, mismatches=%ld\n",
                 staleNs, staleSteps, staleBad);
          totalMismatches += staleBad;

          printf("        relax recovery curve (relaxBudget(4096) x N):\n");
          for(int N=1;N<=8;N++){
            staleCsdf.relaxBudget(4096);
            // re-validate
            long rBad=0;
            for(auto& r: gather){
              Hit ha,hb; bool ra=traceNaive(r.o,r.d,r.tmax,ha);
              bool rb=traceCSDF(staleCsdf,r.o,r.d,r.tmax,hb);
              float td; if(!hitEq(ra,ha,rb,hb,td)) rBad++;
            }
            flushCounters(); tl_steps=0; tl_rays=0;
            auto r0=std::chrono::high_resolution_clock::now();
            for(auto& r: gather){ Hit h; traceCSDF(staleCsdf,r.o,r.d,r.tmax,h); }
            auto r1=std::chrono::high_resolution_clock::now();
            double rns=std::chrono::duration<double,std::nano>(r1-r0).count()/(double)gather.size();
            double rsteps=(double)tl_steps/(double)gather.size();
            flushCounters();
            printf("          N=%d: %.2f ns/ray, %.4f steps/ray, mismatches=%ld\n",N,rns,rsteps,rBad);
            totalMismatches += rBad;
          }
        }

        // (v-HX) staleness + bounded-edit cost for HX
        {
          std::unique_ptr<Scene> spre(makeScene(scenes[si]));
          spre->build();
          HX staleHx; staleHx.build();
          ODF staleOdf; staleOdf.build();
          s->build(); s->applyEvent(); rebuildAllBricks();
          collectEmitters();
          { long odfBad=0;
            for(auto& r: gather){
              Hit ha,hb; bool ra=traceNaive(r.o,r.d,r.tmax,ha);
              bool rb=traceODF(staleOdf,r.o,r.d,r.tmax,hb);
              float td; if(!hitEq(ra,ha,rb,hb,td)) odfBad++;
            }
            auto q0=std::chrono::high_resolution_clock::now();
            staleOdf.onPlace(ev.center, ev.radius);
            auto q1=std::chrono::high_resolution_clock::now();
            printf("  (vi) ODF staleness (gather, pre-edit fields): mismatches=%ld | onPlace(r=%.0f): %.3f ms\n",
                   odfBad, ev.radius, std::chrono::duration<double,std::milli>(q1-q0).count());
            totalMismatches += odfBad; }
          long hxBad=0;
          for(auto& r: gather){
            Hit ha,hb; bool ra=traceNaive(r.o,r.d,r.tmax,ha);
            bool rb=traceHX(staleHx,r.o,r.d,r.tmax,hb);
            float td; if(!hitEq(ra,ha,rb,hb,td)) hxBad++;
          }
          flushCounters(); tl_steps=0; tl_rays=0;
          auto h0=std::chrono::high_resolution_clock::now();
          for(auto& r: gather){ Hit h2; traceHX(staleHx,r.o,r.d,r.tmax,h2); }
          auto h1=std::chrono::high_resolution_clock::now();
          double hns=std::chrono::duration<double,std::nano>(h1-h0).count()/(double)gather.size();
          double hsteps=(double)tl_steps/(double)gather.size();
          flushCounters();
          printf("  (v) HX staleness (gather, pre-edit dist fields): %.2f ns/ray, %.4f steps/ray, mismatches=%ld\n",hns,hsteps,hxBad);
          totalMismatches += hxBad;
          // bounded place-edit cost (worst case: clamp windows at both levels)
          auto p0=std::chrono::high_resolution_clock::now();
          staleHx.onPlace(ev.center, ev.radius);
          auto p1=std::chrono::high_resolution_clock::now();
          printf("      HX onPlace(r=%.0f vox): %.3f ms (bounded window, world-size independent)\n",
                 ev.radius, std::chrono::duration<double,std::milli>(p1-p0).count());
          printf("      HX relax recovery (relaxBudget(4096,64) x N):\n");
          for(int N=1;N<=4;N++){
            staleHx.relaxBudget(4096,64);
            long rBad=0;
            for(auto& r: gather){
              Hit ha,hb; bool ra=traceNaive(r.o,r.d,r.tmax,ha);
              bool rb=traceHX(staleHx,r.o,r.d,r.tmax,hb);
              float td; if(!hitEq(ra,ha,rb,hb,td)) rBad++;
            }
            flushCounters(); tl_steps=0; tl_rays=0;
            auto r0=std::chrono::high_resolution_clock::now();
            for(auto& r: gather){ Hit h2; traceHX(staleHx,r.o,r.d,r.tmax,h2); }
            auto r1=std::chrono::high_resolution_clock::now();
            double rns=std::chrono::duration<double,std::nano>(r1-r0).count()/(double)gather.size();
            flushCounters();
            printf("        N=%d: %.2f ns/ray, mismatches=%ld\n",N,rns,rBad);
            totalMismatches += rBad;
          }
        }

        // (iv) ColMap staleness: pre-edit ColMap used post-edit (intervals too wide, never wrong)
        {
          std::unique_ptr<Scene> spre(makeScene(scenes[si]));
          spre->build();
          ColMap staleCol; staleCol.build(); staleCol.buildBrickCols();
          // restore post-edit world
          s->build(); s->applyEvent(); rebuildAllBricks();
          collectEmitters();

          long colBad=0;
          for(auto& r: sunshadow){
            Hit ha,hb; bool ra=traceNaive(r.o,r.d,r.tmax,ha);
            bool rb=traceBrickCol(staleCol,r.o,r.d,r.tmax,hb);
            float td; if(!hitEq(ra,ha,rb,hb,td)) colBad++;
          }
          flushCounters(); tl_steps=0; tl_rays=0;
          auto c0=std::chrono::high_resolution_clock::now();
          for(auto& r: sunshadow){ Hit h; traceBrickCol(staleCol,r.o,r.d,r.tmax,h); }
          auto c1=std::chrono::high_resolution_clock::now();
          double cns=std::chrono::duration<double,std::nano>(c1-c0).count()/(double)sunshadow.size();
          double csteps=(double)tl_steps/(double)sunshadow.size();
          flushCounters();
          printf("  (iv) ColMap staleness (sunshadow, pre-edit ymin/ymax): %.2f ns/ray, %.4f steps/ray, mismatches=%ld\n",
                 cns, csteps, colBad);
          totalMismatches += colBad;
        }
        printf("\n");

        // restore canonical post-edit world state for any subsequent use
        s->build(); s->applyEvent(); rebuildAllBricks(); collectEmitters();
      }
    }
  }

  fclose(fcsv); fclose(fmeta);
  printf("\n==========================================\n");
  printf("TOTAL MISMATCHES: %ld\n", totalMismatches);
  printf("==========================================\n");
  return totalMismatches?1:0;
}
