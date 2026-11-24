#pragma once
#include "resources.hpp"
#include "vertex_layout.hpp"
#include "game_render_api.h"
#include <array>
#include <map>
namespace lt2 {
struct SceneLight { float color[4]{}, direction[3]{}; bool enabled=false; };
struct SceneBuffer : BufferStorage {
  bool dynamic=false;
  uint32_t indexBytes=0;
  explicit SceneBuffer(size_t size):BufferStorage(size) {}
};
struct SceneProgram { uint32_t shader; std::vector<uint32_t> declaration; };
// Native scene state. The game port supplies uniforms and raster settings
// directly; this class has no COM objects, API state numbers or import shims.
class SceneRenderer {
public:
  Platform &platform;
  Renderer renderer;
  Raster raster;
  Uniforms uniforms{};
  std::array<Sampler,4> samplers;
  std::array<std::shared_ptr<ImageStorage>,4> images;
  std::shared_ptr<SceneBuffer> vertices, indices;
  std::array<SceneLight,8> lights;
  uint32_t width,height,clientWidth=0,clientHeight=0;
  uint32_t vertexFormat=0,stride=0,indexBase=0;
  float depthMin=0,depthMax=1;
  std::map<uint32_t,SceneProgram> programs;
  std::map<uint32_t,VertexLayout> layouts;
  std::vector<uint32_t> scratchIndices;
  explicit SceneRenderer(Platform &,uint32_t,uint32_t);
  void resize(uint32_t,uint32_t);
  void present();
  void draw(const LT2DrawPacket &);
private:
  bool extentDirty=true;
  bool worldDrawn=false;
  void submit(uint32_t,const uint8_t *,uint32_t,uint32_t,uint32_t,const uint32_t *,uint32_t);
};
}
