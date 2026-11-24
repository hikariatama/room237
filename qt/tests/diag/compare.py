"""Side-by-side numbers of two scenarios.py runs: compare.py BASE_DIR FIX_DIR [--json OUT]."""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from summarize import summarize  # noqa: E402

KEYS = [("settledMs", "settle ms"), ("resets", "model resets"), ("coreMs", "core ms"), ("stat", "stat"), ("open", "open"),
        ("write", "write"), ("fsync", "fsync"), ("mainStat", "main-thread stat"), ("toasts", "toasts")]


def flat(row):
    io, main = row["io"], row["ioMain"]
    return {"settledMs": row["settledMs"], "resets": row["model"].get("model-reset", 0), "coreMs": row["coreMs"],
            "stat": io.get("stat", 0), "open": io.get("open", 0), "write": io.get("write", 0), "fsync": io.get("fsync", 0),
            "mainStat": main.get("stat", 0), "toasts": len([t for t in row["toasts"]]), "ok": row["ok"],
            "core": row["core"], "albumsReadyMs": row["extra"].get("albumsReadyMs"), "firstTilesMs": row["extra"].get("firstTilesMs")}


def compare(base, fix):
    a, b = summarize(base), summarize(fix)
    out = {}
    for name in a:
        if name in b:
            out[name] = {"base": flat(a[name]), "fix": flat(b[name])}
    return out


if __name__ == "__main__":
    data = compare(sys.argv[1], sys.argv[2])
    if "--json" in sys.argv:
        Path(sys.argv[sys.argv.index("--json") + 1]).write_text(json.dumps(data, indent=1))
    for name, r in data.items():
        print(f"== {name}  ok {r['base']['ok']}/{r['fix']['ok']}")
        for key, label in KEYS:
            x, y = r["base"][key], r["fix"][key]
            delta = f"{(y - x) / x * 100:+.0f}%" if isinstance(x, (int, float)) and x else ""
            print(f"   {label:18} {x:>10} -> {y:>10} {delta}")
