// Scene 5: CITY — 24m-render-distance benchmark (768x128x768, ~75M voxels,
// 48m x 6m x 48m at 6.25cm voxels). Camera sits at the central street
// intersection => >= 384 voxels (24 m) of live scene in EVERY horizontal
// direction. 6x6 grid of city blocks: hollow multi-storey buildings with
// interior emissive ceiling panels, windows/doors, parks with trees, plazas.
// Deterministic procedural layout (hash of block coords, no global RNG state).
// Event: blast carves a street-facing corner of the "hero" building adjacent
// to the camera intersection (same semantics as town's event).
#include "scenes.h"

enum {
  C_ASPHALT=1, C_SIDEWALK=2, C_BRICK=3, C_PLASTER=4, C_ROOF=5,
  C_WOOD=6, C_GRASS=7, C_CONCRETE=8, C_LIGHT=9, C_TRIM=10
};

struct SceneCity : Scene {
  SceneCity(){ name="city";
    event.geomEdit=true;
    // hero building NE corner faces the central intersection; carving exposes
    // its shaded ground-floor interior to direct sun.
    event.center=V3(409.f,9.f,409.f); event.radius=14.f;
  }

  // hollow building: brick shell, plaster interior, floor slabs, flat roof.
  void building(int x0,int z0,int x1,int z1,int y0,int yRoof,int wt,int storeyH){
    boxFill(x0,y0,z0,x1,yRoof,z1,C_BRICK);
    boxFill(x0+wt,y0+1,z0+wt,x1-wt,yRoof-1,z1-wt,C_PLASTER);
    // hollow storeys + ceiling light panels
    for(int fy=y0+1; fy+storeyH<yRoof; fy+=storeyH){
      boxFill(x0+wt,fy+1,z0+wt,x1-wt,fy+storeyH-1,z1-wt,0);
      // ceiling light panel grid (emissive, faces down into the room)
      int cy=fy+storeyH-1;
      for(int lx=x0+wt+6; lx<=x1-wt-6; lx+=24)
        for(int lz=z0+wt+6; lz<=z1-wt-6; lz+=24)
          boxFill(lx,cy,lz,std::min(lx+5,x1-wt),cy,std::min(lz+5,z1-wt),C_LIGHT);
    }
    boxFill(x0,yRoof,z0,x1,yRoof,z1,C_ROOF);
  }
  void opening(int cx,int cz,int y0,int y1,int halfW,int wt,int axis){
    if(axis==0) boxFill(cx-wt,y0,cz-halfW,cx+wt,y1,cz+halfW,0);   // E/W facade
    else        boxFill(cx-halfW,y0,cz-wt,cx+halfW,y1,cz+wt,0);   // N/S facade
  }
  void windows(int x0,int z0,int x1,int z1,int y0,int yRoof,int wt,int storeyH){
    for(int fy=y0+1; fy+storeyH<yRoof; fy+=storeyH){
      int wy0=fy+3, wy1=std::min(fy+storeyH-3, yRoof-2);
      for(int wz=z0+10; wz<=z1-10; wz+=16){           // E + W facades
        opening(x0, wz, wy0, wy1, 3, wt, 0);
        opening(x1, wz, wy0, wy1, 3, wt, 0);
      }
      for(int wx=x0+10; wx<=x1-10; wx+=16){           // N + S facades
        opening(wx, z0, wy0, wy1, 3, wt, 1);
        opening(wx, z1, wy0, wy1, 3, wt, 1);
      }
    }
  }
  void tree(int cx,int cz,int gy){
    boxFill(cx,gy,cz,cx,gy+5,cz,C_WOOD);
    boxFill(cx-2,gy+4,cz-2,cx+2,gy+8,cz+2,C_GRASS);
  }
  static uint32_t bhash(uint32_t a,uint32_t b){
    uint32_t h=a*0x9E3779B9u ^ (b+0x7F4A7C15u)*0x85EBCA6Bu;
    h^=h>>13; h*=0xC2B2AE35u; h^=h>>16; return h;
  }

  void build() override {
    setDims(768,128,768);
    for(auto& m:MAT) m={{0,0,0},{0,0,0}};
    MAT[C_ASPHALT] ={{0.13f,0.13f,0.14f},{}};
    MAT[C_SIDEWALK]={{0.45f,0.44f,0.42f},{}};
    MAT[C_BRICK]   ={{0.5f,0.22f,0.15f},{}};
    MAT[C_PLASTER] ={{0.7f,0.66f,0.58f},{}};
    MAT[C_ROOF]    ={{0.3f,0.32f,0.35f},{}};
    MAT[C_WOOD]    ={{0.4f,0.28f,0.16f},{}};
    MAT[C_GRASS]   ={{0.28f,0.45f,0.22f},{}};
    MAT[C_CONCRETE]={{0.55f,0.55f,0.55f},{}};
    MAT[C_LIGHT]   ={{0.8f,0.8f,0.75f},{18,17,14}};
    MAT[C_TRIM]    ={{0.7f,0.7f,0.7f},{}};

    SKY.enabled=true;
    SKY.sunDir=norm(V3(0.45f,0.62f,0.3f));
    SKY.sunE=V3(16,15,12.5f);
    SKY.zenith =V3(0.35f,0.55f,0.95f)*0.85f;
    SKY.horizon=V3(0.75f,0.8f,0.95f)*0.85f;

    // ground slab + default grass top layer
    boxFill(0,0,0,NX-1,1,NZ-1,C_ASPHALT);
    boxFill(0,2,0,NX-1,2,NZ-1,C_GRASS);

    // street grid: centers at 128,256,384,512,640 both axes; width 22 + 4 sw
    auto street=[&](int c){
      int lo=c-11, hi=c+10;
      boxFill(lo,2,0,hi,2,NZ-1,C_ASPHALT);
      boxFill(0,2,lo,NX-1,2,hi,C_ASPHALT);
      boxFill(lo-4,2,0,lo-1,2,NZ-1,C_SIDEWALK);
      boxFill(hi+1,2,0,hi+4,2,NZ-1,C_SIDEWALK);
      boxFill(0,2,lo-4,NX-1,2,lo-1,C_SIDEWALK);
      boxFill(0,2,hi+1,NX-1,2,hi+4,C_SIDEWALK);
    };
    for(int k=1;k<=5;k++) street(k*128);

    // blocks: 6x6, block i spans [edge_i .. edge_{i+1}] minus street margins
    // block interior bounds (after streets+sidewalks): see edges[] below.
    int lo[6]={0,144,272,400,528,656}, hi[6]={112,240,368,496,624,752};
    // clamp block 0/5 to map (block 0 starts at 2; block 5 ends NX-3)
    lo[0]=4; hi[5]=763;

    for(int bi=0;bi<6;bi++)for(int bj=0;bj<6;bj++){
      int x0=lo[bi], x1=hi[bi], z0=lo[bj], z1=hi[bj];
      // hero building gets fixed geometry (block 3,3 = NE of center crossing)
      if(bi==3&&bj==3){
        int hx0=407, hz0=407, hx1=487, hz1=487;   // corner at intersection
        building(hx0,hz0,hx1,hz1,3,42,2,13);
        windows(hx0,hz0,hx1,hz1,3,42,2,13);
        opening(hx0, hz0+24, 3,12, 5,2, 0);       // street-facing door (W)
        opening(hx0+24, hz0, 3,12, 5,2, 1);       // street-facing door (S)
        continue;
      }
      uint32_t hsh=bhash(bi,bj);
      int kind = hsh%8;            // 0,1 park; 2 plaza; else building
      int gy=3;
      if(kind<=1){                 // park: tree grid
        for(int tx=x0+10; tx<=x1-10; tx+=22)
          for(int tz=z0+10+( (tx/22)%2 )*8; tz<=z1-10; tz+=24)
            tree(tx,tz,gy);
        continue;
      }
      if(kind==2){                 // plaza: concrete pad + planters
        boxFill(x0+4,2,z0+4,x1-4,2,z1-4,C_CONCRETE);
        boxFill(x0+14,3,z0+14,x1-14,4,z0+18,C_BRICK);
        boxFill(x0+14,3,z1-18,x1-14,4,z1-14,C_BRICK);
        continue;
      }
      // building block: margins + hashed size/height
      int mx=6+(hsh>>4)%14, mz=6+(hsh>>8)%14;
      int bx0=x0+mx, bx1=x1-(6+(hsh>>12)%14);
      int bz0=z0+mz, bz1=z1-(6+(hsh>>16)%14);
      if(bx1-bx0<28||bz1-bz0<28){ bx0=x0+8; bx1=x1-8; bz0=z0+8; bz1=z1-8; }
      int hgt = 24 + (hsh>>20)%56;            // roof y in [27..82]
      int storeyH = 12 + (hsh>>26)%5;
      building(bx0,bz0,bx1,bz1,3,3+hgt,2,storeyH);
      windows(bx0,bz0,bx1,bz1,3,3+hgt,2,storeyH);
      opening(bx0,(bz0+bz1)/2, 3,11, 4,2, 0); // one door W facade
    }
    rebuildAllBricks();
  }

  void applyEvent() override {
    // blast: carve sphere at hero-building corner (LOCAL loop, not full-grid)
    V3 c=event.center; float r=event.radius;
    int x0=std::max(1,(int)(c.x-r)), x1=std::min(NX-2,(int)(c.x+r));
    int y0=std::max(1,(int)(c.y-r)), y1=std::min(NY-2,(int)(c.y+r));
    int z0=std::max(1,(int)(c.z-r)), z1=std::min(NZ-2,(int)(c.z+r));
    for(int z=z0;z<=z1;z++)for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++){
      float dx=x-c.x, dy=y-c.y, dz=z-c.z;
      if(dx*dx+dy*dy*1.2f+dz*dz < r*r) grid[gi(x,y,z)]=0;
    }
    rebuildAllBricks();
  }

  // camera at the central intersection (384,*,384): 24 m of scene in every
  // horizontal direction. Slight offset + look down the +x street toward the
  // hero building corner.
  void camera() override { setupCam(V3(376,16,378), V3(440,8,416), 72); }
};

Scene* makeCityScene(){ return new SceneCity(); }
