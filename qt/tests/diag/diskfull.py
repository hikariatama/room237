"""Import into a nearly full volume: diskfull.py BINARY WORK. A 24 MB disk image holds the library."""
import json, os, shutil, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from diag_lib import launch, wait, read_trace, now_ms  # noqa: E402
from PIL import Image
import numpy as np

work = Path(sys.argv[2]); shutil.rmtree(work, ignore_errors=True); work.mkdir(parents=True)
image, mnt = work / "small.sparseimage", work / "mnt"
subprocess.run(["hdiutil", "create", "-type", "SPARSE", "-fs", "APFS", "-size", "24m", "-volname", "small", str(image)], check=True, capture_output=True)
mnt.mkdir(); subprocess.run(["hdiutil", "attach", "-nobrowse", "-mountpoint", str(mnt), str(image)], check=True, capture_output=True)
root, src = mnt / "lib", work / "src"
app = None
try:
    for i in range(5):
        Image.new("RGB", (200, 150), (i * 40, 80, 120)).save((root / "A").mkdir(parents=True, exist_ok=True) or root / "A" / f"a{i}.jpg")
    src.mkdir()
    rng = np.random.default_rng(1)
    for i in range(12):  # Noise compresses badly: about 3 MB each, 36 MB for a 24 MB volume.
        Image.fromarray(rng.integers(0, 255, (1000, 1000, 3), dtype=np.uint8)).save(src / f"big{i:02}.png")
    app = launch(sys.argv[1], root, work / "profile", work, "app")
    app.wait_albums(60); app.wait_idle(60); app.op("activate", album="A", noWait=True)
    t = now_ms()
    app.op("drop", target="gallery", paths=[str(p) for p in sorted(src.iterdir())], holdMs=0)
    wait(lambda: any(k == "toast-update" for _, k, _ in read_trace(app.trace_path, t)), 120)
    time.sleep(4)
    rows = read_trace(app.trace_path, t)
    disk = sorted(p.name for p in (root / "A").iterdir())
    tmp = [n for n in disk if n.endswith(".tmp") or n.startswith(".tmp")]
    imported = [n for n in disk if n.startswith("big")]
    sizes_ok = all((root / "A" / n).stat().st_size == (src / n).stat().st_size for n in imported if (src / n).exists())
    listed = sorted(app.op("inventory", album="A")["labels"])
    free = shutil.disk_usage(mnt).free
    res = {"imported": len(imported), "completeFiles": sizes_ok, "tempLeft": tmp, "listedMatches": sorted(n for n in listed if n.startswith("big")) == imported,
           "toasts": [d for _, k, d in rows if k.startswith("toast")], "errors": [d[:160] for _, k, d in rows if k in ("core-error", "log")][:6], "freeBytes": free,
           "alive": app.op("ping")["variant"] == "qt"}
    print(json.dumps(res, ensure_ascii=False, indent=1))
finally:
    if app:
        app.close()
    subprocess.run(["hdiutil", "detach", "-force", str(mnt)], capture_output=True)
