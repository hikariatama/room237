#!/usr/bin/env python3
"""Imports every fixture through the running app and checks thumbnail, metadata, viewer and playback.
Usage: test_formats.py APP_BINARY FIXTURES EXPECTED_JSON WORK_DIR [--wrapper CMD...] writes results.json in WORK_DIR.
--encrypted encrypts the library with security hardening (QA test factor) before the checks."""
import json, os, shutil, sys, time
from datetime import datetime, timezone
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, require_fenced
binary, fixtures, expected_path, work = sys.argv[1:5]
wrapper = None
path_map = lambda value: str(value)
if "--wine" in sys.argv:
    wrapper = ["wine"]
    path_map = lambda value: "Z:" + str(value).replace("/", "\\")
fixtures, work = Path(fixtures), Path(work)
require_fenced(work)
require_fenced(fixtures)
expected = json.load(open(expected_path))
if work.exists():
    raise ValueError("fresh work directory required; old evidence retained")
root, profile = work / "lib", work / "profile"
(root / "Formats").mkdir(parents=True)
if wrapper:
    # Wine on Rosetta crashes on FFmpeg's automatic hardware device creation and under many parallel FFmpeg starts.
    # Neither hardware decoding nor worker scaling is what this run tests.
    profile.mkdir(parents=True)
    (profile / "settings.json").write_text(json.dumps({"ffmpeg": {"threads": 1, "timeoutSecs": 30, "hwaccel": "none", "processWaitPollMs": 50}, "preload": {"thumbWorkers": 1, "metaWorkers": 1, "hashWorkers": 1, "progressEmitMs": 500, "thumbHashQueueDelayMs": 10, "thumbHashOnlyAfterIdle": True, "thumbHashRetryOnThumbChange": True}}))
log = work.parent / (work.name + ".log")
log.unlink(missing_ok=True)
hashes = {f.name: f.read_bytes() for f in fixtures.iterdir() if f.is_file()}
app = App(binary, path_map(root), path_map(profile), log=log, wrapper=wrapper, wait=90)
results = {}
try:
    app.wait_albums(120)
    app.op("activate", album="Formats", noWait=True)
    names = list(expected)
    # One at a time under Wine: each import batch hashes and thumbnails in parallel inside the core.
    for start in range(0, len(names), 1 if wrapper else len(names)):
        app.op("import_prepare", paths=[path_map(fixtures / name) for name in names[start:start + (1 if wrapper else len(names))]])
        print("import", names[start], app.op("import_run", album="Formats"), flush=True)
    for _ in range(600):
        state = app.op("state")
        if not state.get("preloading"):
            break
        time.sleep(0.5)
    time.sleep(1.5)
    if "--encrypted" in sys.argv:
        app.op("core_call", method="vault_setup", params={"root": path_map(root), "password": "formats password", "factors": ["test"]})
        for _ in range(1200):
            if json.loads((root / ".room237-vault").read_text())["state"] == "encrypted":
                break
            time.sleep(0.25)
        print("encrypted", json.loads((root / ".room237-vault").read_text())["state"], flush=True)
        app.op("open_root", path=path_map(root))
        app.wait_albums(120)
        time.sleep(1.5)
    total = app.op("activate", album="Formats", noWait=True)["total"]
    print("total", total)
    for index in range(total):
        item = app.op("item", index=index)
        # In an encrypted root the file on disk has a random name; the original is its label.
        name, stored = item.get("label") or item["name"], item["name"]
        facts = expected.get(name, {})
        result = {"listed": True, "width": item["width"], "height": item["height"], "video": item["video"], "dateTaken": item["dateTaken"], "dateAdded": item["dateAdded"]}
        thumb = None
        for candidate in (root / "Formats" / ".room237-thumb").glob(stored + "*"):
            if candidate.suffix == ".webp" and not candidate.name.endswith(".blur.webp"):
                thumb = candidate
        result["thumbnail"] = bool(thumb and thumb.stat().st_size > 0)
        ok = True
        if facts.get("kind") in ("image", "video"):
            ok &= result["thumbnail"]
            if not (item["width"] == facts["w"] and item["height"] == facts["h"]):
                ok = False
                result["sizeProblem"] = f"{item['width']}x{item['height']} != {facts['w']}x{facts['h']}"
            if facts.get("date"):
                when = datetime.fromtimestamp(item["dateTaken"], timezone.utc).strftime("%Y-%m-%d") if item["dateTaken"] else None
                result["date"] = when
                ok &= when == facts["date"]
            ok &= item["video"] == (facts["kind"] == "video")
        reply = app.op("viewer_open", check=False, index=index)
        viewer = None
        for _ in range(100):
            viewer = app.op("viewer_state")
            if viewer["ready"] or viewer["failed"]:
                break
            time.sleep(0.1)
        result["viewerReady"] = viewer["ready"]
        result["viewerFailed"] = viewer["failed"]
        if facts.get("kind") == "broken":
            ok = viewer["failed"] and not viewer["ready"]
        elif facts.get("playback", True) and (viewer["failed"] or not viewer["ready"]):
            ok = False
        if facts.get("animated"):
            first = viewer["animationFrames"]
            time.sleep(1.0)
            later = app.op("viewer_state")
            result["animationFrames"] = later["animationFrames"] - first
            ok &= later["animated"] and later["animationFrames"] - first >= 4
            app.op("set_unfocused", unfocused=False)
        if facts.get("kind") == "video":
            before = app.op("viewer_state")["videoFrames"]
            time.sleep(1.2)
            state = app.op("viewer_state")
            probe = app.op("video_probe")
            result["videoFramesIn1200ms"] = state["videoFrames"] - before
            result["position"] = probe.get("currentTime")
            result["duration"] = probe.get("duration")
            if facts.get("playback", True):
                ok &= state["videoFrames"] - before >= 5
            else:
                result["playbackExpected"] = False
        snapshot = work / f"{index:02d}-{name}.png"
        app.op("snapshot", path=path_map(snapshot))
        app.op("viewer_close")
        time.sleep(0.15)
        result["ok"] = bool(ok)
        results[name] = result
        print(("PASS" if ok else "FAIL"), name, {k: v for k, v in result.items() if k in ("width", "height", "viewerFailed", "animationFrames", "videoFramesIn1200ms", "playbackExpected", "date", "sizeProblem", "thumbnail")})
    results_missing = [name for name, facts in expected.items() if name not in results]
    for name in results_missing:
        results[name] = {"listed": False, "ok": False}
        print("FAIL", name, "not listed")
    # Originals are never changed by import, preview or playback.
    changed = [name for name, data in hashes.items() if name in expected and (root / "Formats" / name).exists() and (root / "Formats" / name).read_bytes() != data]
    results["__originals_changed"] = changed
    print("originals changed:", changed)
except Exception as error:
    import traceback
    results["__exception"] = {"ok":False,"error":repr(error),"traceback":traceback.format_exc()}
    print("FAIL suite exception",repr(error),flush=True)
finally:
    app.close()
    print("app exit", app.exit_code, flush=True)
results["__exit_code"] = {"ok": app.exit_code == 0, "code": app.exit_code}
(work / "results.json").write_text(json.dumps({"build": app.build, "results": results}, indent=1))
failed = [name for name, value in results.items() if isinstance(value, dict) and not value.get("ok")]
print(f"{len(results) - 1 - len(failed)}/{len(results) - 1} passed", failed)
sys.exit(1 if failed or results["__originals_changed"] else 0)
