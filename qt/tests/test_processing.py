#!/usr/bin/env python3
"""The processing screen: it covers the room while a library is indexed or encrypted, takes every click and shortcut,
changes the worker counts at once, and goes away by itself when the work is done.
Usage: test_processing.py APP_BINARY WORK_DIR. WORK_DIR is new and inside ROOM237_QA_FENCE. Writes results.json there.
Needs a QA build: encryption uses the test factor."""
import json, sys, time, traceback
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, require_fenced
from PIL import Image

binary, work = sys.argv[1], Path(sys.argv[2])
require_fenced(work)
work.mkdir(parents=True, exist_ok=False)
results = []


def check(name, ok, detail=""):
    results.append(dict(case=name, ok=bool(ok), detail=str(detail)[:600]))
    print(("ok  " if ok else "FAIL"), name, detail if not ok else "", flush=True)


def wait(fn, timeout=60, step=.1):
    end = time.time() + timeout
    while time.time() < end:
        value = fn()
        if value:
            return value
        time.sleep(step)
    return fn()


app = None
try:
    root, profile = work / "lib", work / "profile"
    # Enough large originals that indexing and encryption both take seconds.
    for album in range(3):
        (root / f"A{album}").mkdir(parents=True)
        for i in range(120):
            Image.new("RGB", (1600, 1200), ((i * 7 + album * 50) % 255, (i * 13) % 255, 90)).save(root / f"A{album}" / f"p{i:03d}.jpg", quality=95)
    app = App(binary, root, profile, log=work / "app.log")
    state = lambda: app.op("processing_state")
    shown = wait(lambda: (lambda s: s if s["visible"] else None)(state()), 60)
    check("indexing: the processing screen covers the room", shown and not shown["vault"], shown)
    check("indexing: clicks on the sidebar and the gallery reach the screen", shown and all(shown["blocksClicks"]), shown)
    check("indexing: menu shortcuts are off", shown and shown["enabledActions"] == 0, shown)
    check("indexing: the room shows through blurred", shown and shown["frosted"], shown)
    app.op("processing_workers", thumbnails=2, metadata=6, encryption=4)
    saved = wait(lambda: (lambda p: p if p["thumbWorkers"] == 2 and p["metaWorkers"] == 6 else None)(app.op("core_call", method="get_settings")["preload"]), 10)
    check("indexing: worker counts set on the screen are saved", saved, app.op("core_call", method="get_settings")["preload"])
    app.wait_idle(900)
    gone = wait(lambda: (lambda s: s if not s["visible"] else None)(state()), 30)
    check("indexing done: the screen goes away", gone, state())
    check("indexing done: shortcuts are back", gone and gone["enabledActions"] > 0, gone)

    app.op("core_call", method="vault_setup", params={"root": str(root), "password": "long password", "factors": ["test"]})
    encrypting = wait(lambda: (lambda s: s if s["visible"] and s["vault"] else None)(state()), 30)
    check("encryption: the screen covers the room", encrypting, state())
    check("encryption: clicks reach the screen", encrypting and all(encrypting["blocksClicks"]), encrypting)
    check("encryption: the room shows through blurred", encrypting and encrypting["frosted"], encrypting)
    app.op("processing_workers", thumbnails=2, metadata=6, encryption=7)
    saved = wait(lambda: (lambda p: p if p["encryptWorkers"] == 7 else None)(app.op("core_call", method="get_settings")["preload"]), 10)
    check("encryption: its worker count set on the screen is saved", saved, app.op("core_call", method="get_settings")["preload"])
    check("encryption done: the screen goes away", wait(lambda: json.loads((root / ".room237-vault").read_text())["state"] == "encrypted", 600) and wait(lambda: not state()["visible"], 60), state())
    app.close()
    check("clean exit", app.exit_code == 0, app.exit_code)
    app = None
except Exception:
    results.append(dict(case="error", ok=False, detail=traceback.format_exc()[-1200:]))
    print("ERROR", traceback.format_exc()[-1200:], flush=True)
finally:
    if app is not None:
        app.close()
(work / "results.json").write_text(json.dumps(results, indent=1))
bad = [r for r in results if not r["ok"]]
print(f"{len(results) - len(bad)}/{len(results)} passed")
sys.exit(1 if bad else 0)
