#!/usr/bin/env python3
"""make_banner.py PHOTO OUT_BASE: the DMG background from the stored photograph, at 1x (640x400) and 2x (1280x800),
joined into OUT_BASE.tiff with macOS tiffutil.

The photograph (dmg-background.png next to this script, see dmg-background.md) is only scaled to the window; the one
addition is the arrow between the two icon positions. Finder places Room237.app at (170, 190) and Applications at
(450, 190), in front of the door, in the 640x400 window (make_dmg.py). Finder draws icon labels black in Light Mode
and white in Dark Mode, so the script stops when either colour would fall below a 3:1 contrast over most of a label.
"""
import argparse
import subprocess
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

WIDTH, HEIGHT = 640, 400
ICONS = ((170, 190), (450, 190))
ICON_HALF = 56
LABEL_ROWS = (250, 272)
LABEL_HALF_WIDTH = 44


def fit(photo, width, height):
    """Centre crop to the window's aspect ratio, then scale."""
    w, h = photo.size
    target = width / height
    if w / h > target:
        crop = round(h * target)
        photo = photo.crop(((w - crop) // 2, 0, (w - crop) // 2 + crop, h))
    else:
        crop = round(w / target)
        photo = photo.crop((0, (h - crop) // 2, w, (h - crop) // 2 + crop))
    return photo.resize((width, height), Image.LANCZOS)


def arrow(canvas, scale):
    """A plain arrow from the app towards Applications, with a soft shadow for the lighter parts of the photo."""
    ss = 4
    w, h = canvas.size
    layer = Image.new('RGBA', (w * ss, h * ss), (0, 0, 0, 0))
    (x0, y), (x1, _) = ICONS
    unit = scale * ss
    start, tip, y = (x0 + ICON_HALF + 24) * unit, (x1 - ICON_HALF - 24) * unit, y * unit
    head, stroke = 12 * unit, round(2.5 * unit)
    for color, offset, blur in (((0, 0, 0, 150), 1 * unit, 3 * unit), ((248, 244, 236, 235), 0, 0)):
        pen_layer = Image.new('RGBA', layer.size, (0, 0, 0, 0))
        pen = ImageDraw.Draw(pen_layer)
        oy = y + offset
        pen.line([(start, oy), (tip, oy)], fill=color, width=stroke)
        pen.line([(tip - head, oy - head), (tip, oy), (tip - head, oy + head)], fill=color, width=stroke, joint='curve')
        for x, yy in ((start, oy), (tip, oy), (tip - head, oy - head), (tip - head, oy + head)):
            pen.ellipse([x - stroke / 2, yy - stroke / 2, x + stroke / 2, yy + stroke / 2], fill=color)
        if blur:
            pen_layer = pen_layer.filter(ImageFilter.GaussianBlur(blur))
        layer = Image.alpha_composite(layer, pen_layer)
    layer = layer.resize((w, h), Image.LANCZOS)
    return Image.alpha_composite(canvas.convert('RGBA'), layer).convert('RGB')


def luminance(rgb):
    channel = np.asarray(rgb, dtype=np.float64) / 255
    linear = np.where(channel <= 0.03928, channel / 12.92, ((channel + 0.055) / 1.055) ** 2.4)
    return linear @ np.array([0.2126, 0.7152, 0.0722])


def label_contrast(image, scale):
    """WCAG contrast of black and of white text over each label area, using the 5th and 95th luminance percentiles."""
    result = {}
    for x, _ in ICONS:
        box = ((x - LABEL_HALF_WIDTH) * scale, LABEL_ROWS[0] * scale, (x + LABEL_HALF_WIDTH) * scale, LABEL_ROWS[1] * scale)
        lum = luminance(np.asarray(image.crop(box)).reshape(-1, 3))
        dark, light = np.percentile(lum, 5), np.percentile(lum, 95)
        result[x] = {'black': (dark + 0.05) / 0.05, 'white': 1.05 / (light + 0.05)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('photo', type=Path, help='qt/packaging/macos/dmg-background.png')
    parser.add_argument('out_base', help='output path without extension')
    args = parser.parse_args()
    photo = Image.open(args.photo).convert('RGB')
    if photo.width < WIDTH * 2 or photo.height < HEIGHT * 2:
        raise SystemExit(f'photo is {photo.width}x{photo.height}; at least {WIDTH * 2}x{HEIGHT * 2} is needed for 2x')
    images = {scale: arrow(fit(photo, WIDTH * scale, HEIGHT * scale), scale) for scale in (1, 2)}
    contrast = label_contrast(images[2], 2)
    for x, values in contrast.items():
        print(f'label at x={x}: black {values["black"]:.2f}:1, white {values["white"]:.2f}:1')
    worst = min(v for values in contrast.values() for v in values.values())
    if worst < 3.0:
        raise SystemExit(f'label contrast {worst:.2f}:1 is below 3:1 for Finder\'s black or white labels')
    one, two = args.out_base + '-1x.png', args.out_base + '-2x.png'
    images[1].save(one, dpi=(72, 72))
    images[2].save(two, dpi=(144, 144))
    subprocess.run(['tiffutil', '-cathidpicheck', one, two, '-out', args.out_base + '.tiff'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
