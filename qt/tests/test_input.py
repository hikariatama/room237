#!/usr/bin/env python3
"""Real mouse and keyboard events (macOS CGEvent) on a built app: hover actions, menus, drag and drop, assistive client.
Usage: test_input.py APP_BINARY FIXTURES WORK_DIR QA_INPUT QA_DRAGSOURCE AX_PROBE"""
import json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, guarded_send, capture_app, capture_window, window_id, app_panels
binary, fixtures, work, input_tool, drag_tool, ax_tool = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], sys.argv[5], sys.argv[6]
from slot_contract import fresh_work, activate
work, fixtures = fresh_work(work, fixtures)
root, profile = work / "lib", work / "profile"
for name in ["Alpha", "Beta", "Gamma", "Many", "Dups", "Clips"]:
    (root / name).mkdir(parents=True)
from PIL import Image as _Image, ImageDraw as _Draw
import random as _random
_rng = _random.Random(237)
for i in range(240):
    size = (_rng.randint(240, 520), _rng.randint(240, 520))
    canvas = _Image.new("RGB", size, tuple(_rng.randint(0, 255) for _ in range(3)))
    pen = _Draw.Draw(canvas)
    for _ in range(12):
        x, y = _rng.randint(0, size[0]), _rng.randint(0, size[1])
        pen.ellipse([x, y, x + _rng.randint(10, 160), y + _rng.randint(10, 160)], fill=tuple(_rng.randint(0, 255) for _ in range(3)))
    canvas.save(root / "Many" / f"m{i:03}.jpg", quality=85)
shutil.copy(fixtures / "plain.jpg", root / "Dups" / "one.jpg")
shutil.copy(fixtures / "plain.jpg", root / "Dups" / "two.jpg")
_Image.open(fixtures / "plain.jpg").convert("RGB").save(root / "Dups" / "three.png")
shutil.copy(fixtures / "h264.mp4", root / "Clips" / "clip.mp4")
KEY_UP, KEY_TAB, KEY_RIGHT, KEY_ESC = 126, 48, 124, 53
for i, source in enumerate(["plain.jpg", "lossy.webp", "plain.bmp", "alpha.png", "plain.tiff", "plain.avif", "plain.heic"]):
    shutil.copy(fixtures / source, root / "Alpha" / f"{i}-{source}")
shutil.copy(fixtures / "plain.jpg", root / "Beta" / "b.jpg")
from slot_contract import authorization
authorization(os.environ.get("ROOM237_SLOT_AUTHORIZATION"), binary)
app = App(binary, root, profile, log=work / "app.log", env={"ROOM237_TRACE": str(work / "trace.txt")})
inp = subprocess.Popen([input_tool], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
results = []
source = None
focus_helper = None


def send(**command):
    return guarded_send(raw_send, app.proc.pid, command, window=getattr(app, "qa_window", None) or window_id(app.proc.pid), allow=trusted)


trusted = []


def raw_send(**command):
    inp.stdin.write(json.dumps(command) + "\n")
    inp.stdin.flush()
    reply = inp.stdout.readline().strip()
    with (work / "input-events.ndjson").open("a") as events:
        events.write(json.dumps({"command":command,"reply":reply,"time":time.time()}) + "\n")
    return reply


def check(name, condition, detail=""):
    results.append({"name": name, "ok": bool(condition), "detail": str(detail)[:300]})
    print("PASS" if condition else "FAIL", name, str(detail)[:150], flush=True)


def wait(predicate, timeout=8, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        value = predicate()
        if value:
            return value
        time.sleep(step)
    return None


def center(rect):
    return rect["x"] + rect["w"] / 2, rect["y"] + rect["h"] / 2


def tile(index):
    rect = app.op("tile_rect", index=index)
    return rect


try:
    app.wait_albums()
    activate(app)
    # Below the area a notch utility may cover; the event guard refuses anything under a foreign window.
    app.op("resize", w=1280, h=640)
    from slot_contract import VIEWPORT
    app.op("place", x=VIEWPORT[0], y=VIEWPORT[1])  # stays in the common slot viewport
    time.sleep(0.4)
    app.op("activate", album="Alpha", timeout=30000)
    app.wait_idle()
    time.sleep(1.3)
    # Click on an album row with a real pointer.
    x, y = center(app.op("album_rect", album="Beta"))
    send(op="click", x=x, y=y)
    check("click album row", wait(lambda: app.op("state")["active"] == "Beta"), app.op("state")["active"])
    x, y = center(app.op("album_rect", album="Alpha"))
    send(op="click", x=x, y=y)
    wait(lambda: app.op("state")["active"] == "Alpha")
    time.sleep(0.5)
    # Hover shows actions: the heart toggles favorite without opening the viewer.
    rect = tile(1)
    x, y = center(rect)
    send(op="move", x=x, y=y)
    time.sleep(0.35)
    app.op("snapshot", path=str(work / "hover.png"))
    send(op="click", x=rect["x"] + rect["w"] - 14, y=rect["y"] + 14)
    name = app.op("item", index=1)["name"]
    check("hover heart sets favorite", wait(lambda: app.op("item", index=1)["favorite"]), name)
    check("heart does not open the viewer", not app.op("viewer_state")["open"])
    send(op="click", x=rect["x"] + rect["w"] - 14, y=rect["y"] + 14)
    wait(lambda: not app.op("item", index=1)["favorite"])
    # Plain click opens the viewer; arrows and Esc work with real keys.
    send(op="click", x=x, y=y)
    check("click opens viewer", wait(lambda: app.op("viewer_state")["ready"]))
    first = app.op("viewer_state")["path"]
    time.sleep(0.3)
    send(op="key", code=124)
    check("right arrow steps", wait(lambda: app.op("viewer_state")["path"] != first))
    send(op="key", code=123)
    check("left arrow returns", wait(lambda: app.op("viewer_state")["path"] == first))
    send(op="key", code=53)
    check("Esc closes", wait(lambda: not app.op("viewer_state")["open"], 3))
    # Cmd-click and Shift-click selection.
    send(op="click", x=center(tile(0))[0], y=center(tile(0))[1], flags=["cmd"])
    send(op="click", x=center(tile(3))[0], y=center(tile(3))[1], flags=["shift"])
    check("cmd-click then shift-click selects a range", wait(lambda: app.op("selection")["count"] == 4), app.op("selection"))
    app.op("snapshot", path=str(work / "selection.png"))
    send(op="key", code=0, flags=["cmd"])
    check("Cmd+A selects the album", wait(lambda: app.op("selection")["count"] == 7))
    send(op="key", code=2, flags=["cmd"])
    check("Cmd+D clears selection", wait(lambda: app.op("selection")["count"] == 0))
    send(op="click", **dict(zip("xy", center(tile(0)))), flags=["cmd"])
    send(op="click", **dict(zip("xy", center(tile(3)))), flags=["shift"])
    # Context menu opens over the tile and closes on Esc.
    send(op="rightclick", x=center(tile(0))[0], y=center(tile(0))[1])
    check("right click opens the menu", wait(lambda: app.op("menu_state")["open"]))
    time.sleep(0.25)
    app.op("snapshot", path=str(work / "menu.png"))
    send(op="key", code=53)
    check("Esc closes the menu", wait(lambda: not app.op("menu_state")["open"], 3))
    # Drag the selection onto another album.
    before = {p.name for p in (root / "Alpha").iterdir() if p.is_file() and not p.name.startswith(".")}
    sx, sy = center(tile(1))
    tx, ty = center(app.op("album_rect", album="Gamma"))
    send(op="drag", x=sx, y=sy, x2=tx, y2=ty, duration=900)
    moved = wait(lambda: len([p for p in (root / "Gamma").iterdir() if p.is_file() and not p.name.startswith(".")]) == 4, 10)
    check("drag selection onto album moves files", moved, sorted(p.name for p in (root / "Gamma").iterdir()))
    app.wait_idle()
    # Files dragged in from another application.
    paths = work / "drag.txt"
    paths.write_text("\n".join([str(fixtures / "plain.jpg"), str(fixtures / "lossy.webp")]))
    source = subprocess.Popen([drag_tool, str(paths)], stdout=subprocess.PIPE, text=True)
    trusted.append(source.pid)  # The drag helper of this test; its panel is the only other window allowed under a gesture.
    ready = source.stdout.readline().split()
    dx, dy = float(ready[1]), float(ready[2])
    time.sleep(0.4)
    tx, ty = center(app.op("album_rect", album="Beta"))
    send(op="drag", x=dx, y=dy, x2=tx, y2=ty, duration=1100)
    dropped = wait(lambda: (root / "Beta" / "lossy.webp").exists(), 10)
    check("files dropped from another app land in the album", dropped, sorted(p.name for p in (root / "Beta").iterdir()))
    source.terminate()
    source.wait(5)
    source = None
    trusted.clear()
    # Album onto album with a real drag.
    sx, sy = center(app.op("album_rect", album="Gamma"))
    tx, ty = center(app.op("album_rect", album="Beta"))
    send(op="drag", x=sx, y=sy, x2=tx, y2=ty, duration=900)
    check("drag album into album", wait(lambda: (root / "Beta/Gamma").is_dir(), 10), sorted(p.name for p in (root / "Beta").iterdir() if p.is_dir()))
    # Flying tiles of a real drag, from window-only frames. Several tiles are picked, so the far ones cross tiles that are not selected.
    # Only a flyer can change those tiles: hover extras appear on the source tile alone.
    app.op("activate", album="Many")
    time.sleep(0.8)
    send(op="click", **dict(zip("xy", center(tile(0)))), flags=["cmd"])
    send(op="click", **dict(zip("xy", center(tile(6)))), flags=["cmd"])
    source_rect = tile(10)
    sx, sy = center(source_rect)
    send(op="click", x=sx, y=sy, flags=["cmd"])
    time.sleep(0.3)
    frames = work / "fly"
    grab = subprocess.Popen([os.environ["QA_FRAMEGRAB"], window_id(app.proc.pid), "1.8", str(frames)], stdout=subprocess.PIPE, text=True)
    grab.stdout.readline()
    time.sleep(0.2)
    send(op="drag", x=sx, y=sy, x2=sx + 90, y2=sy + 60, duration=800)
    grab.communicate(timeout=20)
    records = json.loads((frames / "frames.json").read_text())
    win = app.op("geom")["window"]
    from PIL import Image, ImageChops, ImageStat
    first = Image.open(frames / records[0]["file"]).convert("L")
    scale = first.width / win["w"]
    box = lambda rect: tuple(int(v * scale) for v in (rect["x"] - win["x"], rect["y"] - win["y"], rect["x"] - win["x"] + rect["w"], rect["y"] - win["y"] + rect["h"]))
    crossed = [box(tile(i)) for i in (1, 5, 8, 9)]
    changes = [max(sum(ImageStat.Stat(ImageChops.difference(first.crop(area), Image.open(frames / r["file"]).convert("L").crop(area))).mean) for area in crossed) for r in records]
    peak = max(range(len(changes)), key=lambda i: changes[i])
    Image.open(frames / records[peak]["file"]).convert("RGB").save(work / "fly-peak.png")
    later = changes[-1]
    check("flying tiles cross unselected tiles during a real drag, then leave them as they were", changes[peak] > 6 and later < 1.5, {"peak": round(changes[peak], 2), "frame": peak, "last": round(later, 2)})
    check("drag over the gallery moves nothing", len(list((root / "Many").glob("*.jpg"))) == 240)
    app.op("select_clear")
    # Layouts keep the visible tile: the first visible item stays on screen across a real click on each layout.
    many = app.op("activate", album="Many", noWait=True)
    app.wait_idle()
    time.sleep(1.0)
    app.op("scroll_to", y=1500)
    time.sleep(0.4)
    before = app.op("first_visible")
    win = app.op("geom")["window"]
    for x, name in ((326, "masonry"), (362, "apple"), (285, "grid")):
        send(op="click", x=win["x"] + x, y=win["y"] + 30)
        seen = wait(lambda: (lambda v: v["first"] <= before["first"] <= v["last"] and v["scrollTop"] > 0)(app.op("first_visible")), 6)
        check(f"scroll anchor survives the switch to {name}", seen, (before, app.op("first_visible")))
    # Focus stays on a clicked album row; keyboard walks the tree, Tab reaches the gallery, arrows move a visible focus.
    ax, ay = center(app.op("album_rect", album="Many"))
    send(op="click", x=ax, y=ay)
    wait(lambda: app.op("state")["active"] == "Many")
    time.sleep(0.4)
    state = app.op("focus_state")
    check("clicked album row keeps focus", state["widget"] == "AlbumTree", state)
    row = state["treeRow"]
    send(op="key", code=KEY_UP)
    time.sleep(0.2)
    state = app.op("focus_state")
    check("arrow moves the tree row with visible focus", state["treeRow"] == row - 1 and state["visible"], state)
    for _ in range(16):
        send(op="key", code=KEY_TAB)
        time.sleep(0.08)
        if app.op("focus_state")["widget"] == "GalleryView":
            break
    check("Tab reaches the gallery", app.op("focus_state")["widget"] == "GalleryView", app.op("focus_state"))
    send(op="key", code=KEY_RIGHT)
    time.sleep(0.2)
    first = app.op("focus_state")["galleryFocus"]
    send(op="key", code=KEY_RIGHT)
    time.sleep(0.2)
    check("arrow moves gallery focus", app.op("focus_state")["galleryFocus"] == first + 1, (first, app.op("focus_state")))
    # Duplicate preview with real input.
    app.op("activate", album="Dups")
    app.wait_idle()
    time.sleep(1.0)
    app.op("dup_open", check=False)
    wait(lambda: app.op("dup_state")["total"] > 0, 30, 0.3)
    rect = app.op("dup_tile_rect", group=0, item=0)
    px, py = center(rect)
    send(op="move", x=px, y=py)
    time.sleep(0.3)
    send(op="click", x=rect["x"] + 30, y=rect["y"] + 30)
    check("clicking a duplicate opens its preview", wait(lambda: (lambda v: v["visible"] and v["ready"])(app.op("dup_preview_state")), 8), app.op("dup_preview_state"))
    name = app.op("dup_preview_state")["name"]
    send(op="key", code=KEY_RIGHT)
    check("preview arrow steps", wait(lambda: app.op("dup_preview_state")["name"] != name, 5), app.op("dup_preview_state"))
    capture_window(app.proc.pid, work / "dup-preview.png")
    send(op="key", code=KEY_ESC)
    check("Esc closes the preview", wait(lambda: not app.op("dup_preview_state")["visible"], 3))
    app.op("dup_close", check=False)
    # Privacy shield: nothing of the gallery stays readable in the window pixels, from this app's own capture.
    app.op("activate", album="Alpha")
    time.sleep(1)
    clear_shot, shield_shot = work / "shield-before.png", work / "shield-after.png"
    capture_window(app.proc.pid, clear_shot)
    settings = app.op("core", method="get_settings")
    settings["privacy"]["enabled"] = True
    app.op("apply_settings", settings=settings)
    app.op("set_locked", locked=False)
    check("privacy shield initially clear", not app.op("privacy_state")["shielded"])
    focus_helper = subprocess.Popen([os.environ["QA_OCCLUDER"], "5", "100", "90", "90"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    focus_helper.stdout.readline()
    focus_helper.stdin.write("front\n"); focus_helper.stdin.flush()
    focus_helper.stdout.readline()
    check("real owned-helper focus loss shields", wait(lambda: app.op("privacy_state")["shielded"]))
    time.sleep(0.5)
    capture_window(app.proc.pid, shield_shot)
    focus_helper.stdin.write("quit\n"); focus_helper.stdin.flush(); focus_helper.wait(5); focus_helper = None
    activate(app)
    app.op("set_locked", locked=False)
    from PIL import Image, ImageStat
    def box(path):
        # The same part of the 1280x640 window at any backing scale (the box was written for 2x captures).
        image = Image.open(path).convert("RGB")
        sx, sy = image.width / 2560, image.height / 1280
        return image.crop((round(560 * sx), round(100 * sy), round(2000 * sx), round(600 * sy)))
    variance = lambda img: sum(ImageStat.Stat(img).stddev)
    check("privacy shield hides tile detail", variance(box(shield_shot)) < variance(box(clear_shot)) * 0.25, (variance(box(clear_shot)), variance(box(shield_shot))))
    # Video controls over the picture, with real input.
    app.op("activate", album="Clips")
    time.sleep(0.8)
    vx, vy = center(tile(0))
    send(op="click", x=vx, y=vy)
    wait(lambda: app.op("video_probe").get("present"), 8)
    time.sleep(0.8)
    win = app.op("geom")["window"]
    send(op="move", x=win["x"] + win["w"] / 2, y=win["y"] + win["h"] / 2)
    time.sleep(0.3)
    chrome = app.op("video_chrome")
    video, bar = chrome.get("video", {}), chrome["bar"]
    inside = chrome["barVisible"] and video and video["y"] <= bar["y"] and bar["y"] + bar["h"] <= video["y"] + video["h"]
    check("video controls sit over the picture", inside, chrome)
    seek = chrome["seek"]
    app.op("video_pause")
    start = app.op("video_probe")["currentTime"]
    target = 0.8 if start < 1.0 else 0.2
    send(op="click", x=seek["x"] + seek["w"] * target, y=seek["y"] + seek["h"] / 2)
    check("seek bar moves playback", wait(lambda: abs(app.op("video_probe")["currentTime"] - start) > 0.3, 4), (start, app.op("video_probe")["currentTime"]))
    for key, direction in (("back10", -1), ("forward10", 1)):
        button = chrome.get(key)
        check(key + " button exposed", bool(button))
        if not button: raise RuntimeError("video_chrome missing " + key)
        # A 3-second fixture still verifies exact clamping for a 10-second skip.
        before = app.op("video_probe")["currentTime"]
        duration = app.op("video_probe")["duration"]
        send(op="click", **dict(zip("xy", center(button))))
        expected = min(duration, max(0, before + direction * 10))
        check(key + " skips/clamps ten seconds", wait(lambda: abs(app.op("video_probe")["currentTime"] - expected) < .35, 4))
    volume = chrome["volumeBar"]
    send(op="click", x=volume["x"] + volume["w"] * 0.3, y=volume["y"] + volume["h"] / 2)
    check("volume bar changes volume", wait(lambda: abs(app.op("video_chrome")["volume"] - 0.3) < 0.15, 3), app.op("video_chrome")["volume"])
    mx, my = center(chrome["mute"])
    send(op="click", x=mx, y=my)
    check("mute toggles", wait(lambda: app.op("video_chrome")["muted"], 2))
    send(op="click", x=mx, y=my)
    # Pixels, not geometry: the same paused frame with and without the bar has to differ where the bar is.
    app.op("video_pause")
    time.sleep(0.3)
    send(op="move", x=win["x"] + win["w"] / 2, y=win["y"] + win["h"] / 2)
    time.sleep(0.4)
    with_bar = work / "video-overlay.png"
    check("video controls window is stacked above the main window", len(app_panels(app.proc.pid)) >= 1, app_panels(app.proc.pid))
    capture_app(app.proc.pid, with_bar)
    bar = app.op("video_chrome")["bar"]
    send(op="move", x=win["x"] + win["w"] / 2, y=win["y"] + 8)
    check("video controls hide when the pointer rests", wait(lambda: not app.op("video_chrome")["barVisible"], 5), app.op("video_chrome")["barVisible"])
    without_bar = work / "video-no-overlay.png"
    capture_app(app.proc.pid, without_bar)
    a, b = Image.open(with_bar).convert("L"), Image.open(without_bar).convert("L")
    Image.open(with_bar).convert("RGB").resize((1000, a.height * 1000 // a.width)).save(work / "video-overlay-small.png")
    scale = a.width / win["w"]
    region = tuple(int(v * scale) for v in (bar["x"] - win["x"], bar["y"] - win["y"], bar["x"] - win["x"] + bar["w"], bar["y"] - win["y"] + bar["h"]))
    from PIL import ImageChops
    difference = sum(ImageStat.Stat(ImageChops.difference(a.crop(region), b.crop(region))).mean)
    check("video controls are painted over the video surface", difference > 4, round(difference, 2))
    send(op="move", x=win["x"] + win["w"] / 2, y=win["y"] + win["h"] / 2)
    check("video controls return on pointer motion", wait(lambda: app.op("video_chrome")["barVisible"], 2))
    chrome = app.op("video_chrome")
    fx, fy = center(chrome["fullscreen"])
    send(op="click", x=fx, y=fy)
    check("fullscreen button enters full screen", wait(lambda: app.op("video_chrome")["windowFullScreen"], 6), app.op("video_chrome")["windowFullScreen"])
    time.sleep(1.5)
    app.op("video_fullscreen_exit", check=False)
    check("full screen exits", wait(lambda: not app.op("video_chrome")["windowFullScreen"], 6))
    time.sleep(1.0)
    activate(app)
    # The controls window must not stay above a lock screen or the shield.
    app.op("set_unfocused", unfocused=True)
    time.sleep(0.4)
    check("video controls hide behind the privacy shield", not app.op("video_chrome")["barVisible"] and not app_panels(app.proc.pid), app_panels(app.proc.pid))
    app.op("set_unfocused", unfocused=False)
    time.sleep(0.3)
    send(op="key", code=KEY_ESC)
    check("Esc closes the video", wait(lambda: not app.op("viewer_state")["open"], 3))
    check("no controls window after close", wait(lambda: not app_panels(app.proc.pid), 2), app_panels(app.proc.pid))
    app.op("activate", album="Alpha")
    time.sleep(0.5)
    (root / "Alpha").rename(root / "RenamedAlpha")
    check("external rename follows active album", wait(lambda: app.op("state")["active"] == "RenamedAlpha"))
    app.op("wait_ready")
    capture_window(app.proc.pid, work / "external-rename-album.png")
    check("renamed album has decoded visible tiles", app.op("visible")["blank"] == 0)
    (root / "RenamedAlpha").rename(root / "Alpha")
    wait(lambda: app.op("state")["active"] == "Alpha")
    # Assistive client: rows visible, selected children readable, pressing a row activates it.
    from qa import guard
    guard(app.proc.pid, window=getattr(app, "qa_window", None))
    probe = subprocess.run([ax_tool, str(app.proc.pid), "Alpha"], capture_output=True, text=True, timeout=180)
    (work / "ax.txt").write_text(probe.stdout + probe.stderr)
    check("assistive walk keeps the app alive", app.proc.poll() is None and "done" in probe.stdout, probe.returncode)
    check("album rows exposed", "AXRow" in probe.stdout or ("'Alpha'" in probe.stdout and "'Beta'" in probe.stdout), probe.stdout[:200])
    check("selected children readable", "selectedChildren=" in probe.stdout and "selectedChildren=-1" not in probe.stdout)
    check("AXPress activates the row", "status=0" in probe.stdout and wait(lambda: app.op("state")["active"] == "Alpha"), [l for l in probe.stdout.splitlines() if l.startswith("pressed")])
finally:
    try:
        send(op="quit")
    except Exception:
        pass
    for owned in (source, focus_helper):
        if owned and owned.poll() is None: owned.terminate(); owned.wait(5)
    inp.wait(5)
    app.close()
    # Written on every path, including a guard refusal or interruption, with the owned PID and exit status.
    (work / "results.json").write_text(json.dumps({"build": app.build, "pid": app.proc.pid, "exitCode": app.exit_code,
        "stopped": repr(sys.exc_info()[1]) if sys.exc_info()[1] else None, "results": results}, indent=1))
check("app exits cleanly", app.exit_code == 0, app.exit_code)
(work / "results.json").write_text(json.dumps({"build": app.build, "pid": app.proc.pid, "exitCode": app.exit_code, "stopped": None, "results": results}, indent=1))
failed = [r["name"] for r in results if not r["ok"]]
print(f"{len(results) - len(failed)}/{len(results)} passed", failed)
sys.exit(1 if failed else 0)
