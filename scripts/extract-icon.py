"""Extract the original PE icon groups without decoding/recompressing pixels."""
from pathlib import Path
import os, struct, sys
import pefile

root=Path(__file__).resolve().parents[1]
game=Path(os.environ.get('LT2_INPUT_DIR') or sys.exit('LT2_INPUT_DIR is not set; run Build.ps1.'))
pe=pefile.PE(str(game/'LT2.exe'))
types={e.id:e for e in pe.DIRECTORY_ENTRY_RESOURCE.entries}
icons={e.id:e for e in types[3].directory.entries}
out=root/'build/native';out.mkdir(parents=True,exist_ok=True)
rc=[]
for group in types[14].directory.entries:
    for language in group.directory.entries:
        item=language.data.struct
        data=pe.get_data(item.OffsetToData,item.Size)
        reserved,kind,count=struct.unpack_from('<HHH',data)
        payloads=[];entries=[];offset=6+16*count
        for index in range(count):
            w,h,colors,r,planes,bits,size,resource=struct.unpack_from('<BBBBHHIH',data,6+14*index)
            variants=icons[resource].directory.entries
            selected=next((v for v in variants if v.id==language.id),variants[0]).data.struct
            pixels=pe.get_data(selected.OffsetToData,selected.Size)
            assert len(pixels)==size
            entries.append(struct.pack('<BBBBHHII',w,h,colors,r,planes,bits,size,offset))
            payloads.append(pixels);offset+=size
        name=f'icon-{group.id}-{language.id}.ico'
        (out/name).write_bytes(struct.pack('<HHH',reserved,kind,count)+b''.join(entries+payloads))
        rc.extend([f'LANGUAGE {language.id & 1023}, {language.id >> 10}',f'{group.id} ICON "{name}"'])
(out/'lt2-icon.rc').write_text('\n'.join(rc)+'\n')
print('Original icon groups extracted:',len(rc)//2)
