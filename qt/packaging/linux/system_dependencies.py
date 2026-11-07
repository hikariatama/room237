#!/usr/bin/env python3
"""Resolve Debian runtime dependencies from every ELF in the packaged tree."""
import os
import re
import subprocess
import sys
from pathlib import Path
root=Path(sys.argv[1]).resolve()
packages={'libc6 (>= 2.35)','libgl1','libegl1'}
owners={}
for path in sorted(root.rglob('*')):
    if not path.is_file() or path.is_symlink():
        continue
    with path.open('rb') as f:
        if f.read(4)!=b'\x7fELF':
            continue
    r=subprocess.run(['ldd',str(path)],capture_output=True,text=True,env=dict(os.environ,LD_LIBRARY_PATH=str(root/'lib')))
    if 'not found' in r.stdout:
        sys.exit(f'{path.name}: unresolved ELF dependency\n{r.stdout}')
    if r.returncode and not any(s in r.stdout+r.stderr for s in ('not a dynamic executable','statically linked')):
        sys.exit(f'ldd failed for {path}')
    for raw in re.findall(r'=>\s+(/\S+)',r.stdout):
        library=Path(raw)
        if library.resolve().is_relative_to(root):
            continue
        owner=owners.get(str(library.resolve()))
        if owner:
            if owner != "libc6": packages.add(owner)
            continue
        for candidate in dict.fromkeys([raw,str(library.resolve()),raw.replace('/usr/lib/','/lib/')]):
            q=subprocess.run(['dpkg-query','-S',candidate],capture_output=True,text=True)
            if q.returncode==0:
                owner=q.stdout.split(': ',1)[0].split(':',1)[0]
                break
        if not owner:
            sys.exit(f'No Debian package owns runtime library {raw}')
        owners[str(library.resolve())]=owner
        if owner!='libc6':
            packages.add(owner)
print(', '.join(sorted(packages)))
