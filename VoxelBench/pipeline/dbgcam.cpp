#include "core.h"
#include "scenes.h"
#include <memory>
#include <cstdio>

int main(){
  std::unique_ptr<Scene> s(makeScene("courtyard"));
  s->build(); collectEmitters();
  s->camera();
  printf("camPos=(%.2f,%.2f,%.2f)\n",camPos.x,camPos.y,camPos.z);
  printf("at(camPos)=%d\n",(int)at((int)camPos.x,(int)camPos.y,(int)camPos.z));
  printf("NX=%d NY=%d NZ=%d\n",NX,NY,NZ);
  // check a column around camera
  for(int y=0;y<20;y++) printf("at(57,%d,8)=%d\n",y,(int)at(57,y,8));
  return 0;
}
