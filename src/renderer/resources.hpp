#pragma once
#include "render.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace lt2 {
// Asset encodings are independent of the graphics API used to upload them.
enum class ImageEncoding { Bgra8, Bgrx8, Rgb565, Argb1555, Xrgb1555, Argb4444, Alpha8, Bc1, Bc2, Bc3 };
struct ImageRegion { uint32_t left, top, right, bottom; };
struct ImageMapping { uint32_t pitch; uint8_t *data; };
class ImageStorage {
public:
  uint32_t width, height;
  ImageEncoding encoding;
  std::vector<std::vector<uint8_t>> levels;
  std::shared_ptr<Texture> gpu;
  bool dirty = true;
  static bool compressed(ImageEncoding e) {
    return e==ImageEncoding::Bc1 || e==ImageEncoding::Bc2 || e==ImageEncoding::Bc3;
  }
  static uint32_t rowBytes(uint32_t width, ImageEncoding e) {
    uint64_t bytes;
    if(compressed(e)) bytes=((uint64_t(width)+3)/4)*(e==ImageEncoding::Bc1?8:16);
    else bytes=uint64_t(width)*(e==ImageEncoding::Alpha8?1:
      e==ImageEncoding::Bgra8 || e==ImageEncoding::Bgrx8?4:2);
    if(bytes>UINT32_MAX)throw std::length_error("Image row overflow");
    return uint32_t(bytes);
  }
  ImageStorage(uint32_t w,uint32_t h,uint32_t count,ImageEncoding e):width(w),height(h),encoding(e) {
    if(!w || !h || w>16384 || h>16384)throw std::invalid_argument("Invalid image size");
    uint32_t maximum=1;
    for(uint32_t n=std::max(w,h);n>1;n>>=1)++maximum;
    if(!count)count=maximum;
    if(count>maximum)throw std::invalid_argument("Invalid mip count");
    for(uint32_t i=0;i<count;++i) {
      uint32_t x=std::max(1u,w>>i), y=std::max(1u,h>>i);
      levels.emplace_back(size_t(rowBytes(x,e))*(compressed(e)?(y+3)/4:y));
    }
  }
  bool map(uint32_t level,const ImageRegion *region,ImageMapping &out) {
    if(level>=levels.size())return false;
    uint32_t w=std::max(1u,width>>level),h=std::max(1u,height>>level);
    ImageRegion r=region?*region:ImageRegion{0,0,w,h};
    if(r.left>=r.right || r.top>=r.bottom || r.right>w || r.bottom>h)return false;
    if(compressed(encoding) && (r.left%4 || r.top%4))return false;
    uint32_t pitch=rowBytes(w,encoding);
    size_t offset=compressed(encoding)?size_t(r.top/4)*pitch+(r.left/4)*(encoding==ImageEncoding::Bc1?8:16):
      size_t(r.top)*pitch+r.left*(pitch/w);
    out={pitch,levels[level].data()+offset};dirty=true;return true;
  }
  TextureData uploadData() const {
    TextureData out{width,height,21,{}};
    if(compressed(encoding)) {
      out.format=encoding==ImageEncoding::Bc1?0x31545844:encoding==ImageEncoding::Bc2?0x33545844:0x35545844;
      out.levels=levels;return out;
    }
    for(size_t l=0;l<levels.size();++l) {
      size_t count=size_t(std::max(1u,width>>l))*std::max(1u,height>>l);
      out.levels.emplace_back(count*4);
      const auto *src=levels[l].data();auto *dst=out.levels.back().data();
      for(size_t i=0;i<count;++i) {
        uint32_t c=0;
        if(encoding==ImageEncoding::Bgra8 || encoding==ImageEncoding::Bgrx8) {
          memcpy(&c,src+i*4,4);if(encoding==ImageEncoding::Bgrx8)c|=0xff000000;
        } else if(encoding==ImageEncoding::Alpha8)c=(uint32_t(src[i])<<24)|0xffffff;
        else {
          uint16_t v;memcpy(&v,src+i*2,2);
          if(encoding==ImageEncoding::Rgb565)c=0xff000000|(((v>>11)*255/31)<<16)|((((v>>5)&63)*255/63)<<8)|((v&31)*255/31);
          else if(encoding==ImageEncoding::Argb4444)c=((v>>12)*17<<24)|(((v>>8)&15)*17<<16)|(((v>>4)&15)*17<<8)|((v&15)*17);
          else c=((encoding==ImageEncoding::Xrgb1555 || (v&0x8000))?0xff000000:0)|(((v>>10)&31)*255/31<<16)|(((v>>5)&31)*255/31<<8)|((v&31)*255/31);
        }
        memcpy(dst+i*4,&c,4);
      }
    }
    return out;
  }
  void upload(Renderer &renderer) {
    if(!dirty && gpu)return;
    gpu=renderer.upload(uploadData());dirty=false;
  }
};
class BufferStorage {
public:
  std::vector<uint8_t> bytes;
  uint64_t revision=1;
  std::shared_ptr<Geometry> resident;
  uint64_t residentRevision=0;
  explicit BufferStorage(size_t size):bytes(size) {}
  uint8_t *map(size_t offset,size_t size) {
    if(offset>bytes.size() || size>bytes.size()-offset)return nullptr;
    ++revision;return bytes.data()+offset;
  }
  void unmap() { ++revision; }
  std::shared_ptr<Geometry> upload(Renderer &renderer) {
    if(!resident || residentRevision!=revision) {
      resident=renderer.uploadGeometry(bytes.data(),bytes.size());residentRevision=revision;
    }
    return resident;
  }
};
} // namespace lt2
