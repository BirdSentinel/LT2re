#version 450
#extension GL_GOOGLE_include_directive : require
#include "state.glsl"
layout(set=0,binding=1) uniform sampler2D tex[4];
layout(location=0) in vec4 color;
layout(location=1) in vec4 spec;
layout(location=2) in vec4 texcoord[4];
layout(location=6) in float fogAmount;
layout(location=0) out vec4 result;
vec4 arg(int a,vec4 current,vec4 t,vec4 temp) {
 vec4 v=color;
 switch(a&15){case 1:v=current;break;case 2:v=t;break;case 3:v=u.factor;break;case 4:v=spec;break;case 5:v=temp;break;}
 // D3DTA_TFACTOR is 3, SPECULAR is 4.
 if((a&16)!=0)v=1.0-v;
 if((a&32)!=0)v=vec4(v.a);
 return v;
}
vec4 combine(int op,vec4 a,vec4 b,vec4 t,vec4 cur){
 switch(op){
 case 2:return a; case 3:return b; case 4:return a*b;
 case 5:return a*b*2;case 6:return a*b*4;case 7:return a+b;
 case 8:return a+b-0.5;case 9:return (a+b-0.5)*2;case 10:return a-b;
 case 11:return a+b-a*b;case 12:return mix(b,a,color.a);
 case 13:return mix(b,a,t.a);case 14:return mix(b,a,u.factor.a);
 case 15:return a+b*(1-t.a);case 16:return mix(b,a,cur.a);
 case 24:return vec4(4*dot(a.rgb-0.5,b.rgb-0.5));
 default:return cur;
 }
}
void main(){
 vec4 cur=color,temp=vec4(0);
 for(int s=0;s<4;++s){
  ivec4 a=u.stage[s*3],b=u.stage[s*3+1],c=u.stage[s*3+2];
  // D3D terminates the cascade when COLORARG1 requests an unbound texture.
  // In particular, the stencil-shadow overlay must retain diffuse RGBA.
  if(a.x==1 || (c.y==0 && (a.y&15)==2))break;
  vec4 coord=texcoord[s];
  if((c.x&256)!=0)coord.xy/=max(1e-8,coord[(c.x&15)-1]);
  vec4 t=vec4(1);
  // Constant indices allow Vulkan 1.0 devices without dynamic sampler indexing.
  if(c.y!=0){switch(s){case 0:t=texture(tex[0],coord.xy);break;case 1:t=texture(tex[1],coord.xy);break;case 2:t=texture(tex[2],coord.xy);break;case 3:t=texture(tex[3],coord.xy);break;}}
  vec4 outc=combine(a.x,arg(a.y,cur,t,temp),arg(a.z,cur,t,temp),t,cur);
  outc.a=a.w==1?cur.a:combine(a.w,arg(b.x,cur,t,temp),arg(b.y,cur,t,temp),t,cur).a;
  if(b.z==5)temp=clamp(outc,0,1);else cur=clamp(outc,0,1);
 }
 if(u.fragment.x!=0){
  float a=cur.a,b=u.fogParams.w;bool pass=true;
  switch(u.fragment.y){case 1:pass=false;break;case 2:pass=a<b;break;case 3:pass=abs(a-b)<0.5/255.0;break;case 4:pass=a<=b;break;case 5:pass=a>b;break;case 6:pass=abs(a-b)>=0.5/255.0;break;case 7:pass=a>=b;break;}
  if(!pass)discard;
 }
 if(u.flags.w!=0)cur.rgb+=spec.rgb;
 if(u.flags.z!=0)cur.rgb=mix(u.fog.rgb,cur.rgb,clamp(fogAmount,0,1));
 result=cur;
}
