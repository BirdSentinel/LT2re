"""Lift runtime-observed callbacks missed by the initial static catalog."""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/pcrecomp'))
from tools.lift import translator as tr

INPUT = Path(os.environ.get('LT2_INPUT_DIR') or sys.exit('LT2_INPUT_DIR is not set; run Build.ps1.'))
info, data, iat = tr.load_pe(str(INPUT / 'LT2.exe'))
dis = tr.Disassembler(data, info.image_base, info.sections)
header = ROOT / 'src/generated/recomp_funcs.h'
known = {int(x, 16) for x in re.findall(r'void sub_([0-9A-F]+)\(void\)', header.read_text())}
pending = [int(x, 16) for x in json.loads((ROOT / 'analysis/extra-seeds.json').read_text())]
recovered = {}
while pending:
    addr = pending.pop()
    if addr in known or addr in recovered:
        continue
    func = dis.disassemble_function(addr, iat)
    if func is None:
        raise RuntimeError(f'Cannot disassemble {addr:08X}')
    recovered[addr] = func
    pending.extend(func.calls_to)
lifter = tr.Lifter(iat_map=iat)
bodies = [lifter.lift_function(func) for _, func in sorted(recovered.items())]
decls = '\n'.join(f'void sub_{a:08X}(void);' for a in sorted(recovered))
code = '#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n#include "recomp_funcs.h"\n' + decls + '\n' + '\n'.join(bodies)
code += '\nrecomp_func_t lt2_lookup_recovered(uint32_t va) {\n switch (va) {\n'
code += '\n'.join(f'case 0x{a:08X}: return sub_{a:08X};' for a in sorted(recovered))
code += '\ndefault: return NULL;\n }\n}\n'
(ROOT / 'src/generated/recomp_extra.c').write_text(code)
print(f'Recovered {len(recovered)} additional functions')
