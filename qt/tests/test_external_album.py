#!/usr/bin/env python3
"""The open album changes behind the application's back: rename, move, delete, nested, with selection, viewer and duplicates.
Usage: test_external_album.py APP_BINARY WORK_DIR. WORK_DIR is new and inside ROOM237_QA_FENCE. Writes results.json next to the fixtures."""
import hashlib, json, os, shutil, sys, time, traceback
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
    # Derived data is the core's own; only the user's files have to survive byte for byte.
    return {f.name: sha(f) for f in root.rglob("*") if f.is_file() and not any(p.startswith(".") for p in f.relative_to(root).parts)}


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


def picture(path, seed):
    # Distinct bars per seed, so no two seeds look alike to the perceptual hash.
    image = Image.new("RGB", (320, 240), (seed * 7 % 255, seed * 13 % 255, seed * 29 % 255))
    for i in range(0, 320, 16):
        image.paste(((i + seed) * 3 % 255, (i * seed) % 255, seed % 255), (i, 0, i + 8, 240))
    image.save(path, quality=90)


class Case:
    def __init__(self, name):
        self.dir = work / name
        self.root, self.profile = self.dir / "lib", self.dir / "profile"
        self.root.mkdir(parents=True)
        self.name = name

    def start(self):
        self.app = App(binary, self.root, self.profile, log=self.dir / "app.log")
        open_cases.append(self.app)
        self.app.wait_albums()
        self.app.wait_idle(120)
        return self.app

    def ids(self):
        return [a["id"] for a in self.app.op("state")["albums"]]

    def finish(self):
        self.app.close()
        check(f"{self.name}: clean exit", self.app.exit_code == 0, self.app.exit_code)


open_cases = []


def remove(path):
    # The way a file manager deletes while the core may still add a derived file.
    for _ in range(50):
        shutil.rmtree(path, ignore_errors=True)
        if not path.exists():
            return
        time.sleep(.05)
    raise OSError(f"cannot remove {path}")


def album(root, name, count, base=0):
    (root / name).mkdir(parents=True, exist_ok=True)
    for i in range(count):
        picture(root / name / f"{name.replace('/', '-')}{i:03}.jpg", base + i)


def rename_active():
    c = Case("rename-active")
    album(c.root, "Keep", 2, 500)
    album(c.root, "Open", 40)
    before = originals(c.root)
    a = c.start()
    a.op("activate", album="Open")
    a.op("wait_ready")
    a.op("scroll_to", y=300)
    time.sleep(.3)
    top = a.op("first_visible")["scrollTop"]
    a.op("select_all")
    (c.root / "Open").rename(c.root / "Renamed")
    ok = wait(lambda: a.op("state")["active"] == "Renamed")
    st = a.op("state")
    check("rename: the open album follows the new name", ok, st["active"])
    check("rename: old name left the tree", "Open" not in c.ids(), c.ids())
    check("rename: files still listed", wait(lambda: a.op("state")["total"] == 40), a.op("state")["total"])
    # The retained page shows until the reload answers.
    check("rename: items carry the new location", wait(lambda: a.op("item", index=0)["album"] == "Renamed"), a.op("item", index=0))
    check("rename: scroll position stays", wait(lambda: a.op("first_visible")["scrollTop"] == top, 4), a.op("first_visible"))
    # An index-based selection may stay only whole; a partial one could point at the wrong files.
    check("rename: selection stays whole or clears", a.op("selection")["count"] in (0, 40), a.op("selection"))
    a.op("wait_ready")
    check("rename: tiles decode from the new path", a.op("visible")["blank"] == 0, a.op("visible"))
    time.sleep(2)
    check("rename: old name is not recreated", not (c.root / "Open").exists())
    check("rename: originals unchanged", originals(c.root) == before)
    (c.root / "Renamed").rename(c.root / "Open")
    check("rename back follows too", wait(lambda: a.op("state")["active"] == "Open"), a.op("state")["active"])
    c.finish()


def nested():
    c = Case("nested")
    album(c.root, "Trips/2021", 12, 100)
    album(c.root, "Trips/2022", 3, 200)
    before = originals(c.root)
    a = c.start()
    a.op("activate", album="Trips/2021")
    a.op("wait_ready")
    (c.root / "Trips/2021").rename(c.root / "Trips/Year21")
    check("nested rename inside a known parent is seen", wait(lambda: a.op("state")["active"] == "Trips/Year21"), a.op("state")["active"])
    (c.root / "Trips").rename(c.root / "Journeys")
    check("parent rename rebinds the open child", wait(lambda: a.op("state")["active"] == "Journeys/Year21"), a.op("state")["active"])
    check("parent rename: tree is new", wait(lambda: "Trips" not in c.ids() and "Journeys/2022" in c.ids()), c.ids())
    check("parent rename: files listed", wait(lambda: a.op("state")["total"] == 12), a.op("state")["total"])
    (c.root / "New").mkdir()
    (c.root / "Journeys/Year21").rename(c.root / "New/Moved")
    check("move to another parent rebinds", wait(lambda: a.op("state")["active"] == "New/Moved"), a.op("state")["active"])
    (c.root / "Journeys/Sub").mkdir()
    check("new nested album appears", wait(lambda: "Journeys/Sub" in c.ids()), c.ids())
    check("originals unchanged", originals(c.root) == before)
    c.finish()


def gone(kind):
    c = Case(f"gone-{kind}")
    album(c.root, "Keep", 3, 300)
    album(c.root, "Doomed", 20, 400)
    outside = c.dir / "outside"
    outside.mkdir()
    a = c.start()
    keep = originals(c.root / "Keep")
    a.op("activate", album="Doomed")
    a.op("wait_ready")
    a.op("select_all")
    if kind == "delete":
        remove(c.root / "Doomed")
    else:
        (c.root / "Doomed").rename(outside / "Doomed")
    check(f"{kind}: open album is released", wait(lambda: a.op("state")["active"] == "" and "Doomed" not in c.ids()), a.op("state")["active"])
    st = a.op("state")
    check(f"{kind}: no stale tiles", wait(lambda: a.op("state")["total"] == 0), st["total"])
    check(f"{kind}: selection cleared", a.op("selection")["count"] == 0, a.op("selection"))
    check(f"{kind}: nothing painted", a.op("visible")["expected"] == 0, a.op("visible"))
    check(f"{kind}: no album recreated", wait(lambda: True) and not (c.root / "Doomed").exists())
    a.op("activate", album="Keep")
    a.op("wait_ready")
    check(f"{kind}: another album opens", a.op("state")["total"] == 3, a.op("state")["total"])
    check(f"{kind}: other album unchanged", originals(c.root / "Keep") == keep)
    if kind == "move":
        check("move: moved files stay intact outside", len(list((outside / "Doomed").glob("*.jpg"))) == 20)
    c.finish()


def viewer_cases():
    c = Case("viewer")
    album(c.root, "Open", 10, 600)
    album(c.root, "Other", 2, 700)
    a = c.start()
    a.op("activate", album="Open")
    a.op("wait_ready")
    a.op("viewer_open", index=3)
    (c.root / "Open").rename(c.root / "Elsewhere")
    check("viewer: closes with the renamed album", wait(lambda: not a.op("viewer_path")["open"]), a.op("viewer_path"))
    check("viewer: gallery follows", wait(lambda: a.op("state")["active"] == "Elsewhere"), a.op("state")["active"])
    # The retained page shows until the reload answers.
    wait(lambda: a.op("item", index=3)["album"] == "Elsewhere")
    a.op("wait_ready")
    a.op("viewer_open", index=3)
    check("viewer: reopens at the new path", "/Elsewhere/" in a.op("viewer_path")["full"], a.op("viewer_path"))
    remove(c.root / "Elsewhere")
    check("viewer: closes with the removed album", wait(lambda: not a.op("viewer_path")["open"]), a.op("viewer_path"))
    check("viewer: nothing stale", wait(lambda: a.op("state")["total"] == 0 and a.op("state")["active"] == ""), a.op("state")["total"])
    c.finish()


def duplicates_cases():
    c = Case("duplicates")
    album(c.root, "Open", 4, 800)
    for i in range(3):
        shutil.copy(c.root / "Open" / f"Open{i:03}.jpg", c.root / "Open" / f"copy{i:03}.jpg")
    a = c.start()
    a.op("activate", album="Open")
    a.op("wait_ready")
    a.op("dup_open", timeout=120)
    check("duplicates: groups found", a.op("state")["duplicateTotal"] == 3, a.op("state")["duplicateTotal"])
    (c.root / "Open").rename(c.root / "Moved")
    check("duplicates: stays open on the renamed album", wait(lambda: a.op("state")["active"] == "Moved" and a.op("state")["duplicates"]), a.op("state"))
    check("duplicates: lists the new location", wait(lambda: a.op("state")["duplicateTotal"] == 3), a.op("state")["duplicateTotal"])
    remove(c.root / "Moved")
    check("duplicates: closes with the removed album", wait(lambda: not a.op("state")["duplicates"] and a.op("state")["active"] == ""), a.op("state")["duplicates"])
    c.finish()


def favorites_cases():
    c = Case("favorites")
    album(c.root, "A", 3, 900)
    album(c.root, "Z", 1, 950)
    a = c.start()
    a.op("activate", album="A")
    a.op("wait_ready")
    a.op("favorite", index=0)
    a.op("album_favorites")
    check("favorites: one file", a.op("state")["total"] == 1, a.op("state")["total"])
    (c.root / "A").rename(c.root / "B")
    # The list is briefly empty while the catalog catches up with the new name.
    first = lambda: (a.op("item", index=0, check=False).get("result") or {}).get("album")
    check("favorites: paths follow an album rename", wait(lambda: a.op("state")["total"] == 1 and first() == "B"), first())
    check("favorites: stays open", a.op("state")["total"] == 1, a.op("state")["total"])
    c.finish()


def replaced():
    c = Case("replaced")
    album(c.root, "Open", 5, 20)
    a = c.start()
    a.op("activate", album="Open")
    a.op("wait_ready")
    a.op("select_all")
    (c.root / "Open").rename(c.dir / "aside")
    album(c.root, "Open", 2, 40)
    check("same name, other folder: shows the new contents", wait(lambda: a.op("state")["total"] == 2), a.op("state")["total"])
    check("same name, other folder: old selection gone", wait(lambda: a.op("selection")["count"] == 0), a.op("selection"))
    c.finish()


failed = 0
def root_switch():
    # Listings started for one root must not land in the other, however fast the switch.
    c = Case("root-switch")
    album(c.root, "OnlyA", 3, 1000)
    other = c.dir / "lib-b"
    album(other, "OnlyB", 2, 1100)
    a = c.start()
    for i in range(13):  # Ends on the other root.
        target = other if i % 2 == 0 else c.root
        a.op("choose_root", kind="root", path=str(target))
        if i % 3 == 0:
            (c.root / "OnlyA").rename(c.root / "OnlyA2") if (c.root / "OnlyA").exists() else (c.root / "OnlyA2").rename(c.root / "OnlyA")
    settled = wait(lambda: a.op("state")["albumsReady"] and [x["id"] for x in a.op("state")["albums"]] == ["OnlyB"], 15)
    st = a.op("state")
    check("root switch: the last root's albums only", settled, [x["id"] for x in st["albums"]])
    check("root switch: the active root is the last one", st["root"] == str(other.resolve()), st["root"])
    check("root switch: nothing of the other root is open", st["active"] in ("", "OnlyB"), st["active"])
    c.finish()


cases = dict(rename=rename_active, rootswitch=root_switch, nested=nested, delete=lambda: gone("delete"), move=lambda: gone("move"), viewer=viewer_cases, duplicates=duplicates_cases, favorites=favorites_cases, replaced=replaced)
for case in [cases[name] for name in sys.argv[3:]] or cases.values():
    try:
        case()
    except Exception as error:
        failed += 1
        results.append(dict(case=getattr(case, "__name__", "gone"), ok=False, detail=traceback.format_exc()[-800:]))
        print("ERROR", traceback.format_exc()[-800:], flush=True)
    while open_cases:
        open_cases.pop().close()
(work / "results.json").write_text(json.dumps(results, indent=1))
bad = [r for r in results if not r["ok"]]
print(f"{len(results) - len(bad)}/{len(results)} passed")
sys.exit(1 if bad else 0)
