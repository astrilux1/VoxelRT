// Common interface every GI technique implements. The harness owns scene state,
// the shared direct pass, references, metrics and budgets; techniques only
// estimate INDIRECT light per pixel into `out` (linear, pre-tonemap).
#pragma once
#include "core.h"
#include "scenes.h"

struct Technique {
  std::vector<V3> out;            // per-pixel indirect radiance L_ind
  virtual const char* name()=0;
  virtual void init()=0;          // scene already built, gbuffer/direct ready
  virtual void onEvent(const SceneEvent& e)=0; // world/lighting changed
  virtual long frame(long rayBudget)=0;        // do one frame of work, return rays spent
  virtual ~Technique(){}
};
