#!/usr/bin/env python3
"""Snapshot -> N/N+1 packages -> signed loopback QA feed. Publishes nothing.

macOS packages are built on macOS, Linux packages on Linux (Ubuntu 22.04 or the same glibc baseline), Windows packages
on Linux or macOS with llvm-mingw. Every toolchain path is an explicit argument or environment variable.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
sys.dont_write_bytecode = True
from pathlib import Path

FEED_KEYS = {'arm64': 'darwin-aarch64', 'x86_64': 'darwin-x86_64'}


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def inventory(root):
    records = []
    for p in sorted(root.rglob('*')):
        rel = str(p.relative_to(root))
        if p.is_symlink():
            if not p.resolve().is_relative_to(root.resolve()):
                raise RuntimeError(f'external source symlink: {rel}')
            records.append({'path': rel, 'link': os.readlink(p)})
        elif p.is_file():
            records.append({'path': rel, 'sha256': digest(p), 'executable': bool(p.stat().st_mode & 0o111)})
    data = (json.dumps(records, ensure_ascii=False, sort_keys=True, indent=1) + '\n').encode()
    return data, hashlib.sha256(data).hexdigest()


def tree_inventory(location):
    entries = []
    for file in sorted(location.rglob('*')):
        if file.is_symlink():
            entries.append({'path': str(file.relative_to(location)), 'link': os.readlink(file)})
        elif file.is_file():
            entries.append({'path': str(file.relative_to(location)), 'sha256': digest(file)})
    return (json.dumps(entries, sort_keys=True, indent=1) + '\n').encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='snapshot or project root containing qt/')
    parser.add_argument('--work', type=Path, required=True, help='new, empty local output directory')
    parser.add_argument('--platform', choices=['macos', 'linux', 'windows'], required=True)
    parser.add_argument('--arch', choices=['arm64', 'x86_64'], action='append', help='macOS architecture; repeat for both (default: both)')
    parser.add_argument('--mode', choices=['rehearsal', 'final'], default='rehearsal')
    parser.add_argument('--freeze-hash', help='required for final: SOURCE_SHA256 printed by a rehearsal of the same tree')
    parser.add_argument('--banner', type=Path, help='multi-resolution DMG background TIFF from macos/make_banner.py')
    parser.add_argument('--key', type=Path, required=True, help='local QA feed key; never embedded or shipped')
    parser.add_argument('--pubkey', type=Path, required=True, help='public half of --key')
    parser.add_argument('--trust-pubkey', type=Path, help='minisign public key the app trusts; default: the production key in qt/core/src/update.rs')
    parser.add_argument('--feed-url', default='http://127.0.0.1:8765', help='loopback feed URL; no server is started')
    parser.add_argument('--linux-qt', type=Path, help='Qt for Linux gcc_64 directory (the one with bin/ lib/ plugins/)')
    parser.add_argument('--deps', type=Path, help='Linux extras: ffmpeg-linux-x86_64, appimagetool, runtime-x86_64')
    parser.add_argument('--ffmpeg-arm64', type=Path)
    parser.add_argument('--ffmpeg-x86_64', type=Path)
    parser.add_argument('--n1-version')
    parser.add_argument('--fixtures', type=Path, help='synthetic codec corpus for packaged FFmpeg verification')
    parser.add_argument('--cache-root', type=Path, help='optional owned build cache; do not share with a concurrent build')
    parser.add_argument('--deps-record', type=Path, action='append', default=[], help='deps-installed.json from ci/fetch_deps.py, copied into toolchain.json')
    parser.add_argument('--min-free-gib', type=float, default=float(os.environ.get('ROOM237_MIN_FREE_GIB', 15)), help='stop when free disk space falls below this (default 15)')
    parser.add_argument('--jobs', type=int, default=int(os.environ.get('ROOM237_BUILD_JOBS', 6)))
    parser.add_argument('--reuse-core', type=Path, help='core_reuse.py manifest: ship the accepted room237-core binaries instead of compiling; refuses on any source/version/key/SHA difference')
    parser.add_argument('--qa', action='store_true', help='also build test variants with the QA driver under n/qa and n1/qa (macOS, Linux); they are never shipped')
    parser.add_argument('--share-identical-with', type=Path, help='accepted release work: byte-identical outputs at the same path share blocks with it (APFS clone)')
    args = parser.parse_args()
    source, work = args.source.resolve(strict=True), args.work.absolute()
    host = platform.system()
    if not (source / 'qt/VERSION').is_file():
        parser.error('--source must contain qt/VERSION')
    if args.mode == 'final' and not args.freeze_hash:
        parser.error('final requires --freeze-hash from a rehearsal')
    if args.platform == 'macos' and host != 'Darwin':
        parser.error('macOS packages are built on macOS')
    if args.platform == 'linux' and host != 'Linux':
        parser.error('Linux packages are built on Linux')
    if args.platform == 'windows' and not shutil.which('makensis'):
        parser.error('makensis (NSIS 3) must be on PATH')
    if work.exists():
        parser.error('--work must be new; old artifacts are preserved')
    work.parent.mkdir(parents=True, exist_ok=True)
    if work.parent.resolve() != work.parent or work.is_relative_to(source):
        parser.error('work must be local, canonical and outside source')
    if not args.feed_url.startswith(('http://127.0.0.1:', 'http://localhost:', 'http://[::1]:')):
        parser.error('--feed-url must be loopback (local-only release)')
    arches = args.arch or ['arm64', 'x86_64']
    work.mkdir()
    records = []
    cache_root = args.cache_root.resolve() if args.cache_root else work / 'cache'
    if cache_root.is_relative_to(source):
        parser.error('cache must be outside source')
    jobs = str(args.jobs)
    env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL=jobs, CARGO_BUILD_JOBS=jobs, ROOM237_BUILD_JOBS=jobs, PYTHONDONTWRITEBYTECODE='1')
    # The shipped app trusts the production updater key; --key/--pubkey only sign the loopback QA feed.
    production = re.search(r'None => "(RW[A-Za-z0-9+/=]+)"', (source / 'qt/core/src/update.rs').read_text()).group(1)
    trust = args.trust_pubkey.read_text().strip().splitlines()[-1] if args.trust_pubkey else production
    env['ROOM237_UPDATE_PUBKEY'] = trust
    reserve = int(args.min_free_gib * 1024**3)

    def disk():
        free = shutil.disk_usage(work).free
        with (work / 'disk.jsonl').open('a') as log:
            log.write(json.dumps({'time': time.time(), 'free_bytes': free}) + '\n')
        if free < reserve:
            raise RuntimeError(f'disk reserve: {free} bytes free; need {args.min_free_gib:g} GiB')

    def run(name, command, extra=None):
        disk()
        start = time.monotonic()
        print(f'{args.mode}: {name}', flush=True)
        with (work / f'{name}.log').open('w') as log:
            result = subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT, env=env | (extra or {}))
        shown = ['<LOCAL_QA_KEY>' if str(x) == str(args.key) else str(x) for x in command]
        records.append({'name': name, 'command': shown, 'exit_code': result.returncode, 'wall_seconds': round(time.monotonic() - start, 3)})
        (work / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
        if result.returncode:
            raise RuntimeError(f'{name} exited {result.returncode}; see {work / (name + ".log")}')

    disk()
    snap = work / 'source'
    (snap / 'qt').mkdir(parents=True)
    run('snapshot-rsync', ['rsync', '-a', '--exclude=target', '--exclude=__pycache__', '--exclude=.DS_Store', str(source / 'qt') + '/', str(snap / 'qt') + '/'])
    data, source_hash = inventory(snap)
    (work / 'source-inventory.json').write_bytes(data)
    (work / 'SOURCE_SHA256').write_text(source_hash + '\n')
    if args.freeze_hash and args.freeze_hash != source_hash:
        raise RuntimeError(f'source hash differs from freeze: {source_hash}')
    version = (snap / 'qt/VERSION').read_text().strip()
    parts = version.split('.')
    n1 = args.n1_version or '.'.join(parts[:-1] + [str(int(parts[-1]) + 1)])
    if n1 == version:
        raise RuntimeError('N+1 version must differ')
    reuse = {'manifest': str(args.reuse_core.resolve(strict=True)), 'manifestSha256': digest(args.reuse_core)} if args.reuse_core else None
    (work / 'RELEASE.json').write_text(json.dumps({'mode': args.mode, 'platform': args.platform, 'arch': arches if args.platform == 'macos' else ['x86_64'], 'reuseCore': reuse, 'shareIdenticalWith': str(args.share_identical_with) if args.share_identical_with else None, 'source_sha256': source_hash, 'version': version, 'n1_version': n1, 'host': platform.platform(), 'signature': 'macOS ad-hoc only; no Developer ID/notarization; Windows unsigned', 'updateTrust': {'embeddedPublicKey': trust, 'source': 'qt/core/src/update.rs production default' if not args.trust_pubkey else str(args.trust_pubkey), 'localFeed': 'qt-update.json in n/n1 is signed with the local QA key (qa-feed-public-key.txt); the shipped app rejects it outside fenced QA fixtures. The release feed is written by assemble_release.py and signed with the production key.'}}, indent=2) + '\n')
    repro = ' '.join(shlex.quote(x) for x in sys.argv[1:])
    (work / 'REPRODUCE.txt').write_text('Use a NEW --work directory. Dependencies are inventoried in toolchain.json.\n' + 'qt/packaging/release_all.sh ' + repro.replace(str(args.key), '<LOCAL_QA_KEY>') + '\n')
    sys.path.insert(0, str(snap / 'qt/packaging'))
    import sign_update
    keyid, private = sign_update.load(args.key)
    if sign_update.public_b64(keyid, private) != args.pubkey.read_text().strip():
        raise RuntimeError('public key does not match local signing key')
    if sign_update.public_b64(keyid, private) == trust:
        raise RuntimeError('the QA feed key must not be the key the app trusts')
    del private
    inputs = [p for p in (args.ffmpeg_arm64, args.ffmpeg_x86_64, args.banner) if p and p.is_file()]
    toolchain = {'host': platform.platform(), 'environment': {k: env[k] for k in ('QT_ROOT', 'QT_HOST', 'QT_WIN', 'LLVM_MINGW', 'TOOLCHAIN', 'FFMPEG_WIN') if k in env}, 'files': {str(p.resolve()): digest(p) for p in inputs}, 'pinnedInputs': {}}
    if 'FFMPEG_WIN' in env and Path(env['FFMPEG_WIN']).is_file():
        toolchain['files'][str(Path(env['FFMPEG_WIN']).resolve())] = digest(Path(env['FFMPEG_WIN']))
    if args.deps:
        toolchain['files'].update({str(p.resolve()): digest(p) for p in args.deps.iterdir() if p.is_file()})
    for record in args.deps_record:
        toolchain['pinnedInputs'].update(json.loads(record.read_text()))
    for exe in ['cargo', 'rustc', 'cmake', 'ninja', 'python3', 'makensis']:
        if shutil.which(exe):
            result = subprocess.run([exe, '-VERSION' if exe == 'makensis' else '--version'], capture_output=True, text=True)
            toolchain[exe] = (result.stdout + result.stderr).strip()
    if args.platform == 'linux' and (not args.linux_qt or not args.deps):
        raise RuntimeError('Linux requires --linux-qt and --deps')
    sdk_dirs = [(k, Path(env[k])) for k in ('QT_ROOT', 'QT_WIN', 'LLVM_MINGW') if k in env] + ([('LINUX_QT', args.linux_qt)] if args.linux_qt else [])
    for name, location in sdk_dirs:
        contents = tree_inventory(location)
        (work / f'toolchain-{name}.json').write_bytes(contents)
        toolchain[name + '_inventory_sha256'] = hashlib.sha256(contents).hexdigest()
    (work / 'toolchain.json').write_text(json.dumps(toolchain, indent=2) + '\n')
    n1snap = work / 'source-n1'
    shutil.copytree(snap, n1snap, symlinks=True)
    (n1snap / 'qt/VERSION').write_text(n1 + '\n')
    n1data, n1hash = inventory(n1snap)
    (work / 'source-n1-inventory.json').write_bytes(n1data)
    (work / 'SOURCE_N1_SHA256').write_text(n1hash + '\n')
    import core_reuse
    inventories = {'n': json.loads(data), 'n1': json.loads(n1data)}

    def reused(label, ver, platform_key):
        path, sha = core_reuse.validate(args.reuse_core, inventories[label], label, ver, trust, platform_key)
        dest = work / 'core-inputs' / label / platform_key / path.name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dest)
        if digest(dest) != sha:
            raise RuntimeError('reuse-core: copied input differs')
        with (work / 'core-inputs.jsonl').open('a') as log:
            log.write(json.dumps({'label': label, 'platform': platform_key, 'file': str(dest), 'sha256': sha, 'from': str(path)}) + '\n')
        return {'ROOM237_CORE_BINARY': str(dest), 'ROOM237_CORE_SHA256': sha}

    def share(label, target):
        if args.share_identical_with:
            ref = args.share_identical_with.resolve(strict=True) / label / target
            disk()
            saved = core_reuse.share_identical(work / label / target, ref, work / 'shared-with-accepted.jsonl')
            records.append({'name': f'{label}-share-{target}', 'command': ['core_reuse.share_identical', str(work / label / target), str(ref)], 'exit_code': 0, 'shared_bytes': saved})
            (work / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
            disk()

    home = Path.home()
    forbid = ['--forbid', work, '--forbid', source]
    for label, src, ver in [('n', snap, version), ('n1', n1snap, n1)]:
        out = work / label
        out.mkdir()
        sbom = out / 'sbom-core.json'
        run(f'{label}-sbom', [sys.executable, src / 'qt/packaging/make_sbom.py', src / 'qt/core/Cargo.lock', sbom, 'Qt=6.11.3=LGPL-3.0-only', 'FFmpeg-Qt=7.1=LGPL-2.1-or-later'])
        (out / 'trust-public-key.txt').write_text(trust + '\n')
        shutil.copy2(args.pubkey, out / 'qa-feed-public-key.txt')
        shutil.copy2(src / 'qt/packaging/THIRD_PARTY_NOTICES.md', out / 'THIRD_PARTY_NOTICES.md')
        base = {'ROOM237_SBOM': str(sbom), 'RUSTFLAGS': f'--remap-path-prefix={work}=/room237-source --remap-path-prefix={home}=/builder'}
        target = args.platform
        dest = out / target
        dest.mkdir()
        entries = []
        if target == 'macos':
            if not args.banner or not args.banner.is_file():
                raise RuntimeError('macOS requires --banner TIFF (see macos/make_banner.py)')
            for arch in arches:
                ffmpeg = args.ffmpeg_arm64 if arch == 'arm64' else args.ffmpeg_x86_64
                if not ffmpeg:
                    raise RuntimeError(f'missing FFmpeg for macOS {arch}')
                flags = f'-ffile-prefix-map={work}=/room237-source -ffile-prefix-map={env["QT_ROOT"]}=/qt-sdk'
                extra = base | {'CORE_TARGET_DIR': str(cache_root / f'core-macos-{arch}'), 'FFMPEG_BIN': str(ffmpeg), 'CXXFLAGS': flags}
                if args.reuse_core:
                    extra |= reused(label, ver, f'macos-{arch}')
                run(f'{label}-macos-{arch}', ['zsh', src / 'qt/packaging/macos/build.sh', arch, dest], extra)
                run(f'{label}-dmg-{arch}', [sys.executable, src / 'qt/packaging/macos/make_dmg.py', dest / arch / 'Room237.app', args.banner, dest / f'Room237-{ver}-macos-{arch}.dmg', 'Room237'])
                run(f'{label}-verify-macos-{arch}', [sys.executable, src / 'qt/packaging/verify_packages.py', '--platform', 'macos', '--root', dest / arch / 'Room237.app', '--dmg', dest / f'Room237-{ver}-macos-{arch}.dmg', '--report', out / f'verify-macos-{arch}.json', *forbid, '--forbid', env['QT_ROOT'], *(['--fixtures', args.fixtures.resolve()] if args.fixtures else [])])
                entries.append((FEED_KEYS[arch], 'tar.gz', dest / f'Room237-{ver}-macos-{arch}.update.tar.gz'))
                if args.qa:
                    (out / 'qa' / target).mkdir(parents=True, exist_ok=True)
                    run(f'{label}-macos-{arch}-qa', ['zsh', src / 'qt/packaging/macos/build.sh', arch, out / 'qa' / target], extra | {'ROOM237_QA': '1'})
        elif target == 'windows':
            flags = f'-ffile-prefix-map={work}=/room237-source -ffile-prefix-map={env["QT_WIN"]}=/qt-sdk -ffile-prefix-map={env["LLVM_MINGW"]}=/compiler-sdk'
            wextra = base | {'CORE_TARGET_DIR': str(cache_root / 'core-windows'), 'CXXFLAGS': flags}
            if args.reuse_core:
                wextra |= reused(label, ver, 'windows-x86_64')
            run(f'{label}-windows', ['zsh', src / 'qt/packaging/windows/build.sh', dest], wextra)
            objdump = Path(env['LLVM_MINGW']) / 'bin/llvm-objdump'
            wforbid = [*forbid, '--forbid', env['QT_WIN'], '--forbid', env['LLVM_MINGW']]
            run(f'{label}-verify-windows', [sys.executable, src / 'qt/packaging/verify_packages.py', '--platform', 'windows', '--root', dest / 'stage', '--objdump', objdump, '--report', out / 'verify-windows.json', *wforbid])
            run(f'{label}-nsis', ['makensis', f'-DSTAGE={dest / "stage"}', f'-DSRCDIR={src / "qt"}', f'-DVERSION={ver}', f'-DOUTFILE={dest}/Room237-{ver}-windows-x86_64-setup.exe', src / 'qt/packaging/windows/room237.nsi'])
            run(f'{label}-verify-installer', [sys.executable, src / 'qt/packaging/verify_packages.py', '--platform', 'windows', '--root', dest / 'stage', '--installer', dest / f'Room237-{ver}-windows-x86_64-setup.exe', '--objdump', objdump, '--report', out / 'verify-windows-installer.json', *wforbid])
            entries.append(('windows-x86_64', 'tar.gz', dest / f'Room237-{ver}-windows-x86_64.update.tar.gz'))
        else:
            cache = cache_root / 'linux'
            cache.mkdir(parents=True, exist_ok=True)
            lextra = base | {'ROOM237_SOURCE': str(src), 'ROOM237_BUILD_CACHE': str(cache), 'ROOM237_QT_LINUX': str(args.linux_qt.resolve()), 'ROOM237_DEPS': str(args.deps.resolve()),
                             'CXXFLAGS': f'-ffile-prefix-map={work}=/room237-source -ffile-prefix-map={args.linux_qt.resolve()}=/qt-sdk -ffile-prefix-map={home}=/builder'}
            if args.reuse_core:
                lextra |= reused(label, ver, 'linux-x86_64')
            run(f'{label}-linux', ['bash', src / 'qt/packaging/linux/build.sh', dest], lextra)
            run(f'{label}-verify-linux', [sys.executable, src / 'qt/packaging/verify_packages.py', '--platform', 'linux', '--root', dest / 'stage/room237', '--appimage', dest / f'Room237-{ver}-linux-x86_64.AppImage', '--report', out / 'verify-linux.json', *forbid, '--forbid', args.linux_qt.resolve(), '--forbid', cache, *(['--fixtures', args.fixtures.resolve()] if args.fixtures else [])])
            entries.append(('linux-x86_64', 'tar.gz', dest / f'Room237-{ver}-linux-x86_64.update.tar.gz'))
            entries.append(('linux-appimage-x86_64', 'appimage', dest / f'Room237-{ver}-linux-x86_64.AppImage'))
            if args.qa:
                (out / 'qa' / target).mkdir(parents=True)
                run(f'{label}-linux-qa', ['bash', src / 'qt/packaging/linux/build.sh', out / 'qa' / target], lextra | {'ROOM237_QA': '1'})
        share(label, target)
        specs = [f'{key}={kind},{args.feed_url.rstrip("/")}/{payload.relative_to(work).as_posix()},{payload}' for key, kind, payload in entries]
        run(f'{label}-sign-feed', [sys.executable, src / 'qt/packaging/sign_update.py', 'manifest', args.key, ver, out / 'qt-update.json', *specs])
        run(f'{label}-verify-feed', [sys.executable, src / 'qt/packaging/verify_feed.py', out / 'qt-update.json', out / 'qa-feed-public-key.txt', work])
        if inventory(src)[1] != (source_hash if label == 'n' else n1hash):
            raise RuntimeError('snapshot changed during build')
        sums = []
        for p in sorted(out.rglob('*')):
            if p.is_file() and not p.is_symlink() and not any(c in p.relative_to(out).parts for c in ('stage', 'qt', 'qt-arm64', 'qt-x86_64', 'arm64', 'x86_64', 'core', 'qa')):
                sums.append(f'{digest(p)}  {p.relative_to(out).as_posix()}')
        (out / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
    disk()
    for path, expected in toolchain['files'].items():
        if Path(path).is_absolute() and digest(Path(path)) != expected:
            raise RuntimeError('external input changed during build: ' + path)
    (work / 'COMPLETE').write_text(f'{args.mode}\nsource_sha256={source_hash}\n')
    print(f'{args.mode} completed: {work}', flush=True)


if __name__ == '__main__':
    main()
