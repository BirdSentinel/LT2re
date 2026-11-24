// Port of the game's graphics entry points to the native scene API.
// The uint32_t records are the original game's memory layout, not graphics interfaces.
#define NOMINMAX
#include "scene.hpp"
#include "platform_win32.hpp"
#include "game_scene_commands.h"
#include "shaders.hpp"
#include <set>
#include <cstdio>
using namespace lt2;
namespace {
constexpr uint32_t bad=0x8876086c, unavailable=0x8876086a;
BOOL CALLBACK findGameWindow(HWND window,LPARAM output) {
 if(!GetPropW(window,L"LT2.SDLWindow"))return TRUE;
 *reinterpret_cast<HWND*>(output)=window;return FALSE;
}
void windowMode(uint32_t *mode) {
 HWND window=nullptr;EnumThreadWindows(GetCurrentThreadId(),findGameWindow,reinterpret_cast<LPARAM>(&window));
 RECT client{};
 mode[0]=uint32_t(GetSystemMetrics(SM_CXSCREEN));mode[1]=uint32_t(GetSystemMetrics(SM_CYSCREEN));
 if(window && GetClientRect(window,&client) && client.right>0 && client.bottom>0) {
  mode[0]=uint32_t(client.right);mode[1]=uint32_t(client.bottom);
 }
 mode[2]=60;mode[3]=22;
}
struct Object;
struct Context {
  Win32Platform platform;
  SceneRenderer scene;
  uint32_t presentation[13]{}, creation[4]{};
  Object *factory=nullptr,*vertex=nullptr,*index=nullptr;
  Object *textures[4]{};
  float material[17]{};
  float lightRecords[8][26]{};
  uint32_t tableFog=0,vertexFog=0,nextProgram=0x80000001;
  Context(HWND window,uint32_t w,uint32_t h):platform(window),scene(platform,w,h) {}
  ~Context();
};
enum class Kind { Factory, Context, Image, Surface, Vertices, Indices };
struct Object {
  uint32_t reserved=0; // The lifted code may read this word before a removed indirect call.
  Kind kind;
  uint32_t refs=1,format=0,usage=0,pool=0,layout=0,level=0,lod=0,priority=0;
  Object *device=nullptr,*owner=nullptr;
  std::shared_ptr<ImageStorage> image;
  std::shared_ptr<SceneBuffer> buffer;
  std::unique_ptr<Context> context;
  explicit Object(Kind k):kind(k) {}
};
std::set<Object*> objects;
Object *make(Kind kind) {auto *o=new Object(kind);objects.insert(o);return o;}
Object *object(uint32_t value) {auto *o=reinterpret_cast<Object*>(uintptr_t(value));if(!objects.count(o))throw std::invalid_argument("Unknown scene object");return o;}
uint32_t retain(Object *o) {return o?++o->refs:0;}
uint32_t release(Object *o) {
  if(!o)return 0;
  uint32_t n=--o->refs;
  if(!n){objects.erase(o);Object *owner=o->owner;delete o;release(owner);}
  return n;
}
Context::~Context(){for(auto *o:textures)release(o);release(vertex);release(index);release(factory);}
void bind(Object *&dst,Object *src){retain(src);release(dst);dst=src;}
template<class T> T *ptr(uint32_t a){return reinterpret_cast<T*>(uintptr_t(a));}
void outputObject(uint32_t out,Object *o,bool add=false){if(!out)throw std::invalid_argument("Null output");if(add)retain(o);*ptr<uint32_t>(out)=uint32_t(uintptr_t(o));}
float scalar(uint32_t v){float f;memcpy(&f,&v,4);return f;}
void color(uint32_t v,float *out){out[0]=((v>>16)&255)/255.f;out[1]=((v>>8)&255)/255.f;out[2]=(v&255)/255.f;out[3]=(v>>24)/255.f;}
bool encoding(uint32_t f,ImageEncoding &out){switch(f){
case 21:out=ImageEncoding::Bgra8;break;case 22:out=ImageEncoding::Bgrx8;break;
case 23:out=ImageEncoding::Rgb565;break;case 24:out=ImageEncoding::Xrgb1555;break;
case 25:out=ImageEncoding::Argb1555;break;case 26:out=ImageEncoding::Argb4444;break;
case 28:out=ImageEncoding::Alpha8;break;case 0x31545844:out=ImageEncoding::Bc1;break;
case 0x33545844:out=ImageEncoding::Bc2;break;case 0x35545844:out=ImageEncoding::Bc3;break;default:return false;}return true;}
Object *imageObject(Object *device,Kind kind,uint32_t w,uint32_t h,uint32_t count,uint32_t use,uint32_t fmt,uint32_t pool){
 ImageEncoding e;if(!encoding(fmt,e))throw std::invalid_argument("Image encoding");
 auto data=std::make_shared<ImageStorage>(w,h,count,e);auto *o=make(kind);
 o->device=device;o->format=fmt;o->usage=use;o->pool=pool;o->image=std::move(data);return o;
}
void imageDesc(Object *o,uint32_t level,uint32_t *out){
 if(!out || !o->image || level>=o->image->levels.size())throw std::invalid_argument("Image description");
 uint32_t words[]={o->format,1,o->usage,o->pool,uint32_t(o->image->levels[level].size()),0,std::max(1u,o->image->width>>level),std::max(1u,o->image->height>>level)};memcpy(out,words,sizeof words);
}
void capabilities(uint32_t out){static const uint32_t words[]={
#include "game_capabilities.inc"
};if(!out)throw std::invalid_argument("Capabilities output");memcpy(ptr<void>(out),words,sizeof words);}
float *matrix(SceneRenderer &s,uint32_t which){
 if(which==2)return s.uniforms.view;if(which==3)return s.uniforms.projection;
 if(which==256)return s.uniforms.world;if(which>=257 && which<=259)return s.uniforms.blendWorld[which-257];
 if(which>=16 && which<=19)return s.uniforms.texture[which-16];
 throw std::invalid_argument("Unused game matrix");
}
void setting(Context &c,uint32_t key,uint32_t value){
 auto &s=c.scene;auto &r=s.raster;auto &u=s.uniforms;
 switch(key){
 case 7:r.ztest=value;break;case 14:r.zwrite=value;break;case 15:u.fragment[0]=value;break;
 case 19:r.src=value;break;case 20:r.dst=value;break;case 22:r.cull=value;break;case 23:r.zfunc=value;break;
 case 24:u.fogParams[3]=value/255.f;break;case 25:u.fragment[1]=value;break;case 27:r.blend=value;break;
 case 28:u.flags[2]=value;break;case 29:u.flags[3]=value;break;case 34:color(value,u.fog);break;
 case 35:c.tableFog=value;u.fogParams[2]=float(c.tableFog?c.tableFog:c.vertexFog);break;
 case 36:u.fogParams[0]=scalar(value);break;case 37:u.fogParams[1]=scalar(value);break;case 38:u.fog[3]=scalar(value);break;
 case 47:r.bias=value;break;case 52:r.stencil=value;break;case 53:r.fail=value;break;case 54:r.zfail=value;break;
 case 55:r.pass=value;break;case 56:r.func=value;break;case 57:r.ref=value;break;case 58:r.readMask=value;break;case 59:r.writeMask=value;break;
 case 60:color(value,u.factor);break;case 137:u.flags[1]=value;break;case 139:color(value,u.ambient);break;
 case 140:c.vertexFog=value;u.fogParams[2]=float(c.tableFog?c.tableFog:c.vertexFog);break;
 case 151:u.fragment[3]=value;break;case 168:r.colorMask=value;break;case 171:r.blendop=value;break;
 // Game initialization writes these fixed-function options, but its shaders do
 // not consume them (same behavior as the tested renderer).
 case 8:case 9:case 16:case 26:case 30:case 40:case 48:case 136:case 141:case 142:case 143:case 145:case 146:case 147:case 148:case 152:case 153:case 154:case 155:case 156:case 157:case 158:case 159:case 160:case 161:case 162:case 163:case 164:case 165:case 166:case 167:case 170:case 172:case 173:break;
 default:fprintf(stderr,"[scene port] unused game option %u=%u\n",key,value);break;
 }
}
void stage(SceneRenderer &s,uint32_t unit,uint32_t key,uint32_t value){
 if(unit>=4)return;auto &m=s.samplers[unit];auto *t=s.uniforms.stage[unit];
 switch(key){case 1:t[0]=value;break;case 2:t[1]=value;break;case 3:t[2]=value;break;case 4:t[3]=value;break;case 5:t[4]=value;break;case 6:t[5]=value;break;
 case 28:t[6]=value;break;case 11:t[7]=value;break;case 24:t[8]=value;break;
 case 13:m.u=value;break;case 14:m.v=value;break;case 16:m.mag=value;break;case 17:m.min=value;break;case 18:m.mip=value;break;case 19:m.bias=scalar(value);break;case 20:m.maxMip=value;break;
 default:break;}
}
uint32_t invoke(LT2SceneCommand op,const uint32_t *a){
 Object *o=object(a[0]);Context *c=o->context.get();SceneRenderer *s=c?&c->scene:nullptr;
 switch(op){
 case LT2_SCENE_AddRef_1:return retain(o);
 case LT2_SCENE_Release_1:return release(o);
 case LT2_SCENE_GetAdapterCount_1:return 1;
 // There is only one available output size: the window. A saved legacy
 // resolution selection cannot select a different render target size.
 case LT2_SCENE_GetAdapterModeCount_2:return a[1]?0:1;
 case LT2_SCENE_GetAdapterIdentifier_4:{if(a[1]||!a[3])return bad;auto *p=ptr<uint8_t>(a[3]);memset(p,0,1072);strcpy(reinterpret_cast<char*>(p),"lt2-native-vulkan");strcpy(reinterpret_cast<char*>(p+512),"LT2 native scene renderer");*reinterpret_cast<uint32_t*>(p+1032)=0x10de;*reinterpret_cast<uint32_t*>(p+1036)=1;return 0;}
 case LT2_SCENE_GetAdapterDisplayMode_3:{if(a[1]||!a[2])return bad;windowMode(ptr<uint32_t>(a[2]));return 0;}
 case LT2_SCENE_EnumAdapterModes_4:{if(a[1]||a[2]||!a[3])return bad;windowMode(ptr<uint32_t>(a[3]));return 0;}
 case LT2_SCENE_GetDeviceCaps_4:if(a[1])return bad;capabilities(a[3]);return 0;
 case LT2_SCENE_GetDeviceCaps_2:capabilities(a[1]);return 0;
 case LT2_SCENE_CheckDeviceFormat_7:{ImageEncoding e;if(a[1])return unavailable;if(a[4]&2)return a[6]==75||a[6]==80?0:unavailable;if(a[4]&1)return unavailable;return encoding(a[6],e)&&(a[5]==1||a[5]==3)?0:unavailable;}
 case LT2_SCENE_CheckDeviceMultiSampleType_6:return !a[1]&&!a[5]?0:unavailable;
 case LT2_SCENE_CheckDepthStencilMatch_6:return !a[1]&&(a[5]==75||a[5]==80)?0:unavailable;
 case LT2_SCENE_CreateDevice_7:{if(a[1]||!a[5]||!a[6])return bad;auto *p=ptr<uint32_t>(a[5]);HWND window=ptr<HWND__>(p[6]?p[6]:a[3]);auto context=std::make_unique<Context>(window,p[0]?p[0]:800,p[1]?p[1]:600);memcpy(context->presentation,p,52);context->creation[1]=1;context->creation[2]=uint32_t(uintptr_t(window));context->creation[3]=a[4];context->factory=o;retain(o);auto *d=make(Kind::Context);d->context=std::move(context);outputObject(a[6],d);return 0;}
 case LT2_SCENE_GetAvailableTextureMem_1:return 512*1024*1024;
 case LT2_SCENE_ResourceManagerDiscardBytes_2:case LT2_SCENE_TestCooperativeLevel_1:case LT2_SCENE_BeginScene_1:case LT2_SCENE_EndScene_1:case LT2_SCENE_PreLoad_1:return 0;
 case LT2_SCENE_ValidateDevice_2:if(!a[1])return bad;*ptr<uint32_t>(a[1])=1;return 0;
 case LT2_SCENE_GetDirect3D_2:outputObject(a[1],c->factory,true);return 0;
 case LT2_SCENE_GetCreationParameters_2:memcpy(ptr<void>(a[1]),c->creation,16);return 0;
 case LT2_SCENE_GetDisplayMode_2:{uint32_t w,h;c->platform.extent(w,h);uint32_t p[]={w?w:s->clientWidth,h?h:s->clientHeight,60,22};memcpy(ptr<void>(a[1]),p,16);return 0;}
 case LT2_SCENE_Reset_2:{auto *p=ptr<uint32_t>(a[1]);if(!p)return bad;s->resize(p[0]?p[0]:800,p[1]?p[1]:600);memcpy(c->presentation,p,52);return 0;}
 case LT2_SCENE_Present_5:s->present();return 0;
 case LT2_SCENE_Clear_7:if(a[1])return bad;s->renderer.clear(a[3],a[4],scalar(a[5]),a[6]);return 0;
 case LT2_SCENE_CreateVertexBuffer_6:case LT2_SCENE_CreateIndexBuffer_6:{bool index=op==LT2_SCENE_CreateIndexBuffer_6;if(!a[5] || (index&&a[3]!=101&&a[3]!=102))return bad;auto buffer=std::make_shared<SceneBuffer>(a[1]);buffer->dynamic=(a[2]&0x200)!=0;buffer->indexBytes=index?(a[3]==101?2:4):0;auto *b=make(index?Kind::Indices:Kind::Vertices);b->device=o;b->usage=a[2];b->format=index?a[3]:100;b->layout=index?0:a[3];b->pool=a[4];b->buffer=std::move(buffer);outputObject(a[5],b);return 0;}
 case LT2_SCENE_CreateTexture_8:outputObject(a[7],imageObject(o,Kind::Image,a[1],a[2],a[3],a[4],a[5],a[6]));return 0;
 case LT2_SCENE_CreateImageSurface_5:outputObject(a[4],imageObject(o,Kind::Surface,a[1],a[2],1,0,a[3],2));return 0;
 case LT2_SCENE_GetSurfaceLevel_3:{if(!o->image||a[1]>=o->image->levels.size()||!a[2])return bad;auto *v=make(Kind::Surface);v->device=o->device;v->owner=o;retain(o);v->image=o->image;v->level=a[1];v->format=o->format;v->pool=o->pool;v->usage=o->usage;outputObject(a[2],v);return 0;}
 case LT2_SCENE_Lock_5:{if(!o->buffer||!a[3])return bad;auto *p=o->buffer->map(a[1],a[2]);if(!p)return bad;*ptr<uint32_t>(a[3])=uint32_t(uintptr_t(p));return 0;}
 case LT2_SCENE_Unlock_1:if(!o->buffer)return bad;o->buffer->unmap();return 0;
 case LT2_SCENE_LockRect_4:{if(!o->image||!a[1])return bad;ImageMapping map;if(!o->image->map(o->level,ptr<ImageRegion>(a[2]),map))return bad;auto *out=ptr<uint32_t>(a[1]);out[0]=map.pitch;out[1]=uint32_t(uintptr_t(map.data));return 0;}
 case LT2_SCENE_UnlockRect_1:case LT2_SCENE_AddDirtyRect_2:if(!o->image)return bad;o->image->dirty=true;return 0;
 case LT2_SCENE_GetDesc_2:if(o->image)imageDesc(o,o->level,ptr<uint32_t>(a[1]));else if(o->buffer){uint32_t p[]={o->format,o->kind==Kind::Vertices?6u:7u,o->usage,o->pool,uint32_t(o->buffer->bytes.size()),o->layout};memcpy(ptr<void>(a[1]),p,o->kind==Kind::Vertices?24:20);}else return bad;return 0;
 case LT2_SCENE_GetLevelDesc_3:imageDesc(o,a[1],ptr<uint32_t>(a[2]));return 0;
 case LT2_SCENE_GetLevelCount_1:return o->image?uint32_t(o->image->levels.size()):0;
 case LT2_SCENE_GetDevice_2:outputObject(a[1],o->device,true);return 0;
 case LT2_SCENE_GetContainer_3:if(!o->owner)return bad;outputObject(a[2],o->owner,true);return 0;
 case LT2_SCENE_GetType_1:return o->kind==Kind::Surface?1:o->kind==Kind::Image?3:o->kind==Kind::Vertices?6:7;
 case LT2_SCENE_SetLOD_2:{auto old=o->lod;o->lod=a[1];return old;}
 case LT2_SCENE_GetLOD_1:return o->lod;
 case LT2_SCENE_SetPriority_2:{auto old=o->priority;o->priority=a[1];return old;}
 case LT2_SCENE_GetPriority_1:return o->priority;
 case LT2_SCENE_SetTexture_3:{if(a[1]>=4)return a[2]?bad:0;Object *t=a[2]?object(a[2]):nullptr;if(t&&!t->image)return bad;bind(c->textures[a[1]],t);s->images[a[1]]=t?t->image:nullptr;return 0;}
 case LT2_SCENE_GetTexture_3:if(a[1]>=4)return bad;outputObject(a[2],c->textures[a[1]],true);return 0;
 case LT2_SCENE_SetStreamSource_4:{if(a[1])return bad;auto *b=a[2]?object(a[2]):nullptr;if(b&&b->kind!=Kind::Vertices)return bad;bind(c->vertex,b);s->vertices=b?b->buffer:nullptr;s->stride=a[3];return 0;}
 case LT2_SCENE_GetStreamSource_4:if(a[1])return bad;outputObject(a[2],c->vertex,true);*ptr<uint32_t>(a[3])=s->stride;return 0;
 case LT2_SCENE_SetIndices_3:{auto *b=a[1]?object(a[1]):nullptr;if(b&&b->kind!=Kind::Indices)return bad;bind(c->index,b);s->indices=b?b->buffer:nullptr;s->indexBase=a[2];return 0;}
 case LT2_SCENE_GetIndices_3:outputObject(a[1],c->index,true);*ptr<uint32_t>(a[2])=s->indexBase;return 0;
 case LT2_SCENE_SetRenderState_3:setting(*c,a[1],a[2]);return 0;
 case LT2_SCENE_SetTextureStageState_4:stage(*s,a[1],a[2],a[3]);return 0;
 case LT2_SCENE_SetTransform_3:memcpy(matrix(*s,a[1]),ptr<void>(a[2]),64);return 0;
 case LT2_SCENE_GetTransform_3:memcpy(ptr<void>(a[2]),matrix(*s,a[1]),64);return 0;
 case LT2_SCENE_SetViewport_2:{auto *v=ptr<uint32_t>(a[1]);for(int i=0;i<4;++i)s->uniforms.viewport[i]=float(v[i]);s->depthMin=scalar(v[4]);s->depthMax=scalar(v[5]);return 0;}
 case LT2_SCENE_GetViewport_2:{auto *v=ptr<uint32_t>(a[1]);for(int i=0;i<4;++i)v[i]=uint32_t(s->uniforms.viewport[i]);memcpy(v+4,&s->depthMin,4);memcpy(v+5,&s->depthMax,4);return 0;}
 case LT2_SCENE_SetMaterial_2:memcpy(c->material,ptr<void>(a[1]),68);memcpy(s->uniforms.material,c->material,16);memcpy(s->uniforms.emissive,c->material+12,16);return 0;
 case LT2_SCENE_GetMaterial_2:memcpy(ptr<void>(a[1]),c->material,68);return 0;
 case LT2_SCENE_SetLight_3:{if(a[1]>=8)return bad;auto *l=ptr<float>(a[2]);memcpy(c->lightRecords[a[1]],l,104);memcpy(s->lights[a[1]].color,l+1,16);memcpy(s->lights[a[1]].direction,l+16,12);return 0;}
 case LT2_SCENE_GetLight_3:if(a[1]>=8)return bad;memcpy(ptr<void>(a[2]),c->lightRecords[a[1]],104);return 0;
 case LT2_SCENE_LightEnable_3:if(a[1]>=8)return bad;s->lights[a[1]].enabled=a[2]!=0;return 0;
 case LT2_SCENE_GetLightEnable_3:if(a[1]>=8)return bad;*ptr<uint32_t>(a[2])=s->lights[a[1]].enabled;return 0;
 case LT2_SCENE_CreateVertexShader_5:{
   if(!a[1]||!a[3])return bad;
   auto *decl=ptr<uint32_t>(a[1]);auto *code=ptr<uint32_t>(a[2]);SceneProgram p{};
   for(unsigned i=0;i<256;++i){p.declaration.push_back(decl[i]);if(decl[i]==0xffffffff)break;}
   if(p.declaration.back()!=0xffffffff)return bad;
   if(code){
     uint32_t hash=2166136261;size_t i=1;bool ended=false;
     while(i<4096){
       uint32_t token=code[i],opcode=token&65535;
       if(opcode==65535){ended=true;break;}
       if(opcode==65534){i+=1+(token>>16);continue;}
       unsigned n=0;
       switch(opcode){case 1:n=2;break;case 4:n=4;break;case 2:case 3:case 5:case 8:case 9:case 10:case 11:case 12:case 13:case 20:case 21:n=3;break;default:return bad;}
       if(i+n>=4096)return bad;
       for(unsigned k=0;k<=n;++k)hash=(hash^code[i+k])*16777619;
       i+=n+1;
     }
     if(!ended)return bad;
     p.shader=hash==world1_hash?1:hash==world2_hash?2:0;if(!p.shader)return bad;
   }
   uint32_t id=c->nextProgram++;s->programs.emplace(id,std::move(p));*ptr<uint32_t>(a[3])=id;return 0;
 }
 case LT2_SCENE_SetVertexShader_2:if((a[1]&0x80000000)&&!s->programs.count(a[1]))return bad;s->vertexFormat=a[1];return 0;
 case LT2_SCENE_GetVertexShader_2:*ptr<uint32_t>(a[1])=s->vertexFormat;return 0;
 case LT2_SCENE_DeleteVertexShader_2:s->programs.erase(a[1]);s->layouts.erase(a[1]);return 0;
 case LT2_SCENE_SetVertexShaderConstant_4:if(a[1]>96||a[3]>96-a[1])return bad;memcpy(s->uniforms.constants[a[1]],ptr<void>(a[2]),a[3]*16);return 0;
 case LT2_SCENE_GetVertexShaderConstant_4:if(a[1]>96||a[3]>96-a[1])return bad;memcpy(ptr<void>(a[2]),s->uniforms.constants[a[1]],a[3]*16);return 0;
 case LT2_SCENE_DrawPrimitive_4:
 case LT2_SCENE_DrawIndexedPrimitive_6:
 case LT2_SCENE_DrawPrimitiveUP_5:{
   LT2DrawPacket p{};p.version=1;p.context=o;p.topology=a[1];
   uint64_t n;
   if(op==LT2_SCENE_DrawIndexedPrimitive_6){p.indexed=1;p.firstVertex=a[2];p.vertexCount=a[3];p.firstIndex=a[4];n=a[5];}
   else if(op==LT2_SCENE_DrawPrimitiveUP_5){p.userMemory=1;p.vertices=ptr<void>(a[3]);p.stride=a[4];n=a[2];}
   else{p.firstVertex=a[2];n=a[3];}
   switch(p.topology){case 1:break;case 2:n*=2;break;case 3:n+=1;break;case 4:n*=3;break;case 5:case 6:n+=2;break;default:return bad;}
   if(n>UINT32_MAX)return bad;if(p.indexed)p.indexCount=uint32_t(n);else p.vertexCount=uint32_t(n);
   s->draw(p);if(p.userMemory)bind(c->vertex,nullptr);return 0;
 }
 default:fprintf(stderr,"[scene port] unimplemented command %u\n",unsigned(op));return bad;
 }
}
}
extern "C" __declspec(dllexport) uint32_t LT2CreateSceneFactory(){return uint32_t(uintptr_t(make(Kind::Factory)));}
extern "C" __declspec(dllexport) int LT2InvokeScene(LT2SceneCommand op,const uint32_t *args,uint32_t *result){
 if(!args||!result)return 0;
 try{*result=invoke(op,args);}catch(const std::exception &e){fprintf(stderr,"[scene port] command %u: %s\n",unsigned(op),e.what());*result=bad;}
 return 1;
}
extern "C" __declspec(dllexport) int LT2SubmitDraw(const LT2DrawPacket *packet,int32_t *result){
 if(!packet||!result||packet->version!=1)return 0;
 try{auto *o=object(uint32_t(uintptr_t(packet->context)));if(!o->context)return 0;o->context->scene.draw(*packet);if(packet->userMemory)bind(o->context->vertex,nullptr);*result=0;}catch(const std::exception &e){fprintf(stderr,"[scene port] draw: %s\n",e.what());*result=int32_t(bad);}
 return 1;
}
