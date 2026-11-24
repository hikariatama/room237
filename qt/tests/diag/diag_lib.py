"""Shared helpers for diagnostic runs: launch a QA build with censored pictures, a trace and the I/O counter,
capture frames, and read the trace back."""
import json, os, shutil, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from qa import App  # noqa: E402


def launch(binary, root, profile, work, name, io_lib=None, extra_env=None, size=(1440, 900)):
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    trace = work / f"{name}.trace"
    env = {"QT_QPA_PLATFORM": "offscreen", "ROOM237_QA_CENSOR": "1", "ROOM237_TRACE": str(trace),
           "R237_IO_ROOT": str(root), "R237_IO_PROFILE": str(profile)}
    if io_lib:
        env["DYLD_INSERT_LIBRARIES"] = str(io_lib)
    env.update(extra_env or {})
    app = App(binary, root, profile, log=work / f"{name}.log", env=env, wait=60)
    app.trace_path = trace
    app.t0 = time.time()
    if size:
        app.op("resize", w=size[0], h=size[1])
    return app


def wait(test, timeout=60, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        value = test()
        if value:
            return value
        time.sleep(step)
    return None


def read_trace(path, since_ms=0):
    rows = []
    for line in Path(path).read_text(errors="replace").splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 2 or not parts[0].isdigit():
            continue
        t = int(parts[0])
        if t >= since_ms:
            rows.append((t, parts[1], parts[2] if len(parts) > 2 else ""))
    return rows


def now_ms():
    return int(time.time() * 1000)


def diag(app, reset=False):
    return app.op("diag", reset=reset)


def io_total(d, scope="all"):
    io = d.get("io") or {}
    return {op: sum(cats.values()) for op, cats in io.get(scope, {}).items()}


def film(app, directory, interval=33, scale=0.5):
    directory = Path(directory)
    if directory.exists():
        shutil.rmtree(directory)
    app.op("film_start", dir=str(directory), interval=interval, scale=scale)


def film_stop(app):
    return app.op("film_stop")


def save_json(path, data):
    Path(path).write_text(json.dumps(data, indent=2, ensure_ascii=False))
