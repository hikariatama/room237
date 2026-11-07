#!/usr/bin/env python3
"""Import of a Tauri-era profile (Room237 6.x) by the Qt app, offscreen.

Usage: test_migration.py QT_BINARY FIXTURES WORK [--profile DIR]
WORK is new and inside ROOM237_QA_FENCE. DIR defaults to fixtures/tauri-profile/dev.dgazizullin.room237.qa.base,
a synthetic profile written by the Tauri app's QA build. The copy given to the app is checked byte for byte afterwards,
as are the library originals. Profile and album-database migration details are covered by the Rust tests in
qt/core (migrate.rs, album_db.rs).
"""
import json, shutil, sys
from pathlib import Path
from PIL import Image
from qa import App
from slot_contract import fresh_work, sha

HERE = Path(__file__).resolve().parent
PROFILE_NAME = 'dev.dgazizullin.room237.qa.base'


def main():
    args = sys.argv[1:]
    profile_source = HERE / 'fixtures/tauri-profile' / PROFILE_NAME
    if '--profile' in args:
        index = args.index('--profile')
        profile_source = Path(args[index + 1])
        del args[index:index + 2]
    binary, fixtures, work = args[:3]
    profile_source = profile_source.resolve(strict=True)
    if not (profile_source / 'state.json').is_file():
        raise ValueError('profile directory needs state.json')
    work, fixtures = fresh_work(Path(work), Path(fixtures))
    root = work / 'lib'
    album = root / 'Stems'
    album.mkdir(parents=True)
    for name, colour in (('same.jpg', 'red'), ('same.png', 'blue'), ('same.webp', 'green')):
        Image.new('RGB', (320, 240), colour).save(album / name)
    legacy = work / 'legacy'
    shutil.copytree(profile_source, legacy)
    state = json.loads(json.loads((legacy / 'state.json').read_text())['room237-storage'])
    state.update(rootDir=str(root), decoyRoot=None, columns=5, language='ru', layout='masonry')
    (legacy / 'state.json').write_text(json.dumps({'room237-storage': json.dumps(state)}))
    originals = {p.name: sha(p) for p in album.iterdir()}
    saved = {p.name: sha(p) for p in legacy.iterdir() if p.is_file()}
    app = App(binary, root, work / 'profile', log=work / 'app.log', argv=['--legacy-profile', str(legacy)])
    rows = []
    try:
        app.wait_albums()
        app.op('activate', album='Stems')
        app.wait_idle()
        ui = json.loads((work / 'profile/ui-state.json').read_text())
        receipt = json.loads((work / 'profile/migration-tauri.json').read_text())
        rows.append(dict(name='Tauri profile imported', ok=bool(receipt['state'] and receipt['settings']) and ui['columns'] == 5 and ui['language'] == 'ru' and ui['layout'] == 'masonry'))
        names = set(app.op('inventory', album='Stems')['names'])
        rows.append(dict(name='same stem different extensions preserved', ok=set(originals) <= names))
    finally:
        app.close()
    rows += [dict(name='original bytes preserved', ok=all(sha(album / n) == h for n, h in originals.items())),
             dict(name='legacy profile preserved', ok=all(sha(legacy / n) == h for n, h in saved.items())),
             dict(name='clean exit', ok=app.exit_code == 0)]
    (work / 'results.json').write_text(json.dumps(dict(route='offscreen Tauri profile import', profileSource=str(profile_source),
                                                       profileSourceSha256={p.name: sha(p) for p in profile_source.iterdir() if p.is_file()},
                                                       rows=rows), indent=2))
    for row in rows:
        print('PASS' if row['ok'] else 'FAIL', row['name'])
    return int(not all(row['ok'] for row in rows))


if __name__ == '__main__':
    raise SystemExit(main())
