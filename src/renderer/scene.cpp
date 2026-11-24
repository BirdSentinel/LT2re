#include "scene.hpp"
#include "performance.hpp"
#include <climits>
#include <cfloat>
#include <cmath>
#include "../display_layout.h"
namespace lt2 {
static void unit(float *m) { memset(m,0,64);m[0]=m[5]=m[10]=m[15]=1; }
SceneRenderer::SceneRenderer(Platform &p,uint32_t w,uint32_t h):platform(p),renderer(p),width(w),height(h) {
  if(!width || !height)throw std::invalid_argument("Invalid scene size");
  platform.extent(clientWidth,clientHeight);
  unit(uniforms.world);unit(uniforms.view);unit(uniforms.projection);
  for(auto &m:uniforms.texture)unit(m);
  for(auto &m:uniforms.blendWorld)unit(m);
  uniforms.viewport[2]=float(w);uniforms.viewport[3]=float(h);
  std::fill(std::begin(uniforms.factor),std::end(uniforms.factor),1.f);
  std::fill(std::begin(uniforms.material),std::end(uniforms.material),1.f);
  uniforms.flags[1]=1;uniforms.fragment[1]=8;
  raster.readMask=raster.writeMask=~0u;
  for(unsigned i=0;i<4;++i) {
    int *s=uniforms.stage[i];
    s[0]=i?1:4;s[1]=2;s[2]=1;s[3]=i?1:2;s[4]=2;s[5]=1;s[6]=1;s[7]=i;
    samplers[i].min=samplers[i].mag=1;samplers[i].mip=0;
  }
}
void SceneRenderer::resize(uint32_t w,uint32_t h) {
  if(!w || !h)throw std::invalid_argument("Invalid scene size");
  // These are the game's logical UI/projection dimensions, never a requested
  // swapchain resolution. Vulkan independently follows the drawable extent.
  width=w;height=h;
  uniforms.viewport[0]=uniforms.viewport[1]=0;
  uniforms.viewport[2]=float(w);uniforms.viewport[3]=float(h);
  depthMin=0;depthMax=1;
}
void SceneRenderer::present() { renderer.present();extentDirty=true;worldDrawn=false; }
void SceneRenderer::draw(const LT2DrawPacket &packet) {
  ++performance.directDraws;
  const uint8_t *data;
  uint32_t inputStride;
  if(packet.userMemory) { data=static_cast<const uint8_t*>(packet.vertices);inputStride=packet.stride; }
  else {
    if(!vertices)throw std::invalid_argument("Missing vertex buffer");
    data=vertices->bytes.data();inputStride=stride;
    uint64_t end=uint64_t(packet.firstVertex)+packet.vertexCount+(packet.indexed?indexBase:0);
    if(end*inputStride>vertices->bytes.size())throw std::out_of_range("Vertex range");
  }
  if(!data || !inputStride)throw std::invalid_argument("Empty vertex stream");
  if(!packet.indexed) {
    submit(packet.topology,data,inputStride,packet.firstVertex,packet.vertexCount,nullptr,0);
    if(packet.userMemory)vertices.reset();
    return;
  }
  if(packet.userMemory || !indices)throw std::invalid_argument("Missing index buffer");
  uint32_t bytes=indices->indexBytes;
  if((bytes!=2 && bytes!=4) || (uint64_t(packet.firstIndex)+packet.indexCount)*bytes>indices->bytes.size())
    throw std::out_of_range("Index range");
  if(!packet.indexCount)return;
  scratchIndices.resize(packet.indexCount);
  uint32_t lo=UINT_MAX,hi=0;
  const uint8_t *source=indices->bytes.data()+size_t(packet.firstIndex)*bytes;
  for(uint32_t i=0;i<packet.indexCount;++i) {
    uint32_t x=0;memcpy(&x,source+size_t(i)*bytes,bytes);
    if(x<packet.firstVertex || uint64_t(x)>=uint64_t(packet.firstVertex)+packet.vertexCount)
      throw std::out_of_range("Index references vertex outside draw");
    scratchIndices[i]=x;lo=std::min(lo,x);hi=std::max(hi,x);
  }
  for(auto &x:scratchIndices)x-=lo;
  submit(packet.topology,data,inputStride,indexBase+lo,hi-lo+1,scratchIndices.data(),packet.indexCount);
}
void SceneRenderer::submit(uint32_t topology,const uint8_t *data,uint32_t inputStride,uint32_t first,uint32_t count,const uint32_t *indexData,uint32_t indexCount) {
  PerfScope timing(performance.adapter);
  if(!count)return;
  // Refresh before drawing, including the first frame after a resize. Querying
  // only after Present applies the old aspect ratio to that frame.
  if(extentDirty) { platform.extent(clientWidth,clientHeight);extentDirty=false; }
  performance.vertices+=count;
  if(topology<1 || topology>6)throw std::invalid_argument("Invalid topology");
  auto found=layouts.find(vertexFormat);
  auto program=programs.find(vertexFormat);
  if(found==layouts.end())found=layouts.emplace(vertexFormat,VertexLayout(vertexFormat,program==programs.end()?nullptr:&program->second.declaration)).first;
  const auto &layout=found->second;
  if(layout.required>inputStride || layout.weights>3)throw std::invalid_argument("Invalid vertex layout");
  GeometryStream stream;
  stream.input.stride=inputStride;
  std::fill(std::begin(stream.input.format),std::end(stream.input.format),0u);
  for(const auto &field:layout.fields)if(field.destination<sizeof(Vertex)) {
    uint32_t location=field.destination/16;
    stream.input.offset[location]=field.source;stream.input.format[location]=field.color?5:field.count;
  }
  stream.data=data+size_t(first)*inputStride;stream.bytes=size_t(count)*inputStride;
  if(vertices && data==vertices->bytes.data() && !vertices->dynamic) {
    stream.resident=vertices->upload(renderer);stream.offset=size_t(first)*inputStride;
  }
  Raster r=raster;r.input=stream.input;r.topology=topology;r.shader=program==programs.end()?0:program->second.shader;
  Uniforms u=uniforms;
  u.vertexMeta[0]=(stream.input.format[2]==5?1:0)|(stream.input.format[3]==5?2:0);
  u.vertexMeta[1]=layout.weights;u.vertexMeta[2]=0;
  for(unsigned i=0;i<9;++i)u.vertexMeta[2]|=std::min(stream.input.format[i],4u)<<(i*3);
  u.flags[0]=!r.shader && (vertexFormat&0xe)==4;
  const bool previewViewport=uniforms.viewport[0]!=0 || uniforms.viewport[1]!=0 ||
      uniforms.viewport[2]<float(width) || uniforms.viewport[3]<float(height);
  // Full-screen fades use device coordinates, unlike authored menu sprites.
  // Detect the complete rectangle before applying HUD anchor/scale rules.
  float bounds[]={FLT_MAX,FLT_MAX,-FLT_MAX,-FLT_MAX};
  if(u.flags[0] && (count==4 || count==6))
    for(uint32_t i=0;i<count;++i) {
      float xy[2];memcpy(xy,data+size_t(first+i)*inputStride,8);
      bounds[0]=std::min(bounds[0],xy[0]);bounds[1]=std::min(bounds[1],xy[1]);
      bounds[2]=std::max(bounds[2],xy[0]);bounds[3]=std::max(bounds[3],xy[1]);
    }
  bool fullOverlay=u.flags[0] && !previewViewport &&
      fabsf(bounds[0])<=1 && fabsf(bounds[1])<=1 &&
      ((fabsf(bounds[2]-width)<=1 && fabsf(bounds[3]-height)<=1) ||
       (fabsf(bounds[2]-800)<=1 && fabsf(bounds[3]-600)<=1));
  static int layoutTrace=std::getenv("LT2_TRACE_UI_LAYOUT")?100:0;
  static bool previewTrace=false;
  if(previewViewport && !previewTrace && std::getenv("LT2_TRACE_UI_LAYOUT")) {layoutTrace=100;previewTrace=true;}
  if(layoutTrace>0) {
    --layoutTrace;float b[]={FLT_MAX,FLT_MAX,-FLT_MAX,-FLT_MAX};
    for(uint32_t i=0;i<count;++i){float xy[2];memcpy(xy,data+size_t(first+i)*inputStride,8);b[0]=std::min(b[0],xy[0]);b[1]=std::min(b[1],xy[1]);b[2]=std::max(b[2],xy[0]);b[3]=std::max(b[3],xy[1]);}
    fprintf(stderr,"[UI layout] logical=%ux%u viewport=%.0f,%.0f,%.0f,%.0f xy=%.1f,%.1f,%.1f,%.1f fvf=%x count=%u image=%ux%u world=%d\n",width,height,u.viewport[0],u.viewport[1],u.viewport[2],u.viewport[3],b[0],b[1],b[2],b[3],vertexFormat,count,images[0]?images[0]->width:0,images[0]?images[0]->height:0,int(worldDrawn));
  }
  // Perspective is already adjusted in the game's camera, before CPU culling.
  u.clipAdjust[0]=1.f;
  u.clipAdjust[1]=u.clipAdjust[2]=1.f;u.clipAdjust[3]=0.f;
  // A car/driver preview is not the race: subsequent menu widgets must keep
  // the same authored canvas as the frames drawn before the preview.
  if(!u.flags[0] && !previewViewport)worldDrawn=true;
  else if(clientWidth && clientHeight) {
    float anchorX=.5f,anchorY=.5f;
    float layoutWidth=float(width),layoutHeight=float(height);
    bool fillWindow=false;
    if(!worldDrawn) {
      // Menu animation coordinates are authored at 800x600, independently of
      // the game's device dimensions. Background vertices already cover the
      // device viewport; the two animated strips are authored at 800x600 too.
      bool background=count==4 && images[0] && images[0]->width==1024 && images[0]->height==1024;
      // Preserve the original 800x600 menu composition, including the baked-in
      // panel frames: every menu element uses the same full-window stretch.
      fillWindow=true;
      if(!background) {
        layoutWidth=800;layoutHeight=600;
        u.viewport[0]=u.viewport[1]=0;u.viewport[2]=layoutWidth;u.viewport[3]=layoutHeight;
      }
      anchorX=1.f;
    }
    if(worldDrawn) {
      // Anchor each HUD batch as one unit, never its vertices separately.
      // This preserves circles, text and widget borders when the aspect changes.
      float left=FLT_MAX,top=FLT_MAX,right=-FLT_MAX,bottom=-FLT_MAX;
      for(uint32_t i=0;i<count;++i) {
        float xy[2];memcpy(xy,data+size_t(first+i)*inputStride,8);
        left=std::min(left,xy[0]);right=std::max(right,xy[0]);
        top=std::min(top,xy[1]);bottom=std::max(bottom,xy[1]);
      }
      float x=(left+right)*.5f/width,y=(top+bottom)*.5f/height;
      anchorX=x<.4f?0.f:x>.6f?1.f:.5f;
      anchorY=y<.4f?0.f:y>.6f?1.f:.5f;
    }
    auto fit=lt2_ui_layout(float(clientWidth),float(clientHeight),layoutWidth,layoutHeight,anchorX,anchorY);
    u.clipAdjust[1]=fillWindow?1.f:fit.scale*layoutWidth/clientWidth;
    u.clipAdjust[2]=fillWindow?1.f:fit.scale*layoutHeight/clientHeight;
    // Two anchor coordinates packed separately in spare vertex metadata/flags.
    u.clipAdjust[3]=anchorX;
    u.vertexMeta[3]=int(anchorY*2);
  }
  if(fullOverlay) {
    u.viewport[0]=bounds[0]+.5f;u.viewport[1]=bounds[1]+.5f;
    u.viewport[2]=bounds[2]-bounds[0];u.viewport[3]=bounds[3]-bounds[1];
    u.clipAdjust[1]=u.clipAdjust[2]=1.f;
  }
  for(unsigned i=0;i<8;++i) {
    const auto &l=lights[i];
    memset(u.lightDirection[i],0,16);memset(u.lightColor[i],0,16);
    if(l.enabled) {
      for(unsigned j=0;j<3;++j)u.lightDirection[i][j]=l.direction[0]*u.view[j]+l.direction[1]*u.view[4+j]+l.direction[2]*u.view[8+j];
      memcpy(u.lightColor[i],l.color,16);
    }
  }
  std::shared_ptr<Texture> textures[4];
  for(unsigned i=0;i<4;++i) {
    u.stage[i][9]=bool(images[i]);
    if(images[i]) { images[i]->upload(renderer);textures[i]=images[i]->gpu; }
  }
  // GPU viewport is independent of the menu's authored coordinate canvas.
  float viewport[]={uniforms.viewport[0]/width,uniforms.viewport[1]/height,uniforms.viewport[2]/width,uniforms.viewport[3]/height,depthMin,depthMax};
  if(previewViewport && !worldDrawn && clientWidth && clientHeight) {
    viewport[0]=uniforms.viewport[0]/800.f;
    viewport[1]=uniforms.viewport[1]/600.f;
    viewport[2]=uniforms.viewport[2]/800.f;
    viewport[3]=uniforms.viewport[3]/600.f;
    // Screen-space geometry in a preview is already local to that viewport.
    if(u.flags[0]) {
      memcpy(u.viewport,uniforms.viewport,sizeof u.viewport);
      u.clipAdjust[1]=u.clipAdjust[2]=1;
    }
  }
  renderer.draw(r,u,nullptr,count,indexData,indexCount,textures,samplers.data(),viewport,&stream);
}
}
