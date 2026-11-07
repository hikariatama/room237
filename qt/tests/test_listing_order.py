#!/usr/bin/env python3
"""Order of album listing answers around a missing open album, with a proxy core that holds and strips answers.
Usage: test_listing_order.py APP_BUNDLE_OR_DIR WORK_DIR. WORK_DIR is new and inside ROOM237_QA_FENCE.
Cases: a stale second miss must not release the album; a miss confirmed by a newer walk must; an answer about an earlier root must not land in the new one."""
import json, os, shutil, sys, time, traceback
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, require_fenced
from PIL import Image
source, work = Path(sys.argv[1]), Path(sys.argv[2])
require_fenced(work)
work.mkdir(parents=True, exist_ok=False)
results = []
def check(name, ok, detail=""):
    results.append(dict(case=name, ok=bool(ok), detail=str(detail)[:300]))
    print(("ok  " if ok else "FAIL"), name, detail if not ok else "", flush=True)
def wait(fn, timeout=8, step=.05):
    end = time.time() + timeout
    while time.time() < end:
        if fn(): return True
        time.sleep(step)
    return bool(fn())
# A copy of the app whose core is the proxy; the real core stays next to it.
bundle = work / "app"
macos = source / "Contents/MacOS" if (source / "Contents/MacOS").is_dir() else source
shutil.copytree(source, bundle / source.name, symlinks=True) if (source / "Contents").is_dir() else shutil.copytree(source, bundle / "dir", symlinks=True)
copy = bundle / (source.name if (source / "Contents").is_dir() else "dir")
bin_dir = copy / "Contents/MacOS" if (copy / "Contents").is_dir() else copy
(bin_dir / "room237-core").rename(bin_dir / "room237-core.real")
shutil.copy(Path(__file__).parent / "listing_proxy.py", bin_dir / "room237-core")
(bin_dir / "room237-core").chmod(0o755)
binary = next(p for p in bin_dir.iterdir() if p.name in ("Room237", "room237"))
def library(root, names):
    for i, name in enumerate(names):
        (root / name).mkdir(parents=True)
        for k in range(3):
            Image.new("RGB", (160, 120), (i * 60 % 255, k * 80, 120)).save(root / name / f"{name}{k}.jpg")
class Run:
    def __init__(self, name, albums=("Open", "Other")):
        self.dir = work / name; self.root = self.dir / "lib"; library(self.root, albums)
        self.control = self.dir / "proxy.json"; self.control.write_text("{}")
        self.app = App(binary, self.root, self.dir / "profile", log=self.dir / "app.log", env={"LISTING_PROXY_CONTROL": str(self.control)})
        self.app.wait_albums(); self.app.wait_idle(120)
    def count(self):
        try: return int((Path(str(self.control) + ".count")).read_text())
        except (OSError, ValueError): return 0
    def arm(self, **rule): self.control.write_text(json.dumps(rule))
    def close(self):
        self.app.close(); check(f"{self.dir.name}: clean exit", self.app.exit_code == 0, self.app.exit_code)
def stale_second_miss():
    r = Run("stale-second-miss"); a = r.app
    a.op("activate", album="Open"); a.op("wait_ready"); time.sleep(1.5)
    n = r.count()
    # Two walks in flight, both answered as if they had raced a move; the second began before the first miss was seen.
    r.arm(hold=[n + 1, n + 2], order=[n + 1, n + 2], drop={str(n + 1): ["Open"], str(n + 2): ["Open"]})
    a.op("albums_refresh"); a.op("albums_refresh")
    released = wait(lambda: Path(str(r.control) + ".released").exists(), 10)
    check("stale: both held answers were delivered in order", released)
    time.sleep(.2)
    check("stale: the second, older miss does not release the album", a.op("state")["active"] == "Open", a.op("state")["active"])
    check("stale: a fresh walk keeps it open", wait(lambda: r.count() >= n + 3 and a.op("state")["active"] == "Open", 6), a.op("state")["active"])
    time.sleep(1); check("stale: still open after the fresh answer", a.op("state")["active"] == "Open" and a.op("state")["total"] == 3, {k: a.op("state")[k] for k in ("active", "total")})
    r.close()
def confirmed_miss():
    r = Run("confirmed-miss"); a = r.app
    a.op("activate", album="Open"); a.op("wait_ready"); time.sleep(1.5)
    n = r.count()
    # The first walk misses; every later one too: the album is gone as far as the tree can tell.
    r.arm(drop={str(k): ["Open"] for k in range(n + 1, n + 30)})
    a.op("albums_refresh")
    check("confirmed: a miss seen again by a newer walk releases the album", wait(lambda: a.op("state")["active"] == "", 6), a.op("state")["active"])
    check("confirmed: nothing stale is listed", a.op("state")["total"] == 0, a.op("state")["total"])
    r.close()
def earlier_root():
    r = Run("earlier-root", albums=("OnlyA",)); a = r.app
    other = r.dir / "lib-b"; library(other, ("OnlyB",))
    n = r.count()
    r.arm(hold=[n + 1], order=[n + 1], waitForRelease=True)
    a.op("albums_refresh")
    wait(lambda: r.count() >= n + 1, 5)
    a.op("choose_root", kind="root", path=str(other))
    check("root: the new root lists its own album", wait(lambda: [x["id"] for x in a.op("state")["albums"]] == ["OnlyB"], 8), [x["id"] for x in a.op("state")["albums"]])
    r.arm(hold=[n + 1], order=[n + 1], waitForRelease=True, release=True)
    wait(lambda: Path(str(r.control) + ".released").exists(), 8)
    time.sleep(.5)
    st = a.op("state")
    check("root: the late answer about the earlier root is dropped", [x["id"] for x in st["albums"]] == ["OnlyB"], [x["id"] for x in st["albums"]])
    check("root: the open root stays the new one", st["root"] == str(other.resolve()), st["root"])
    r.close()
for case in [dict(stale=stale_second_miss, confirmed=confirmed_miss, root=earlier_root)[n] for n in sys.argv[3:]] or (stale_second_miss, confirmed_miss, earlier_root):
    try: case()
    except Exception:
        results.append(dict(case=getattr(case, "__name__", "?"), ok=False, detail=traceback.format_exc()[-600:])); print("ERROR", traceback.format_exc()[-600:], flush=True)
(work / "results.json").write_text(json.dumps(results, indent=1))
bad = [x for x in results if not x["ok"]]
print(f"{len(results) - len(bad)}/{len(results)} passed")
sys.exit(1 if bad else 0)
