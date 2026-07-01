// Scene 4: TOWN — headline realistic-scale benchmark (320x96x320, ~10M voxels,
// 1 voxel = 6.25cm => a 20m x 6m x 20m city block). Two crossing streets divide
// the map into four quadrants, each with brick buildings (hollow, 2-story
// interiors, windows/doors), a tower, a small park, and interior emissive
// lighting. Event: a blast carves a hole in a street-facing building corner,
// exposing its shadowed interior to direct sunlight.
#include "scenes.h"

// materials (indices into MAT[])
enum {
  M_ASPHALT=1, M_SIDEWALK=2, M_BRICK=3, M_PLASTER=4, M_ROOF=5,
  M_WOOD=6, M_GRASS=7, M_TRIM=8, M_LIGHT=9
};

struct SceneTown : Scene {
  SceneTown(){ name="town";
    event.geomEdit=true;
    // Building A's SE corner: street-facing (sunlit south facade), at
    // street level, within camera view. Carving here exposes the
    // (currently sun-shaded-from-this-side) ground-floor interior to
    // direct sunlight after the blast.
    event.center=V3(110.f, 9.f, 122.f); event.radius=13.f;
  }

  // ---- helpers -------------------------------------------------------------
  // hollow rectangular building: brick shell (wall thickness wt), plaster
  // interior, optional floor slab, flat roof.
  void building(int x0,int z0,int x1,int z1,int y0,int yRoofTop,int wt,
                 bool floorSlab,int floorY, bool tower=false){
    uint8_t wallMat = M_BRICK;
    // shell
    boxFill(x0,y0,z0,x1,yRoofTop,z1,wallMat);
    // hollow interior
    int ix0=x0+wt, iz0=z0+wt, ix1=x1-wt, iz1=z1-wt;
    int iy1 = yRoofTop-1; // leave roof slab on top
    boxFill(ix0,y0+1,iz0,ix1,iy1,iz1,M_PLASTER);
    if(floorSlab){
      boxFill(ix0,floorY,iz0,ix1,floorY+1,iz1,M_PLASTER);
    }
    // flat roof cap (top wt-thickness becomes roof material)
    boxFill(x0,yRoofTop,z0,x1,yRoofTop,z1,M_ROOF);
    (void)tower;
  }

  // carve an opening (window/door) into a wall face. axis: 0 = wall runs along
  // Z (opening cut in X-thin wall, i.e. east/west facade), 1 = wall runs along
  // X (opening cut in Z-thin wall, north/south facade).
  void opening(int cx,int cz,int y0,int y1,int halfW,int wt,int axis){
    if(axis==0){ // east/west facade: thin in X, wide in Z
      boxFill(cx-wt,y0,cz-halfW,cx+wt,y1,cz+halfW,0);
    } else {     // north/south facade: thin in Z, wide in X
      boxFill(cx-halfW,y0,cz-wt,cx+halfW,y1,cz+wt,0);
    }
  }

  void tree(int cx,int cz,int groundY){
    // trunk
    boxFill(cx,groundY,cz,cx,groundY+5,cz,M_WOOD);
    // canopy (cubic blob, grass-colored per spec)
    boxFill(cx-2,groundY+4,cz-2,cx+2,groundY+8,cz+2,M_GRASS);
  }

  void planter(int x0,int z0,int x1,int z1,int groundY){
    boxFill(x0,groundY,z0,x1,groundY+1,z1,M_BRICK);
    boxFill(x0+1,groundY+1,z0+1,x1-1,groundY+2,z1-1,M_GRASS);
  }

  void build() override {
    setDims(320,96,320);
    for(auto& m:MAT) m={{0,0,0},{0,0,0}};
    MAT[M_ASPHALT] ={{0.13f,0.13f,0.14f},{}};
    MAT[M_SIDEWALK]={{0.45f,0.44f,0.42f},{}};
    MAT[M_BRICK]   ={{0.5f,0.22f,0.15f},{}};
    MAT[M_PLASTER] ={{0.7f,0.66f,0.58f},{}};
    MAT[M_ROOF]    ={{0.3f,0.32f,0.35f},{}};
    MAT[M_WOOD]    ={{0.4f,0.28f,0.16f},{}};
    MAT[M_GRASS]   ={{0.28f,0.45f,0.22f},{}};
    MAT[M_TRIM]    ={{0.7f,0.7f,0.7f},{}};
    MAT[M_LIGHT]   ={{0.8f,0.8f,0.75f},{18,17,14}};

    SKY.enabled=true;
    SKY.sunDir=norm(V3(0.45f,0.62f,0.3f));
    SKY.sunE=V3(16,15,12.5f);
    SKY.zenith =V3(0.35f,0.55f,0.95f)*0.85f;
    SKY.horizon=V3(0.75f,0.8f,0.95f)*0.85f;

    // ---- ground slab ----------------------------------------------------
    boxFill(0,0,0,NX-1,1,NZ-1,M_ASPHALT);
    // top layer default = grass (parks dominate edges); streets/sidewalks
    // overwritten below.
    boxFill(0,2,0,NX-1,2,NZ-1,M_GRASS);

    // ---- cross streets (24 voxels wide), centered at 160 ----------------
    int sLo=148, sHi=171; // inclusive range, width 24
    boxFill(sLo,2,0,sHi,2,NZ-1,M_ASPHALT);   // N-S street (varies in x)
    boxFill(0,2,sLo,NX-1,2,sHi,M_ASPHALT);   // E-W street (varies in z)
    // sidewalks: 4-voxel bands flanking the streets
    int swW=4;
    boxFill(sLo-swW,2,0,sLo-1,2,NZ-1,M_SIDEWALK);
    boxFill(sHi+1,2,0,sHi+swW,2,NZ-1,M_SIDEWALK);
    boxFill(0,2,sLo-swW,NX-1,2,sLo-1,M_SIDEWALK);
    boxFill(0,2,sHi+1,NX-1,2,sHi+swW,M_SIDEWALK);

    // ============================================================
    // NW quadrant (x: 2..146, z: 2..146): Building A (large, GI focus)
    // ============================================================
    // Footprint x:14..127 (114 wide), z:18..123 (106 deep). Walls 2 thick.
    int Ax0=14, Az0=18, Ax1=127, Az1=123;
    int Ay0=3, Afloor=20, Aroof=40;
    building(Ax0,Az0,Ax1,Az1,Ay0,Aroof,2,true,Afloor);
    // Door opening on the EAST facade (faces the N-S street, sunlit side,
    // x normal = +x, sun has +x component).
    opening(Ax1,Az1-30, Ay0,Ay0+9, 5,2, 0);  // wide door, ground floor
    // Window rows on EAST facade (sunlit), both floors
    for(int wz=Az0+10; wz<Az1-10; wz+=18){
      opening(Ax1, wz, Ay0+2, Ay0+11, 4,2, 0);          // ground floor
      opening(Ax1, wz, Afloor+2, Afloor+11, 4,2, 0);    // 2nd floor
    }
    // SOUTH facade faces the E-W street (also sunlit, +z component): windows
    for(int wx=Ax0+12; wx<Ax1-12; wx+=20){
      opening(wx, Az1, Ay0+2, Ay0+11, 4,2, 1);
      opening(wx, Az1, Afloor+2, Afloor+11, 4,2, 1);
    }
    // WEST facade (shaded, -x normal): a single window row + the GI test
    // interior is illuminated through here only by sun-shading + emissive.
    for(int wz=Az0+14; wz<Az1-14; wz+=22){
      opening(Ax0, wz, Ay0+2, Ay0+11, 4,2, 0);
      opening(Ax0, wz, Afloor+2, Afloor+11, 4,2, 0);
    }
    // NORTH facade (shaded, -z normal): windows
    for(int wx=Ax0+12; wx<Ax1-12; wx+=20){
      opening(wx, Az0, Ay0+2, Ay0+11, 4,2, 1);
      opening(wx, Az0, Afloor+2, Afloor+11, 4,2, 1);
    }
    // Alley notch: a 6-voxel-wide recess cut into the south facade (between
    // two window groups), full ground-floor height. Its two side walls face
    // +x (sunlit) and -x (shaded) -- an occlusion-contrast pair visible
    // straight-on from the street.
    boxFill(94,Ay0,Az1-12,99,Ay0+12,Az1-2, 0);

    // interior emissive ceiling light strips (ground floor, under floor slab)
    boxFill(Ax0+10,Afloor-1,Az0+10,Ax0+10+16,Afloor-1,Az0+10, M_LIGHT);
    boxFill(Ax1-26,Afloor-1,Az1-10,Ax1-10,Afloor-1,Az1-10, M_LIGHT);
    // 2nd floor ceiling light strip
    boxFill(Ax0+20,Aroof-1,Az0+20,Ax0+36,Aroof-1,Az0+20, M_LIGHT);

    // Small secondary building A2, west of A, separated by a 6-voxel alley
    // (gap along x). A's west wall (shaded) and A2's east wall (sunlit) form
    // an occlusion-contrast alley visible from the street.
    int A2x1=Ax0-6, A2x0=A2x1-34, A2z0=Az0+10, A2z1=Az1-10;
    building(A2x0,A2z0,A2x1,A2z1,Ay0,32,2,false,0);
    // door facing the street (east wall of A2, opens toward the alley/street)
    opening(A2x1, A2z1-14, Ay0,Ay0+9, 4,2, 0);

    // ============================================================
    // SW quadrant (x: 2..146, z: 174..318): grass park + planters
    // ============================================================
    boxFill(2,2,174,146,2,318, M_GRASS);
    // a paved path through the park
    boxFill(2,2,236,146,2,242, M_SIDEWALK);
    // trees scattered
    tree(30,200,3); tree(60,210,3); tree(95,260,3); tree(40,290,3);
    tree(120,230,3); tree(70,300,3);
    // planters near the street edge
    planter(20,178,28,186,2);
    planter(40,178,48,186,2);
    planter(60,178,68,186,2);

    // ============================================================
    // NE quadrant (x: 174..318, z: 2..146): mid building + TOWER
    // ============================================================
    // Mid building (B) near the street corner, sunlit facades face the street
    int Bx0=180, Bz0=20, Bx1=265, Bz1=110;
    building(Bx0,Bz0,Bx1,Bz1,3,38,2,true,20);
    opening(Bx0, Bz1-20, 3+0,3+9, 5,2, 0);      // door on west facade (faces N-S street)
    for(int wz=Bz0+10; wz<Bz1-10; wz+=18){
      opening(Bx0, wz, 3+2,3+11, 4,2, 0);
      opening(Bx0, wz, 20+2,20+11, 4,2, 0);
    }
    for(int wx=Bx0+12; wx<Bx1-12; wx+=20){
      opening(wx, Bz1, 3+2,3+11, 4,2, 1);
    }
    boxFill(Bx0+10,20-1,Bz0+10,Bx0+30,20-1,Bz0+10, M_LIGHT);

    // TOWER ~56x56 footprint, up to y=90, set back from the street so it
    // looms in the background down the N-S street.
    int Tx0=210, Tz0=140, Tx1=266, Tz1=196; // 56x56
    building(Tx0,Tz0,Tx1,Tz1,3,90,2,true,20);
    // additional internal floor slabs every ~20 voxels for visual interest
    boxFill(Tx0+2,40,Tz0+2,Tx1-2,41,Tz1-2, M_PLASTER);
    boxFill(Tx0+2,60,Tz0+2,Tx1-2,61,Tz1-2, M_PLASTER);
    // door at street level (west face, toward N-S street)
    opening(Tx0, Tz0+28, 3,3+9, 5,2, 0);
    // window rows up the tower
    for(int fy=8; fy<88; fy+=14){
      opening(Tx0, Tz0+15, fy,fy+8, 4,2, 0);
      opening(Tx0, Tz0+40, fy,fy+8, 4,2, 0);
    }

    // ============================================================
    // SE quadrant (x: 174..318, z: 174..318): another building
    // ============================================================
    int Cx0=185, Cz0=185, Cx1=300, Cz1=290;
    building(Cx0,Cz0,Cx1,Cz1,3,42,2,true,20);
    opening(Cx0, Cz0+20, 3,3+9, 5,2, 0);   // door, west facade (faces N-S street)
    for(int wz=Cz0+12; wz<Cz1-12; wz+=18){
      opening(Cx0, wz, 3+2,3+11, 4,2, 0);
      opening(Cx0, wz, 20+2,20+11, 4,2, 0);
    }
    for(int wx=Cx0+12; wx<Cx1-12; wx+=20){
      opening(wx, Cz0, 3+2,3+11, 4,2, 1);
      opening(wx, Cz0, 20+2,20+11, 4,2, 1);
    }
    boxFill(Cx0+10,20-1,Cz0+10,Cx0+30,20-1,Cz0+10, M_LIGHT);

    // ---- 1-voxel-thick wall section (leak test), visible from street -----
    // A thin partition wall inside Building A near its east (street-facing)
    // window row: only 1 voxel thick instead of the normal 2.
    boxFill(Ax1-1,Ay0+1,Az0+30,Ax1-1,Ay0+8,Az0+34, M_BRICK);

    // white trim accents around Building A's main door
    boxFill(Ax1,Ay0+9,Az1-31,Ax1,Ay0+9,Az1-29, M_TRIM);
    boxFill(Ax1,Ay0,Az1-31,Ax1,Ay0+9,Az1-31, M_TRIM);
    boxFill(Ax1,Ay0,Az1-29,Ax1,Ay0+9,Az1-29, M_TRIM);

    rebuildAllBricks();
  }

  void applyEvent() override {
    // Blast sphere at a visible building corner (Building A, SE corner facing
    // the street) -- carve grid, exposing the shaded interior to direct
    // sunlight after the blast.
    sphereCarve(event.center, event.radius, 0, 1.f);
    rebuildAllBricks();
  }

  void camera() override {
    setupCam(V3(38,14,150), V3(180,12,170), 70);
  }
};

Scene* makeTownScene(){ return new SceneTown(); }
