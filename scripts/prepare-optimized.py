"""Derive local-register bodies without changing the preserved lifted sources.
Matches the register/exit/slot transforms in pcrecomp tools/lift/generate.py.
"""
from pathlib import Path
import re, os, json
ROOT=Path(__file__).resolve().parents[1]
NATIVE=bool(os.environ.get('LT2_NATIVE_SCENE'))
OUT=ROOT/('build/native-generated' if NATIVE else 'build/optimized');OUT.mkdir(exist_ok=True)
FUNCTION=re.compile(r'^void (sub_[0-9A-F]{8})\(void\) \{\n(.*?)^\}',re.M|re.S)
DRAW_SITES={
    '004C3FBC':0,'004C3FEA':0,
    '004C4072':1,'004C40EA':1,'004C73ED':1,'004C7508':1,
    '00530FC9':1,'00530FF5':1,
    '004C4721':2,'004C48A7':2,'005316E9':2,
}
DRAW_CALL=re.compile(r'RECOMP_ICALL\((MEM32\(\w+ \+ 0x(?:118|11C|120)\))\); /\* 0x([0-9A-F]{8}):')
def direct_draw(match):
    target,site=match.groups()
    if site not in DRAW_SITES: return match[0]
    return f'LT2_GAME_DRAW({DRAW_SITES[site]}, {target}); /* 0x{site}:'
def transform(match):
    name,body=match.groups()
    if NATIVE and name=='sub_004A4020':
        # This routine emits a whole string (including its shadow) into the
        # shared sprite batch. Preserve that semantic boundary so HUD layout
        # never anchors unrelated left/center/right labels as one rectangle.
        flush='''    {
        uint32_t text_eax=eax,text_ecx=ecx,text_edx=edx;
        ecx=text_renderer;
        RECOMP_CALL(sub_004C7410);
        eax=text_eax;ecx=text_ecx;edx=text_edx;
    }
'''
        body=body.replace('L_004A4020:\n','L_004A4020:\n    uint32_t text_renderer=ecx;\n'+flush)
        body=body.replace('L_004A4210:\n','L_004A4210:\n'+flush)
    if NATIVE and name=='sub_004036C0':
        # Loading sprites anchor against the device size (+AC/+B0), while their
        # sizes/offsets are authored at 800x600. Keep that composition coherent
        # before the native renderer stretches it; restore state on every exit.
        body=body.replace('    RECOMP_ENTER(0x004036C0u);', '''    uint32_t loading_view=MEM32(0x5768D0);
    if(loading_view)loading_view=MEM32(loading_view+0x5C);
    uint32_t loading_width=loading_view?MEM32(loading_view+0xAC):0;
    uint32_t loading_height=loading_view?MEM32(loading_view+0xB0):0;
    if(loading_view){MEM32(loading_view+0xAC)=800;MEM32(loading_view+0xB0)=600;}
    RECOMP_ENTER(0x004036C0u);''')
        body=body.replace('return;', 'if(loading_view){MEM32(loading_view+0xAC)=loading_width;MEM32(loading_view+0xB0)=loading_height;} return;')
    if 'RECOMP_REGS_LOCALS' not in body:
        body='    RECOMP_REGS_LOCALS\n'+body
        body=re.sub(r'\breturn;', '{ RECOMP_REGS_OUT(); return; }',body)
        body+='    RECOMP_REGS_OUT();\n'
    body=re.sub(r'\b_st\[(\d+)\]',r'_ST(\1)',body)
    body=re.sub(r'\bg_st_i64\[(\d+)\]',r'_STI(\1)',body)
    body=body.replace('lt2_poll_mouse(ecx);', 'RECOMP_REGS_OUT(); lt2_poll_mouse(ecx); RECOMP_REGS_IN();')
    return f'void {name}(void) {{\n{body}}}'
def main():
    count=0
    observed=set();direct_count=0
    scene_sites=json.loads((ROOT/'analysis/graphics-sites.json').read_text()) if NATIVE else {}
    if NATIVE:
        scene_sites.update(json.loads((ROOT/'analysis/graphics-sites-static.json').read_text()))
    for path in (ROOT/'src/generated').glob('*.c'):
        text=path.read_text()
        for call in DRAW_CALL.finditer(text):
            if call[2] in DRAW_SITES:
                observed.add(call[2]);direct_count+=1
        output,n=FUNCTION.subn(transform,text);count+=n
        output=DRAW_CALL.sub(direct_draw,output)
        if NATIVE:
            # Projection rebuild, frustum dirty check, and plane extraction.
            # Apply in game memory before visibility tests, not just on the GPU.
            camera_hooks={'004A93F0':'ecx','004A9430':'ecx'}
            camera_changed=False
            for site,register in camera_hooks.items():
                marker=f'L_{site}:\n'
                if marker in output:
                    output=output.replace(marker,marker+f'    lt2_fit_camera({register});\n')
                    camera_changed=True
            output,projection_hooks=re.subn(r'(    MEM8\(esi \+ 0x35C\) = [^\n]+/\* 0x004A923B:[^\n]+\n)',
                                           r'\1    lt2_fit_camera(esi);\n',output)
            camera_changed |= bool(projection_hooks)
            if camera_changed:output='void lt2_fit_camera(unsigned int camera);\n'+output
            def scene_call(m):
                entry=scene_sites.get(m[2])
                if not entry:return m[0]
                return f'LT2_GAME_SCENE(LT2_SCENE_{entry["method"]}_{entry["arity"]}, {entry["arity"]}, 0x{m[2]}); /* 0x{m[2]}:'
            output=re.sub(r'RECOMP_ICALL\(([^\n]*?)\); /\* 0x([0-9A-F]{8}):',scene_call,output)
        if os.environ.get('LT2_AUDIT_GRAPHICS'):
            output=re.sub(r'(RECOMP_ICALL\([^\n]*?\); /\* 0x([0-9A-F]{8}):)',
                lambda m: f'lt2_graphics_site=0x{m[2]}; ++lt2_graphics_serial; '+m[1],output)
            output='extern unsigned lt2_graphics_site, lt2_graphics_serial;\n'+output
        if 'LT2_GAME_DRAW(' in output or 'LT2_GAME_SCENE(' in output:
            output='#include "../../src/game_render_bridge.h"\n'+output
        expected=len(re.findall(r'^void sub_[0-9A-F]{8}\(void\) \{',text,re.M))
        assert n==expected,(path,n,expected)
        (OUT/path.name).write_text(output)
    assert count>9900,count
    assert observed==set(DRAW_SITES),(observed,set(DRAW_SITES)-observed)
    assert direct_count==13,direct_count
    print(f'Prepared {count} functions with local register synchronization')
    print(f'Prepared {direct_count} direct geometry calls at {len(observed)} audited addresses')
if __name__=='__main__': main()
