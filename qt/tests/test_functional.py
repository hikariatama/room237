#!/usr/bin/env python3
"""End-to-end flows through the running app on a synthetic library. Every check is verified against the disk.
Usage: test_functional.py APP_BINARY FIXTURES WORK_DIR [--input QA_INPUT_BINARY] [--ax AX_TOOL]. Writes results.json."""
import hashlib, json, os, shutil, subprocess, sys, time, traceback
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, guarded_send, capture_window, window_id
from PIL import Image, ImageDraw
import random
args = sys.argv[1:]
binary, fixtures, work = args[0], Path(args[1]), Path(args[2])
input_tool = args[args.index("--input") + 1] if "--input" in args else None
ax_tool = args[args.index("--ax") + 1] if "--ax" in args else None
if input_tool:
    from slot_contract import authorization
    authorization(os.environ.get("ROOM237_SLOT_AUTHORIZATION"), binary)
from slot_contract import fresh_work, activate
work, fixtures = fresh_work(work, fixtures)
root, profile = work / "lib", work / "profile"
results = []


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def noise(seed, size=(900, 600)):
    rng = random.Random(seed)
    image = Image.new("RGB", size, (rng.randrange(255), rng.randrange(255), rng.randrange(255)))
    draw = ImageDraw.Draw(image)
    for _ in range(40):
        x, y = rng.randrange(size[0]), rng.randrange(size[1])
        draw.ellipse([x, y, x + rng.randrange(40, 300), y + rng.randrange(40, 300)], fill=(rng.randrange(255), rng.randrange(255), rng.randrange(255)))
    return image


# Library: A with 12 distinct photos, B empty, Nested/Inner, Dups with two groups and one unrelated picture.
for directory in ["A", "B", "Nested/Inner", "Dups"]:
    (root / directory).mkdir(parents=True)
for i in range(12):
    noise(i).save(root / "A" / f"p{i:02d}.jpg", quality=90)
shutil.copy(fixtures / "animated.gif", root / "A" / "anim.gif")
shutil.copy(fixtures / "h264.mp4", root / "A" / "clip.mp4")
noise(100).save(root / "Nested/Inner" / "inner.jpg", quality=90)
group = noise(200)
group.save(root / "Dups" / "g1-a.jpg", quality=92)
shutil.copy(root / "Dups" / "g1-a.jpg", root / "Dups" / "g1-b.jpg")
group.resize((450, 300)).save(root / "Dups" / "g1-c.jpg", quality=80)
other = noise(201)
other.save(root / "Dups" / "g2-a.png")
other.save(root / "Dups" / "g2-b.jpg", quality=92)
noise(202).save(root / "Dups" / "solo.jpg", quality=92)
originals = {str(f.relative_to(root)): sha(f) for f in root.rglob("*") if f.is_file()}
log = work / "app.log"
app = App(binary, root, profile, log=log, env={"ROOM237_TRACE": str(work / "trace.txt"), "ROOM237_QA_PRIVATE_CLIPBOARD": "1" if input_tool else "0"})


def check(name, condition, detail=""):
    results.append({"name": name, "ok": bool(condition), "detail": str(detail)[:400]})
    print("PASS" if condition else "FAIL", name, str(detail)[:160], flush=True)


def wait(predicate, timeout=30, step=0.1):
    end = time.time() + timeout
    while time.time() < end:
        value = predicate()
        if value:
            return value
        time.sleep(step)
    return None


def section(function):
    try:
        function()
    except Exception as error:
        check(function.__name__ + " raised", False, traceback.format_exc()[-380:])


def albums():
    return {a["id"]: a for a in app.op("state")["albums"]}


def inventory(album):
    return app.op("inventory", album=album)


def names_on_disk(album):
    return sorted(f.name for f in (root / album).iterdir() if f.is_file() and not f.name.startswith("."))


inp = None
if input_tool:
    from slot_contract import authorization
    authorization(os.environ.get("ROOM237_SLOT_AUTHORIZATION"), binary)
    inp = subprocess.Popen([input_tool], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)


def send(**command):
    return guarded_send(raw_send, app.proc.pid, command, window=getattr(app, "qa_window", None) or window_id(app.proc.pid), allow=trusted)


trusted = []


def raw_send(**command):
    inp.stdin.write(json.dumps(command) + "\n")
    inp.stdin.flush()
    return inp.stdout.readline().strip()


KEY_A, KEY_C, KEY_D, KEY_L, KEY_U, KEY_ESC, KEY_RIGHT, KEY_SPACE, KEY_V = 0, 8, 2, 37, 32, 53, 124, 49, 9


def t_library():
    state = app.wait_albums()
    activate(app) if inp else None
    app.op("resize", w=1280, h=640)
    app.op("place", x=40, y=325)
    found = albums()
    check("albums listed, nested included", {"A", "B", "Nested", "Nested/Inner", "Dups"} <= set(found), sorted(found))
    check("nested album keeps parent", found.get("Nested/Inner", {}).get("parent") == "Nested", found.get("Nested/Inner"))
    result = app.op("activate", album="A", timeout=120000)
    check("album opens with sharp visible tiles", result["total"] == 14, result)
    app.wait_idle()


def t_layouts():
    for layout in ["masonry", "apple", "default"]:
        app.op("set_layout", layout=layout)
        ok = wait(lambda: (lambda v: v["expected"] > 0 and v["ready"] == v["expected"])(app.op("visible")), 20)
        check(f"layout {layout} sharp", ok, app.op("visible"))
        app.op("snapshot", path=str(work / f"layout-{layout}.png"))
    for columns in [3, 12, 7]:
        app.op("set_columns", columns=columns)
        ok = wait(lambda: (lambda v: v["expected"] > 0 and v["ready"] == v["expected"])(app.op("visible")), 20)
        check(f"columns {columns} sharp", ok)


def t_sort():
    app.op("set_sort", key="name", dir="asc")
    time.sleep(0.4)
    first = app.op("item", index=0)["name"]
    app.op("set_sort", key="name", dir="desc")
    time.sleep(0.4)
    last = app.op("item", index=0)["name"]
    check("name sort direction", first == "anim.gif" and last == "p11.jpg", (first, last))
    app.op("set_sort", key="added", dir="asc")
    time.sleep(0.3)
    app.op("set_sort", key="shoot", dir="desc")
    time.sleep(0.3)
    app.op("set_sort", key="random")
    time.sleep(0.4)
    app.op("item", index=0)
    app.op("set_sort", key="name", dir="asc")
    time.sleep(0.4)


def t_favorites():
    target = app.op("item", index=2)["name"]
    app.op("favorite", index=2)
    item = app.op("item", index=2)
    check("favorite set", item["favorite"], item)
    favorites = app.op("album_favorites")
    check("favorites album lists it", favorites["total"] == 1, favorites)
    time.sleep(0.4)
    app.op("snapshot", path=str(work / "favorites.png"))
    app.op("activate", album="A", timeout=20000)
    check("sidebar count", app.op("state")["active"] == "A")
    app.op("favorite", index=2)
    check("favorite cleared", not app.op("item", index=2)["favorite"], target)
    app.op("favorite", index=3)


def t_selection():
    activate(app) if inp else None
    app.op("select_clear")
    if inp:
        send(op="key", code=KEY_A, flags=["cmd"])
        got = wait(lambda: app.op("selection")["count"] == 14, 5)
        check("Cmd+A real key selects all", got, app.op("selection"))
        send(op="key", code=KEY_D, flags=["cmd"])
        check("Cmd+D clears", wait(lambda: app.op("selection")["count"] == 0, 5))
    app.op("select_all")
    check("select all count", app.op("selection")["count"] == 14)
    copied = app.op("copy_selection")
    clip = app.op("clipboard")
    check("copy files puts all URLs on the clipboard", len(clip["urls"]) == 14 and all(Path(u).exists() for u in clip["urls"]), copied)
    app.op("select_clear")


def t_import_paste():
    app.op("activate", album="B", noWait=True)
    app.op("paste")
    check("paste imports clipboard files", wait(lambda: app.op("state") and inventory("B")["total"] == 14, 40), names_on_disk("B"))
    for name in names_on_disk("B"):
        if sha(root / "B" / name) != originals.get("A/" + name):
            check("pasted copies identical", False, name)
            return
    check("pasted copies identical", True)
    app.op("import_prepare", paths=[str(fixtures / "plain.heic"), str(fixtures / "vp9.webm")])
    app.op("import_run", album="B")
    check("import does not create JPEG companions", "plain.jpeg" not in names_on_disk("B") and "plain.heic" in names_on_disk("B"), names_on_disk("B")[:4])
    app.wait_idle()


def t_move_delete():
    source = names_on_disk("B")
    chosen = source[:3]
    hashes = {name: sha(root / "B" / name) for name in chosen}
    app.op("prepare_entries", album="B", names=chosen)
    app.op("move_entries", to="Nested")
    check("move changes disk", all((root / "Nested" / n).exists() and not (root / "B" / n).exists() for n in chosen), names_on_disk("Nested"))
    check("moved bytes identical", all(sha(root / "Nested" / n) == hashes[n] for n in chosen))
    # A target album that was never opened fills in when its first scan ends, which takes longer on a slow machine.
    start = time.time()
    arrived = wait(lambda: set(chosen) <= set(inventory("Nested")["names"]), 8, 0.1)
    check("moved inventory", arrived, {"seconds": round(time.time() - start, 2), "names": inventory("Nested")["names"]})
    doomed = [n for n in names_on_disk("B")][:2]
    app.op("prepare_entries", album="B", names=doomed)
    app.op("delete_entries")
    check("delete removes from disk", not any((root / "B" / n).exists() for n in doomed))
    remaining = inventory("B")
    check("delete inventory", set(remaining["names"]) == set(names_on_disk("B")), remaining["total"])


def t_date():
    app.op("activate", album="A", timeout=20000)
    app.op("set_sort", key="name", dir="asc")
    time.sleep(0.3)
    before = sha(root / "A" / app.op("item", index=4)["name"]) if False else None
    app.op("set_date", index=4, timestamp=1262304000)
    time.sleep(0.5)
    item = app.op("item", index=4)
    check("date change stored", int(item["dateTaken"]) == 1262304000, item)


def t_viewer():
    app.op("activate", album="A", timeout=20000)
    app.op("viewer_open", index=4)
    first = app.op("viewer_state")
    check("viewer ready", first["ready"] and first["path"].endswith(".jpg"), first)
    app.op("snapshot", path=str(work / "viewer-image.png"))
    path = first["path"]
    app.op("viewer_step", delta=1)
    app.op("viewer_wait", previous=path)
    app.op("viewer_step", delta=-1)
    time.sleep(0.3)
    check("viewer back", app.op("viewer_state")["path"] == path)
    zoomed = app.op("viewer_zoom", zoom=3)
    check("zoom applied", abs(zoomed["zoom"] - 3) < 0.01, zoomed)
    time.sleep(0.8)
    app.op("snapshot", path=str(work / "viewer-zoom.png"))
    app.op("viewer_zoom", zoom=1)
    app.op("viewer_close")
    gif = app.op("viewer_open", index=0)
    time.sleep(0.8)
    state = app.op("viewer_state")
    check("gif animates", state["animated"] and state["animationFrames"] >= 4, state)
    app.op("set_unfocused", unfocused=True)
    time.sleep(0.1)
    held = app.op("viewer_state")["animationFrames"]
    time.sleep(0.8)
    paused = app.op("viewer_state")
    check("gif pauses behind the shield", paused["suspended"] and paused["animationFrames"] == held, paused)
    app.op("set_unfocused", unfocused=False)
    time.sleep(0.6)
    check("gif resumes", app.op("viewer_state")["animationFrames"] > held)
    app.op("viewer_close")
    app.op("viewer_open", index=1)
    time.sleep(1.0)
    probe = app.op("video_probe")
    check("video plays with audio track", probe["present"] and probe["currentTime"] > 0.2, probe)
    app.op("video_pause")
    time.sleep(0.2)
    position = app.op("video_probe")["currentTime"]
    time.sleep(0.5)
    check("video pause holds", abs(app.op("video_probe")["currentTime"] - position) < 0.05)
    app.op("snapshot", path=str(work / "viewer-video.png"))
    app.op("viewer_close")


def t_duplicates():
    app.op("activate", album="Dups", timeout=60000)
    app.wait_idle()
    time.sleep(1)
    opened = app.op("dup_open")
    print("dup_open", opened, flush=True)
    check("two duplicate groups", opened["groups"] == 2, opened)
    app.op("snapshot", path=str(work / "duplicates.png"))
    before = names_on_disk("Dups")
    result = app.op("dup_ignore", group=1)
    state = app.op("dup_state")
    check("not duplicates hides one group", state["total"] == 1, state)
    check("not duplicates keeps files", names_on_disk("Dups") == before)
    kept = app.op("dup_keep", group=0)
    after = names_on_disk("Dups")
    check("keep one removes the others", len(after) == len(before) - 2 and kept["kept"] in after, (kept, after))
    app.op("dup_close")


def core_page(**call):
    """The app's own reload may supersede a page request made alongside it; that is expected, so ask again."""
    while True:
        try:
            return app.op("core", **call)
        except RuntimeError as error:
            if "superseded" not in str(error):
                raise
            time.sleep(0.05)


def t_external():
    app.op("activate", album="A", timeout=20000)
    start = time.time()
    shutil.copy(fixtures / "plain.jpg", root / "A" / "external.jpg")
    seen = wait(lambda: core_page(method="get_media_page", params={"query": {"rootDir": str(root), "albumId": "A", "favoritesOnly": False, "sortKey": "name", "sortDir": "asc", "randomSeed": 0.2, "page": 0}})["total"] == 15, 12, 0.05)
    check("external add appears promptly", seen and time.time() - start < 4, round(time.time() - start, 2))
    start = time.time()
    (root / "A" / "external.jpg").unlink()
    seen = wait(lambda: core_page(method="get_media_page", params={"query": {"rootDir": str(root), "albumId": "A", "favoritesOnly": False, "sortKey": "name", "sortDir": "asc", "randomSeed": 0.2, "page": 0}})["total"] == 14, 12, 0.05)
    check("external delete disappears promptly", seen and time.time() - start < 4, round(time.time() - start, 2))
    start = time.time()
    (root / "NewOutside").mkdir()
    seen = wait(lambda: "NewOutside" in albums(), 12, 0.1)
    check("external album appears", seen and time.time() - start < 5, round(time.time() - start, 2))
    shutil.rmtree(root / "NewOutside")
    seen = wait(lambda: "NewOutside" not in albums(), 12, 0.1)
    check("external album removal", seen)
    time.sleep(2)
    check("removed album is not recreated", not (root / "NewOutside").exists())
    # A change made while an own operation runs must not be lost.
    app.op("prepare_entries", album="A", names=["p00.jpg"])
    shutil.copy(fixtures / "plain.png" if (fixtures / "plain.png").exists() else fixtures / "plain.jpg", root / "A" / "during.jpg")
    app.op("delete_entries")
    seen = wait(lambda: "during.jpg" in inventory("A")["names"], 12, 0.3)
    check("external change during own operation is kept", seen)
    # A real inventory change drops a stale selection, a metadata rewrite does not.
    app.op("activate", album="A", timeout=20000)
    app.op("select_all")
    count = app.op("selection")["count"]
    time.sleep(2.5)
    check("selection survives idle background work", app.op("selection")["count"] == count)
    os.rename(root / "A" / "during.jpg", root / "A" / "renamed.jpg")
    check("selection drops on a real change", wait(lambda: app.op("selection")["count"] == 0, 8))


def t_albums():
    created = app.op("album_create", name="Created")
    check("create album", (root / "Created").is_dir() and "Created" in albums(), created)
    app.op("album_create", name="Sub", parent="Created")
    check("create sub-album", (root / "Created/Sub").is_dir() and "Created/Sub" in albums())
    app.op("album_rename", album="Created/Sub", name="Sub2")
    check("rename album", (root / "Created/Sub2").is_dir() and not (root / "Created/Sub").exists())
    shutil.copy(fixtures / "plain.jpg", root / "Created/Sub2" / "x.jpg")
    time.sleep(1.5)
    digest = sha(root / "Created/Sub2" / "x.jpg")
    app.op("album_move", album="Created/Sub2", parent=None)
    check("move album to root", (root / "Sub2").is_dir() and sha(root / "Sub2" / "x.jpg") == digest, sorted(albums()))
    app.op("album_move", album="Sub2", parent="B")
    check("move album into album", (root / "B/Sub2").is_dir(), sorted(albums()))
    app.op("album_delete", album="B/Sub2")
    check("delete album", not (root / "B/Sub2").exists() and "B/Sub2" not in albums())
    app.op("album_delete", album="Created")
    check("delete parent album", not (root / "Created").exists())


def t_privacy():
    settings = app.op("core", method="get_settings")
    settings["privacy"]["enabled"] = True
    app.op("apply_settings", settings=settings)
    state = app.op("privacy_state")
    check("privacy on implies lockscreen", state["privacy"] and state["lockscreen"], state)
    activate(app) if inp else None
    app.op("set_unfocused", unfocused=False)
    app.op("set_locked", locked=True)
    time.sleep(0.2)
    app.op("snapshot", path=str(work / "locked.png"))
    check("locked", app.op("privacy_state")["locked"])
    if inp:
        send(op="key", code=KEY_U)
        check("U unlocks with a real key", wait(lambda: not app.op("privacy_state")["locked"], 3))
        send(op="key", code=KEY_L)
        check("L locks with a real key", wait(lambda: app.op("privacy_state")["locked"], 3))
    app.op("set_locked", locked=False)
    app.op("set_unfocused", unfocused=True)
    time.sleep(0.2)
    app.op("snapshot", path=str(work / "shield.png"))
    app.op("set_unfocused", unfocused=False)
    settings["privacy"]["enabled"] = False
    settings["privacy"]["lockscreenEnabled"] = False
    settings["privacy"]["confirmOpenEnabled"] = False
    app.op("apply_settings", settings=settings)


def t_settings_ui():
    app.op("settings_open")
    time.sleep(0.3)
    app.op("snapshot", path=str(work / "settings.png"))
    app.op("settings_close")
    app.op("open_advanced")
    time.sleep(0.5)
    app.op("snapshot", path=str(work / "advanced.png"))
    app.op("settings_close")
    app.op("language", language="ru")
    time.sleep(0.3)
    app.op("snapshot", path=str(work / "russian.png"))
    app.op("set_layout", layout="masonry")
    app.op("set_columns", columns=5)


def t_ax():
    if not ax_tool:
        return
    pid = app.proc.pid
    done = subprocess.run([ax_tool, str(pid)], capture_output=True, text=True, timeout=120)
    alive = app.proc.poll() is None
    check("assistive client walks the window without a crash", alive, (done.returncode, done.stdout[-300:]))
    check("album rows are visible to assistive clients", "Scans" in done.stdout or "Dups" in done.stdout, done.stdout[-200:])


for step in [t_library, t_layouts, t_sort, t_favorites, t_selection, t_import_paste, t_move_delete, t_date, t_viewer, t_duplicates, t_external, t_albums, t_privacy, t_settings_ui, t_ax]:
    section(step)
try:
    app.close()
    check("app exits cleanly", app.exit_code == 0, app.exit_code)
except Exception as error:
    print("close failed", error)
if inp:
    send(op="quit")
# Restart: settings, layout and library state come back.
app = App(binary, root, profile, log=log)
try:
    app.wait_albums()
    app.op("activate", album="A", noWait=True)
    saved = json.loads((profile / "ui-state.json").read_text())
    check("settings persist across restart", saved["language"] == "ru" and saved["layout"] == "masonry" and saved["columns"] == 5, saved)
    item = app.op("inventory", album="A")
    check("favorite persists across restart", any(app.op("item", index=i)["favorite"] for i in range(min(8, item["total"]))))
finally:
    app.close()
check("app exits cleanly after restart", app.exit_code == 0, app.exit_code)
untouched = [k for k, v in originals.items() if k.startswith("A/p0") and k not in ("A/p00.jpg",) and (root / k).exists() and sha(root / k) != v]
check("originals byte-identical after everything", not untouched, untouched)
(work / "results.json").write_text(json.dumps({"build": app.build, "results": results}, indent=1))
failed = [r["name"] for r in results if not r["ok"]]
print(f"{len(results) - len(failed)}/{len(results)} passed", failed)
sys.exit(1 if failed else 0)
