"""Extract just the official LLVM compiler and builtin headers, locally."""
from pathlib import Path
import hashlib, tarfile, shutil
root=Path(__file__).resolve().parents[1]/'tools/llvm'
archive=root/'llvm-21.1.8.tar.xz'
expected='749d22f565fcd5718dbed06512572d0e5353b502c03fe1f7f17ee8b8aca21a47'
with archive.open('rb') as f: actual=hashlib.file_digest(f,'sha256').hexdigest()
if actual!=expected: raise RuntimeError('LLVM archive hash mismatch')
count=0
with tarfile.open(archive,'r|xz') as tar:
    for member in tar:
        parts=Path(member.name).parts
        rel=Path(*parts[1:])
        wanted=(rel.parent==Path('bin') and (rel.name in ('clang.exe','clang-cl.exe','clang-21.exe','lld-link.exe') or rel.suffix=='.dll')) or str(rel).replace('\\','/').startswith('lib/clang/21/include/') or 'LICENSE' in rel.name
        if not wanted or not member.isfile(): continue
        target=(root/rel).resolve()
        if not target.is_relative_to(root.resolve()): raise RuntimeError('Invalid archive path')
        target.parent.mkdir(parents=True,exist_ok=True)
        with tar.extractfile(member) as src,target.open('wb') as dst: shutil.copyfileobj(src,dst)
        count+=1
print(f'Extracted {count} compiler/header/license files')
