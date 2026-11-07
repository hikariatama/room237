#!/usr/bin/env python3
"""Album creation scans only the new album; a parent outside the open library keeps the library-wide scan.
Usage: test_album_create_scope.py APP_BINARY WORK_DIR. WORK_DIR is new and inside ROOM237_QA_FENCE. Writes results.json there."""
import hashlib, json, shutil, sys, time, traceback
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, require_fenced
from PIL import Image

binary, work = sys.argv[1], Path(sys.argv[2])
require_fenced(work)
work.mkdir(parents=True, exist_ok=False)
results = []


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def originals(root):
    return {str(f.relative_to(root)): sha(f) for f in root.rglob("*") if f.is_file() and not any(p.startswith(".") for p in f.relative_to(root).parts)}


def check(name, ok, detail=""):
    results.append(dict(case=name, ok=bool(ok), detail=str(detail)[:400]))
    print(("ok  " if ok else "FAIL"), name, detail if not ok else "", flush=True)


def wait(fn, timeout=12, step=.05):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(step)
    return bool(fn())


def album(root, name, count, seed):
    (root / name).mkdir(parents=True)
    for i in range(count):
        image = Image.new("RGB", (240, 180), ((seed + i) * 7 % 255, (seed + i) * 13 % 255, (seed + i) * 29 % 255))
        image.save(root / name / f"p{i:03d}.jpg", quality=90)


app = None
try:
    root, profile, outside = work / "lib", work / "profile", work / "outside"
    for name, count, seed in (("Main", 30, 1), ("Trips", 20, 300), ("Family/Kids", 12, 600)):
        album(root, name, count, seed)
    outside.mkdir()
    before = originals(root)
    app = App(binary, root, profile, log=work / "app.log")
    app.wait_albums()
    app.wait_idle(120)
    ids = lambda: [a["id"] for a in app.op("state")["albums"]]
    known = sorted(ids())

    created = app.op("album_create", name="Scoped")
    check("create in root: scan is limited to the new album", created.get("scan") == "album:Scoped", created)
    check("create in root: folder and album exist", (root / "Scoped").is_dir() and wait(lambda: "Scoped" in ids()), ids())

    nested = app.op("album_create", name="Child", parent="Family")
    check("nested create: scan is limited to the nested album", nested.get("scan") == "album:Family/Child", nested)
    check("nested create: folder and album exist", (root / "Family/Child").is_dir() and wait(lambda: "Family/Child" in ids()), ids())
    check("nested create: sibling album untouched", "Family/Kids" in ids() and app.op("inventory", album="Family/Kids")["total"] == 12)

    source = root / "Main" / "p000.jpg"
    app.op("import_prepare", paths=[str(source)])
    app.op("import_run", album="Scoped")
    copy = root / "Scoped" / "p000.jpg"
    check("import into the new album: copy is exact", copy.is_file() and sha(copy) == sha(source))
    check("import into the new album: visible in its inventory", wait(lambda: app.op("inventory", album="Scoped")["total"] == 1), app.op("inventory", album="Scoped"))
    check("import into the new album: other albums keep their counts",
          app.op("inventory", album="Main")["total"] == 30 and app.op("inventory", album="Trips")["total"] == 20)

    fallback = app.op("album_create", name="Elsewhere", at=str(outside))
    check("parent outside the library: library-wide scan kept", fallback.get("scan") == "library", fallback)
    check("parent outside the library: folder created there only", (outside / "Elsewhere").is_dir() and not (root / "Elsewhere").exists())
    check("parent outside the library: album list unchanged by it",
          wait(lambda: sorted(ids()) == sorted(known + ["Scoped", "Family/Child"])), sorted(ids()))

    (root / "Taken").write_bytes(b"not a folder")
    refused = app.op("album_create", name="Taken")
    check("name taken by a file: no album, app answers", refused.get("exists") is False and (root / "Taken").is_file() and "Taken" not in ids(), refused)
    (root / "Taken").unlink()

    unfenced = app.op("album_create", check=False, name="Escape", at="/tmp")
    check("explicit parent outside the QA fence is refused", not unfenced.get("ok", True) or "error" in json.dumps(unfenced), unfenced)

    app.op("album_delete", album="Family/Child")
    app.op("prepare_entries", album="Scoped", names=["p000.jpg"])
    app.op("delete_entries")
    app.op("album_delete", album="Scoped")
    check("cleanup: created albums removed", wait(lambda: not (root / "Scoped").exists() and not (root / "Family/Child").exists()), ids())
    after = originals(root)
    check("originals: every pre-existing file byte for byte", all(after.get(k) == v for k, v in before.items()), [k for k, v in before.items() if after.get(k) != v][:5])
    check("originals: nothing else left behind", set(after) == set(before), sorted(set(after) ^ set(before))[:5])
    app.close()
    check("clean exit", app.exit_code == 0, app.exit_code)
    app = None
except Exception:
    results.append(dict(case="error", ok=False, detail=traceback.format_exc()[-800:]))
    print("ERROR", traceback.format_exc()[-800:], flush=True)
finally:
    if app is not None:
        app.close()
(work / "results.json").write_text(json.dumps(results, indent=1))
bad = [r for r in results if not r["ok"]]
print(f"{len(results) - len(bad)}/{len(results)} passed")
sys.exit(1 if bad else 0)
