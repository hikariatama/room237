#!/usr/bin/env python3
"""Tauri profile discovery at the platform default location and import from there.

The Tauri build's tauri-plugin-store 2.5 resolved state.json against Tauri's app data dir (dirs::data_dir()/dev.dgazizullin.room237):
Linux $XDG_DATA_HOME or ~/.local/share, macOS ~/Library/Application Support, Windows %APPDATA%. The binary reports where a normal start
looks (--print-default-paths) under a synthetic HOME; that path must equal Tauri's. The state.json copied there was written by the real
Tauri QA-base build on macOS (platform independent format). This is discovery + import by the Qt build on this platform, not a live
Tauri round trip on this platform.
Usage: test_legacy_discovery.py BINARY WORK --offscreen-profile PROFILE_DIR"""
import json, os, shutil, subprocess, sys
from pathlib import Path
from PIL import Image
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, build_id
from slot_contract import fresh_work, sha

def tauri_data_dir(home, env):
    if sys.platform == 'darwin': return home / 'Library/Application Support/dev.dgazizullin.room237'
    if os.name == 'nt': return Path(env['APPDATA']) / 'dev.dgazizullin.room237'
    return Path(env.get('XDG_DATA_HOME') or home / '.local/share') / 'dev.dgazizullin.room237'

def main():
    binary, work = sys.argv[1], Path(sys.argv[2])
    source = Path(sys.argv[sys.argv.index('--offscreen-profile') + 1]).resolve(strict=True)
    if source.name not in ('dev.dgazizullin.room237', 'dev.dgazizullin.room237.qa.base') or not (source / 'state.json').is_file():
        raise ValueError('synthetic Tauri QA-base profile with state.json required')
    work = fresh_work(work); rows = []
    for variant in ['default'] + (['xdg'] if sys.platform.startswith('linux') else []):
        case = work / variant; home = case / 'home'; home.mkdir(parents=True)
        env = {k: v for k, v in os.environ.items() if k not in ('XDG_DATA_HOME', 'XDG_CONFIG_HOME')}
        env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), APPDATA=str(home / 'AppData/Roaming'))
        if variant == 'xdg': env.update(XDG_DATA_HOME=str(home / 'xdg-data'), XDG_CONFIG_HOME=str(home / 'xdg-config'))
        printed = subprocess.run([binary, '--print-default-paths'], capture_output=True, text=True, env=env, timeout=60)
        paths = dict(line.split('=', 1) for line in printed.stdout.splitlines() if '=' in line)
        expected = tauri_data_dir(home, env)
        rows.append(dict(name=f'{variant}: normal start looks for the Tauri profile in Tauri app data dir', ok=printed.returncode == 0 and Path(paths.get('legacy', '')) == expected, detail=dict(printed=paths, expected=str(expected))))
        root = case / 'lib'; (root / 'Stems').mkdir(parents=True); Image.new('RGB', (64, 48), 'red').save(root / 'Stems' / 'a.jpg')
        legacy = Path(paths.get('legacy') or expected); legacy.parent.mkdir(parents=True, exist_ok=True); shutil.copytree(source, legacy)
        state = json.loads(json.loads((legacy / 'state.json').read_text())['room237-storage'])
        state.update(rootDir=str(root), decoyRoot=None, columns=5, language='ru', layout='masonry')
        (legacy / 'state.json').write_text(json.dumps({'room237-storage': json.dumps(state)}))
        saved = {p.name: sha(p) for p in legacy.iterdir() if p.is_file()}
        app = App(binary, root, case / 'profile', log=case / 'app.log', env={k: env[k] for k in ('HOME', 'CFFIXED_USER_HOME', 'APPDATA')}, argv=['--legacy-profile', str(legacy)])
        try:
            app.wait_albums()
            receipt_path, ui_path = case / 'profile/migration-tauri.json', case / 'profile/ui-state.json'
            receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else {}
            ui = json.loads(ui_path.read_text()) if ui_path.exists() else {}
            rows.append(dict(name=f'{variant}: profile at the discovered location imported', ok=bool(receipt.get('state')) and ui.get('columns') == 5 and ui.get('layout') == 'masonry', detail=dict(receipt=receipt, columns=ui.get('columns'), layout=ui.get('layout'))))
        finally: app.close()
        rows += [dict(name=f'{variant}: legacy source preserved', ok=all(sha(legacy / n) == h for n, h in saved.items())), dict(name=f'{variant}: clean exit', ok=app.exit_code == 0)]
    (work / 'results.json').write_text(json.dumps(dict(route='platform default discovery (--print-default-paths) + import of a real-Tauri-written synthetic state.json; no live Tauri on this platform', profileSource=str(source), profileSourceSha256={p.name: sha(p) for p in source.iterdir() if p.is_file()}, build=build_id(binary), results=rows), indent=2))
    for row in rows: print('PASS' if row['ok'] else 'FAIL', row['name'], json.dumps(row.get('detail', ''))[:300])
    return int(not all(r['ok'] for r in rows))

if __name__ == '__main__': raise SystemExit(main())
