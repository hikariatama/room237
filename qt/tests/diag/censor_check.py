"""Refuses a screenshot that may show picture detail: every tile region must be a smooth field.

The censored build turns pictures into 3x3 averages stretched to size, so inside a tile the second differences of
brightness stay tiny. Interface text and icons live outside tiles. Usage: censor_check.py PNG RECTS_JSON
(RECTS_JSON: [[x, y, w, h], ...] in image pixels), or import check(image, rects)."""
import json, sys
import numpy as np
from PIL import Image


def roughness(region):
    g = region.astype(np.float32)
    if g.shape[0] < 6 or g.shape[1] < 6:
        return 0.0
    lap = np.abs(g[2:, 1:-1] + g[:-2, 1:-1] + g[1:-1, 2:] + g[1:-1, :-2] - 4 * g[1:-1, 1:-1])
    return float(np.percentile(lap, 99))


def check(path, rects, inset=6, limit=12.0):
    gray = np.asarray(Image.open(path).convert("L"))
    worst = 0.0
    for x, y, w, h in rects:
        x0, y0, x1, y1 = int(x + inset), int(y + inset), int(x + w - inset), int(y + h - inset)
        if x1 - x0 < 8 or y1 - y0 < 8:
            continue
        region = gray[max(0, y0):max(0, y1), max(0, x0):max(0, x1)]
        if region.size:
            worst = max(worst, roughness(region))
    return worst <= limit, worst


if __name__ == "__main__":
    ok, worst = check(sys.argv[1], json.loads(open(sys.argv[2]).read()))
    print(json.dumps({"ok": ok, "worst": worst}))
    sys.exit(0 if ok else 1)
