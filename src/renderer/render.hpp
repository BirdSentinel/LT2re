#pragma once
// Portable rendering contract. No HWND, COM, Direct3D or SDL types here.
#include <cstdint>
#include <memory>
#include <vector>
#include <volk.h>
namespace lt2 {
struct Platform {
  virtual ~Platform() = default;
  virtual PFN_vkGetInstanceProcAddr loader() const { return nullptr; }
  virtual const char *surfaceExtension() const = 0;
  virtual VkSurfaceKHR surface(VkInstance) = 0;
  virtual void extent(uint32_t &, uint32_t &) const = 0;
};
struct Vertex {
  float pos[4], normal[4], diffuse[4], specular[4], uv[4][4], weights[4];
};
struct VertexInput {
  uint32_t stride = sizeof(Vertex);
  uint32_t offset[9] = {0,16,32,48,64,80,96,112,128};
  // 0 = constant default, 1..4 = float components, 5 = packed RGBA8.
  uint32_t format[9] = {4,4,4,4,4,4,4,4,4};
};
struct Geometry { virtual ~Geometry() = default; };
struct GeometryStream {
  VertexInput input;
  const void *data = nullptr;
  size_t bytes = 0, offset = 0;
  std::shared_ptr<Geometry> resident;
};
struct alignas(16) Uniforms {
  float world[16], view[16], projection[16], texture[4][16];
  float constants[96][4];
  float viewport[4], factor[4], ambient[4], material[4], emissive[4];
  float lightDirection[8][4], lightColor[8][4];
  int flags[4];       // transformed, lighting, fog enabled, specular enabled
  int stage[4][12];   // color op/args, alpha op/args, result, coord, transform,
                      // bound
  float fog[4];       // color rgb, density
  float fogParams[4]; // start, end, mode, alpha reference
  float blendWorld[3][16];
  float clipAdjust[4];
  int fragment[4]; // alpha test, compare, fog mode, unused
  int vertexMeta[4]; // packed diffuse/specular mask, implicit blend weight count
};
struct Raster {
  uint32_t topology = 4, cull = 3, ztest = 1, zwrite = 1, zfunc = 4;
  uint32_t blend = 0, src = 2, dst = 1, blendop = 1, colorMask = 15;
  uint32_t stencil = 0, fail = 1, zfail = 1, pass = 1, func = 8, ref = 0,
           readMask = 255, writeMask = 255;
  uint32_t shader = 0, bias = 0;
  VertexInput input;
  bool operator<(const Raster &b) const;
};
struct Sampler {
  uint32_t u = 1, v = 1, min = 2, mag = 2, mip = 2;
  float bias = 0;
  uint32_t maxMip = 0;
};
struct TextureData {
  uint32_t width, height, format;
  std::vector<std::vector<uint8_t>> levels;
};
struct Texture {
  virtual ~Texture() = default;
};
class Renderer {
  struct Impl;
  std::unique_ptr<Impl> p;

public:
  Renderer(Platform &);
  ~Renderer();
  std::shared_ptr<Texture> upload(const TextureData &);
  std::shared_ptr<Geometry> uploadGeometry(const void *, size_t);
  void clear(uint32_t flags, uint32_t color, float depth, uint32_t stencil);
  void draw(const Raster &, const Uniforms &, const Vertex *, uint32_t,
            const uint32_t *, uint32_t, const std::shared_ptr<Texture> *,
            const Sampler *, const float *viewport, const GeometryStream * = nullptr);
  void present();
  void reset();
};
} // namespace lt2
