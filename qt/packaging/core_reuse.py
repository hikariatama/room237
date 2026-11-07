#!/usr/bin/env python3
"""Explicit reuse of the room237-core binaries of an accepted release for a client-only release.

prepare H_WORK OUT_DIR   copies the core of every platform/version out of the accepted release H_WORK,
                         proves each copy against the shipped archives and writes OUT_DIR/manifest.json.
The release driver validates that manifest (validate()) and refuses on any difference: core sources, Cargo files,
version, update trust key, file SHA-256. No cache is consulted; only the files the manifest names are used.
"""
import hashlib, io, json, os, re, shutil, subprocess, sys, tarfile, zipfile
from pathlib import Path

PLATFORMS = {
    # platform: (path in label dir, archive, member, cargo target in the accepted build script)
    'macos-arm64': ('macos/arm64/Room237.app/Contents/MacOS/room237-core', 'macos/Room237-{v}-macos-arm64.update.tar.gz', './Contents/MacOS/room237-core', 'aarch64-apple-darwin'),
    'macos-x86_64': ('macos/x86_64/Room237.app/Contents/MacOS/room237-core', 'macos/Room237-{v}-macos-x86_64.update.tar.gz', './Contents/MacOS/room237-core', 'x86_64-apple-darwin'),
    'linux-x86_64': ('linux/stage/room237/room237-core', 'linux/Room237-{v}-linux-x86_64.tar.gz', 'room237/room237-core', 'x86_64-unknown-linux-gnu (container default)'),
    'windows-x86_64': ('windows/stage/room237-core.exe', 'windows/Room237-{v}-windows-x86_64-portable.zip', 'Room237/room237-core.exe', 'x86_64-pc-windows-gnullvm'),
}
CORE_PREFIXES = ('qt/core/',)


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def member_sha(archive, member):
    if archive.suffix == '.zip':
        with zipfile.ZipFile(archive) as z:
            return hashlib.sha256(z.read(member)).hexdigest()
    with tarfile.open(archive) as t:
        f = t.extractfile(member)
        if f is None:
            raise RuntimeError(f'{member} missing in {archive}')
        return hashlib.sha256(f.read()).hexdigest()


def core_entries(inventory):
    return {r['path']: r.get('sha256', r.get('link')) for r in inventory if r['path'].startswith(CORE_PREFIXES)}


def prepare(h_work, out):
    h_work, out = Path(h_work).resolve(strict=True), Path(out).absolute()
    if out.exists():
        raise SystemExit('output must be new')
    if not (h_work / 'COMPLETE').read_text().startswith('final'):
        raise SystemExit('accepted release must be a completed final build')
    release = json.loads((h_work / 'RELEASE.json').read_text())
    trust = release['updateTrust']['embeddedPublicKey']
    qa_key = (h_work / 'n/qa-feed-public-key.txt').read_text().strip().splitlines()[-1]
    inventories = {'n': json.loads((h_work / 'source-inventory.json').read_text()), 'n1': json.loads((h_work / 'source-n1-inventory.json').read_text())}
    commands = {c['name']: c for c in json.loads((h_work / 'commands.json').read_text())}
    toolchain = json.loads((h_work / 'toolchain.json').read_text())
    scripts = {name: (h_work / 'source/qt/packaging' / name).read_text() for name in ('macos/build.sh', 'linux/build.sh', 'windows/build.sh')}
    cargo_lines = {name: [l.strip() for l in text.splitlines() if 'cargo build' in l] for name, text in scripts.items()}
    out.mkdir(parents=True)
    manifest = {
        'operation': 'reuse accepted room237-core binaries; client-only rebuild',
        'acceptedWork': str(h_work),
        'acceptedSourceSha256': (h_work / 'SOURCE_SHA256').read_text().strip(),
        'acceptedSourceN1Sha256': (h_work / 'SOURCE_N1_SHA256').read_text().strip(),
        'version': release['version'], 'n1Version': release['n1_version'],
        'trustPublicKey': trust,
        'coreSources': {label: core_entries(inv) for label, inv in inventories.items()},
        'acceptedBuild': {'cargoCommands': cargo_lines, 'buildScriptSha256': {k: hashlib.sha256(v.encode()).hexdigest() for k, v in scripts.items()},
                          'cargo': toolchain.get('cargo'), 'rustc': toolchain.get('rustc'), 'linuxImage': toolchain.get('linux_image'),
                          'rustflags': '--remap-path-prefix=<work>=/room237-source --remap-path-prefix=<home>=/builder (release_driver)',
                          'env': 'ROOM237_VERSION=<version> ROOM237_UPDATE_PUBKEY=<trust> ROOM237_SBOM=<sbom>',
                          'exitCodes': {k: v['exit_code'] for k, v in commands.items() if re.match(r'n1?-(macos|linux|windows)', k)}},
        'binaries': {},
    }
    if inventories['n'] and core_entries(inventories['n']) != core_entries(inventories['n1']):
        raise SystemExit('accepted N and N+1 core sources differ')
    for label, version in (('n', release['version']), ('n1', release['n1_version'])):
        for platform, (rel, archive, member, target) in PLATFORMS.items():
            source = h_work / label / rel
            sha = digest(source)
            packaged = member_sha(h_work / label / archive.format(v=version), member)
            if packaged != sha:
                raise SystemExit(f'{label} {platform}: staged core differs from the shipped archive')
            data = source.read_bytes()
            exact = lambda v: re.search(rb'(?<![0-9.])' + re.escape(v.encode()) + rb'(?![0-9])', data) is not None
            checks = {'version': exact(version), 'otherVersionAbsent': not exact(release['n1_version'] if label == 'n' else release['version']),
                      'trustKey': trust.encode() in data, 'qaKeyAbsent': qa_key.encode() not in data}
            if not all(checks.values()):
                raise SystemExit(f'{label} {platform}: embedded checks failed {checks}')
            dest = out / label / platform / Path(rel).name
            dest.parent.mkdir(parents=True)
            shutil.copy2(source, dest)
            if digest(dest) != sha:
                raise SystemExit('copy changed bytes')
            kind = subprocess.run(['file', '-b', str(dest)], capture_output=True, text=True).stdout.strip()
            manifest['binaries'].setdefault(label, {})[platform] = {'file': str(dest.relative_to(out)), 'sha256': sha, 'size': dest.stat().st_size,
                'acceptedPath': str(source), 'shippedArchive': str(h_work / label / archive.format(v=version)), 'shippedMember': member,
                'shippedMemberSha256': packaged, 'cargoTarget': target, 'fileType': kind, 'embedded': checks}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(out / 'manifest.json')


def validate(manifest_path, snapshot_inventory, label, version, trust, platform):
    """Return (binary path, sha256) for one platform or raise. snapshot_inventory: inventory records of the new snapshot."""
    manifest_path = Path(manifest_path).resolve(strict=True)
    m = json.loads(manifest_path.read_text())
    expected_version = m['version'] if label == 'n' else m['n1Version']
    if version != expected_version:
        raise RuntimeError(f'reuse-core: version {version} differs from accepted {expected_version}')
    if trust != m['trustPublicKey']:
        raise RuntimeError('reuse-core: update trust key differs from the accepted core')
    mine = core_entries(snapshot_inventory)
    if mine != m['coreSources'][label]:
        changed = sorted(set(mine.items()) ^ set(m['coreSources'][label].items()))
        raise RuntimeError(f'reuse-core: core sources differ from the accepted build: {changed[:5]}')
    entry = m['binaries'][label][platform]
    path = manifest_path.parent / entry['file']
    if digest(path) != entry['sha256']:
        raise RuntimeError(f'reuse-core: {path} does not match its manifest SHA-256')
    return path, entry['sha256']


def share_identical(dest, reference, journal):
    """Make byte-identical regular files at the same relative path share blocks with the accepted release (APFS clone).
    Content, mode and path stay as they are; each replacement is verified before it takes effect."""
    dest, reference = Path(dest), Path(reference)
    shared = 0
    with open(journal, 'a') as log:
        for file in sorted(dest.rglob('*')):
            if file.is_symlink() or not file.is_file():
                continue
            rel = file.relative_to(dest)
            ref = reference / rel
            if not ref.exists() and rel.parts[0].startswith('appimage-'):
                # The verifier extracts each AppImage into a randomly named directory.
                twins = sorted(reference.glob('appimage-*'))
                if len(twins) == 1:
                    ref = twins[0].joinpath(*rel.parts[1:])
            if ref.is_symlink() or not ref.is_file() or ref.stat().st_size != file.stat().st_size or (ref.stat().st_mode & 0o7777) != (file.stat().st_mode & 0o7777):
                continue
            sha = digest(file)
            if digest(ref) != sha:
                continue
            temporary = file.with_name(file.name + '.room237-share')
            subprocess.run(['/bin/cp', '-c', '-p', str(ref), str(temporary)], check=True)
            if digest(temporary) != sha:
                temporary.unlink()
                raise RuntimeError(f'clone differs: {file}')
            os.chmod(temporary, file.stat().st_mode & 0o7777)
            os.replace(temporary, file)
            shared += file.stat().st_size
            log.write(json.dumps({'file': str(file), 'sharesBlocksWith': str(ref), 'sha256': sha, 'size': file.stat().st_size}) + '\n')
    return shared


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'prepare':
        prepare(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
