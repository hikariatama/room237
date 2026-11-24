"""Finds short-lived visual changes in a captured frame sequence (film_start/film_stop output).

A flicker is a screen cell that changes and returns to its earlier look within a few frames (A -> B -> A), or a
cell that changes twice in quick succession on the way to its final look (A -> B -> C with B short-lived). Cells
that change in most frames (spinners, progress bars) are reported as animated instead.

flicker.py FILM_DIR [OUT_DIR]  ->  prints JSON; with OUT_DIR, writes strips of frames around each incident."""
import json, re, sys
from pathlib import Path
import numpy as np
from PIL import Image

CELL = 8          # pixels of the captured (half-size) frame
DELTA = 10.0      # mean brightness change that counts as a change
REVERT = 4.0      # back within this distance counts as a return
WINDOW = 8        # frames (~260 ms at 33 ms) a transient look may last


def load(film):
    files = sorted(Path(film).glob("f*.png"))
    times, cells = [], []
    for f in files:
        m = re.match(r"f(\d+)_(\d+)\.png", f.name)
        g = np.asarray(Image.open(f).convert("L"), dtype=np.float32)
        h, w = g.shape[0] // CELL * CELL, g.shape[1] // CELL * CELL
        c = g[:h, :w].reshape(h // CELL, CELL, w // CELL, CELL).mean(axis=(1, 3))
        times.append(int(m.group(2)))
        cells.append(c)
    return files, np.array(times), np.stack(cells) if cells else np.zeros((0, 1, 1))


def region(y, x, shape):
    # Half-size capture of a 1440x900 window: sidebar to x=150, header to y=38.
    px, py = x * CELL, y * CELL
    if px < 150:
        return "sidebar"
    if py < 38:
        return "header"
    if py > shape[0] * CELL - 90 and px > shape[1] * CELL * 0.6:
        return "toast"
    return "content"


def analyze(film, out=None):
    files, times, v = load(film)
    if len(v) < 3:
        return {"frames": len(v), "incidents": []}
    d = np.abs(np.diff(v, axis=0))
    changed = d > DELTA
    activity = changed.mean(axis=0)
    animated = activity > 0.3
    events = []  # (frame, y, x, kind, magnitude, duration)
    T, H, W = v.shape
    for y in range(H):
        for x in range(W):
            if animated[y, x]:
                continue
            series = v[:, y, x]
            i = 1
            while i < T:
                if abs(series[i] - series[i - 1]) > DELTA:
                    before = series[i - 1]
                    # Settles where it stays put for 3 frames.
                    j = i
                    while j + 3 < T and not np.all(np.abs(series[j:j + 3] - series[j]) < REVERT):
                        j += 1
                    end = min(j, T - 1)
                    duration = end - i
                    if duration <= WINDOW:
                        final = series[end]
                        peak = np.max(np.abs(series[i:end + 1] - before))
                        if abs(final - before) < REVERT and peak > DELTA:
                            events.append((i, y, x, "revert", float(peak), int(duration)))
                        elif duration >= 1:
                            # A detour: the cell passed through a look far from both ends.
                            mid = series[i:end]
                            off = np.min(np.abs(mid - final)) if len(mid) else 0
                            if off > DELTA and np.min(np.abs(mid - before)) > DELTA:
                                events.append((i, y, x, "detour", float(off), int(duration)))
                    i = max(end, i + 1)
                else:
                    i += 1
    # Cluster events that start within 2 frames of each other.
    events.sort()
    incidents = []
    for e in events:
        if incidents and e[0] - incidents[-1]["last"] <= 2:
            inc = incidents[-1]
        else:
            inc = {"first": e[0], "last": e[0], "cells": [], "kinds": {}, "regions": {}}
            incidents.append(inc)
        inc["last"] = e[0]
        inc["cells"].append((e[1], e[2], e[4], e[5]))
        inc["kinds"][e[3]] = inc["kinds"].get(e[3], 0) + 1
        r = region(e[1], e[2], (H, W))
        inc["regions"][r] = inc["regions"].get(r, 0) + 1
    report = []
    for n, inc in enumerate(incidents):
        ys = [c[0] for c in inc["cells"]]
        xs = [c[1] for c in inc["cells"]]
        size = len(inc["cells"])
        if size < 4:  # Antialiasing noise at text edges.
            continue
        item = {"tMs": int(times[inc["first"]]), "frame": inc["first"], "cells": size, "kinds": inc["kinds"],
                "regions": inc["regions"], "maxDurationFrames": max(c[3] for c in inc["cells"]),
                "bbox": [min(xs) * CELL * 2, min(ys) * CELL * 2, (max(xs) + 1) * CELL * 2, (max(ys) + 1) * CELL * 2],
                "peak": round(max(c[2] for c in inc["cells"]), 1)}
        report.append(item)
        if out:
            strip(files, inc, item, Path(out) / f"incident_{item['tMs']:06d}.png")
    anim = np.argwhere(animated)
    return {"frames": int(T), "spanMs": int(times[-1] - times[0]), "animatedCells": int(animated.sum()),
            "animatedBBox": [int(anim[:, 1].min()) * CELL * 2, int(anim[:, 0].min()) * CELL * 2, int(anim[:, 1].max() + 1) * CELL * 2, int(anim[:, 0].max() + 1) * CELL * 2] if len(anim) else None,
            "incidents": report}


def strip(files, inc, item, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    a, b = max(0, inc["first"] - 2), min(len(files) - 1, inc["last"] + item["maxDurationFrames"] + 2)
    frames = [Image.open(files[i]).convert("RGB") for i in range(a, b + 1)][:12]
    x0, y0, x1, y1 = [c // 2 for c in item["bbox"]]
    pad = 24
    box = (max(0, x0 - pad), max(0, y0 - pad), min(frames[0].width, x1 + pad), min(frames[0].height, y1 + pad))
    crops = [f.crop(box) for f in frames]
    scale = min(1.0, 300 / max(1, crops[0].width))
    crops = [c.resize((max(1, int(c.width * scale)), max(1, int(c.height * scale)))) for c in crops]
    w, h = crops[0].width, crops[0].height
    sheet = Image.new("RGB", (w * len(crops) + 4 * (len(crops) - 1), h), (255, 0, 255))
    for k, c in enumerate(crops):
        sheet.paste(c, (k * (w + 4), 0))
    sheet.save(path)


if __name__ == "__main__":
    print(json.dumps(analyze(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None), indent=1))
