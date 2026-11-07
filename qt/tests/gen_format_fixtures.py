#!/usr/bin/env python3
"""Synthetic media in every supported format, with known size, date, orientation, alpha and animation.
Usage: gen_format_fixtures.py OUT_DIR. HEIC needs macOS sips; the result is plain files usable on any OS."""
import json, shutil, subprocess, sys
from pathlib import Path
from PIL import Image, ImageDraw
OUT = Path(sys.argv[1])
OUT.mkdir(parents=True, exist_ok=True)
FFMPEG = shutil.which("ffmpeg")
SRC = OUT / "_src"
SRC.mkdir(exist_ok=True)
expected = {}


def picture(w=640, h=360, alpha=False, tag=(40, 120, 220)):
    image = Image.new("RGBA" if alpha else "RGB", (w, h), (0, 0, 0, 0) if alpha else tag)
    draw = ImageDraw.Draw(image)
    draw.rectangle([0, 0, w // 2, h // 2], fill=(230, 60, 60, 255) if alpha else (230, 60, 60))
    draw.ellipse([w // 2, h // 2, w - 1, h - 1], fill=(60, 220, 90, 255) if alpha else (60, 220, 90))
    return image


def note(name, **facts):
    expected[name] = facts


base = picture()
base.save(OUT / "plain.jpg", quality=88)
note("plain.jpg", w=640, h=360, kind="image")
exif = Image.Exif()
exif[0x0112] = 6  # Rotate 90 CW when displayed.
exif[0x0132] = "2019:03:04 05:06:07"
exif.get_ifd(0x8769)[0x9003] = "2019:03:04 05:06:07"
base.save(OUT / "rotated-dated.jpeg", quality=88, exif=exif)
note("rotated-dated.jpeg", w=360, h=640, kind="image", date="2019-03-04")
picture(alpha=True).save(OUT / "alpha.png")
note("alpha.png", w=640, h=360, kind="image", alpha=True)
base.save(OUT / "plain.bmp")
note("plain.bmp", w=640, h=360, kind="image")
base.save(OUT / "plain.tiff")
note("plain.tiff", w=640, h=360, kind="image")
base.save(OUT / "lossy.webp", quality=80)
note("lossy.webp", w=640, h=360, kind="image")
picture(alpha=True).save(OUT / "alpha.webp", lossless=True)
note("alpha.webp", w=640, h=360, kind="image", alpha=True)
frames = [picture(tag=(20 * i, 80, 200 - 20 * i)) for i in range(8)]
frames[0].save(OUT / "animated.gif", save_all=True, append_images=frames[1:], duration=60, loop=0)
note("animated.gif", w=640, h=360, kind="image", animated=True)
frames[0].save(OUT / "animated.webp", save_all=True, append_images=frames[1:], duration=60, loop=0, quality=70)
note("animated.webp", w=640, h=360, kind="image", animated=True)
base.save(SRC / "base.png")
picture(alpha=True).save(SRC / "alpha.png")


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


subprocess.run(["sips", "-s", "format", "heic", "-s", "formatOptions", "70", str(SRC / "base.png"), "--out", str(OUT / "plain.heic")], check=True, stdout=subprocess.DEVNULL)
note("plain.heic", w=640, h=360, kind="image")
subprocess.run(["sips", "-s", "format", "heic", str(SRC / "alpha.png"), "--out", str(OUT / "alpha.heif")], check=True, stdout=subprocess.DEVNULL)
note("alpha.heif", w=640, h=360, kind="image", alpha=True)
subprocess.run(["sips", "-r", "90", str(SRC / "base.png"), "--out", str(SRC / "turned.png")], check=True, stdout=subprocess.DEVNULL)
subprocess.run(["sips", "-s", "format", "avif", str(SRC / "base.png"), "--out", str(OUT / "plain.avif")], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
if not (OUT / "plain.avif").exists():
    run(FFMPEG, "-y", "-i", str(SRC / "base.png"), "-c:v", "libaom-av1", "-still-picture", "1", "-crf", "30", str(OUT / "plain.avif"))
note("plain.avif", w=640, h=360, kind="image")
subprocess.run(["sips", "-s", "format", "avif", str(SRC / "alpha.png"), "--out", str(OUT / "alpha.avif")], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
if (OUT / "alpha.avif").exists():
    note("alpha.avif", w=640, h=360, kind="image", alpha=True)
video_in = ["-f", "lavfi", "-i", "testsrc2=d=3:s=640x360:r=30", "-f", "lavfi", "-i", "sine=f=440:d=3"]
date_tag = ["-metadata", "creation_time=2021-05-06T07:08:09Z"]
for name, codec, extra, audio in [
    ("h264.mp4", ["-c:v", "libx264", "-pix_fmt", "yuv420p"], date_tag, ["-c:a", "aac"]),
    ("hevc.mp4", ["-c:v", "libx265", "-tag:v", "hvc1", "-pix_fmt", "yuv420p"], [], ["-c:a", "aac"]),
    ("h264.mov", ["-c:v", "libx264", "-pix_fmt", "yuv420p"], date_tag, ["-c:a", "aac"]),
    ("mjpeg.avi", ["-c:v", "mjpeg", "-q:v", "4"], [], ["-c:a", "libmp3lame"]),
    ("mpeg4.avi", ["-c:v", "mpeg4", "-q:v", "4"], [], ["-c:a", "libmp3lame"]),
    ("vp9.webm", ["-c:v", "libvpx-vp9", "-b:v", "300k", "-deadline", "realtime", "-cpu-used", "8"], [], ["-c:a", "libopus"]),
    ("av1.webm", ["-c:v", "libaom-av1", "-crf", "45", "-cpu-used", "8", "-row-mt", "1"], [], ["-c:a", "libopus"]),
    ("h264.mkv", ["-c:v", "libx264", "-pix_fmt", "yuv420p"], [], ["-c:a", "aac"]),
    ("h264.m4v", ["-c:v", "libx264", "-pix_fmt", "yuv420p"], [], ["-c:a", "aac"]),
    ("flv1.flv", ["-c:v", "flv"], [], ["-c:a", "libmp3lame", "-ar", "44100"]),
    ("theora.ogv", ["-c:v", "libtheora", "-q:v", "6"], [], ["-c:a", "libvorbis"]),
    ("mpeg2.mpg", ["-c:v", "mpeg2video", "-q:v", "4"], [], ["-c:a", "mp2"]),
    ("wmv2.wmv", ["-c:v", "wmv2", "-q:v", "4"], [], ["-c:a", "wmav2"]),
    ("h264.3gp", ["-c:v", "libx264", "-pix_fmt", "yuv420p"], [], ["-c:a", "aac", "-ar", "16000", "-ac", "1"]),
    ("h264.mts", ["-c:v", "libx264", "-pix_fmt", "yuv420p"], [], ["-c:a", "aac", "-f", "mpegts"]),
]:
    try:
        run(FFMPEG, "-y", *video_in, *codec, *extra, *audio, "-shortest", str(OUT / name))
    except subprocess.CalledProcessError as error:
        print("SKIP", name, error)
        continue
    # Qt Multimedia's libav* is built without dav1d, so the viewer shows no AV1 frames (CODECS.md).
    note(name, w=640, h=360, kind="video", **({"date": "2021-05-06"} if extra else {}), **({"playback": False} if "av1" in name else {}))
shutil.rmtree(SRC)
(OUT / "broken.heic").write_bytes(b"this is not a picture")
note("broken.heic", kind="broken")
(OUT / "broken.mp4").write_bytes(b"this is not a video")
note("broken.mp4", kind="broken")
(OUT.parent / (OUT.name + ".expected.json")).write_text(json.dumps(expected, indent=1))
print(len(expected), "fixtures")
