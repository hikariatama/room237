#!/usr/bin/env python3
"""lottie_to_webp.py FRAMES_DIR OUT.webp FPS: joins rendered frames, applying the CSS invert the web app used."""
import sys
from pathlib import Path
from PIL import Image, ImageChops
frames = []
for path in sorted(Path(sys.argv[1]).glob("*.png")):
    image = Image.open(path).convert("RGBA")
    r, g, b, a = image.split()
    frames.append(Image.merge("RGBA", (ImageChops.invert(r), ImageChops.invert(g), ImageChops.invert(b), a)))
step = 1
fps = int(sys.argv[3])
frames[0].save(sys.argv[2], save_all=True, append_images=frames[1:], duration=round(1000 / fps), loop=0, lossless=False, quality=82, method=4)
print(sys.argv[2], len(frames))
