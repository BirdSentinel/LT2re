layout(set=0,binding=0,std140) uniform State {
 mat4 world,view,projection,texMatrix[4];
 vec4 c[96];
 vec4 viewport,factor,ambient,material,emissive;
 vec4 lightDirection[8],lightColor[8];
 ivec4 flags;
 ivec4 stage[12];
 vec4 fog,fogParams;
 mat4 blendWorld[3];
 vec4 clipAdjust;
 ivec4 fragment;
 ivec4 vertexMeta;
} u;
