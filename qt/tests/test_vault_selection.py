#!/usr/bin/env python3
"""Selection and mixed-album moves on disposable fixtures; APP_BINARY is a QA build."""
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import zlib


def png(color):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress((b'\0' + bytes(color) * 32) * 32)) + chunk(b'IEND', b''))


def wait(condition):
    end = time.monotonic() + 20
    while time.monotonic() < end:
        if condition():
            return
        time.sleep(.05)
    raise AssertionError('Timed out waiting for application state')


def main():
    with tempfile.TemporaryDirectory(prefix='room237-selection-') as temporary:
        os.environ['ROOM237_QA_FENCE'] = temporary
        from qa import App
        base = Path(temporary)
        root = base / 'library'
        expected = {}
        for album, name, color, favorite in [('A', 'picked.png', (255, 0, 0), True),
                                              ('A', 'shared.png', (0, 0, 255), False),
                                              ('B', 'shared.png', (0, 255, 0), True),
                                              ('C', 'keep.png', (255, 255, 0), False)]:
            folder = root / album
            folder.mkdir(parents=True, exist_ok=True)
            file = folder / name
            file.write_bytes(png(color))
            expected[album, name] = hashlib.sha256(file.read_bytes()).hexdigest()
            meta = folder / '.room237-metadata'
            meta.mkdir(exist_ok=True)
            legacy = meta / 'album.json'
            data = json.loads(legacy.read_text()) if legacy.exists() else {'files': {}}
            data['files'][name] = {'favorite': favorite}
            legacy.write_text(json.dumps(data))
        try:
            app = App(sys.argv[1], root, base / 'profile', log=base / 'app.log')
        except Exception:
            print((base / 'app.log').read_text(errors='replace')[-5000:], file=sys.stderr)
            raise
        try:
            print('Waiting for albums', flush=True)
            app.wait_albums()
            print('Albums ready', flush=True)
            settings = app.op('core_call', method='get_settings')
            settings['privacy'].update(enabled=False, confirmOpenEnabled=False)
            app.op('apply_settings', settings=settings)
            print('Activating A', flush=True)
            app.op('activate', album='A', noWait=True)
            wait(lambda: app.op('state')['total'] == 2)
            time.sleep(.3)
            rect = app.op('tile_rect', index=0)
            at = dict(x=rect['x'] + rect['w']//2, y=rect['y'] + rect['h']//2)
            app.op('pointer', action='press', **at)
            app.op('pointer', action='release', **at)
            assert app.op('selection')['count'] == 1
            assert not app.op('viewer_state')['open']
            app.op('pointer', action='doubleclick', **at)
            wait(lambda: app.op('viewer_state')['open'])
            app.op('viewer_close')
            app.wait_idle(timeout=60)
            print('Opening Favorites', flush=True)
            app.op('album_favorites')
            wait(lambda: app.op('state')['total'] == 2)
            app.op('select_all')
            assert app.op('prepare_selection')['count'] == 2
            print('Moving mixed selection', flush=True)
            app.op('move_entries', to='C')
            for name, source in [('picked.png', 'A'), ('shared.png', 'B')]:
                assert not (root / source / name).exists()
                assert hashlib.sha256((root / 'C' / name).read_bytes()).hexdigest() == expected[source, name]
            assert hashlib.sha256((root / 'A' / 'shared.png').read_bytes()).hexdigest() == expected['A', 'shared.png']
            assert hashlib.sha256((root / 'C' / 'keep.png').read_bytes()).hexdigest() == expected['C', 'keep.png']
            print('PASS: single selection, double open, mixed Favorites move, unselected originals preserved')
        except Exception:
            print((base / 'app.log').read_text(errors='replace')[-5000:], file=sys.stderr)
            raise
        finally:
            app.close()


if __name__ == '__main__':
    main()
