"""Compile lifted C with LLVM; run from the x86 Visual Studio environment."""
from pathlib import Path
import concurrent.futures, subprocess, sys, os
ROOT=Path(__file__).resolve().parents[1]
NATIVE=bool(os.environ.get('LT2_NATIVE_SCENE'))
OUT=ROOT/('build/native' if NATIVE else 'build/clang');OUT.mkdir(exist_ok=True)
compiler=ROOT/'tools/llvm/bin/clang-cl.exe'
flags=['--target=i686-pc-windows-msvc','/nologo','/O2','/Ob2','/fp:strict','/MT','/W0',
       '/DRECOMP_X87_RING','/DRECOMP_X87_LOCAL','/DRECOMP_LOCAL_REGS',
       '/FI'+str(ROOT/'src/generated/recomp_funcs.h'),
       '/I'+str(ROOT/'tools/pcrecomp/runtime/recomp32'),
       '/I'+str(ROOT/'tools/pcrecomp/runtime/native32'),'/I'+str(ROOT/'src/generated')]
if NATIVE:flags.append('/DLT2_NATIVE_SCENE')
def compile(path):
    result=subprocess.run([str(compiler),*flags,'/c',str(path),'/Fo'+str(OUT/(path.stem+'.obj'))],capture_output=True,text=True)
    print(path.name+': '+str(result.returncode),flush=True)
    if result.returncode:
        print(result.stdout+result.stderr,flush=True)
        raise RuntimeError(path.name)
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    sources=sorted((ROOT/('build/native-generated' if NATIVE else 'build/optimized')).glob('*.c'))
    if len(sys.argv)>1:
        selected=set(sys.argv[1:]);sources=[p for p in sources if p.name in selected]
        if len(sources)!=len(selected):raise ValueError('Unknown compilation unit')
    list(pool.map(compile,sources))
