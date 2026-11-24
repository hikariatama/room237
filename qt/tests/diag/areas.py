"""Viewer stepping, the duplicates screen and memory on the real album. areas.py BINARY IMAGE PROFILE OUT ALBUM"""
import json, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from diag_lib import launch, wait, read_trace, now_ms, save_json  # noqa: E402
from scenarios import mount, unmount, MNT  # noqa: E402


def rss(pid):
    return int(subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip() or 0) // 1024


binary, image, profile_src, out, album = sys.argv[1:6]
out = Path(out); shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
clone = mount(Path(image), out)
app = None
res = {}
try:
    profile = out / "profile"; shutil.copytree(profile_src, profile)
    app = launch(binary, MNT / "lib", profile, out, "app")
    app.wait_albums(120); wait(lambda: not app.op("processing_state")["visible"], 300, 0.2)
    app.op("activate", album=album, timeout=120000)
    res["rssAfterOpenMB"] = rss(app.pid)
    t = time.time(); app.op("viewer_open", index=0); res["viewerOpenMs"] = round((time.time() - t) * 1000)
    steps = []
    for i in range(40):
        prev = app.op("viewer_path")["full"]
        t = time.time(); app.op("viewer_step", delta=1); app.op("viewer_wait", previous=prev); steps.append((time.time() - t) * 1000)
    steps.sort()
    res["viewerStepMs"] = {"p50": round(steps[len(steps) // 2]), "p95": round(steps[int(len(steps) * 0.95)]), "max": round(steps[-1])}
    for i in range(60):  # fast stepping without waiting, as holding an arrow key
        app.op("viewer_step", delta=1); time.sleep(0.03)
    ok = wait(lambda: app.op("viewer_state")["ready"], 10)
    res["viewerAfterFastStepping"] = {"ready": bool(ok), "failed": app.op("viewer_state")["failed"]}
    res["rssAfterViewerMB"] = rss(app.pid)
    app.op("viewer_close"); time.sleep(1)
    t = time.time()
    first = app.op("dup_open", check=False)
    states = []
    while time.time() - t < 240:
        st = app.op("dup_state"); v = app.op("visible")
        states.append((round(time.time() - t, 1), st["total"], st["files"], st["loading"], v["visible"], v["ready"], v["expected"]))
        if not st["loading"] and (v["expected"] == 0 or v["ready"] == v["expected"]) and time.time() - t > 2:
            break
        time.sleep(1)
    res["duplicates"] = {"openReply": first.get("error") or first.get("result"), "settledS": states[-1][0], "groups": states[-1][1], "files": states[-1][2],
                         "loading": states[-1][3], "visible": states[-1][4:], "trajectory": states[::max(1, len(states) // 8)]}
    app.op("snapshot", path=str(out / "duplicates.png"))
    app.op("dup_close"); time.sleep(1)
    for y in range(0, 400000, 20000):
        app.op("scroll_to", y=y); time.sleep(0.05)
    time.sleep(2)
    res["rssAfterScrollMB"] = rss(app.pid)
    res["cache"] = {k: app.op("state")[k] for k in ("cacheBytes", "cacheCount", "decoded")}
    res["errors"] = [d[:160] for _, k, d in read_trace(app.trace_path, 0) if k == "core-error" and "superseded" not in d][:5]
    print(json.dumps(res, indent=1))
    save_json(out / "areas.json", res)
finally:
    if app:
        app.close()
    unmount(clone)
