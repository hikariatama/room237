#!/usr/bin/env python3
"""Near-duplicate quality on planted cases with known answers, plus alpha compositing.
Usage: test_near_duplicates.py QT_BINARY WORK. Reports pair precision and recall against ground truth."""
import io, itertools, json, random, shutil, sys, time, sqlite3
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, require_fenced
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter
binary, work = sys.argv[1], Path(sys.argv[2])
require_fenced(work)
if work.exists():
    raise ValueError("fresh work directory required; old evidence retained")
album = work / "lib" / "Set"
album.mkdir(parents=True)


def scene(seed, size=(1400, 1000)):
    rng = random.Random(seed)
    image = Image.new("RGB", size, tuple(rng.randint(0, 255) for _ in range(3)))
    pen = ImageDraw.Draw(image)
    for _ in range(60):
        x, y = rng.randint(0, size[0]), rng.randint(0, size[1])
        w, h = rng.randint(30, 420), rng.randint(30, 420)
        fill = tuple(rng.randint(0, 255) for _ in range(3))
        (pen.ellipse if rng.random() < 0.5 else pen.rectangle)([x, y, x + w, y + h], fill=fill)
    return image.filter(ImageFilter.GaussianBlur(2))


truth = {}  # file name -> group id; None means it must stay alone.
positive_kinds = {}


def add(image, name, group, kind=None, **save):
    image.save(album / name, **save)
    truth[name] = group
    if kind:
        positive_kinds[name] = kind


for g in range(25):
    base = scene(g)
    add(base, f"g{g:02}-orig.png", g)
    add(base, f"g{g:02}-jpeg90.jpg", g, "recompress", quality=90)
    add(base, f"g{g:02}-jpeg45.jpg", g, "low quality", quality=45)
    add(base.resize((700, 500), Image.LANCZOS), f"g{g:02}-half.jpg", g, "half size", quality=92)
    add(base, f"g{g:02}-copy.webp", g, "other format", quality=90)
    add(ImageEnhance.Brightness(base).enhance(1.06), f"g{g:02}-bright.jpg", g, "brightness +6%", quality=92)
    add(base.crop((14, 10, 1386, 990)), f"g{g:02}-crop.jpg", g, "2% crop", quality=92)
# Different pictures that share a structure must stay apart: same palette and layout seed family, moved content.
for n in range(60):
    add(scene(1000 + n), f"solo{n:02}.jpg", None)
for n in range(20):
    # A pair with the same background and different subject.
    rng = random.Random(5000 + n)
    background = scene(2000 + n)
    one, two = background.copy(), background.copy()
    ImageDraw.Draw(one).ellipse([100, 100, 800, 800], fill=(rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255)))
    ImageDraw.Draw(two).rectangle([600, 200, 1300, 900], fill=(rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255)))
    add(one, f"hard{n:02}-a.jpg", None, quality=92)
    add(two, f"hard{n:02}-b.jpg", None, quality=92)
# Transparency: the same picture in two containers is a duplicate; transparent areas composite over one fixed colour, so a different hidden colour under alpha 0 changes nothing.
transparent = scene(3000).convert("RGBA")
mask = Image.new("L", transparent.size, 0)
ImageDraw.Draw(mask).ellipse([200, 100, 1200, 900], fill=255)
transparent.putalpha(mask)
add(transparent, "alpha-a.png", "alpha")
add(transparent, "alpha-b.webp", "alpha", "alpha container", lossless=True, exact=True)
hidden = transparent.copy()
pixels = hidden.split()
hidden = Image.merge("RGBA", (Image.new("L", hidden.size, 17), Image.new("L", hidden.size, 200), Image.new("L", hidden.size, 90), mask))
visible_same = Image.composite(transparent, hidden, mask)
visible_same.putalpha(mask)
add(visible_same, "alpha-c.png", "alpha", "other colour under alpha 0")
other_shape = scene(3000).convert("RGBA")
mask2 = Image.new("L", other_shape.size, 0)
ImageDraw.Draw(mask2).rectangle([0, 500, 1400, 1000], fill=255)
other_shape.putalpha(mask2)
add(other_shape, "alpha-other-shape.png", None)
expected = {frozenset(pair) for pair in itertools.chain.from_iterable(itertools.combinations(sorted(n for n, g in truth.items() if g == group), 2) for group in {g for g in truth.values() if g is not None})}
def persisted_hashes():
    database = album / '.room237-metadata/album.sqlite'
    if not database.exists(): return {}
    with sqlite3.connect('file:'+str(database)+'?mode=ro',uri=True) as connection:
        return {name:json.loads(data) for name,data in connection.execute('select name,data from files') if name in truth}

app = App(binary, work / "lib", work / "profile", log=work / "app.log")
try:
    app.wait_albums()
    app.op("activate", album="Set", noWait=True)
    app.wait_idle(300)
    end = time.time() + 120
    while time.time() < end:
        state = app.op("state")
        cached = persisted_hashes()
        if len(cached) == len(truth) and all(row.get('hash') and row.get('hash_failed') is not True for row in cached.values()):
            break
        time.sleep(0.5)
    time.sleep(2)
    groups, offset, signature = [], 0, None
    while True:
        page = app.op("core", method="find_duplicate_groups", params={"dir": str(album), "offset": offset, "signature": signature})
        signature = page["signature"]
        rows = page["groups"]
        if not rows:
            break
        for row in rows:
            members, at = [], 0
            while len(members) < row["count"]:
                part = app.op("core", method="duplicate_group_members", params={"dir": str(album), "root": row["root"], "signature": signature, "offset": at})
                if not part:
                    break
                members += [m["name"] for m in part]
                at += len(part)
            groups.append(sorted(members))
        offset += len(rows)
        if offset >= page["total"]:
            break
    settings = app.op("core", method="get_settings", params={})["duplicates"]
finally:
    app.close()
found = {frozenset(pair) for group in groups for pair in itertools.combinations(group, 2)}
true_positive, false_positive, false_negative = found & expected, found - expected, expected - found
precision = len(true_positive) / max(1, len(found))
recall = len(true_positive) / max(1, len(expected))
by_kind = {}
for name, kind in positive_kinds.items():
    group = truth[name]
    base = "alpha-a.png" if group == "alpha" else f"g{group:02}-orig.png"
    entry = by_kind.setdefault(kind, [0, 0])
    entry[1] += 1
    entry[0] += frozenset((name, base)) in found
cache = persisted_hashes()
hash_failed = [name for name in truth if name not in cache or not cache[name].get('hash') or cache[name].get('hash_failed') is True]
log_hash_failed = "hash_failed" in (work / "app.log").read_text(errors="replace")
report = {"hash_failed": hash_failed, "log_hash_failed": log_hash_failed, "settings": settings, "files": len(truth), "expected_pairs": len(expected), "found_pairs": len(found), "precision": round(precision, 4), "recall": round(recall, 4), "by_kind": by_kind, "false_positive": [sorted(p) for p in list(false_positive)[:30]], "false_negative": [sorted(p) for p in list(false_negative)[:30]], "exit": app.exit_code, "build": app.build}
(work / "results.json").write_text(json.dumps(report, indent=1))
print(json.dumps({k: report[k] for k in ("files", "expected_pairs", "found_pairs", "precision", "recall", "by_kind", "exit")}))
print("false positives", report["false_positive"][:8])
print("false negatives", report["false_negative"][:8])
alpha_ok = frozenset(("alpha-a.png", "alpha-b.webp")) in found and frozenset(("alpha-a.png", "alpha-c.png")) in found
alpha_apart = not any("alpha-other-shape.png" in p for p in found)
print("alpha same picture grouped:", alpha_ok, "| other alpha shape apart:", alpha_apart)
sys.exit(0 if not hash_failed and not log_hash_failed and precision >= 0.99 and recall >= 0.9 and alpha_ok and alpha_apart and app.exit_code == 0 else 1)
