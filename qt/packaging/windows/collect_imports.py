#!/usr/bin/env python3
"""Complete the PE import closure using only the selected Qt/compiler SDKs."""
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Windows 10/11 OS DLLs. Never copy these from a host or Wine installation.
SYSTEM = set('''advapi32 authz avicap32 avrt bcrypt bcryptprimitives cabinet cfgmgr32 clbcatq combase comctl32
comdlg32 credui crypt32 cryptbase cryptnet cryptui d2d1 d3d9 d3d11 d3d12 d3dcompiler_47
 dcomp dinput8 dnsapi dsound dwmapi dwrite dxcore dxgi dxva2 evr gdi32 gdi32full glu32
hid imm32 iphlpapi kernel32 kernelbase mf mfcore mfplat mfreadwrite mfuuid mpr msacm32
msimg32 msvcrt ncrypt netapi32 normaliz ntdll ole32 oleacc oleaut32 opengl32 powrprof
propsys psapi rasapi32 rpcrt4 secur32 setupapi shcore shell32 shlwapi srvcli sspicli
ucrtbase urlmon user32 userenv usp10 uxtheme version win32u winhttp wininet winmm
winspool wintrust winusb wldap32 ws2_32 wsock32 wtsapi32 mswsock nsi dxguid'''.split())

def system(name):
    return name.lower().removesuffix('.dll') in SYSTEM or name.lower().startswith(('api-ms-win-', 'ext-ms-win-'))

def imports(path, objdump):
    out = subprocess.run([objdump, '-p', str(path)], capture_output=True, text=True, check=True).stdout
    return re.findall(r'^\s+DLL Name:\s*(\S+)', out, re.M)

def collect(stage, objdump, sources):
    available = {p.name.lower(): p for root in sources for p in Path(root).glob('*.dll')}
    seen = set()
    while True:
        shipped = {p.name.lower(): p for p in stage.rglob('*') if p.suffix.lower() in ('.dll', '.exe')}
        todo = [p for p in shipped.values() if p not in seen]
        if not todo:
            break
        for binary in todo:
            seen.add(binary)
            for name in imports(binary, objdump):
                if system(name) or name.lower() in shipped:
                    continue
                if name.lower() not in available:
                    raise RuntimeError(f'{binary.name}: unresolved import {name}')
                target = stage / name
                shutil.copy2(available[name.lower()], target)
                shipped[name.lower()] = target
                print(f'{binary.name} -> {name}')

if __name__ == '__main__':
    collect(Path(sys.argv[1]), sys.argv[2], sys.argv[3:])
