#version 450
#extension GL_GOOGLE_include_directive : require
#include "state.glsl"
layout(location=0) in vec4 inputPosition;
layout(location=1) in vec4 inputNormal;
layout(location=2) in vec4 inputDiffuse;
layout(location=3) in vec4 inputSpecular;
layout(location=4) in vec4 inputUv0;
layout(location=5) in vec4 inputUv1;
layout(location=6) in vec4 inputUv2;
layout(location=7) in vec4 inputUv3;
layout(location=8) in vec4 inputWeights;
vec4 completed(vec4 value, vec4 defaults, int location) {
 int n=(u.vertexMeta.z>>(3*location))&7;
 if(n>0)for(int i=n;i<4;++i)value[i]=defaults[i];
 return value;
}
#define position completed(inputPosition,vec4(0,0,0,1),0)
#define normal completed(inputNormal,vec4(0,0,1,0),1)
#define diffuse (((u.vertexMeta.x&1)!=0)?inputDiffuse.bgra:completed(inputDiffuse,vec4(1),2))
#define specular (((u.vertexMeta.x&2)!=0)?inputSpecular.bgra:completed(inputSpecular,vec4(0),3))
#define uv0 completed(inputUv0,vec4(0,0,0,1),4)
#define uv1 completed(inputUv1,vec4(0,0,0,1),5)
#define uv2 completed(inputUv2,vec4(0,0,0,1),6)
#define uv3 completed(inputUv3,vec4(0,0,0,1),7)
vec4 blendWeights() {
 vec4 result=completed(inputWeights,vec4(1,0,0,0),8);
 int n=u.vertexMeta.y;
 if(n>0 && n<=3){float last=1;for(int i=0;i<n;++i)last-=result[i];result[n]=last;}
 return result;
}
#define weights blendWeights()
layout(location=0) out vec4 color;
layout(location=1) out vec4 spec;
layout(location=2) out vec4 texcoord[4];
layout(location=6) out float fogAmount;
void main() {
 vec4 pos=vec4(position.xyz,1.0);
 mat4 world=u.world;
 if(u.fragment.w>0 && u.fragment.w<=3){
  world=u.world*weights.x;
  for(int i=1;i<=u.fragment.w;++i)world+=u.blendWorld[i-1]*weights[i];
 }
 vec4 eye=u.view*world*pos;
 vec3 n=normalize(transpose(inverse(mat3(u.view*world)))*normal.xyz);
 color=diffuse; spec=specular;
 gl_Position=u.projection*eye;
 if(u.flags.x!=0) {
  float w=1.0/max(position.w,1e-20);
  gl_Position=vec4((2.0*(position.xy-u.viewport.xy+vec2(0.5))/u.viewport.zw-1.0)*w,position.z*w,w);
  gl_Position.xy=gl_Position.xy*u.clipAdjust.yz+
      (vec2(u.clipAdjust.w,float(u.vertexMeta.w)*0.5)*2.0-1.0)*(vec2(1)-u.clipAdjust.yz)*w;
 } else {
  gl_Position.y=-gl_Position.y;
  gl_Position.x*=u.clipAdjust.x;
  if(u.flags.y!=0) {
   color=vec4(u.emissive.rgb+u.ambient.rgb*u.material.rgb,u.material.a);
   for(int i=0;i<8;++i) color.rgb+=u.material.rgb*u.lightColor[i].rgb*max(0.0,dot(n,-u.lightDirection[i].xyz));
   color=clamp(color,0.0,1.0);
  }
 }
 vec4 coords[4]=vec4[](uv0,uv1,uv2,uv3);
 for(int s=0;s<4;++s) {
  int tc=u.stage[s*3+1].w;
  vec4 t=coords[tc&3];
  if((tc>>16)==1)t=vec4(n,1);
  if((tc>>16)==2)t=eye;
  if((tc>>16)==3)t=vec4(reflect(normalize(eye.xyz),n),1);
  if(u.stage[s*3+2].x!=0)t=u.texMatrix[s]*t;
  texcoord[s]=t;
 }
 fogAmount=1.0;
 if(u.flags.z!=0) {
  float d=abs(eye.z);
  if(u.fogParams.z==3.0)fogAmount=(u.fogParams.y-d)/max(1e-6,u.fogParams.y-u.fogParams.x);
  if(u.fogParams.z==1.0)fogAmount=exp(-u.fog.w*d);
  if(u.fogParams.z==2.0)fogAmount=exp(-pow(u.fog.w*d,2.0));
 }
}
