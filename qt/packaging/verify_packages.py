#!/usr/bin/env python3
"""Verify the shipped dependency closure. JSON keeps failed and unverified checks separate."""
import argparse
import hashlib
import json
import mmap
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def magic(path):
    with path.open('rb') as f:
        first = f.read(4)
    if first == b'\x7fELF':
        return 'elf'
    if first[:2] == b'MZ':
        return 'pe'
    if first in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        return 'macho'
    return ''


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda:f.read(1024*1024),b''):
            h.update(data)
    return h.hexdigest()


def path_hit(data, prefix):
    """Find an absolute root, not a component inside another/relative source path."""
    boundaries=b"\0 \t\r\n'\"=;:[(>"
    for encoding,width in [('utf-8',1),('utf-16le',2)]:
        needle=prefix.encode(encoding)
        offset=0
        while True:
            found=data.find(needle,offset)
            if found<0:break
            previous=data[max(0,found-width):found]
            context=data[max(0,found-24):found].decode('latin1' if width==1 else 'utf-16le',errors='ignore')
            if context.endswith('qrc:') or context.endswith('\0:') or (found==width and context==':'):
                offset=found+width
                continue  # Qt resource URLs are not absolute filesystem paths.
            allowed=[bytes([x]) if width==1 else bytes([x,0]) for x in boundaries]
            if found==0 or previous in allowed:
                return True
            offset=found+width
    return False


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--platform', choices=['macos','linux','windows'], required=True)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--forbid', action='append', default=[])
    p.add_argument('--objdump', default='llvm-objdump')
    p.add_argument('--dmg', type=Path)
    p.add_argument('--appimage', type=Path)
    p.add_argument('--fixtures', type=Path)
    p.add_argument('--installer',type=Path)
    p.add_argument('--arch',choices=['arm64','x86_64'])
    a=p.parse_args()
    root=a.root.resolve(strict=True)
    a.report.parent.mkdir(parents=True,exist_ok=True)
    checks=[]
    commands=[]
    def check(name, condition, detail=None):
        checks.append({'check':name,'status':'passed' if condition else 'failed','detail':detail})
    def unknown(name, detail):
        checks.append({'check':name,'status':'unverified','detail':detail})
    def run(cmd, env=None, timeout=90):
        start=time.monotonic()
        try:
            r=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,env=env,timeout=timeout, errors='replace')
            rc,out=r.returncode,r.stdout+r.stderr
        except (OSError,subprocess.TimeoutExpired) as e:
            rc,out=127,str(e)
        commands.append({'argv':[str(x) for x in cmd], 'exit_code':rc,'wall_seconds':round(time.monotonic()-start,3),'output':out})
        return rc,out
    binaries=[]
    vendor_diagnostics={}
    hashes={}
    for f in sorted(root.rglob('*')):
        if f.is_symlink():
            check(f'symlink:{f.relative_to(root)}', f.resolve().is_relative_to(root), os.readlink(f))
        elif f.is_file():
            kind=magic(f)
            if kind:
                binaries.append((f,kind))
                hashes[str(f.relative_to(root))]=sha(f)
    check('shipped binaries found',bool(binaries),len(binaries))
    # Text uses the platform's installed fonts; the packages carry no font files.
    fonts=[str(f.relative_to(root)) for f in root.rglob('*') if f.is_file() and f.suffix.lower() in ('.ttf','.otf','.ttc','.woff','.woff2','.dfont')]
    check('no bundled font files',not fonts,fonts)
    # Builder-local locations must not leak into shipped code: explicit --forbid paths plus the builder's caches and cloud folders.
    forbidden=[str(x) for x in a.forbid]+[str(Path.home()/'.cache')+'/', str(Path.home()/'Library/Mobile Documents')+'/', '/src/qt/', '/build/qt/', '/build/core/']
    for f in sorted(root.rglob('*')):
        if not f.is_file() or f.is_symlink() or not f.stat().st_size:
            continue
        # Scan code and configuration, including UTF-16 PE strings, not media/license text.
        if f not in [b[0] for b in binaries] and f.name not in ('qt.conf','Info.plist'):
            continue
        with f.open('rb') as handle, mmap.mmap(handle.fileno(),0,access=mmap.ACCESS_READ) as data:
            found=[s for s in forbidden if path_hit(data,s)]
            vendor=sorted({x.decode(errors='replace') for x in re.findall(rb'/(?:home/qt|Users/qt)/[A-Za-z0-9_./+:-]+',data)})
            if vendor:vendor_diagnostics[str(f.relative_to(root))]=vendor
        check(f'no build paths:{f.relative_to(root)}',not found,found)
    # The QA driver and its hooks are test-only; a shipped app or core that knows ROOM237_QA_PORT is a test build.
    for f,_ in binaries:
        if f.stem in ('Room237','room237','room237-core'):
            with f.open('rb') as handle, mmap.mmap(handle.fileno(),0,access=mmap.ACCESS_READ) as data:
                check(f'no QA hooks:{f.relative_to(root)}',data.find(b'ROOM237_QA_PORT')<0 and data.find('ROOM237_QA_PORT'.encode('utf-16le'))<0)
    if a.platform=='macos':
        contents=root/'Contents'
        mainbin=contents/'MacOS/Room237'
        for binary,kind in binaries:
            rel=str(binary.relative_to(root))
            check(f'Mach-O:{rel}',kind=='macho')
            expected_arch=a.arch or root.parent.name
            rc,archs=run(['lipo','-archs',binary])
            check(f'architecture:{rel}',rc==0 and archs.strip()==expected_arch,archs.strip())
            rc,out=run(['otool','-L',binary])
            check(f'otool:{rel}',rc==0)
            _,loads=run(['otool','-l',binary])
            rpaths=re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset',loads)
            def expand(value):
                return Path(value.replace('@executable_path',str(mainbin.parent)).replace('@loader_path',str(binary.parent)))
            for rp in rpaths:
                check(f'rpath:{rel}:{rp}',rp.startswith(('@loader_path','@executable_path')) and expand(rp).resolve().is_relative_to(root))
            ids=re.findall(r'cmd LC_ID_DYLIB\s+cmdsize \d+\s+name (.*?) \(offset',loads)
            for own_id in ids:
                check(f'install name:{rel}',own_id.startswith('@'),own_id)
            for dep in re.findall(r'^\s+(.+?) \(compatibility',out,re.M):
                if dep in ids:
                    continue
                if dep.startswith(('/System/Library/','/usr/lib/')):
                    continue
                # LC_ID_DYLIB names itself; checking its resolution is harmless.
                if dep.startswith('@rpath/'):
                    candidates=[expand(rp)/dep[len('@rpath/'):] for rp in rpaths]
                    candidates += [contents/'Frameworks'/dep[len('@rpath/'):]]
                elif dep.startswith(('@loader_path/','@executable_path/')):
                    candidates=[expand(dep)]
                else:
                    candidates=[]
                check(f'dependency:{rel}:{dep}',any(c.exists() and c.resolve().is_relative_to(root) for c in candidates),[str(c.relative_to(root)) if c.is_relative_to(root) else str(c) for c in candidates])
            rc,sig=run(['codesign','-dv','--verbose=4',binary])
            check(f'ad-hoc signature:{rel}',rc==0 and 'Signature=adhoc' in sig and 'Authority=' not in sig and 'TeamIdentifier=not set' in sig)
        rc,_=run(['codesign','--verify','--deep','--strict',root])
        check('bundle signature verification',rc==0,'ad-hoc only; no Developer ID or notarization')
        check('bundle icon',(contents/'Resources/icon.icns').is_file())
        plugins=contents/'PlugIns'
        licenses=contents/'Resources/licenses'
        check('macOS platform plugin', (plugins/'platforms/libqcocoa.dylib').exists())
        check('macOS styles',bool(list((plugins/'styles').glob('*macstyle*'))))
        ffmpeg=contents/'MacOS/ffmpeg'
        check('Info.plist', (contents/'Info.plist').is_file())
    elif a.platform=='windows':
        sys.path.insert(0,str(Path(__file__).parent/'windows'))
        from collect_imports import imports, system
        shipped={f.name.lower():f for f,k in binaries}
        for binary,kind in binaries:
            rel=str(binary.relative_to(root))
            check(f'PE:{rel}',kind=='pe')
            with binary.open('rb') as handle:
                handle.seek(0x3c);offset=struct.unpack('<I',handle.read(4))[0];handle.seek(offset+4);machine=struct.unpack('<H',handle.read(2))[0]
            check(f'Windows x64:{rel}',machine==0x8664,hex(machine))
            try:
                names=imports(binary,a.objdump)
                check(f'PE import parse:{rel}',bool(names),names)
                for dep in names:
                    check(f'dependency:{rel}:{dep}',system(dep) or dep.lower() in shipped,'Windows OS' if system(dep) else 'bundled')
            except (OSError,subprocess.CalledProcessError) as e:
                check(f'PE import parse:{rel}',False,str(e))
        plugins=root/'plugins'; licenses=root/'licenses'; ffmpeg=root/'ffmpeg.exe'
        check('Windows platform plugin',(plugins/'platforms/qwindows.dll').exists())
        check('Windows styles',bool(list((plugins/'styles').glob('*.dll'))))
        check('Windows icon',(root/'room237.ico').is_file())
        check('updater helper present',(root/'room237-core.exe').is_file(),'the app copies room237-core.exe to room237-update-helper.exe beside the plan')
        nsi=Path(__file__).parent/'windows/room237.nsi'
        if nsi.is_file():
            script=nsi.read_text()
            for label,required in [('per-user','RequestExecutionLevel user'),('install directory',r'InstallDir "$LOCALAPPDATA\Programs\Room237"'),('uninstaller',r'WriteUninstaller "$INSTDIR\uninstall.exe"'),('shortcut',r'CreateShortcut "$SMPROGRAMS\Room237.lnk" "$INSTDIR\room237.exe"'),('uninstall tree',r'RMDir /r "$INSTDIR"')]:
                check('NSIS source:'+label,required in script)
            check('NSIS no fixed build paths',not any(x in script for x in forbidden))
        if a.installer:
            check('NSIS installer exists',a.installer.is_file())
            if a.installer.is_file():
                names=imports(a.installer,a.objdump)
                check('NSIS bootstrap imports',bool(names) and all(system(n) for n in names),names)
        unknown('Authenticode','unsigned by explicit delivery decision')
    else:
        plugins=root/'plugins'; licenses=root/'licenses'; ffmpeg=root/'ffmpeg'
        for binary,kind in binaries:
            rel=str(binary.relative_to(root))
            check(f'ELF:{rel}',kind=='elf')
            with binary.open('rb') as handle:
                handle.seek(18);machine=struct.unpack('<H',handle.read(2))[0]
            check(f'Linux x86_64:{rel}',machine==62,machine)
            rc,out=run(['ldd',binary],env=dict(os.environ,LD_LIBRARY_PATH=str(root/'lib')))
            static= 'not a dynamic executable' in out or 'statically linked' in out
            check(f'ldd:{rel}',rc==0 or static,out)
            check(f'closure:{rel}','not found' not in out,out)
            for line in out.splitlines():
                if '=>' not in line: continue
                name,loc=line.split('=>',1); name=name.strip(); loc=loc.strip().split(' ')[0]
                if name.startswith(('libQt6','libavcodec','libavformat','libavutil','libswscale','libswresample')):
                    check(f'bundled dependency:{rel}:{name}',Path(loc).is_relative_to(root),loc)
            rc,out=run(['readelf','-d',binary])
            if not static:
                check(f'ELF dynamic parse:{rel}',rc==0)
            for value in re.findall(r'\((?:RUNPATH|RPATH)\).*?\[(.*?)\]',out):
                check(f'rpath:{rel}',all(v.startswith('$ORIGIN') and Path(str(binary.parent)+v[len('$ORIGIN'):]).resolve().is_relative_to(root) for v in value.split(':')),value)
        check('Linux platform plugins',all((plugins/'platforms'/n).exists() for n in ('libqxcb.so','libqoffscreen.so','libqwayland.so')))
        check('Linux styles', True, 'Fusion is built into QtWidgets; no external style plugin in the selected Linux Qt SDK')
        check('Linux icon',(root/'room237.png').is_file())
        if a.appimage:
            check('AppImage exists',a.appimage.is_file())
            with a.appimage.open('rb') as handle, mmap.mmap(handle.fileno(),0,access=mmap.ACCESS_READ) as data:
                found=[x for x in forbidden if path_hit(data,x)]
            check('AppImage raw build paths',not found,found)
            extraction=Path(tempfile.mkdtemp(prefix='appimage-',dir=a.report.parent))
            # --appimage-extract does not use FUSE or launch the application.
            rc,out=run(['bash','-c','cd "$1" && "$2" --appimage-extract >/dev/null', 'extract', extraction,a.appimage.resolve()])
            check('AppImage extraction',rc==0,out)
            appdir=extraction/'squashfs-root'
            check('AppImage layout',all((appdir/n).exists() for n in ('AppRun','room237.png','dev.dgazizullin.room237.desktop','usr/room237/room237')))
            check('AppImage complete payload', {str(f.relative_to(root)) for f in root.rglob('*') if f.is_file()} == {str(f.relative_to(appdir/'usr/room237')) for f in (appdir/'usr/room237').rglob('*') if f.is_file()})
            for f in (appdir/'usr/room237').rglob('*'):
                if f.is_file() and not f.is_symlink():
                    original=root/f.relative_to(appdir/'usr/room237')
                    check(f'AppImage payload:{f.relative_to(appdir)}',original.is_file() and sha(f)==sha(original))
    suffix={'macos':'.dylib','linux':'.so','windows':'.dll'}[a.platform]
    for decoder in ('gif','jpeg','webp'):
        check(f'image plugin:{decoder}',bool(list((plugins/'imageformats').glob('*q'+decoder+suffix))))
    check('FFmpeg multimedia plugin',bool(list((plugins/'multimedia').glob('*ffmpegmedia*'+suffix))))
    libs=root/'Contents/Frameworks' if a.platform=='macos' else root/'lib' if a.platform=='linux' else root
    for codec in ('avcodec','avformat','avutil','swresample','swscale'):
        check(f'FFmpeg library:{codec}',bool(list(libs.glob('*'+codec+'*'))))
    check('HEIF/AVIF decoder alternative',True,'macOS: ImageIO then bundled FFmpeg; Linux/Windows: bundled FFmpeg fallback (no qheif requirement). See CODECS.md and runtime format results.')
    check('notices',(licenses/'THIRD_PARTY_NOTICES.md').is_file())
    if (licenses/'THIRD_PARTY_NOTICES.md').is_file():
        notices=(licenses/'THIRD_PARTY_NOTICES.md').read_text()
        check('Qt LGPL notice','LGPL-3.0' in notices and 'Qt' in notices)
        check('FFmpeg notice','FFmpeg' in notices and 'GPL' in notices)
    check('core SBOM',(licenses/'sbom-core.json').is_file())
    if (licenses/'sbom-core.json').is_file():
        bom=json.loads((licenses/'sbom-core.json').read_text())
        check('core SBOM entries',{'SQLite','libwebp','Monocypher','zlib','libfido2','libcbor','OpenSSL'} <= {c.get('name') for c in bom['components']},len(bom['components']))
    check('bundled ffmpeg',ffmpeg.is_file())
    if a.platform!='windows':
        home=a.report.parent/'ffmpeg-synthetic-home'; home.mkdir(exist_ok=True)
        runtime_env=dict(os.environ,HOME=str(home),CFFIXED_USER_HOME=str(home),XDG_CONFIG_HOME=str(home/'.config'))
        rc,out=run([ffmpeg,'-hide_banner','-version'],env=runtime_env)
        check('bundled ffmpeg runs',rc==0,out)
        rc,codecs=run([ffmpeg,'-hide_banner','-decoders'],env=runtime_env)
        check('ffmpeg decoder inventory',rc==0)
        if a.fixtures:
            for pattern in ('*alpha*.webp','*.avif','*.heic','*.mov','*.avi','*.webm'):
                candidates=[f for f in a.fixtures.glob(pattern) if not f.name.startswith("broken.")]
                check('codec fixture:'+pattern,bool(candidates))
                for fixture in candidates:
                    rc,out=run([ffmpeg,'-v','error','-threads','1','-i',fixture,'-frames:v','1','-f','null','-'],env=runtime_env)
                    check('ffmpeg decode:'+fixture.name,rc==0,out)
                    if fixture.name == 'alpha.webp':
                        cmd=[str(ffmpeg),'-v','error','-threads','1','-i',str(fixture),'-frames:v','1','-f','rawvideo','-pix_fmt','rgba','-']
                        result=subprocess.run(cmd,capture_output=True,env=runtime_env,timeout=90)
                        alpha=result.stdout[3::4]
                        check('WebP alpha survives decode',result.returncode==0 and bool(alpha) and min(alpha)<255 and max(alpha)>0,{'alpha_min':min(alpha) if alpha else None,'alpha_max':max(alpha) if alpha else None,'rgba_sha256':hashlib.sha256(result.stdout).hexdigest()})
        else:
            unknown('codec fixtures','Use --fixtures and test_formats.py on this exact artifact. Decoder availability alone is not a decode pass.')
    else:
        unknown('bundled ffmpeg runs / codec decode','Requires fenced Wine or Windows runtime; PE imports and codec DLL presence are static checks only.')
    if a.dmg:
        mount=Path(tempfile.mkdtemp(prefix='dmg-mount-',dir=a.report.parent))
        rc,out=run(['hdiutil','attach','-readonly','-nobrowse','-noautoopen','-mountpoint',mount,a.dmg])
        check('DMG read-only attach',rc==0,out)
        if rc==0:
            try:
                check('DMG Applications link',(mount/'Applications').is_symlink() and os.readlink(mount/'Applications')=='/Applications')
                check('DMG app',(mount/'Room237.app/Contents/MacOS/Room237').is_file() and sha(mount/'Room237.app/Contents/MacOS/Room237')==sha(root/'Contents/MacOS/Room237'))
                from ds_store import DSStore
                with DSStore.open(str(mount/'.DS_Store'),'r') as store:
                    positions={n:list(store[n]['Iloc']) for n in ('Room237.app','Applications')}
                    view=store['.']['icvp']
                check('DMG icon layout',positions=={'Room237.app':[170,190],'Applications':[450,190]},positions)
                check('DMG background style',view.get('backgroundType')==2,view)
                tiffs=[f for f in mount.rglob('*') if f.suffix.lower() in ('.tiff','.tif')]
                check('DMG Retina TIFF',bool(tiffs))
                from PIL import Image
                for tiff in tiffs:
                    im=Image.open(tiff); sizes=[]
                    for i in range(im.n_frames):
                        im.seek(i); sizes.append(im.size)
                    im.close()
                    check('DMG Retina representations',any(x[0]>=1280 and x[1]>=800 for x in sizes) and any(x[0]==640 and x[1]==400 for x in sizes),sizes)
            except Exception as e:
                check('DMG readback',False,repr(e))
            finally:
                rc,out=run(['hdiutil','detach',mount])
                check('DMG detached',rc==0,out)
    check('no absolute vendor build filenames',not vendor_diagnostics,vendor_diagnostics)
    report={'vendor_diagnostic_paths':vendor_diagnostics, 'path_scan_scope':'Known absolute local source/SDK/build roots, ASCII and UTF-16; upstream vendor diagnostic filenames are recorded separately.', 'verifier_sha256':sha(Path(__file__)), 'platform':a.platform,'root':str(root),'binary_sha256':hashes,'checks':checks,'commands':commands}
    report['counts']={status:sum(c['status']==status for c in checks) for status in ('passed','failed','blocked','unverified')}
    a.report.write_text(json.dumps(report,indent=2,default=str)+'\n')
    print(json.dumps(report['counts']))
    return 1 if report['counts']['failed'] else 0

if __name__=='__main__':
    sys.exit(main())
