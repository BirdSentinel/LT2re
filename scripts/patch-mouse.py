"""Pinned LT2 instruction-site adaptations, including duplicated lifted blocks."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
draw_sites = {0x40A07D: 1, 0x419FF1: 18, 0x41F699: 1, 0x49E433: 1}
seen = dict.fromkeys(draw_sites, 0)
polls = 0
for path in sorted((root / 'src/generated').glob('recomp_00*.c')):
    source = path.read_text()
    for address in draw_sites:
        pattern = rf'    (?:RECOMP_ICALL\(MEM32\(edx \+ 0x4\)\);|esp \+= 20; /\* Windows cursor replaces Mouse.DCT \*/) /\* 0x{address:08X}: call dword ptr \[edx \+ 4\] \*/'
        source, count = re.subn(pattern, f'    esp += 20; /* Windows cursor replaces Mouse.DCT */ /* 0x{address:08X}: call dword ptr [edx + 4] */', source)
        seen[address] += count
    anchor = 'L_004C16C0:\n'
    hook = '    lt2_poll_mouse(ecx); esp += 8; return; /* thiscall ret 4 */\n'
    polls += source.count(anchor)
    if anchor in source and hook not in source:
        source = 'void lt2_poll_mouse(unsigned int input);\n' + source.replace(anchor, anchor + hook)
    if source != path.read_text():
        path.write_text(source)
if seen != draw_sites or polls != 1:
    raise RuntimeError(f'Unexpected lifted cursor sites: {seen}, poll entries={polls}')
print('Windows cursor: 21 sprite draws suppressed, absolute mouse poll installed')
