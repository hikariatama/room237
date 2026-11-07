#!/usr/bin/env python3
"""Negative tests of the synthetic input guard, using only windows this test owns.
Usage: test_input_guard.py APP_BINARY FIXTURES WORK QA_INPUT. Needs QA_GUARD, QA_WINID, QA_OCCLUDER in the environment."""
import json, os, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, GuardRefused, guard, guarded_send, capture_window, window_id
from PIL import Image
binary, fixtures, work, input_tool = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
from slot_contract import fresh_work, activate
work, fixtures = fresh_work(work, fixtures)
root = work / "lib"
(root / "A").mkdir(parents=True)
shutil.copy(fixtures / "plain.jpg", root / "A" / "a.jpg")
results = []


def check(name, condition, detail=""):
    results.append({"name": name, "ok": bool(condition), "detail": str(detail)[:300]})
    print("PASS" if condition else "FAIL", name, str(detail)[:150], flush=True)


inp = subprocess.Popen([input_tool], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)


def raw(**command):
    inp.stdin.write(json.dumps(command) + "\n")
    inp.stdin.flush()
    return inp.stdout.readline().strip()


def occluder(x, y, w, h):
    process = subprocess.Popen([os.environ["QA_OCCLUDER"], str(x), str(y), str(w), str(h)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    ready = process.stdout.readline().split()
    time.sleep(0.2)
    return process, int(ready[1])


def refused(call):
    try:
        call()
    except GuardRefused as error:
        return str(error)
    return None


def clicks_seen(process):
    """Ends the occluder and returns how many clicks it reported."""
    process.stdin.write("quit\n")
    process.stdin.flush()
    out = process.communicate(timeout=5)[0]
    return out.count("clicked")


from slot_contract import authorization
authorization(os.environ.get("ROOM237_SLOT_AUTHORIZATION"), binary)
panel = None
app = App(binary, root, work / "profile", log=work / "app.log")
try:
    app.wait_albums()
    app.op("activate", album="A")
    app.op("resize", w=1000, h=500)
    app.op("place", x=120, y=360)
    activate(app)
    time.sleep(0.6)
    pid = app.proc.pid
    target = window_id(pid)
    g = app.op("geom")["window"]
    inside = (g["x"] + 700, g["y"] + 400)
    covered = (g["x"] + 500, g["y"] + 250)
    check("own window at a clear point passes", guard(pid, *inside, target) == "ok own window")
    check("guarded move reaches the input helper", guarded_send(raw, pid, {"op": "move", "x": inside[0], "y": inside[1]}, target).startswith("ok"))
    # Occlusion by an unknown owner.
    panel, panel_pid = occluder(covered[0] - 40, covered[1] - 40, 80, 80)
    reason = refused(lambda: guarded_send(raw, pid, {"op": "click", "x": covered[0], "y": covered[1]}, target))
    check("click under a foreign window is refused", reason and "above the target" in reason, reason)
    reason = refused(lambda: guarded_send(raw, pid, {"op": "rightclick", "x": covered[0], "y": covered[1]}, target))
    check("right click under a foreign window is refused", reason and "above the target" in reason, reason)
    reason = refused(lambda: guarded_send(raw, pid, {"op": "drag", "x": inside[0], "y": inside[1], "x2": covered[0], "y2": covered[1], "duration": 100}, target))
    check("drag ending under a foreign window is refused", reason and "above the target" in reason, reason)
    check("other points of the target still pass while it is occluded elsewhere", guard(pid, *inside, target).startswith("ok"))
    # Capture stays inside the target window.
    shot = work / "occluded.png"
    capture_window(pid, shot)
    image = Image.open(shot).convert("RGB")
    scale = image.width / g["w"]
    sample = image.getpixel((int(500 * scale), int(250 * scale)))
    check("window capture does not contain the window above it", sample != (255, 0, 255), sample)
    # The trusted drag helper passes only when named.
    check("a named trusted helper passes", guard(pid, *covered, target, allow=[panel_pid]).startswith("ok trusted"))
    reason = refused(lambda: guard(pid, *covered, target, allow=[panel_pid + 100000]))
    check("another allowed pid does not open the point", reason is not None, reason)
    # Loss of foreground.
    panel.stdin.write("front\n")
    panel.stdin.flush()
    panel.stdout.readline()
    time.sleep(0.4)
    reason = refused(lambda: guarded_send(raw, pid, {"op": "click", "x": inside[0], "y": inside[1]}, target))
    check("click after a focus change is refused", reason and "foreground" in reason, reason)
    reason = refused(lambda: guarded_send(raw, pid, {"op": "key", "code": 53}, target))
    check("key after a focus change is refused", reason and "foreground" in reason, reason)
    check("the occluder received no event", clicks_seen(panel) == 0)
    panel = None
    # Recovery is an explicit API activation by the test, never a click on the screen.
    activate(app)
    time.sleep(0.5)
    check("after API activation the point passes again", guard(pid, *covered, target).startswith("ok"))
    reason = refused(lambda: guard(pid, 700, 5, target))
    check("menu bar area is refused", reason and "visible frame" in reason, reason)
    reason = refused(lambda: guard(pid, *inside, 999999))
    check("a target window that is gone is refused", reason and "not on screen" in reason, reason)
    # The app's own second window is fine, including over the main window.
    app.op("own_window", x=covered[0] - 40, y=covered[1] - 40)
    time.sleep(0.4)
    check("own dialog or menu window passes", guard(pid, *covered, target) == "ok own window")
    app.op("own_window", show=False)
finally:
    if panel and panel.poll() is None:
        panel.terminate(); panel.wait(5)
    inp.stdin.write('{"op":"quit"}\n')
    inp.stdin.flush()
    inp.wait(5)
    app.close()
check("app exits cleanly", app.exit_code == 0, app.exit_code)
(work / "results.json").write_text(json.dumps({"build": app.build, "results": results}, indent=1))
print(f"{sum(r['ok'] for r in results)}/{len(results)} passed", [r["name"] for r in results if not r["ok"]])
sys.exit(0 if all(r["ok"] for r in results) else 1)
