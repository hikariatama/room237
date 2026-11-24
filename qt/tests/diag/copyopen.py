"""Opening a library: a copy of an indexed one (new inodes and ctimes), or with --cold the image as it is.

copyopen.py BINARY IMAGE PROFILE OUT [--cold]"""
import json, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from diag_lib import launch, wait, read_trace, now_ms, save_json  # noqa: E402
from scenarios import mount, unmount, MNT, IO_LIB  # noqa: E402

binary, image, profile_src, out = sys.argv[1:5]
out = Path(out); shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
clone = mount(Path(image), out)
app = None
try:
    cold = "--cold" in sys.argv
    if not cold:
        subprocess.run(["cp", "-Rp", str(MNT / "lib"), str(MNT / "copy")], check=True)  # As a backup restore: new inodes and ctimes.
    library = MNT / ("lib" if cold else "copy")
    profile = out / "profile"
    if profile_src != "-":
        shutil.copytree(profile_src, profile)
    t0 = time.time()
    app = launch(binary, library, profile, out, "app", io_lib=IO_LIB, extra_env={"R237_IO_ROOT": str(library)})
    app.wait_albums(120)
    sampler = subprocess.Popen(["sample", str(app.pid), "8", "1", "-file", str(out / "sample.txt")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    marks = {}
    while time.time() - t0 < 900:
        s = app.op("state"); p = app.op("processing_state")
        if not p["visible"] and "processingDone" not in marks and time.time() - t0 > 3 and not s.get("preloading"):
            marks["processingDone"] = round(time.time() - t0, 1)
        if "processingDone" in marks and s["hashTotal"] and s["hashDone"] >= s["hashTotal"]:
            marks["hashDone"] = round(time.time() - t0, 1); break
        if "processingDone" in marks and time.time() - t0 - marks["processingDone"] > 5 and not s["hashTotal"]:
            marks["hashDone"] = None; break
        time.sleep(0.5)
    sampler.wait(60)
    d = app.op("diag")
    io = d.get("io", {}).get("all", {})
    res = {**marks, "io": {k: sum(v.values()) for k, v in io.items()}, "ioByKind": {k: {c: n for c, n in v.items() if n} for k, v in io.items() if k in ("fullsync", "fsync", "rename", "mkdir", "write", "read")},
           "core": {m: (v["count"], round(v["ms"])) for m, v in d["core"].items() if m != "is_preloading"},
           "thumbsRegenerated": sum(1 for _, k, x in read_trace(app.trace_path, 0) if k == "core-event" and "media-processed" in x)}
    print(json.dumps(res, indent=1))
    save_json(out / "copyopen.json", res)
finally:
    if app:
        app.close()
    unmount(clone)
