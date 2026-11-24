"""Prints a compact table of a scenarios.py output directory (or a JSON of it with --json)."""
import json, sys
from pathlib import Path


def summarize(run_dir):
    rows = {}
    for f in sorted(Path(run_dir).glob("*.json")):
        if f.name == "summary.json":
            continue
        r = json.loads(f.read_text())
        if "diag" not in r:
            continue
        d = r["diag"]
        io = d.get("io") or {}
        tot = lambda scope: {op: sum(c.values()) for op, c in io.get(scope, {}).items()}
        trace = r["trace"]
        kinds = {}
        for _, k, _ in trace:
            kinds[k] = kinds.get(k, 0) + 1
        rows[r["name"]] = {
            "settledMs": r["settledMs"],
            "ok": r.get("ok", True),
            "core": {m: v["count"] for m, v in d["core"].items()},
            "coreMs": round(sum(v["ms"] for v in d["core"].values()), 1),
            "model": d["model"],
            "paints": sum(d["paints"].values()),
            "galleryPaints": d["paints"].get("GalleryView/viewport", 0),
            "io": tot("all"), "ioMain": tot("main"),
            "toasts": [x for _, k, x in trace if k in ("toast-show", "toast-update", "toast-custom")],
            "popovers": [x for _, k, x in trace if k == "popover-open"],
            "loopGaps": [float(x) for _, k, x in trace if k == "loop-gap"],
            "maxBlankTiles": r.get("maxBlankTiles"), "longFrames": None, "frames": r.get("frames"),
            "warnings": [x for _, k, x in trace if k == "log"][:10],
            "extra": {k: v for k, v in r.items() if k in ("albumsReadyMs", "processingDoneMs", "firstTilesMs", "dropMs", "copiedMs", "moveMs", "deleteMs", "files", "total")},
        }
    return rows


if __name__ == "__main__":
    rows = summarize(sys.argv[1])
    if "--json" in sys.argv:
        print(json.dumps(rows, indent=1, ensure_ascii=False))
    else:
        for name, r in rows.items():
            io, iom = r["io"], r["ioMain"]
            print(f"== {name}: settled {r['settledMs']} ms ok={r['ok']} {r['extra']}")
            print(f"   core {r['coreMs']} ms {r['core']}")
            print(f"   model {r['model']} paints {r['paints']} (gallery {r['galleryPaints']}) frames {r['frames']} long {r['longFrames']} blank {r['maxBlankTiles']} gaps {r['loopGaps'][:8]}")
            print(f"   io stat {io.get('stat')} open {io.get('open')} read {io.get('read')} write {io.get('write')} fsync {io.get('fsync')} rename {io.get('rename')} unlink {io.get('unlink')} | main stat {iom.get('stat')} open {iom.get('open')} read {iom.get('read')}")
            print(f"   toasts {r['toasts']} popovers {r['popovers']}")
            if r["warnings"]:
                print(f"   warnings {r['warnings'][:3]}")
