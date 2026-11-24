"""Edge cases of adding and changing files, on a synthetic library. Records what the app does; prints one line per case.

edge.py BINARY WORK [--only name,name]   (WORK: new directory inside $ROOM237_QA_FENCE)"""
import argparse, hashlib, json, os, shutil, sys, threading, time, unicodedata
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from diag_lib import launch, wait, read_trace, now_ms  # noqa: E402
from PIL import Image

COLORS = [(200, 40, 40), (40, 200, 40), (40, 40, 200), (200, 200, 40), (200, 40, 200), (40, 200, 200), (120, 120, 120)]


def img(path, color, size=(320, 240), fmt=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.new("RGB", size, color).save(path, fmt or ("JPEG" if path.suffix.lower() in (".jpg", ".jpeg") else path.suffix[1:].upper()))
    return path


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()[:12]


class Edge:
    def __init__(self, app, root, src, trace):
        self.app, self.root, self.src, self.trace = app, root, src, trace
        self.results = []

    def labels(self, album):
        return sorted(self.app.op("inventory", album=album)["labels"])

    def names(self, album):
        return sorted(self.app.op("inventory", album=album)["names"])

    def disk(self, album):
        d = self.root / album
        return sorted(p.name for p in d.iterdir() if not p.name.startswith(".room237"))

    def album_size(self, name):
        for a in self.app.op("state")["albums"]:
            if a["name"] == name:
                return a["size"]
        return None

    def settle(self, seconds=2.5):
        time.sleep(seconds)

    def events(self, since):
        rows = read_trace(self.trace, since)
        return {"toasts": [d for _, k, d in rows if k in ("toast-show", "toast-update", "toast-custom")],
                "errors": [d[:200] for _, k, d in rows if k == "core-error" and "superseded" not in d],
                "warnings": [d[:200] for _, k, d in rows if k == "log"],
                "popovers": [d for _, k, d in rows if k == "popover-open"],
                "resets": [d for _, k, d in rows if k == "model-reset"]}

    def visible(self):
        return self.app.op("visible")

    def record(self, name, verdict, expected, observed, since):
        ev = self.events(since)
        row = {"case": name, "verdict": verdict, "expected": expected, "observed": observed, **ev}
        self.results.append(row)
        print(f"[{verdict:5}] {name}: {json.dumps(observed, ensure_ascii=False)[:400]}", flush=True)
        if ev["toasts"]:
            print(f"        toasts {ev['toasts'][:4]}", flush=True)
        if ev["errors"] or ev["warnings"]:
            print(f"        errors {ev['errors'][:3]} warnings {ev['warnings'][:3]}", flush=True)

    def drop(self, target, paths, **kw):
        return self.app.op("drop", target=target, paths=[str(p) for p in paths], holdMs=50, **kw)


def run(args):
    work = Path(args.work)
    shutil.rmtree(work, ignore_errors=True)
    root, src, outside = work / "lib", work / "src", work / "outside"
    for i in range(24):
        img(root / "A" / f"a{i:02}.jpg", COLORS[i % 7])
    for i in range(6):
        img(root / "B" / f"b{i}.jpg", COLORS[(i + 3) % 7])
    img(root / "A" / "same.jpg", (255, 0, 0))
    img(root / "A" / "Case.jpg", (0, 255, 0))
    img(root / "A" / unicodedata.normalize("NFC", "café.jpg"), (0, 0, 255))
    img(outside / "secret.jpg", (250, 250, 250))
    (outside / "not-an-image.txt").write_text("plain text outside the library")
    app = launch(args.binary, root, work / "profile", work, "edge", extra_env={"R237_IO_ROOT": str(root)})
    e = Edge(app, root, src, app.trace_path)
    only = set(filter(None, args.only.split(",")))
    want = lambda n: not only or n in only
    try:
        app.wait_albums(60)
        app.wait_idle(120)
        app.op("activate", album="A", noWait=True)

        # ───────── names
        if want("dnd_same_name"):
            t = now_ms(); f = img(src / "x" / "same.jpg", (0, 0, 0)); before = sha(root / "A" / "same.jpg")
            e.drop("gallery", [f]); wait(lambda: len(e.disk("A")) > 27, 15); e.settle()
            files = [n for n in e.disk("A") if n.startswith("same")]
            ok = sha(root / "A" / "same.jpg") == before and len(files) == 2
            e.record("dnd_same_name", "ok" if ok else "BUG", "original kept, new one renamed", {"files": files, "labels": [l for l in e.labels("A") if l.startswith("same")]}, t)

        if want("dnd_two_same_names"):
            t = now_ms(); p, q = img(src / "p" / "dup.jpg", (10, 10, 10)), img(src / "q" / "dup.jpg", (240, 240, 10))
            e.drop("gallery", [p, q]); wait(lambda: sum(n.startswith("dup") for n in e.disk("A")) >= 2, 15); e.settle()
            files = [n for n in e.disk("A") if n.startswith("dup")]
            hashes = {sha(root / "A" / n) for n in files}
            e.record("dnd_two_same_names", "ok" if len(files) == 2 and len(hashes) == 2 else "BUG", "both kept", {"files": files}, t)

        if want("dnd_case_only"):
            t = now_ms(); f = img(src / "c" / "case.jpg", (1, 2, 3)); before = sha(root / "A" / "Case.jpg")
            e.drop("gallery", [f]); e.settle(3)
            files = [n for n in e.disk("A") if n.lower().startswith("case")]
            ok = sha(root / "A" / "Case.jpg") == before and len(files) == 2
            e.record("dnd_case_only", "ok" if ok else "BUG", "Case.jpg kept, case.jpg renamed", {"files": files}, t)

        if want("dnd_nfd_name"):
            t = now_ms(); f = img(src / "n" / unicodedata.normalize("NFD", "café.jpg"), (9, 9, 9)); before = sha(root / "A" / "café.jpg")
            e.drop("gallery", [f]); e.settle(3)
            files = [n for n in e.disk("A") if unicodedata.normalize("NFC", n).startswith("caf")]
            ok = sha(root / "A" / "café.jpg") == before and len(files) == 2
            e.record("dnd_nfd_name", "ok" if ok else "BUG", "NFC original kept, NFD copy renamed", {"files": [ascii(x) for x in files]}, t)

        if want("dnd_unicode_long"):
            t = now_ms()
            emoji = img(src / "u" / "😀 фото «тест».jpg", (50, 60, 70))
            long = img(src / "u" / ("д" * 120 + ".jpg"), (70, 60, 50))  # 245 bytes in UTF-8
            spaces = img(src / "u" / "  leading and trailing  .jpg", (5, 6, 7))
            e.drop("gallery", [emoji, long, spaces]); e.settle(3)
            labels = e.labels("A")
            obs = {"emoji": any("😀" in l for l in labels), "long": any(l.startswith("ддд") for l in labels),
                   "spaces": [ascii(l) for l in labels if "leading" in l]}
            e.record("dnd_unicode_long", "ok" if obs["emoji"] and obs["long"] and obs["spaces"] else "BUG", "all three imported", obs, t)

        # ───────── strange files by drag and drop
        if want("dnd_strange"):
            t = now_ms(); d = src / "s"; d.mkdir(parents=True, exist_ok=True)
            (d / "._apple.jpg").write_bytes(b"\x00\x05\x16\x07" + b"\x00" * 4092)  # AppleDouble header with a .jpg name
            img(d / ".hidden.jpg", (100, 0, 0))
            (d / "empty.jpg").write_bytes(b"")
            good = img(d / "trunc-src.jpg", (0, 100, 0), (1600, 1200)); (d / "truncated.jpg").write_bytes(good.read_bytes()[:3000]); good.unlink()
            img(d / "png-named.jpg", (0, 0, 100), fmt="PNG")
            img(d / "UPPER.JPG", (100, 100, 0))
            before = set(e.disk("A"))
            e.drop("gallery", sorted(d.iterdir()))
            app.op("confirm_panel", name="hiddenFiles", choice="accept", check=False)
            e.settle(4)
            added = sorted(set(e.disk("A")) - before)
            wait(lambda: (lambda v: v["ready"] == v["expected"])(e.visible()), 10)
            v = e.visible()
            app.op("snapshot", path=str(work / "strange.png"))
            e.record("dnd_strange", "info", "junk refused or shown as broken, valid ones added", {"added": added, "visible": v}, t)

        if want("dnd_garbage"):
            t = now_ms(); g = src / "g" / "garbage.jpg"; g.parent.mkdir(parents=True, exist_ok=True); g.write_bytes(os.urandom(6000))
            e.drop("gallery", [g]); wait(lambda: "garbage.jpg" in e.labels("A"), 15)
            app.op("set_sort", key="name", dir="desc"); time.sleep(0.5)
            ok = wait(lambda: (lambda v: v["ready"] == v["expected"])(e.visible()), 20)
            app.op("snapshot", path=str(work / "garbage.png"))
            app.op("set_sort", key="name", dir="asc")
            e.record("dnd_garbage", "ok" if ok else "BUG", "an undecodable file shows as broken, not as loading", {"settled": bool(ok), "visible": e.visible()}, t)

        if want("dnd_folder_and_text"):
            t = now_ms(); folder = src / "folder"
            for i in range(3):
                img(folder / f"f{i}.jpg", COLORS[i])
            img(folder / "sub" / "deep.jpg", (1, 99, 1)); img(folder / ".hidden-dir" / "skip.jpg", (99, 1, 1)); img(folder / ".skip.jpg", (9, 1, 1))
            os.symlink(outside, folder / "linked-dir")
            before = set(e.disk("A"))
            e.drop("gallery", [folder])
            asked = app.op("confirm_panel", name="hiddenFiles", choice="secondary", check=False).get("ok")  # .skip.jpg in the folder
            wait(lambda: (root / "A" / "folder" / "sub" / "deep.jpg").exists(), 15); e.settle(3)
            tree = sorted(str(p.relative_to(root / "A")) for p in (root / "A").rglob("*.jpg") if "folder" in str(p) or p.name in ("f0.jpg", "deep.jpg"))
            before_root = sum(1 for _ in root.rglob("*.jpg"))
            e.drop("gallery", [root]); e.drop("gallery", [root / "B"]); e.settle(3)
            after_root = sum(1 for _ in root.rglob("*.jpg"))
            e.drop("gallery", [outside / "not-an-image.txt"]); e.settle(2)
            albums = sorted(a["id"] for a in app.op("state")["albums"])
            ok = tree == ["folder/f0.jpg", "folder/f1.jpg", "folder/f2.jpg", "folder/sub/deep.jpg"] and after_root == before_root and "A/folder/sub" in albums
            e.record("dnd_folder_and_text", "ok" if ok else "BUG", "a folder becomes an album with its subfolders (not hidden, not linked); library folders are not copied",
                     {"tree": tree, "askedAboutHidden": bool(asked), "albums": [a for a in albums if a.startswith("A/folder")], "libraryCopied": after_root - before_root}, t)

        if want("dnd_hidden_choice"):
            out = {}
            for choice in ("accept", "secondary", "cancel"):
                t = now_ms(); d = src / f"h-{choice}"
                vis, hid = img(d / f"vis-{choice}.jpg", (1, 50, 1)), img(d / f".hid-{choice}.jpg", (50, 1, 1))
                e.drop("gallery", [vis, hid])
                answered = app.op("confirm_panel", name="hiddenFiles", choice=choice, check=False)
                e.settle(3)
                out[choice] = {"asked": bool(answered.get("ok")), "added": sorted(n for n in e.disk("A") if choice in n)}
            ok = (out["accept"]["added"] == [".hid-accept.jpg", "vis-accept.jpg"] and out["secondary"]["added"] == ["vis-secondary.jpg"]
                  and out["cancel"]["added"] == [] and all(v["asked"] for v in out.values()))
            e.record("dnd_hidden_choice", "ok" if ok else "BUG", "hidden files: asked; add, skip or cancel", out, t)

        if want("dnd_folder_names"):
            t = now_ms(); top = src / "names"
            for name in ("Bad:Name", "con", "trail.", "ok"):
                img(top / name / "p.jpg", (3, 3, 99))
            e.drop("gallery", [top]); wait(lambda: (root / "A" / "names").exists(), 10); e.settle(3)
            sub = sorted(p.name for p in (root / "A" / "names").iterdir() if p.is_dir() and not p.name.startswith(".")) if (root / "A" / "names").exists() else []
            e.record("dnd_folder_names", "ok" if sub == ["Bad_Name", "con_", "ok", "trail"] else "BUG", "folder names become valid album names", {"albums": sub}, t)
            t = now_ms(); e.drop("gallery", [top]); e.settle(3)
            files = sorted(p.name for p in (root / "A" / "names" / "ok").iterdir() if p.is_file() and not p.name.startswith(".")) if (root / "A" / "names" / "ok").exists() else []
            e.record("dnd_folder_again", "ok" if files == ["p.jpg", "p_1.jpg"] else "BUG", "the same folder again merges into its album", {"files": files}, t)

        if want("dnd_unreadable"):
            t = now_ms(); f1, f2 = img(src / "r" / "readable.jpg", (1, 1, 200)), img(src / "r" / "locked.jpg", (200, 1, 1))
            os.chmod(f2, 0)
            e.drop("gallery", [f1, f2]); e.settle(3)
            os.chmod(f2, 0o644)
            e.record("dnd_unreadable", "info", "readable added, partial toast names the other", {"added": [n for n in e.disk("A") if n in ("readable.jpg", "locked.jpg")]}, t)

        if want("dnd_own_file"):
            t = now_ms(); before = e.disk("A")
            e.drop("gallery", [root / "A" / "a00.jpg"]); e.settle(2)
            e.record("dnd_own_file", "ok" if e.disk("A") == before else "BUG", "dropping a file of the open album changes nothing", {"new": sorted(set(e.disk("A")) - set(before))}, t)

        # ───────── symlinks and hard links
        if want("dnd_symlink"):
            t = now_ms(); ln = src / "links"; ln.mkdir(parents=True, exist_ok=True)
            os.symlink(outside / "secret.jpg", ln / "link-to-outside.jpg")
            os.symlink(outside, ln / "link-to-dir")
            before = set(e.disk("A"))
            e.drop("gallery", [ln / "link-to-outside.jpg", ln / "link-to-dir"]); e.settle(3)
            added = sorted(set(e.disk("A")) - before)
            kinds = {n: ("symlink" if (root / "A" / n).is_symlink() else "file") for n in added}
            e.record("dnd_symlink", "ok" if all(k == "file" for k in kinds.values()) else "BUG", "a symlink is imported as a copy of its target, never as a link", kinds, t)

        if want("fs_symlink_outside"):
            t = now_ms(); before = app.op("state")["total"]
            os.symlink(outside / "secret.jpg", root / "A" / "zz-outside-link.jpg")
            os.symlink(root / "B" / "b0.jpg", root / "A" / "zz-inside-link.jpg")
            e.settle(4)
            labels = e.labels("A")
            obs = {"listed": [l for l in labels if l.startswith("zz-")], "totalDelta": app.op("state")["total"] - before,
                   "thumbs": sorted(p.name for p in (root / "A" / ".room237-thumb").iterdir() if p.name.startswith("zz-"))}
            e.record("fs_symlink_outside", "info", "links in an album folder are not followed outside the root", obs, t)
            (root / "A" / "zz-outside-link.jpg").unlink(); (root / "A" / "zz-inside-link.jpg").unlink(); e.settle(2)

        if want("fs_symlink_dirs"):
            t = now_ms()
            os.symlink(root / "B", root / "A" / "link-album")
            os.symlink(root / "A", root / "A" / "loop")
            os.symlink(outside, root / "outside-album")
            e.settle(4)
            app.op("albums_refresh"); e.settle(2)
            albums = [a["id"] for a in app.op("state")["albums"]]
            e.record("fs_symlink_dirs", "ok" if not any("link" in a or "loop" in a or "outside" in a for a in albums) else "BUG", "linked folders are not albums; no loop", {"albums": albums}, t)
            for p in (root / "A" / "link-album", root / "A" / "loop", root / "outside-album"):
                p.unlink()
            e.settle(2)

        if want("fs_hardlink"):
            t = now_ms(); before = e.album_size("A")
            os.link(root / "B" / "b1.jpg", root / "A" / "hard-b1.jpg")
            wait(lambda: e.album_size("A") == before + 1, 10); e.settle(2)
            e.record("fs_hardlink", "info", "a hard link is an ordinary file", {"listed": "hard-b1.jpg" in e.labels("A"), "size": e.album_size("A") - before}, t)

        # ───────── external changes
        if want("fs_replace_in_place"):
            t = now_ms(); target = root / "A" / "a01.jpg"; old = sha(target)
            thumb_dir = root / "A" / ".room237-thumb"
            thumbs = {p.name: p.stat().st_mtime_ns for p in thumb_dir.iterdir() if p.name.startswith("a01")}
            tmp = img(work / "repl.jpg", (255, 255, 255), (320, 240))
            shutil.copyfile(tmp, target)  # same inode, new bytes, like an editor saving in place
            e.settle(5)
            after = {p.name: p.stat().st_mtime_ns for p in thumb_dir.iterdir() if p.name.startswith("a01")}
            e.record("fs_replace_in_place", "ok" if after != thumbs else "BUG", "the preview is rebuilt", {"thumbBefore": len(thumbs), "rebuilt": after != thumbs, "contentChanged": sha(target) != old}, t)

        if want("fs_rename_favorite"):
            t = now_ms(); app.op("activate", album="A", noWait=True)
            idx = next(i for i in range(40) if app.op("item", index=i)["name"] == "a02.jpg")
            app.op("favorite", index=idx); e.settle(1)
            os.rename(root / "A" / "a02.jpg", root / "A" / "a02-renamed.jpg"); e.settle(4)
            app.op("activate", album="A", noWait=True)
            favs = [app.op("item", index=i) for i in range(app.op("state")["total"])]
            fav = [x["name"] for x in favs if x["favorite"]]
            e.record("fs_rename_favorite", "ok" if "a02-renamed.jpg" in fav else "BUG", "a favorite renamed outside stays a favorite", {"favorites": fav}, t)

        if want("fs_slow_write"):
            t = now_ms(); big = img(work / "big.png", (0, 128, 255), (2400, 1800), "PNG"); data = big.read_bytes()
            dest = root / "A" / "slow.png"
            with open(dest, "wb") as fh:  # Written in pieces over ~3 s, like a slow network copy.
                for k in range(10):
                    fh.write(data[k * len(data) // 10:(k + 1) * len(data) // 10]); fh.flush(); time.sleep(0.3)
            e.settle(6)
            app.op("activate", album="A", noWait=True)
            item = next((app.op("item", index=i) for i in range(app.op("state")["total"]) if app.op("item", index=i)["name"] == "slow.png"), None)
            thumb = root / "A" / ".room237-thumb" / "slow.png.webp"
            e.record("fs_slow_write", "ok" if item and thumb.exists() and item.get("width") == 2400 else "BUG", "a file written slowly ends up with its real preview and size",
                     {"item": item and {k: item.get(k) for k in ("width", "height")}, "thumb": thumb.exists()}, t)

        if want("fs_atomic_save"):
            t = now_ms(); tmp = root / "A" / ".atomic.jpg.tmp"
            img(tmp, (33, 66, 99), fmt="JPEG"); os.rename(tmp, root / "A" / "atomic.jpg"); e.settle(3)
            e.record("fs_atomic_save", "ok" if "atomic.jpg" in e.labels("A") else "BUG", "listed once, under its final name", {"labels": [l for l in e.labels("A") if "atomic" in l]}, t)

        if want("fs_junk"):
            t = now_ms()
            (root / "A" / "._a03.jpg").write_bytes(b"\x00\x05\x16\x07" + b"\x00" * 4092)
            (root / "A" / ".DS_Store").write_bytes(b"\x00" * 100)
            (root / "A" / "Thumbs.db").write_bytes(b"\x00" * 100)
            (root / "A" / "noext").write_bytes(Image.new("RGB", (10, 10)).tobytes())
            (root / "A" / "dir.jpg").mkdir()
            e.settle(4)
            labels = e.labels("A")
            e.record("fs_junk", "ok" if not any(l in labels for l in ("._a03.jpg", "Thumbs.db", "noext", "dir.jpg")) else "BUG",
                     "junk and folders with a media extension are not listed", {"listed": [l for l in labels if l in ("._a03.jpg", "Thumbs.db", "noext", "dir.jpg")], "albums": [a["id"] for a in app.op("state")["albums"]]}, t)

        if want("fs_burst_1000"):
            t = now_ms(); seed = img(work / "seed.jpg", (90, 90, 90), (64, 48)); before = e.album_size("B")
            for i in range(1000):
                shutil.copyfile(seed, root / "B" / f"burst{i:04}.jpg")
            ok = wait(lambda: e.album_size("B") == before + 1000, 120, 0.2)
            took = now_ms() - t
            app.op("activate", album="B", noWait=True); ready = wait(lambda: (lambda v: v["expected"] > 0 and v["ready"] == v["expected"])(e.visible()), 60, 0.2)
            e.record("fs_burst_1000", "ok" if ok else "BUG", "1000 files copied into a closed album show up", {"listedMs": took, "ok": bool(ok), "processingShown": e.events(t)["popovers"], "ready": bool(ready)}, t)
            app.op("activate", album="A", noWait=True)

        # ───────── races
        if want("race_move_while_processing"):
            t = now_ms(); files = [img(src / "m" / f"m{i:02}.jpg", COLORS[i % 7], (1200, 900)) for i in range(30)]
            e.drop("gallery", files); wait(lambda: sum(n.startswith("m") and n[1:3].isdigit() for n in e.disk("A")) >= 30, 30)
            names = [n for n in e.disk("A") if n.startswith("m") and n[1:3].isdigit()]
            ok_listed = wait(lambda: set(names) <= set(e.labels("A")), 30)
            from collections import Counter
            inv = app.op("inventory", album="A")
            dups = [n for n, c in Counter(inv["names"]).items() if c > 1]
            if dups or inv["total"] != len(inv["names"]):
                e.record("race_inventory_duplicates", "BUG", "each file listed once", {"dups": dups[:5], "total": inv["total"], "names": len(inv["names"])}, t)
            app.op("prepare_entries", album="A", names=names)
            app.op("move_entries", to="B"); e.settle(5)
            ev = e.events(t)
            failed = [w for w in ev["warnings"] if "Failed" in w]
            e.record("race_move_while_processing", "ok" if not failed else "BUG", "moving files whose previews are being built records no failures",
                     {"inB": sum(n.startswith("m") and n[1:3].isdigit() for n in e.disk("B")), "failures": failed[:3]}, t)
            app.op("activate", album="B", noWait=True); rb = wait(lambda: (lambda v: v["expected"] > 0 and v["ready"] == v["expected"])(e.visible()), 30, 0.2)
            e.record("race_moved_previews", "ok" if rb else "BUG", "moved files show previews in their new album", {"ready": bool(rb), "visible": e.visible()}, t)
            app.op("activate", album="A", noWait=True)

        if want("race_two_drops"):
            t = now_ms(); s1 = [img(src / "d1" / f"race{i}.jpg", (i * 20 + 10, 0, 0)) for i in range(10)]; s2 = [img(src / "d2" / f"race{i}.jpg", (0, i * 20 + 10, 0)) for i in range(10)]
            e.app.op("drop", target="gallery", paths=[str(p) for p in s1], holdMs=0)
            e.app.op("drop", target="gallery", paths=[str(p) for p in s2], holdMs=0)
            wait(lambda: sum(n.startswith("race") for n in e.disk("A")) >= 20, 30); e.settle(3)
            files = [n for n in e.disk("A") if n.startswith("race")]
            hashes = {sha(root / "A" / n) for n in files}
            listed = [l for l in e.labels("A") if l.startswith("race")]
            e.record("race_two_drops", "ok" if len(files) == 20 and len(hashes) == 20 and len(listed) == 20 else "BUG", "20 distinct files from two overlapping drops", {"files": len(files), "distinct": len(hashes), "listed": len(listed)}, t)

        if want("race_switch_album"):
            t = now_ms(); files = [img(src / "sw" / f"sw{i:02}.jpg", COLORS[i % 7]) for i in range(64)]
            app.op("drop", target="gallery", paths=[str(p) for p in files], holdMs=0)
            time.sleep(0.4); app.op("activate", album="B", noWait=True)
            wait(lambda: sum(n.startswith("sw") for n in e.disk("A")) >= 64, 30); e.settle(3)
            st = app.op("state")
            e.record("race_switch_album", "ok" if st["active"] == "B" and st["total"] == e.album_size("B") else "BUG", "the album opened during an import stays open and lists its own files",
                     {"active": st["active"], "total": st["total"], "B": e.album_size("B"), "resets": e.events(t)["resets"][-4:]}, t)
            app.op("activate", album="A", noWait=True)

        if want("race_delete_during_import"):
            t = now_ms(); files = [img(src / "dd" / f"dd{i:02}.jpg", COLORS[i % 7], (1200, 900)) for i in range(64)]
            app.op("drop", target="gallery", paths=[str(p) for p in files], holdMs=0)
            time.sleep(0.3)
            gone = [root / "A" / f"a{i:02}.jpg" for i in range(10, 16)]
            for p in gone:
                p.unlink(missing_ok=True)
            wait(lambda: sum(n.startswith("dd") for n in e.disk("A")) >= 64, 30); e.settle(4)
            nfc = lambda n: unicodedata.normalize("NFC", n)
            listed = [nfc(l) for l in e.labels("A") if not l.startswith(".")]
            disk = [nfc(n) for n in e.disk("A") if not n.startswith(".") and (root / "A" / n).is_file()]
            stale = [n for n in listed if n not in disk]
            missing = [n for n in disk if n not in listed and n.lower().endswith((".jpg", ".png", ".jpeg"))]
            e.record("race_delete_during_import", "ok" if not stale and not missing else "BUG", "the listing matches the folder after both settle", {"stale": stale[:6], "missing": missing[:6]}, t)
        # ───────── more names and albums
        if want("move_same_name"):
            t = now_ms(); img(root / "B" / "same.jpg", (77, 0, 77)); wait(lambda: "same.jpg" in e.labels("B"), 15)
            originals = {n: sha(root / "A" / n) for n in e.disk("A") if n.startswith("same")}
            app.op("prepare_entries", album="B", names=["same.jpg"]); app.op("move_entries", to="A"); e.settle(2)
            kept = all(sha(root / "A" / n) == h for n, h in originals.items())
            new = sorted(set(n for n in e.disk("A") if n.startswith("same")) - set(originals))
            e.record("move_same_name", "ok" if kept and len(new) == 1 and "same.jpg" not in e.disk("B") else "BUG", "moved file renamed, nothing overwritten", {"new": new, "kept": kept}, t)

        if want("fs_replace_new_inode"):
            t = now_ms(); target = root / "A" / "a04.jpg"
            thumb = root / "A" / ".room237-thumb" / "a04.jpg.webp"; before = thumb.stat().st_mtime_ns if thumb.exists() else None
            target.unlink(); img(target, (255, 128, 0)); e.settle(5)
            after = thumb.stat().st_mtime_ns if thumb.exists() else None
            e.record("fs_replace_new_inode", "ok" if after and after != before else "BUG", "Finder-style replace rebuilds the preview", {"rebuilt": after != before, "listed": "a04.jpg" in e.labels("A")}, t)

        if want("nfd_delete"):
            t = now_ms(); name = unicodedata.normalize("NFD", "ёжик.jpg"); img(root / "A" / name, (3, 3, 3)); e.settle(3)
            listed = [l for l in e.labels("A") if unicodedata.normalize("NFC", l) == unicodedata.normalize("NFC", name)]
            try:
                app.op("prepare_entries", album="A", names=listed); app.op("delete_entries"); err = None
            except RuntimeError as x:
                err = str(x)
            e.settle(2)
            left = [n for n in e.disk("A") if unicodedata.normalize("NFC", n) == unicodedata.normalize("NFC", name)]
            e.record("nfd_delete", "ok" if not left and not err else "BUG", "a decomposed name is deleted from the app", {"listed": [ascii(x) for x in listed], "left": [ascii(x) for x in left], "error": err}, t)

        if want("album_rename_existing"):
            t = now_ms()
            try:
                app.op("album_rename", album="B", name="A"); err = None
            except RuntimeError as x:
                err = str(x)
            e.settle(1)
            albums = sorted(a["id"] for a in app.op("state")["albums"])
            e.record("album_rename_existing", "ok" if err and "A" in albums and "B" in albums else "BUG", "refused, both albums intact", {"error": err, "albums": albums}, t)

        if want("race_album_renamed_during_import"):
            app.op("album_create", name="C"); wait(lambda: e.album_size("C") is not None, 10)
            t = now_ms(); files = [img(src / "ri" / f"ri{i:02}.jpg", COLORS[i % 7], (1600, 1200)) for i in range(64)]
            app.op("drop", target="C", paths=[str(p) for p in files], holdMs=0)
            time.sleep(0.5); os.rename(root / "C", root / "C2"); e.settle(8)
            on_disk = sum(1 for d in ("C", "C2") if (root / d).exists() for p in (root / d).iterdir() if p.name.startswith("ri"))
            albums = {a["id"]: a["size"] for a in app.op("state")["albums"]}
            e.record("race_album_renamed_during_import", "info", "every file lands once; the renamed album is listed with them", {"onDisk": on_disk, "albums": albums}, t)

        if want("race_album_deleted_during_import"):
            app.op("album_create", name="D"); wait(lambda: e.album_size("D") is not None, 10)
            t = now_ms(); files = [img(src / "rd" / f"rd{i:02}.jpg", COLORS[i % 7], (1600, 1200)) for i in range(64)]
            app.op("drop", target="D", paths=[str(p) for p in files], holdMs=0)
            time.sleep(0.5)
            for _ in range(20):  # The import keeps writing while the folder is removed.
                shutil.rmtree(root / "D", ignore_errors=True)
                if not (root / "D").exists():
                    break
                time.sleep(0.1)
            e.settle(8)
            albums = {a["id"]: a["size"] for a in app.op("state")["albums"]}
            e.record("race_album_deleted_during_import", "ok" if not (root / "D").exists() else "BUG", "a deleted album is not recreated by the import", {"recreated": (root / "D").exists(), "albums": albums}, t)
        if want("album_case_rename"):
            app.op("album_create", name="Trips"); wait(lambda: e.album_size("Trips") is not None, 10)
            t = now_ms()
            try:
                app.op("album_rename", album="Trips", name="trips"); err = None
            except RuntimeError as x:
                err = str(x)
            e.settle(2)
            ids = sorted(a["id"] for a in app.op("state")["albums"])
            e.record("album_case_rename", "ok" if "trips" in ids and not err else "BUG", "changing only the case of an album name works", {"error": err, "albums": ids, "onDisk": sorted(p.name for p in root.iterdir() if p.name.lower() == "trips")}, t)

        if want("album_bad_names"):
            t = now_ms(); out = {}
            for name in ("a/b", "..", ".room237-x", "x:y", "   ", "con"):
                try:
                    r = app.op("album_create", name=name); out[name] = r.get("exists")
                except RuntimeError as x:
                    out[name] = "error: " + str(x)[:80]
            e.settle(1)
            out["onDisk"] = sorted(p.name for p in root.iterdir())
            created = [n for n in ("a/b", "..", ".room237-x", "x:y", "   ", "con") if out.get(n) is True]
            e.record("album_bad_names", "ok" if not created else "BUG", "invalid names are refused with a message; nothing odd on disk", out, t)
    finally:
        (work / "edge.json").write_text(json.dumps(e.results, indent=1, ensure_ascii=False))
        app.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("binary"); ap.add_argument("work"); ap.add_argument("--only", default="")
    run(ap.parse_args())
