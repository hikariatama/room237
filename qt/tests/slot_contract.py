"""Fail-closed contracts shared by screen-slot drivers. Importing performs no UI work."""
import datetime as dt
import hashlib
import json, time
import os
from pathlib import Path
import shutil
import subprocess

# Common window frame for every live variant: x, y, width, height in global CoreGraphics points, and the expected backing scale.
# Set ROOM237_SLOT_VIEWPORT="x,y,w,h" and ROOM237_SLOT_SCALE to an unobstructed area of the display the run may use.
VIEWPORT = tuple(int(v) for v in os.environ.get('ROOM237_SLOT_VIEWPORT', '80,80,1280,720').split(','))
VIEW_SCALE = int(os.environ.get('ROOM237_SLOT_SCALE', '1'))
# Read-only compositing probe (a compiled helper, see tests/README.md); required only by visible-window suites.
GATE_TOOL = Path(os.environ.get('ROOM237_GATE_TOOL', 'passive_window'))

def gate(pid, log=None, tag=''):
    """One read-only compositing gate sample; with ROOM237_GATE_LOG every sample is appended there."""
    state = json.loads(subprocess.run([str(GATE_TOOL), 'gate', str(pid)], capture_output=True, text=True, timeout=10).stdout)
    target = log or os.environ.get('ROOM237_GATE_LOG')
    if target:
        with open(target, 'a') as stream: stream.write(json.dumps(dict(tag=tag, wall=time.time(), gate=state)) + '\n')
    return state

def place(pid):
    """Moves and sizes the own app's largest window to VIEWPORT through AX (qa_place addresses only this PID)."""
    x, y, w, h = VIEWPORT
    out = subprocess.run([os.environ['QA_PLACE'], str(pid), str(x), str(y), str(w), str(h)], capture_output=True, text=True, timeout=10).stdout.strip()
    if out != 'ok': raise RuntimeError('own window placement failed: ' + out)
    time.sleep(.4)

def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()

def authorization(path, binary, variant='new'):
    if not path: raise ValueError('written manager --authorization-file required')
    record = json.loads(Path(path).read_text())
    for key in ('sourceFrozen', 'buildsIdle', 'acceptanceAuthorized', 'machineUnused', 'machineUnlocked', 'ownWindowsOnly', 'syntheticDataOnly', 'userReauthorizedAfterDeferral'):
        if record.get(key) is not True: raise ValueError('authorization requires ' + key + '=true')
    for key in ('manager', 'coordinator', 'recordedAt', 'startsAt', 'expiresAt', 'slotId'):
        if not isinstance(record.get(key), str) or not record[key].strip(): raise ValueError('missing authorization ' + key)
    def date(key):
        value = dt.datetime.fromisoformat(record[key].replace('Z', '+00:00'))
        if value.tzinfo is None: raise ValueError('authorization timestamps require timezone')
        return value
    now = dt.datetime.now(dt.timezone.utc)
    if not date('startsAt') <= now < date('expiresAt'): raise ValueError('outside authorized slot')
    if date('expiresAt') - date('startsAt') > dt.timedelta(minutes=100): raise ValueError('slot longer than 100 minutes')
    if record.get('binarySha256', {}).get(variant) != sha(binary): raise ValueError('final binary SHA-256 mismatch: ' + variant)
    return record

def fresh_work(work, fixtures=None):
    import qa
    if 'ROOM237_QA_FENCE' not in os.environ: raise ValueError('explicit canonical ROOM237_QA_FENCE required')
    boundary = Path(os.environ['ROOM237_QA_FENCE'])
    if not boundary.is_absolute() or boundary.resolve(strict=True) != boundary: raise ValueError('full canonical fence required; old component names refused')
    qa.FENCE = str(boundary)
    work = qa.require_fenced(work)
    work.mkdir(parents=True, exist_ok=False)
    if fixtures:
        source = Path(fixtures).resolve(strict=True)
        target = work / 'fixtures'
        shutil.copytree(source, target)
        (work / 'fixture-provenance.json').write_text(json.dumps({'source':str(source), 'files':{p.name:sha(p) for p in source.iterdir() if p.is_file()}}, indent=2))
        return work, target
    return work

def front(pid):
    """Standard activation of the own app through AX (qa_front addresses only this PID); cooperative activation may refuse app self-activation."""
    out = subprocess.run([os.environ['QA_FRONT'], str(pid)], capture_output=True, text=True, timeout=10).stdout.strip()
    if not out.startswith('ok'): raise RuntimeError('own AX activation refused: ' + out)
    return out

def activate(app):
    authorization(os.environ.get('ROOM237_SLOT_AUTHORIZATION'), app.binary if hasattr(app, 'binary') else app.proc.args[0])
    pressure()
    pid = app.pid if hasattr(app, 'pid') else app.proc.pid
    place(pid)
    # Foreground runs are authorized, so windows above ours before raising it are expected; after raising, nothing may stay above it.
    before = gate(pid, tag='pre-activation')
    if before.get('visible') is not True or before.get('hidden') is not False or before.get('withinVisibleFrame') is not True:
        raise RuntimeError('own window not visible, hidden or off the visible frame before activation; no raise attempted: ' + json.dumps(before))
    app.op('activate_window')
    front(pid)
    time.sleep(.5)
    after = gate(pid, tag='post-activation')
    if after.get('foreignOverlay') is not False or after.get('exposure') is not True:
        raise RuntimeError('a foreign window stays above the own window after activation, or exposure is unknown; stopped: ' + json.dumps(after))
    import qa
    target = qa.window_id(pid)
    qa.guard(pid, window=target)
    app.qa_window = target
    return target

def pressure(policy=None):
    """strict (default, performance): only normal (1). functional (correctness stages): normal or warn (2) is recorded and allowed;
    critical (4) or an unknown value still stops. Set per stage through ROOM237_PRESSURE_POLICY."""
    policy = policy or os.environ.get('ROOM237_PRESSURE_POLICY', 'strict')
    value = subprocess.run(['sysctl','-n','kern.memorystatus_vm_pressure_level'], capture_output=True, text=True, check=True).stdout.strip()
    allowed = ('1', '2') if policy in ('functional', 'perf-warn') else ('1',)
    if value not in allowed: raise RuntimeError(f'memory pressure {value} not allowed for {policy} stage')
    return value
