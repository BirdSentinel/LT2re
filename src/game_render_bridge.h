#pragma once
int lt2_submit_game_draw(unsigned kind);
#ifdef LT2_NATIVE_SCENE
#include "renderer/game_scene_commands.h"
void lt2_game_scene(unsigned command, unsigned arguments, unsigned site);
void lt2_native_draw(unsigned kind);
#define LT2_GAME_SCENE(command, arguments, site) do { \
  RECOMP_REGS_OUT(); lt2_game_scene(command, arguments, site); RECOMP_REGS_IN(); \
} while(0)
#define LT2_GAME_DRAW(kind, target) do { \
  RECOMP_REGS_OUT(); lt2_native_draw(kind); RECOMP_REGS_IN(); \
} while(0)
#else
// Called at audited game draw sites. State synchronization is identical to a
// lifted call, but the direct renderer does not need a native x86 COM thunk.
#define LT2_GAME_DRAW(kind, target) do { \
  RECOMP_REGS_OUT(); \
  int _direct = lt2_submit_game_draw(kind); \
  RECOMP_REGS_IN(); \
  if (!_direct) { RECOMP_ICALL(target); } \
} while (0)
#endif
