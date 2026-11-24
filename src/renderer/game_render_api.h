#pragma once
#include <stdint.h>
// Versioned native geometry submission. No COM or D3D types cross this boundary.
#define LT2_RENDER_API_VERSION 1
typedef struct LT2DrawPacket {
  uint32_t version;
  void *context;
  uint32_t topology; // 1 points, 2 lines, 3 line strip, 4 triangles, 5 strip, 6 fan
  uint32_t indexed, userMemory;
  uint32_t firstVertex, vertexCount, firstIndex, indexCount;
  const void *vertices, *indices;
  uint32_t stride, indexBytes;
} LT2DrawPacket;
typedef int (*LT2SubmitDrawFn)(const LT2DrawPacket *, int32_t *result);
