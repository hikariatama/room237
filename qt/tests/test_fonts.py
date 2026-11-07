#!/usr/bin/env python3
"""The packaged app ships no font files and sets its text in an installed font: on macOS SF Pro Text when it is
installed, otherwise the system font spaced to the advances of SF Pro Text.
Usage: test_fonts.py APP_BINARY WORK_DIR. WORK_DIR is new and inside ROOM237_QA_FENCE. Writes results.json there.
On macOS the widths are compared with those of SF Pro Text (numbers measured on macOS 27 with Qt 6.11)."""
import json, os, sys, traceback
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, require_fenced
from PIL import Image

binary, work = Path(sys.argv[1]), Path(sys.argv[2])
require_fenced(work)
work.mkdir(parents=True, exist_ok=False)
results = []
FONT_SUFFIXES = (".ttf", ".otf", ".ttc", ".woff", ".woff2", ".dfont")
# Advance of "Room237 Albums Альбомы 2024" in SF Pro Text, by (pixel size, weight).
SF_PRO_TEXT = {(10, 500): 161.39, (12, 400): 189.20, (14, 400): 220.73, (14, 600): 230.23, (16, 500): 258.45, (20, 600): 329.02}


def check(name, ok, detail=""):
    results.append(dict(case=name, ok=bool(ok), detail=str(detail)[:600]))
    print(("ok  " if ok else "FAIL"), name, detail if not ok else "", flush=True)


def package_root(path):
    for parent in path.parents:
        if parent.suffix == ".app":
            return parent
    return path.parent


app = None
try:
    root = package_root(binary.resolve())
    files = [p for p in root.rglob("*") if p.is_file() and not p.is_symlink()]
    fonts = [str(p.relative_to(root)) for p in files if p.suffix.lower() in FONT_SUFFIXES]
    check("package: no font files", not fonts, fonts)

    library, profile = work / "lib", work / "profile"
    (library / "Album").mkdir(parents=True)
    Image.new("RGB", (64, 48), (120, 80, 40)).save(library / "Album" / "a.jpg", quality=90)
    app = App(binary, library, profile, log=work / "app.log")
    app.wait_albums()
    report = app.op("fonts")
    (work / "fonts.json").write_text(json.dumps(report, ensure_ascii=False, indent=1))
    check("resources: no font files", report["bundledFontFiles"] == [], report["bundledFontFiles"])
    samples = {(s["px"], s["weight"]): s for s in report["samples"]}
    families = sorted({s["family"] for s in report["samples"]})
    check("text resolves to an installed family", all(families) and report["applicationFont"] in families, (families, report["applicationFont"]))
    check("pixel sizes kept", all(s["pixelSize"] == px for (px, _), s in samples.items()), [(k, s["pixelSize"]) for k, s in samples.items()])
    check("weights differ: 14px 600 is wider than 400", samples[(14, 600)]["advance"] > samples[(14, 400)]["advance"], (samples[(14, 400)]["advance"], samples[(14, 600)]["advance"]))
    if sys.platform == "darwin":
        check("macOS: SF Pro Text or the system UI font", families in ([".AppleSystemUIFont"], ["SF Pro Text"]), families)
        drift = {f"{px}/{w}": round(100 * (samples[(px, w)]["advance"] / ref - 1), 2) for (px, w), ref in SF_PRO_TEXT.items()}
        check("macOS: widths within 1.5% of SF Pro Text", all(abs(d) <= 1.5 for d in drift.values()), drift)
        results[-1]["drift"] = drift
    elif os.name == "nt":
        check("Windows: Segoe UI", families == ["Segoe UI"], families)
    else:
        check("Linux: requested system family first with Noto Sans and DejaVu Sans after it", all(s["requested"][-2:] == ["Noto Sans", "DejaVu Sans"] for s in report["samples"]), report["samples"][0]["requested"])
    app.close()
    app = None
except Exception:
    check("exception", False, traceback.format_exc())
finally:
    if app:
        app.close()
(work / "results.json").write_text(json.dumps(dict(results=results, failed=sum(not r["ok"] for r in results)), ensure_ascii=False, indent=1))
sys.exit(1 if any(not r["ok"] for r in results) else 0)
