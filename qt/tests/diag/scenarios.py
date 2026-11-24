"""Diagnostic scenarios on a real library inside a disposable disk image.

scenarios.py BINARY IMAGE PROFILE OUT --album NAME [--film] [--encrypted PASSWORD] [--only a,b]
IMAGE is a sparse image holding lib/ (the root) and incoming/ (sets of new files); it is cloned, mounted at
$ROOM237_QA_FENCE/mnt/run, used and detached. Each scenario writes OUT/<name>.json with timings, counters, the trace
slice and, with --film, frames under OUT/film/<name>/."""
import argparse, json, os, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from diag_lib import launch, wait, read_trace, now_ms, save_json  # noqa: E402

FENCE = Path(os.environ["ROOM237_QA_FENCE"])
MNT = FENCE / "mnt" / "run"
IO_LIB = os.environ.get("R237_IO_LIB")


def mount(image, work):
    clone = work / "run.sparseimage"
    if clone.exists():
        clone.unlink()
    subprocess.run(["cp", "-c", str(image), str(clone)], check=True)
    MNT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-mountpoint", str(MNT), str(clone)], check=True, capture_output=True)
    return clone


def unmount(clone):
    for _ in range(5):
        if subprocess.run(["hdiutil", "detach", str(MNT)], capture_output=True).returncode == 0:
            break
        time.sleep(1)
    else:
        subprocess.run(["hdiutil", "detach", "-force", str(MNT)], capture_output=True)
    clone.unlink(missing_ok=True)


class Run:
    def __init__(self, app, out, film):
        self.app, self.out, self.film = app, out, film

    def album(self, name):
        for a in self.app.op("state")["albums"]:
            if a["name"] == name:
                return a
        return None

    def begin(self, name, reset=True):
        self.name = name
        if reset:
            self.app.op("diag", reset=True)
        self.t0 = now_ms()
        if self.film:
            d = self.out / "film" / name
            shutil.rmtree(d, ignore_errors=True)
            self.app.op("film_start", dir=str(d), interval=33, scale=0.5)
        if not self.app.op("state")["duplicates"]:
            self.app.op("record_start")
            self.recording = True
        else:
            self.recording = False

    def end(self, extra, settle=2.0):
        t_done = now_ms()
        time.sleep(settle)
        rec = self.app.op("record_stop") if self.recording else None
        film = self.app.op("film_stop") if self.film else None
        diag = self.app.op("diag")
        trace = read_trace(self.app.trace_path, self.t0)
        blank = max((s["blank"] for s in rec["samples"]), default=0) if rec else None
        frames = rec["frameTimes"] if rec else []
        gaps = [b - a for a, b in zip(frames, frames[1:])]
        result = {"name": self.name, "settledMs": t_done - self.t0, **extra, "diag": diag, "film": film,
                  "maxBlankTiles": blank, "frames": len(frames), "frameGaps": gaps,
                  "trace": [[t - self.t0, k, d[:300]] for t, k, d in trace if k not in ("focus",)]}
        save_json(self.out / f"{self.name}.json", result)
        print(f"{self.name}: settled {result['settledMs']} ms, blank {blank}, toasts {[d for _, k, d in result['trace'] if k.startswith('toast-show')]}", flush=True)
        return result


def visible_ready(app):
    v = app.op("visible")
    return v["expected"] > 0 and v["ready"] == v["expected"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary"); ap.add_argument("image"); ap.add_argument("profile"); ap.add_argument("out")
    ap.add_argument("--film", action="store_true")
    ap.add_argument("--encrypted")
    ap.add_argument("--only", default="")
    ap.add_argument("--album", required=True, help="the album of the library the scenarios work in")
    args = ap.parse_args()
    out = Path(args.out)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    only = set(filter(None, args.only.split(",")))
    want = lambda n: not only or n in only
    clone = mount(Path(args.image), out)
    profile = out / "profile"
    shutil.copytree(args.profile, profile)
    root, incoming = MNT / "lib", MNT / "incoming"
    app = None
    try:
        t_launch = now_ms()
        app = launch(args.binary, root, profile, out, "app", io_lib=IO_LIB, extra_env={"R237_IO_ROOT": str(root)})
        run = Run(app, out, args.film)
        run.begin("startup", reset=False)
        run.t0 = t_launch
        if args.encrypted:
            # The unlock screen comes first; the albums are listed after the password.
            wait(lambda: app.op("vault_state").get("visible"), 60, 0.1)
            unlocked = app.op("vault_unlock_ui", password=args.encrypted)
            if unlocked.get("error"):
                raise RuntimeError(f"unlock: {unlocked}")
        app.wait_albums(120)
        t_albums = now_ms() - t_launch
        wait(lambda: not app.op("processing_state")["visible"], 300, 0.2)
        t_open = now_ms() - t_launch
        activated = app.op("activate", album=args.album, timeout=120000)
        t_tiles = now_ms() - t_launch
        run.end({"albumsReadyMs": t_albums, "processingDoneMs": t_open, "firstTilesMs": t_tiles, "activate": activated})

        app.op("album_create", name="Inbox")
        app.op("album_create", name="Burst")
        app.op("activate", album=args.album)
        wait(lambda: visible_ready(app), 30)
        time.sleep(1)
        base = app.op("state")["total"]

        if want("dnd_gallery"):
            files = sorted(str(p) for p in (incoming / "dnd-gallery").iterdir())
            run.begin("dnd_gallery")
            reply = app.op("drop", target="gallery", paths=files, holdMs=300)
            ok = wait(lambda: app.op("state")["total"] == base + len(files) and visible_ready(app), 120, 0.03)
            run.end({"dropMs": reply["ms"], "ok": bool(ok), "files": len(files), "total": app.op("state")["total"]})
            base = app.op("state")["total"]

        if want("dnd_sidebar"):
            files = sorted(str(p) for p in (incoming / "dnd-sidebar").iterdir())
            before = run.album("Inbox")["size"]
            run.begin("dnd_sidebar")
            reply = app.op("drop", target="Inbox", paths=files, holdMs=300)
            ok = wait(lambda: run.album("Inbox")["size"] == before + len(files), 120, 0.03)
            run.end({"dropMs": reply["ms"], "ok": bool(ok), "files": len(files), "inbox": run.album("Inbox")["size"]})

        if want("direct_copy"):
            files = sorted((incoming / "direct").iterdir())
            run.begin("direct_copy")
            for f in files:  # Like a Finder copy of one file after another.
                shutil.copy2(f, root / args.album / f.name)
                time.sleep(0.05)
            copied = now_ms() - run.t0
            ok = wait(lambda: app.op("state")["total"] == base + len(files) and visible_ready(app), 120, 0.03)
            run.end({"copiedMs": copied, "ok": bool(ok), "files": len(files), "total": app.op("state")["total"]}, settle=3.0)
            base = app.op("state")["total"]

        if want("burst_other"):
            files = sorted((incoming / "burst").iterdir())
            run.begin("burst_other")
            for f in files:
                shutil.copy2(f, root / "Burst" / f.name)
            copied = now_ms() - run.t0
            ok = wait(lambda: run.album("Burst")["size"] == len(files), 120, 0.03)
            run.end({"copiedMs": copied, "ok": bool(ok), "files": len(files), "burst": run.album("Burst")["size"], "total": app.op("state")["total"]}, settle=3.0)

        if want("move_drag"):
            app.op("scroll_to", y=0)
            wait(lambda: visible_ready(app), 30)
            app.op("select", indices=list(range(30)))
            inbox = run.album("Inbox")["size"]
            run.begin("move_drag")
            reply = app.op("drop", target="Inbox", internal=True, holdMs=300)
            ok = wait(lambda: app.op("state")["total"] == base - 30 and run.album("Inbox")["size"] == inbox + 30 and visible_ready(app), 120, 0.03)
            run.end({"dropMs": reply["ms"], "ok": bool(ok), "total": app.op("state")["total"], "inbox": run.album("Inbox")["size"]})
            base = app.op("state")["total"]

        if want("move_back"):
            app.op("activate", album="Inbox")
            names = [app.op("item", index=i)["name"] for i in range(20)]
            app.op("prepare_entries", album="Inbox", names=names)
            inbox = run.album("Inbox")["size"]
            run.begin("move_back")
            reply = app.op("move_entries", to=args.album)
            ok = wait(lambda: app.op("state")["total"] == inbox - 20 and visible_ready(app), 60, 0.03)
            run.end({"moveMs": reply["ms"], "ok": bool(ok), "total": app.op("state")["total"]})
            app.op("activate", album=args.album)
            wait(lambda: visible_ready(app), 30)
            base = app.op("state")["total"]

        if want("delete"):
            names = [app.op("item", index=i)["name"] for i in range(10)]
            app.op("prepare_entries", album=args.album, names=names)
            run.begin("delete")
            reply = app.op("delete_entries")
            ok = wait(lambda: app.op("state")["total"] == base - 10 and visible_ready(app), 60, 0.03)
            run.end({"deleteMs": reply["ms"], "ok": bool(ok), "total": app.op("state")["total"]})
            base = app.op("state")["total"]

        if want("external_delete"):
            victims = sorted(p for p in (root / args.album).iterdir() if p.is_file() and not p.name.startswith("."))[100:110]
            run.begin("external_delete")
            for p in victims:
                p.unlink()
            ok = wait(lambda: app.op("state")["total"] == base - len(victims) and visible_ready(app), 60, 0.03)
            run.end({"ok": bool(ok), "total": app.op("state")["total"]}, settle=3.0)
            base = app.op("state")["total"]

        if want("idle"):
            run.begin("idle")
            time.sleep(10)
            run.end({"ok": True}, settle=0)

        summary = {"exitState": app.op("state")}
    finally:
        if app:
            app.close()
        unmount(clone)
    save_json(out / "summary.json", summary)


if __name__ == "__main__":
    main()
