#pragma once
#include "render.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace lt2 {
// Decode the declaration once, rather than interpreting its tokens for every
// vertex. Source stride is validated once per draw, before touching the data.
struct VertexLayout {
  struct Field { unsigned source, destination, count; bool color; };
  std::vector<Field> fields;
  Vertex defaults{};
  unsigned required = 0, weights = 0;
  VertexLayout(uint32_t fvf, const std::vector<uint32_t> *declaration) {
    defaults.pos[3] = defaults.weights[0] = defaults.normal[2] = 1;
    std::fill(defaults.diffuse, defaults.diffuse + 4, 1.f);
    for (auto &uv : defaults.uv) uv[3] = 1;
    auto add = [&](unsigned destination, unsigned count, bool color = false) {
      fields.push_back({required, destination, count, color});
      required += color ? 4 : count * 4;
    };
    if (declaration) {
      for (uint32_t token : *declaration) {
        if (token == 0xffffffff) break;
        unsigned type = token >> 29;
        if (type == 1 && (token & 15))
          throw std::runtime_error("Multiple vertex streams unsupported");
        if (type != 2) continue;
        if (token & 0x10000000) { required += ((token >> 16) & 15) * 4; continue; }
        unsigned reg = token & 31, fmt = (token >> 16) & 15;
        if (fmt > 4) throw std::runtime_error("Unsupported vertex declaration format");
        unsigned destination = reg == 0 ? offsetof(Vertex, pos) :
          reg == 3 ? offsetof(Vertex, normal) : reg == 5 ? offsetof(Vertex, diffuse) :
          reg == 6 ? offsetof(Vertex, specular) :
          reg >= 7 && reg <= 10 ? offsetof(Vertex, uv) + 16 * (reg - 7) : sizeof(Vertex);
        add(destination, fmt == 4 ? 4 : fmt + 1, fmt == 4);
      }
      return;
    }
    unsigned kind = fvf & 0x00e;
    add(offsetof(Vertex, pos), kind == 4 ? 4 : 3);
    if (kind >= 6 && kind <= 12) {
      weights = (kind - 4) / 2;
      add(offsetof(Vertex, weights), weights);
    }
    if (fvf & 0x10) add(offsetof(Vertex, normal), 3);
    if (fvf & 0x20) required += 4;
    if (fvf & 0x40) add(offsetof(Vertex, diffuse), 4, true);
    if (fvf & 0x80) add(offsetof(Vertex, specular), 4, true);
    for (unsigned i = 0; i < std::min((fvf >> 8) & 15, 4u); ++i) {
      unsigned size = (fvf >> (16 + 2*i)) & 3;
      add(offsetof(Vertex, uv) + 16*i, size == 0 ? 2 : size == 1 ? 3 : size == 2 ? 4 : 1);
    }
  }
  void decode(Vertex &v, const uint8_t *source) const {
    v = defaults;
    for (const auto &f : fields) {
      if (f.destination == sizeof(Vertex)) continue;
      float *out = reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(&v) + f.destination);
      const uint8_t *in = source + f.source;
      if (f.color) {
        uint32_t c; memcpy(&c, in, 4);
        out[0] = ((c >> 16) & 255) / 255.f; out[1] = ((c >> 8) & 255) / 255.f;
        out[2] = (c & 255) / 255.f; out[3] = (c >> 24) / 255.f;
      } else {
        // Fixed sizes allow the compiler to inline these short copies.
        switch (f.count) {
        case 1: memcpy(out, in, 4); break;
        case 2: memcpy(out, in, 8); break;
        case 3: memcpy(out, in, 12); break;
        case 4: memcpy(out, in, 16); break;
        }
      }
    }
    if (weights) {
      float last = 1;
      for (unsigned i = 0; i < weights; ++i) last -= v.weights[i];
      v.weights[weights] = last;
    }
  }
};
}
