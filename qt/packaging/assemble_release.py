#!/usr/bin/env python3
"""Collects the packages of every platform into one flat release directory and writes the release update feed.

  assemble_release.py --repo OWNER/REPO --tag TAG --out NEW_DIR [--key KEY | --unsigned] PACKAGE_DIR [PACKAGE_DIR ...]

PACKAGE_DIR holds one job's packages (ci/build.sh ROOT/upload/TARGET/packages, or WORK/n/<platform> together with
WORK/n). The output holds every installer and update payload, sbom-core.json, THIRD_PARTY_NOTICES.md,
trust-public-key.txt, qt-update.json with qt-update.json.minisig, and SHA256SUMS over all of them.

The feed lists one payload per platform key with its release download URL
https://github.com/OWNER/REPO/releases/download/TAG/<file>, size and SHA-256. --key is the production updater key
(see sign_update.py for formats and the password variable); the script refuses to sign unless the key matches
trust-public-key.txt, which must be identical in every PACKAGE_DIR and equal the key compiled into the packages.
--unsigned writes the feed without a signature, to check the layout without the key.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sign_update  # noqa: E402

COMMON = ('sbom-core.json', 'THIRD_PARTY_NOTICES.md', 'trust-public-key.txt')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def feed_entries(version, files):
    names = {p.name: p for p in files}
    table = [
        ('darwin-aarch64', 'tar.gz', f'Room237-{version}-macos-arm64.update.tar.gz'),
        ('darwin-x86_64', 'tar.gz', f'Room237-{version}-macos-x86_64.update.tar.gz'),
        ('linux-x86_64', 'tar.gz', f'Room237-{version}-linux-x86_64.update.tar.gz'),
        ('linux-appimage-x86_64', 'appimage', f'Room237-{version}-linux-x86_64.AppImage'),
        ('windows-x86_64', 'tar.gz', f'Room237-{version}-windows-x86_64.update.tar.gz'),
    ]
    missing = [name for _, _, name in table if name not in names]
    if missing:
        raise SystemExit('missing update payloads: ' + ', '.join(missing))
    return [(key, kind, names[name]) for key, kind, name in table]


def expected_files(version):
    return {f'Room237-{version}-macos-arm64.dmg', f'Room237-{version}-macos-x86_64.dmg', f'Room237-{version}-linux-x86_64.AppImage',
            f'room237_{version}_amd64.deb', f'Room237-{version}-linux-x86_64.tar.gz', f'Room237-{version}-windows-x86_64-setup.exe',
            f'Room237-{version}-windows-x86_64-portable.zip'} | {
        f'Room237-{version}-{p}.update.tar.gz' for p in ('macos-arm64', 'macos-x86_64', 'linux-x86_64', 'windows-x86_64')}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--repo', required=True, help='OWNER/REPO')
    p.add_argument('--tag', required=True)
    p.add_argument('--version', help='default: qt/VERSION of this checkout')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--key', type=Path)
    p.add_argument('--unsigned', action='store_true')
    p.add_argument('--link', action='store_true', help='hard-link package files instead of copying when on one filesystem')
    p.add_argument('dirs', nargs='+', type=Path)
    a = p.parse_args()
    if bool(a.key) == a.unsigned:
        p.error('exactly one of --key and --unsigned')
    version = a.version or (HERE.parent / 'VERSION').read_text().strip()
    if a.tag != f'room237-v{version}':
        p.error(f'tag must be room237-v{version}')
    if a.out.exists():
        p.error('--out must be new')
    prefix = f'https://github.com/{a.repo}/releases/download/{a.tag}/'
    packages, common = {}, {}
    for d in a.dirs:
        for f in sorted(d.iterdir()):
            if not f.is_file() or f.is_symlink():
                continue
            if f.name in COMMON:
                if f.name in common and common[f.name].read_bytes() != f.read_bytes():
                    raise SystemExit(f'{f.name} differs between {common[f.name].parent} and {d}')
                common[f.name] = f
            elif f.name in expected_files(version):
                if f.name in packages:
                    raise SystemExit(f'{f.name} found twice')
                packages[f.name] = f
    missing = sorted(expected_files(version) - set(packages)) + sorted(set(COMMON) - set(common))
    if missing:
        raise SystemExit('missing release files: ' + ', '.join(missing))
    trust_file = common['trust-public-key.txt']
    compiled = re.search(r'kPublicKey = "(RW[A-Za-z0-9+/=]+)"', (HERE.parent / 'core/src/update.cpp').read_text()).group(1)
    if trust_file.read_text().strip() != compiled:
        raise SystemExit('trust-public-key.txt is not the updater key in qt/core/src/update.cpp')
    a.out.mkdir(parents=True)
    out = a.out.resolve()
    for f in list(packages.values()) + list(common.values()):
        target = out / f.name
        if a.link:
            try:
                os.link(f, target)
                continue
            except OSError:
                pass
        shutil.copy2(f, target)
    specs = [f'{key}={kind},{prefix}{payload.name},{out / payload.name}' for key, kind, payload in feed_entries(version, [out / n for n in packages])]
    feed = out / 'qt-update.json'
    if a.unsigned:
        feed.write_text(sign_update.manifest_text(version, specs))
    else:
        os.environ['ROOM237_EXPECT_PUBKEY'] = str(trust_file)
        subprocess.run([sys.executable, str(HERE / 'sign_update.py'), 'manifest', str(a.key), version, str(feed), *specs], check=True)
        subprocess.run([sys.executable, str(HERE / 'verify_feed.py'), str(feed), str(out / 'trust-public-key.txt'), str(out),
                        '--url-prefix', prefix, '--require-platforms'], check=True)
    lines = [f'{sha256(f)}  {f.name}' for f in sorted(out.iterdir()) if f.is_file() and f.name != 'SHA256SUMS']
    (out / 'SHA256SUMS').write_text('\n'.join(lines) + '\n')
    print(f'{len(lines)} files in {out}; feed {"unsigned" if a.unsigned else "signed and verified"}')


if __name__ == '__main__':
    main()
