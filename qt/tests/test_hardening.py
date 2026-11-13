#!/usr/bin/env python3
"""Security hardening end to end: encrypt a root from the interface, lock, unlock with the factor, the password, the
decoy password and the recovery key, decrypted copies for other apps, a nested folder, and removing the encryption.
Usage: test_hardening.py APP_BINARY WORK_DIR. WORK_DIR is new and inside ROOM237_QA_FENCE. Writes results.json there.
Needs a QA build: it confirms with the test factor instead of Touch ID."""
import hashlib, json, sys, time, traceback
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from qa import App, require_fenced
from PIL import Image

binary, work = sys.argv[1], Path(sys.argv[2])
require_fenced(work)
work.mkdir(parents=True, exist_ok=False)
results = []
PASSWORD, DECOY = "correct horse battery", "decoy staple"


def check(name, ok, detail=""):
    results.append(dict(case=name, ok=bool(ok), detail=str(detail)[:600]))
    print(("ok  " if ok else "FAIL"), name, detail if not ok else "", flush=True)


def wait(fn, timeout=30, step=.1):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(step)
    return bool(fn())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def originals(root):
    return {str(f.relative_to(root)): sha(f) for f in root.rglob("*") if f.is_file() and not any(p.startswith(".") for p in f.relative_to(root).parts)}


def leaks(root, needles):
    """Files under root whose path or bytes contain one of the needles."""
    found = []
    for f in root.rglob("*"):
        try:
            data = f.read_bytes() if f.is_file() else b""
        except FileNotFoundError:  # A temporary file of the core, gone meanwhile.
            continue
        for needle in needles:
            if needle.encode() in str(f).encode() or needle.encode() in data:
                found.append(f"{f.relative_to(root)}: {needle}")
    return found


app = None
try:
    root, decoy, profile = work / "lib", work / "decoy", work / "profile"
    for album, count, seed in (("Holiday", 4, 1), ("Family/Kids", 3, 50)):
        (root / album).mkdir(parents=True)
        for i in range(count):
            Image.new("RGB", (320, 240), ((seed + i) * 37 % 255, (seed + i) * 11 % 255, 90)).save(root / album / f"secret-{album.replace('/', '-')}-{i}.jpg", quality=92)
    frames = [Image.new("RGB", (64, 64), (i * 60, 20, 200)) for i in range(4)]
    frames[0].save(root / "Holiday" / "secret-animation.gif", save_all=True, append_images=frames[1:], duration=80, loop=0)
    decoy.mkdir()
    Image.new("RGB", (200, 200), (10, 200, 10)).save(decoy / "cat.jpg")
    before = originals(root)

    app = App(binary, root, profile, log=work / "app.log")
    app.wait_albums()
    app.wait_idle(120)
    settings = app.op("core_call", method="get_settings")
    settings["privacy"].update(enabled=True, hardening=True)
    app.op("choose_root", kind="decoy", path=str(decoy))
    app.op("apply_settings", settings=settings)
    check("hardening on: encryption is offered for the open root", wait(lambda: "encryptOffer" in app.op("vault_state")["panels"]), app.op("vault_state"))
    app.op("encrypt_offer", choice="encrypt")
    check("offer: the setup opens", wait(lambda: "hardeningSetup" in app.op("vault_state")["panels"]))
    refused = app.op("hardening_setup", password=PASSWORD, repeat=PASSWORD + "x", kinds=["test"], finish=False)
    check("setup: mismatched passwords are refused", refused["error"] and not refused["recoveryKey"], refused)
    created = app.op("hardening_setup", password=PASSWORD, decoy=DECOY, kinds=["test"], wrongCheck=["AAAA", "BBBB"])
    recovery = created["recoveryKey"]
    check("setup: the recovery key is shown blurred", len(recovery) > 40 and created["blurred"], created)
    check("setup: wrong groups of the key are refused", bool(created.get("wrongCheckError")), created)
    check("setup: the right groups of the key finish the setup", created.get("checkError") == "" and wait(lambda: "hardeningSetup" not in app.op("vault_state")["panels"], 5), created)
    vault = json.loads((root / ".room237-vault").read_text())
    check("setup: vault file with a test, a recovery and no plain password", {s["kind"] for s in vault["slots"]} == {"test", "recovery"} and PASSWORD not in json.dumps(vault), [s["kind"] for s in vault["slots"]])

    names = lambda album: app.op("inventory", album=album)
    check("encryption finishes", wait(lambda: json.loads((root / ".room237-vault").read_text())["state"] == "encrypted", 120))
    check("on disk: no original name, pixel or plain index left", wait(lambda: not leaks(root, ["secret-"]), 30), leaks(root, ["secret-"])[:5])
    check("on disk: every original is a sealed file", all(f.suffix == ".r237" for f in root.rglob("*") if f.is_file() and not any(p.startswith(".") for p in f.relative_to(root).parts)))
    app.wait_albums()
    holiday = names("Holiday")
    check("in the room: original names are shown", sorted(holiday["labels"]) == sorted(k.split("/")[-1] for k in before if k.startswith("Holiday/")), holiday)
    state = app.op("vault_state")
    check("in the room: the root counts as protected", state["protected"], state)
    sharp = lambda: (lambda v: v["expected"] > 0 and v["ready"] == v["expected"])(app.op("visible"))
    check("in the room: encrypted previews decode in the grid", wait(sharp, 30), app.op("visible"))
    thumbs = [f for f in (root / "Holiday" / ".room237-thumb").iterdir() if f.suffix == ".webp"]
    check("on disk: previews are sealed too", thumbs and all(f.read_bytes()[:8] == b"R237ENC1" for f in thumbs), [f.name for f in thumbs][:5])

    app.op("viewer_open", index=0)
    viewer = app.op("viewer_state")
    check("viewer: an encrypted original decodes", viewer["ready"] and not viewer["failed"], viewer)
    app.op("viewer_close")

    app.op("select_all")
    app.op("copy_selection")
    check("copy: asks before decrypting for other apps", wait(lambda: "exportConfirm" in app.op("vault_state")["panels"], 5), app.op("vault_state"))
    app.op("export_confirm")
    clip = app.op("clipboard")
    copies = [Path(u) for u in clip["urls"]]
    check("copy: decrypted copies under the original names", copies and all(c.is_file() and c.name.startswith("secret-") for c in copies), clip)
    check("copy: the copies match the originals", sorted(sha(c) for c in copies if c.suffix == ".jpg") == sorted(v for k, v in before.items() if k.startswith("Holiday/") and k.endswith(".jpg")), clip)

    # Everyday operations on encrypted files: favorite, date, move to another album, delete.
    app.op("inventory", album="Holiday")
    labels = {app.op("item", index=i)["label"]: i for i in range(5)}
    fav = labels["secret-Holiday-0.jpg"]
    app.op("favorite", index=fav)
    app.op("set_date", index=labels["secret-Holiday-3.jpg"], timestamp=1600000000)
    app.op("prepare_entries", album="Holiday", names=["secret-Holiday-1.jpg"])
    app.op("move_entries", to="Family/Kids")
    app.op("prepare_entries", album="Holiday", names=["secret-Holiday-2.jpg"])
    app.op("delete_entries")
    holiday = names("Holiday")
    kids = names("Family/Kids")
    check("operations: moved and deleted files leave the album", "secret-Holiday-1.jpg" not in holiday["labels"] and "secret-Holiday-2.jpg" not in holiday["labels"] and holiday["total"] == 3, holiday)
    check("operations: the moved file is listed by its original name", "secret-Holiday-1.jpg" in kids["labels"], kids)
    check("operations: nothing plain appears on disk", not leaks(root, ["secret-"]), leaks(root, ["secret-"])[:5])
    expected = dict(before)
    expected["Family/Kids/secret-Holiday-1.jpg"] = expected.pop("Holiday/secret-Holiday-1.jpg")
    expected.pop("Holiday/secret-Holiday-2.jpg")

    app.op("set_locked", locked=True)
    state = app.op("vault_state")
    check("lock: the unlock screen replaces the room", state["visible"] and state["root"] == "" and state["resumable"] and not state["passwordShown"], state)
    check("lock: decrypted copies are deleted", not any(c.exists() for c in copies), [str(c) for c in copies if c.exists()])
    unlocked = app.op("vault_unlock_ui")
    check("resume: the factor alone unlocks", not unlocked["visible"] and not unlocked["error"], unlocked)
    app.wait_albums()
    check("resume: the room is back", app.op("vault_state")["protected"] and names("Holiday")["total"] == 3)

    app.close()
    app = App(binary, root, profile, log=work / "app.log")
    state = app.op("vault_state")
    check("restart: the password is asked first", wait(lambda: app.op("vault_state")["visible"]) and app.op("vault_state")["passwordShown"], state)
    wrong = app.op("vault_unlock_ui", password="wrong password")
    check("wrong password: refused, the room stays locked", wrong["visible"] and wrong["error"], wrong)
    fake = app.op("vault_unlock_ui", password=DECOY)
    state = app.op("vault_state")
    check("decoy password: the decoy gallery opens", not fake["visible"] and state["decoy"] and state["root"] == str(decoy.resolve()) and not state["protected"], state)
    app.close()

    app = App(binary, root, profile, log=work / "app.log")
    wait(lambda: app.op("vault_state")["visible"])
    recovered = app.op("vault_unlock_ui", password=PASSWORD, recoveryKey=recovery, recovery=True)
    check("recovery key: unlocks with the password", not recovered["visible"] and not recovered["error"], recovered)
    check("recovery key: a new second factor is offered", wait(lambda: "replaceFactor" in app.op("vault_state")["panels"], 5), app.op("vault_state"))
    app.op("settings_close")
    app.wait_albums()

    nested = root / "Family"
    app.op("open_root", path=str(nested))
    app.wait_albums()
    check("nested folder as a root: empty, no error", app.op("state")["albums"] == [] and not (nested / ".room237-catalog").exists(), app.op("state")["albums"])
    app.op("open_root", path=str(root))
    state = app.op("vault_state")
    check("leaving the root locked it: coming back takes the factor", state["visible"] and state["resumable"] and state["unlockRoot"] == str(root), state)
    back = app.op("vault_unlock_ui")
    check("coming back: unlocked with the factor", not back["visible"] and not back["error"], back)
    app.wait_albums()

    app.op("core_call", method="vault_decrypt", params={"root": str(root)})
    check("removal: the vault is gone", wait(lambda: not (root / ".room237-vault").exists(), 120))
    after = originals(root)
    check("removal: every original is back byte for byte, moved and deleted ones where they went", after == expected, sorted(set(after) ^ set(expected))[:5])
    check("removal: no sealed file left", not [f for f in root.rglob("*.r237*")], [str(f) for f in root.rglob("*.r237*")][:5])
    app.wait_albums()
    check("removal: the room lists the original names", wait(lambda: sorted(names("Holiday")["names"]) == sorted(k.split("/")[-1] for k in expected if k.startswith("Holiday/")), 30), names("Holiday"))
    check("removal: the root is no longer protected", not app.op("vault_state")["protected"])
    app.op("inventory", album="Holiday")
    restored = {app.op("item", index=i)["label"]: app.op("item", index=i) for i in range(3)}
    check("removal: the favorite set while encrypted is kept", restored["secret-Holiday-0.jpg"]["favorite"], restored["secret-Holiday-0.jpg"])
    check("removal: the date set while encrypted is kept", int(restored["secret-Holiday-3.jpg"]["dateTaken"]) == 1600000000, restored["secret-Holiday-3.jpg"])
    app.close()
    check("clean exit", app.exit_code == 0, app.exit_code)
    app = None
except Exception:
    results.append(dict(case="error", ok=False, detail=traceback.format_exc()[-1200:]))
    print("ERROR", traceback.format_exc()[-1200:], flush=True)
finally:
    if app is not None:
        app.close()
(work / "results.json").write_text(json.dumps(results, indent=1))
bad = [r for r in results if not r["ok"]]
print(f"{len(results) - len(bad)}/{len(results)} passed")
sys.exit(1 if bad else 0)
