#!/usr/bin/env python3
"""QA isolation: explicit synthetic root/profile/legacy; no unfenced production starts.
Usage: test_isolation.py APP_BINARY NEW_WORK_DIR. All fixtures retained.
"""
import json, os, subprocess, sys, time
from pathlib import Path
from qa import App, FENCE, build_id, require_fenced
binary, work = sys.argv[1], Path(sys.argv[2])
require_fenced(work)
work.mkdir(parents=True, exist_ok=False)
home = work / 'home'
home.mkdir()
env = dict(HOME=str(home), CFFIXED_USER_HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'),
           QT_QPA_PLATFORM='offscreen')
results = []
def check(name, condition, detail=''):
    results.append(dict(name=name, ok=bool(condition), detail=str(detail)))
    print('PASS' if condition else 'FAIL', name, detail, flush=True)

# This early-return command derives locations only; it never creates an application or reads a profile.
printed = subprocess.run([binary, '--print-default-paths'], env=dict(os.environ, **env),
                         capture_output=True, text=True, timeout=15)
paths = dict(line.split('=', 1) for line in printed.stdout.strip().splitlines())
check('default paths are synthetic', printed.returncode == 0 and len(paths) == 2 and
      all(home.resolve() in Path(p).resolve().parents for p in paths.values()))
if not results[-1]['ok']:
    (work / 'results.json').write_text(json.dumps(dict(results=results), indent=2))
    sys.exit(1)
legacy_default = Path(paths['legacy'])
legacy_default.mkdir(parents=True)
# Nonblocking FIFO sentinel: absence of discovery in the trace is checked as well as preserved metadata.
os.mkfifo(legacy_default / 'state.json')
(legacy_default / 'settings.json').write_text('{}')
root = work / 'root'
root.mkdir()
legacy = work / 'explicit-legacy'
legacy.mkdir()
profile = work / 'profile'
trace = work / 'explicit.trace'
app = App(binary, root, profile, env=dict(env, ROOM237_TRACE=str(trace)),
          argv=['--legacy-profile', str(legacy)], log=work / 'app.log')
try:
    app.wait_albums()
    check('QA explicit startup succeeds', app.op('state')['root'] == str(root))
    text = trace.read_text()
    check('no default legacy discovery', 'legacy-discovery' not in text)
    check('default legacy FIFO remains untouched', (legacy_default / 'state.json').is_fifo())
    check('no default profile created', not Path(paths['profile']).exists())
    receipt = json.loads((profile / 'migration-tauri.json').read_text())
    check('empty explicit legacy imports no state/settings', receipt == {'source':str(legacy), 'settings':False, 'state':False})
finally:
    app.close()
check('QA exits cleanly', app.exit_code == 0, app.exit_code)
# Explicit synthetic legacy migration is still supported under the same fence.
(legacy / 'state.json').write_text(json.dumps({'room237-storage': json.dumps({'rootDir': str(root)})}))
(legacy / 'settings.json').write_text('{}')
app = App(binary, root, work / 'migrated-profile', env=env,
          argv=['--legacy-profile', str(legacy)], log=work / 'migration.log')
try:
    app.wait_albums()
    receipt = json.loads((work / 'migrated-profile' / 'migration-tauri.json').read_text())
    check('explicit fenced legacy migration succeeds', receipt == {'source':str(legacy), 'settings':True, 'state':True})
finally:
    app.close()
check('migration exits cleanly', app.exit_code == 0, app.exit_code)
# Missing mandatory arguments refuse before discovery/core. No UI is driven.
for label, args in [('missing root', ['--profile', str(profile)]),
                    ('missing profile', ['--root', str(root)]),
                    ('component-only fence', ['--root', str(root), '--profile', str(profile)])]:
    boundary = Path(FENCE).name if label == 'component-only fence' else FENCE
    run = subprocess.run([binary, *args], env=dict(os.environ, **env, ROOM237_QA_PORT='1', ROOM237_QA_FENCE=boundary),
                         capture_output=True, text=True, timeout=15)
    check(label + ' refused', run.returncode == 2, run.stderr.strip())
(work / 'results.json').write_text(json.dumps(dict(build=build_id(binary), results=results), indent=2))
sys.exit(0 if all(r['ok'] for r in results) else 1)
