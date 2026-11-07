#!/usr/bin/env python3
"""Download, check and install pinned build inputs from deps.lock.json.

  fetch_deps.py --dest DIR [--cache DIR] GROUP [GROUP ...]
  fetch_deps.py --list

Every download is checked against the size and SHA-256 in the lock before it is used; a mismatch stops the run.
Archives are unpacked with bsdtar (libarchive: tar, zip and 7z), tar archives also with GNU tar. DIR/deps-installed.json records what was installed.
Runs on the Python that ships with the build host (3.9+), before any Python packages are installed.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

LOCK = Path(__file__).with_name('deps.lock.json')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def download(entry, cache):
    target = cache / entry['sha256'] / entry['url'].rsplit('/', 1)[-1].replace('%2B', '+')
    if target.is_file() and target.stat().st_size == entry['size'] and sha256(target) == entry['sha256']:
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + '.part')
    for attempt in range(4):
        try:
            request = urllib.request.Request(entry['url'], headers={'User-Agent': 'room237-build'})
            with urllib.request.urlopen(request, timeout=60) as response, open(partial, 'wb') as out:
                shutil.copyfileobj(response, out, 1 << 20)
            break
        except OSError as error:
            if attempt == 3:
                raise RuntimeError(f"{entry['name']}: download failed: {error}") from None
            time.sleep(5 * (attempt + 1))
    size, digest = partial.stat().st_size, sha256(partial)
    if size != entry['size'] or digest != entry['sha256']:
        partial.unlink()
        raise RuntimeError(f"{entry['name']}: got {size} bytes, sha256 {digest}; lock expects {entry['size']}, {entry['sha256']}")
    os.replace(partial, target)
    return target


def extract_member(archive, member, out):
    out.parent.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z, z.open(member) as src, open(out, 'wb') as dst:
            shutil.copyfileobj(src, dst)
    else:
        with tarfile.open(archive) as t:
            src = t.extractfile(member)
            if src is None:
                raise RuntimeError(f'{archive.name}: {member} is not a regular file')
            with open(out, 'wb') as dst:
                shutil.copyfileobj(src, dst)
    out.chmod(0o755)


def install(entry, archive, dest):
    kind, into = entry['install'], dest / entry['into']
    if kind == 'file':
        into.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(archive, into)
        into.chmod(0o755)
    elif kind.startswith('member:'):
        extract_member(archive, kind[len('member:'):], into)
    elif kind == 'tree':
        into.mkdir(parents=True, exist_ok=True)
        tar = shutil.which('bsdtar') or ('/usr/bin/tar' if sys.platform == 'darwin' else None)
        if not tar:
            if archive.suffix == '.7z':
                raise RuntimeError('bsdtar (libarchive-tools) is required for 7z archives')
            tar = shutil.which('tar')
        strip = ['--strip-components', str(entry['strip'])] if entry.get('strip') else []
        command = [tar, '-x', *strip, '-f', str(archive), '-C', str(into)]
        subprocess.run(command, check=True)
    else:
        raise RuntimeError(f"{entry['name']}: unknown install kind {kind}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dest', type=Path)
    parser.add_argument('--cache', type=Path, help='download cache (default: DEST/.downloads)')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('groups', nargs='*')
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text())
    groups = sorted({a['group'] for a in lock['artifacts']})
    if args.list:
        for a in lock['artifacts']:
            print(a['group'], a['name'], a['size'], a['url'])
        return
    unknown = set(args.groups) - set(groups)
    if not args.dest or not args.groups or unknown:
        parser.error(f'--dest and one or more groups required; groups: {", ".join(groups)}')
    dest = args.dest.absolute()
    cache = (args.cache or dest / '.downloads').absolute()
    dest.mkdir(parents=True, exist_ok=True)
    record_path = dest / 'deps-installed.json'
    record = json.loads(record_path.read_text()) if record_path.exists() else {}
    for entry in lock['artifacts']:
        if entry['group'] not in args.groups or entry['name'] in record:
            continue
        print(f"{entry['name']}: {entry['url']}", flush=True)
        archive = download(entry, cache)
        install(entry, archive, dest)
        record[entry['name']] = {k: entry[k] for k in ('url', 'size', 'sha256', 'into')}
        record_path.write_text(json.dumps(record, indent=1, sort_keys=True) + '\n')
    print(f'{len(record)} inputs installed in {dest}')


if __name__ == '__main__':
    main()
