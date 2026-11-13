#!/usr/bin/env python3
"""APFS block sharing of byte-identical release outputs with an accepted release work directory."""
import hashlib, json, os, subprocess
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


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
