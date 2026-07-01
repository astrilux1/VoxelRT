// VoxelBench scenes. Each scene defines: voxel world, material palette, sky/sun,
// camera, and a mid-run "event" (destruction edit or lighting change) that tests
// dynamic response. Event regions are exposed so techniques can do localized
// invalidation (engine knows what was edited).
#pragma once
#include "core.h"

struct SceneEvent {
  bool geomEdit=false;        // voxels changed
  V3 center{}; float radius=0;// bounding sphere of the edit (voxels)
  bool lightChange=false;     // sun/sky changed
};

struct Scene {
  std::string name;
  SceneEvent event;
  virtual void build()=0;          // pre-event state
  virtual void applyEvent()=0;     // mutate to post-event state
  virtual void camera()=0;
  virtual ~Scene(){}
};

static void boxFill(int x0,int y0,int z0,int x1,int y1,int z1,uint8_t m){
  for(int z=z0;z<=z1;z++)for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++) if(inB(x,y,z)) grid[gi(x,y,z)]=m;
}
static void sphereCarve(V3 c, float r, uint8_t m, float sqx=1.f){
  for(int z=0;z<NZ;z++)for(int y=0;y<NY;y++)for(int x=0;x<NX;x++){
    float dx=x-c.x,dy=y-c.y,dz=z-c.z;
    if(dx*dx*sqx+dy*dy+dz*dz<r*r) grid[gi(x,y,z)]=m;
  }
}

// ---------------- Scene 1: BUNKER (indirect-dominated indoor) ----------------
// Two rooms joined by a doorway; the camera's room is lit only via bounce light
// from an emissive panel in the other room. Event: wall blasted open.
// mats: 1 white, 2 red, 3 green, 4 LIGHT, 5 dark gray, 6 blue
struct SceneBunker : Scene {
  SceneBunker(){ name="bunker";
    event.geomEdit=true; event.center=V3(62,36,22); event.radius=11; }
  void build() override {
    setDims(96,64,96);
    for(auto& m:MAT) m={{0,0,0},{0,0,0}};
    MAT[1]={{0.73f,0.73f,0.73f},{}};
    MAT[2]={{0.70f,0.13f,0.13f},{}};
    MAT[3]={{0.15f,0.55f,0.15f},{}};
    MAT[4]={{0.78f,0.78f,0.78f},{26,24,20}};
    MAT[5]={{0.35f,0.35f,0.38f},{}};
    MAT[6]={{0.2f,0.3f,0.65f},{}};
    SKY=SkyLight{}; // disabled
    boxFill(0,0,0,NX-1,NY-1,NZ-1,1);
    boxFill(1,1,1,NX-2,NY-2,NZ-2,0);
    boxFill(1,1,0,60,NY-2,0,2);
    boxFill(1,1,NZ-1,60,NY-2,NZ-1,3);
    boxFill(61,1,1,63,NY-2,NZ-2,1);
    boxFill(61,1,34,63,26,56,0);
    boxFill(70,NY-2,20,90,NY-2,76,4);
    boxFill(NX-1,1,1,NX-1,NY-2,NZ-2,6);
    boxFill(20,1,8,32,18,20,5);
    boxFill(38,1,60,46,30,68,1);
    boxFill(14,1,72,26,10,86,6);
    rebuildAllBricks();
  }
  void applyEvent() override {
    for(int z=1;z<NZ-1;z++)for(int y=1;y<NY-1;y++)for(int x=61;x<=63;x++){
      float dx=x-event.center.x, dy=y-event.center.y, dz=z-event.center.z;
      if(dx*dx*0.15f+dy*dy+dz*dz < event.radius*event.radius) grid[gi(x,y,z)]=0;
    }
    rebuildAllBricks();
  }
  void camera() override { setupCam(V3(7,40,46), V3(63,16,52), 72); }
};

// ---------------- Scene 2: COURTYARD (sun+sky, interior seen via openings) ---
// Walled courtyard with a small house. Camera looks into the house interior,
// lit by sun/sky through a window + door. Event: sun jumps from morning to
// late-evening angle (worst-case time-of-day step; no geometry change).
struct SceneCourtyard : Scene {
  SceneCourtyard(){ name="courtyard"; event.lightChange=true; }
  void setSun(bool post){
    SKY.enabled=true;
    SKY.zenith=V3(0.35f,0.55f,0.95f)*0.9f;
    SKY.horizon=V3(0.75f,0.8f,0.95f)*0.9f;
    if(!post){ SKY.sunDir=norm(V3(0.55f,0.72f,0.42f)); SKY.sunE=V3(17,16,13.5f); }
    else     { SKY.sunDir=norm(V3(-0.75f,0.25f,-0.3f)); SKY.sunE=V3(14,9,4.5f);  // low orange sun
               SKY.zenith=V3(0.3f,0.4f,0.7f)*0.55f; SKY.horizon=V3(0.9f,0.6f,0.45f)*0.7f; }
  }
  void build() override {
    setDims(96,64,96);
    for(auto& m:MAT) m={{0,0,0},{0,0,0}};
    MAT[1]={{0.62f,0.6f,0.55f},{}};   // stone
    MAT[2]={{0.55f,0.32f,0.2f},{}};   // wood/brick
    MAT[3]={{0.3f,0.5f,0.25f},{}};    // grass
    MAT[5]={{0.45f,0.45f,0.5f},{}};   // roof slate
    MAT[6]={{0.7f,0.65f,0.5f},{}};    // interior plaster
    MAT[7]={{0.65f,0.15f,0.12f},{}};  // red rug
    setSun(false);
    // ground
    boxFill(0,0,0,NX-1,2,NZ-1,3);
    // courtyard walls (perimeter, height 14)
    boxFill(0,3,0,NX-1,16,1,1); boxFill(0,3,NZ-2,NX-1,16,NZ-1,1);
    boxFill(0,3,0,1,16,NZ-1,1); boxFill(NX-2,3,0,NX-1,16,NZ-1,1);
    // house: shell x 40..78, z 30..70, walls h 3..22
    boxFill(40,3,30,78,22,70,2);
    boxFill(42,3,32,76,21,68,0);          // hollow interior
    boxFill(42,3,32,76,3,68,7);           // rug floor... full floor red
    boxFill(42,3,32,76,3,40,6);           // part plaster floor
    // interior walls plaster
    boxFill(42,4,32,76,21,33,6); boxFill(42,4,67,76,21,68,6);
    boxFill(42,4,32,43,21,68,6); boxFill(75,4,32,76,21,68,6);
    // roof
    boxFill(40,22,30,78,24,70,5);
    // door (south wall, z=30..31): opening
    boxFill(54,3,30,62,14,33,0);
    // window (west wall x=40..43)
    boxFill(40,9,44,43,16,58,0);
    // interior pillar + table
    boxFill(58,3,48,62,12,52,2);
    boxFill(66,3,56,72,7,62,1);
    rebuildAllBricks();
  }
  void applyEvent() override { setSun(true); }
  void camera() override { setupCam(V3(57,9,8), V3(58,8,50), 68); }
};

// ---------------- Scene 3: CAVERN (emissive HDR + destruction skylight) ------
// Enclosed cave lit by lava pools + crystal clusters. Event: ceiling collapse
// opens a shaft, flooding a sun beam into the dark cave (geometry + lighting).
struct SceneCavern : Scene {
  SceneCavern(){ name="cavern";
    event.geomEdit=true; event.center=V3(48,58,48); event.radius=12; }
  void build() override {
    setDims(96,64,96);
    for(auto& m:MAT) m={{0,0,0},{0,0,0}};
    MAT[1]={{0.4f,0.36f,0.33f},{}};       // rock
    MAT[2]={{0.55f,0.5f,0.45f},{}};       // light rock
    MAT[4]={{0.9f,0.5f,0.2f},{34,11,2}};  // lava
    MAT[5]={{0.5f,0.75f,0.9f},{2.2f,6.f,9.f}}; // crystal
    MAT[6]={{0.32f,0.3f,0.42f},{}};       // purple rock
    SKY.enabled=true;                      // only matters after collapse
    SKY.sunDir=norm(V3(0.15f,0.95f,0.1f)); SKY.sunE=V3(16,15,13);
    SKY.zenith=V3(0.4f,0.6f,0.95f)*0.8f; SKY.horizon=V3(0.8f,0.85f,0.95f)*0.8f;
    // solid rock, carve cave
    boxFill(0,0,0,NX-1,NY-1,NZ-1,1);
    sphereCarve(V3(48,22,48),26,0,1.f);
    sphereCarve(V3(26,18,30),14,0,1.f);
    sphereCarve(V3(70,20,64),15,0,1.f);
    sphereCarve(V3(48,14,20),11,0,1.f);
    // floor
    boxFill(0,0,0,NX-1,4,NZ-1,1);
    // lava pool (recessed floor patches)
    boxFill(38,4,54,52,4,68,4);
    boxFill(20,4,26,28,4,34,4);
    // crystal clusters on walls
    boxFill(64,18,38,68,26,42,5);
    boxFill(30,24,56,34,30,60,5);
    // purple rock band
    boxFill(0,28,0,NX-1,34,NZ-1,6);
    sphereCarve(V3(48,22,48),26,0,1.f); // re-carve through band
    sphereCarve(V3(26,18,30),14,0,1.f);
    sphereCarve(V3(70,20,64),15,0,1.f);
    // stalagmites
    for(int i=0;i<7;i++){
      int sx=18+i*9, sz=24+((i*37)%48);
      int hgt=6+(i*13)%10;
      boxFill(sx,5,sz,sx+2,5+hgt,sz+2,2);
    }
    rebuildAllBricks();
  }
  void applyEvent() override {
    // vertical shaft through the ceiling rock to the sky
    for(int y=20;y<NY;y++)for(int z=0;z<NZ;z++)for(int x=0;x<NX;x++){
      float dx=x-event.center.x, dz=z-event.center.z;
      if(dx*dx+dz*dz < 9.f*9.f) grid[gi(x,y,z)]=0;
    }
    rebuildAllBricks();
  }
  void camera() override { setupCam(V3(27,26,27), V3(52,12,54), 74); }
};

Scene* makeTownScene(); // scenes_town.cpp — large-scale benchmark scene

Scene* makeCityScene();
static Scene* makeScene(const std::string& n){
  if(n=="bunker") return new SceneBunker();
  if(n=="courtyard") return new SceneCourtyard();
  if(n=="cavern") return new SceneCavern();
  if(n=="town") return makeTownScene();
  if(n=="city") return makeCityScene();
  return nullptr;
}
