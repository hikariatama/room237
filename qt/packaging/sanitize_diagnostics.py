#!/usr/bin/env python3
"""Map vendor __FILE__ diagnostics to relative names in staged binaries only.

String length, offsets, load commands, rpaths and imports are unchanged. The map
is shipped with the notices. This runs before macOS ad-hoc signing.
"""
import hashlib
import json
import mmap
import re
import sys
from pathlib import Path

SOURCE_SUFFIXES=('.c','.cc','.cpp','.cxx','.h','.hh','.hpp','.mm','.m','.rs')
ASCII=re.compile(rb'/(?:home/qt|Users/qt)/[A-Za-z0-9_./+:-]+')
WIDE=re.compile(rb'/\x00(?:h\x00o\x00m\x00e\x00/\x00q\x00t\x00|U\x00s\x00e\x00r\x00s\x00/\x00q\x00t\x00)/\x00(?:[A-Za-z0-9_./+:-]\x00)+')

def sha(file):
    h=hashlib.sha256()
    with file.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
    return h.hexdigest()


def main():
    root=Path(sys.argv[1]).resolve(strict=True);out=Path(sys.argv[2]).absolute()
    if not out.is_relative_to(root):raise SystemExit('map must be inside the staged package')
    if out.exists():raise SystemExit('refusing to replace an existing diagnostic map')
    records=[]
    for file in sorted(root.rglob('*')):
        if not file.is_file() or file.is_symlink() or not file.stat().st_size:continue
        with file.open('rb') as f:magic=f.read(4)
        if not (magic==b'\x7fELF' or magic[:2]==b'MZ' or magic in (b'\xcf\xfa\xed\xfe',b'\xfe\xed\xfa\xcf',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca')):continue
        patches=[]
        with file.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
            for pattern,encoding in [(ASCII,'ascii'),(WIDE,'utf-16le')]:
                for match in pattern.finditer(data):
                    text=match.group().decode(encoding)
                    if text.endswith(SOURCE_SUFFIXES):
                        patches.append({'offset':match.start(),'encoding':encoding,'before':text,'after':'.'+text[1:]})
        if not patches:continue
        before=sha(file)
        with file.open('r+b') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_WRITE) as data:
            for patch in patches:
                old=patch['before'].encode(patch['encoding']);new=patch['after'].encode(patch['encoding']);offset=patch['offset']
                assert len(old)==len(new) and data[offset:offset+len(old)]==old
                data[offset:offset+len(new)]=new
            data.flush()
        records.append({'file':str(file.relative_to(root)),'sha256_before':before,'sha256_after':sha(file),'patches':patches})
    for record in records:
        for patch in record['patches']:
            patch['original_filename_sha256']=hashlib.sha256(patch['before'].encode(patch['encoding'])).hexdigest()
            patch['before']=patch['before'].lstrip('/')  # Metadata also avoids absolute build paths.
            patch['original_was_absolute']=True
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps({'operation':'length-preserving relative vendor diagnostic filenames; no loader/ABI changes','files':records},indent=2)+'\n')
    print(f'{sum(len(r["patches"]) for r in records)} diagnostic strings mapped in {len(records)} binaries')

if __name__=='__main__':main()
