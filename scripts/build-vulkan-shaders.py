"""Compile fixed-function and the game's original VS1.1 programs to SPIR-V.
Only validated opcodes are accepted; unsupported shader programs fail the build.
"""
from pathlib import Path
import struct, subprocess, os, sys
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/('build/native' if os.environ.get('LT2_NATIVE_SCENE') else 'build/vulkan');OUT.mkdir(parents=True,exist_ok=True)
SRC=ROOT/'src/renderer'
compiler=ROOT/'tools/vulkan/glslang/bin/glslang.exe'
GAME=Path(os.environ.get('LT2_INPUT_DIR') or sys.exit('LT2_INPUT_DIR is not set; run Build.ps1.'))
counts={1:2,2:3,3:3,4:4,5:3,8:3,9:3,10:3,11:3,12:3,13:3,20:3,21:3}
def reg(t):
 kind=(t>>28)&7;n=t&2047
 return {0:f'r[{n}]',1:{0:'position',3:'normal',5:'diffuse',6:'specular',7:'uv0'}.get(n,f'uv{n-7}'),2:f'u.c[{n}]',4:{0:'clip',1:'fogv',2:'pointv'} .get(n,'INVALID'),5:{0:'color',1:'spec'}.get(n,'INVALID'),6:f'texcoord[{n}]'}[kind]
def source(t):
 v=reg(t)+'.'+''.join('xyzw'[(t>>(16+2*i))&3] for i in range(4))
 mod=(t>>24)&15
 if mod==1:v='(-'+v+')'
 elif mod:raise ValueError(f'unsupported source modifier {mod}')
 return v
header=[]
for idx in (1,2):
 raw=(GAME/f'misc/dcWorld{idx}.vso').read_bytes()
 words=struct.unpack('<%dI'%(len(raw)//4),raw);i=1;lines=[];h=2166136261
 while True:
  op=words[i]&65535
  if op==65535:break
  if op==65534:i+=1+(words[i]>>16);continue
  count=counts[op];args=words[i+1:i+1+count]
  for word in words[i:i+1+count]: h=((h^word)*16777619)&0xffffffff
  dst=args[0];a=source(args[1]);b=source(args[2]) if count>2 else '';c=source(args[3]) if count>3 else ''
  if op in (20,21):
   n=4 if op==20 else 3
   expr='vec4('+','.join(f'dot({a}, {source(args[2]+k)})' for k in range(n))+(',0.0' if n==3 else '')+')'
  else:
   expr={1:a,2:f'({a}+{b})',3:f'({a}-{b})',4:f'({a}*{b}+{c})',5:f'({a}*{b})',8:f'vec4(dot(({a}).xyz,({b}).xyz))',9:f'vec4(dot({a},{b}))',10:f'min({a},{b})',11:f'max({a},{b})',12:f'vec4(lessThan({a},{b}))',13:f'vec4(greaterThanEqual({a},{b}))'}[op]
  mask=''.join('xyzw'[k] for k in range(4) if dst&(1<<(16+k)))
  lines.append(f' {reg(dst)}.{mask}=({expr}).{mask};')
  i+=count+1
 pre=(SRC/'fixed.vert').read_text().split('void main()')[0]
 text=pre+'void main(){\n vec4 r[12]; for(int i=0;i<12;++i)r[i]=vec4(0);\n vec4 clip=vec4(0),fogv=vec4(1),pointv=vec4(1);\n color=diffuse;spec=vec4(0); for(int i=0;i<4;++i)texcoord[i]=vec4(0,0,0,1);\n'+'\n'.join(lines)+'\n gl_Position=clip;gl_Position.y=-gl_Position.y;gl_Position.x*=u.clipAdjust.x;fogAmount=fogv.x;\n}\n'
 (OUT/f'world{idx}.vert').write_text(text)
 header.append(f'constexpr unsigned world{idx}_hash=0x{h:08x};\n')
for name,path in [('fixed_vert',SRC/'fixed.vert'),('fixed_frag',SRC/'fixed.frag'),('world1_vert',OUT/'world1.vert'),('world2_vert',OUT/'world2.vert')]:
 target=OUT/(name+'.spv')
 subprocess.run([str(compiler),'-V','--target-env','vulkan1.0','-I'+str(SRC),'-o',str(target),str(path)],check=True)
 b=target.read_bytes();w=struct.unpack('<%dI'%(len(b)//4),b)
 header.append('constexpr uint32_t '+name+'[]={'+','.join(hex(x) for x in w)+'};\n')
(OUT/'shaders.hpp').write_text(''.join(header))
