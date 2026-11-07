#!/usr/bin/env python3
"""Signed local N -> N+1 acceptance on disposable installed copies.

PLATFORM WORK KEY SIGN_TOOL INSTALL_N PAYLOAD_N1 --fixture IMAGE [--wine]
Use KEY=- to generate a throwaway key under WORK (recommended). An explicit key is
only read for signing; it is never copied. ROOM237_QA_FENCE must be a canonical
absolute task directory containing WORK. WORK must be new or empty. No global input.
The same suite runs offscreen on macOS/Linux and native Windows; --wine maps paths.
"""
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import shutil
import socketserver
import subprocess
import sys
import tarfile
import threading
import time

platform, work_arg, key, sign_tool, source_arg, payload_arg = sys.argv[1:7]
wine = "--wine" in sys.argv
work = Path(work_arg)
fence = Path(os.environ["ROOM237_QA_FENCE"])
if (not fence.is_absolute() or fence.resolve() != fence or not fence.is_dir() or
        fence == Path(fence.anchor) or not work.is_absolute() or ".." in work.parts or
        work.resolve() != work or fence not in work.parents):
    raise ValueError("canonical absolute WORK strictly inside explicit ROOM237_QA_FENCE required")
if work.exists() and any(work.iterdir()):
    raise ValueError("WORK must be new or empty; previous evidence is never deleted")
work.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(Path(__file__).parent))
import qa
qa.FENCE = str(fence)
from qa import App, require_fenced
sign_tool = str(Path(sign_tool).resolve(strict=True))
source, payload = Path(source_arg).resolve(strict=True), Path(payload_arg).resolve(strict=True)
fixture_image = Path(sys.argv[sys.argv.index("--fixture") + 1]).resolve(strict=True)
path_map = (lambda p: "Z:" + Path(p).as_posix()) if wine else (lambda p: Path(p).as_posix())
wrapper = None
if wine:
    # qa.App validates host paths; translate only the child fence at the Wine boundary.
    launcher = work / "wine-launch.py"
    launcher.write_text("import os, sys\ne = dict(os.environ)\np = e['ROOM237_QA_FENCE']\ne['ROOM237_QA_FENCE'] = p if p.startswith('Z:') else 'Z:' + p\nos.execvpe('wine', ['wine', *sys.argv[1:]], e)\n")
    wrapper = [sys.executable, str(launcher)]
N, N1 = os.environ.get("ROOM237_TEST_N", "7.0.0"), os.environ.get("ROOM237_TEST_N1", "7.0.1")
feed = work / "feed"
feed.mkdir()
results = []
apps = []
slow = threading.Event()


def check(name, ok, detail=""):
    results.append(dict(name=name, ok=bool(ok), detail=str(detail)[:1500]))
    print("PASS" if ok else "FAIL", name, str(detail)[:250], flush=True)


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for data in iter(lambda: f.read(1 << 20), b""):
            h.update(data)
    return h.hexdigest()


def run(args, **kwargs):
    return subprocess.run(args, check=True, cwd=work, **kwargs)


key = str(work / "fixture.key") if key == "-" else str(Path(key).resolve(strict=True))
if not Path(key).exists():
    run([sys.executable, sign_tool, "keygen", key, str(work / "fixture.pub")])
# Derive the public half without emitting private material, including explicit release-key runs.
sys.path.insert(0, str(Path(sign_tool).parent))
import sign_update
key_id, private = sign_update.load(key)
public_key = sign_update.public_b64(key_id, private)
del private


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(feed), **kw)

    def log_message(self, *a):
        pass

    def copyfile(self, src, dst):
        while data := src.read(16384):
            if slow.is_set():
                time.sleep(.025)
            dst.write(data)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def handle_error(self, *args):
        pass  # Deliberate cancellation closes this test-owned connection.


server = Server(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
FEED = f"http://127.0.0.1:{server.server_address[1]}/qt-update.json"


def sign(version=N1, data_path=payload, signer=key, url=None, sha=None, platform_key=platform):
    dst = feed / data_path.name
    if data_path != dst and (not dst.exists() or digest(dst) != digest(data_path)):
        shutil.copy2(data_path, dst)
    obj = dict(version=version, notes="local fixture", platforms={platform_key: dict(
        url=url or FEED.rsplit("/", 1)[0] + "/" + dst.name,
        sha256=sha or digest(dst), size=dst.stat().st_size,
        kind="appimage" if "appimage" in platform else "tar.gz")})
    (feed / "qt-update.json").write_text(json.dumps(obj), encoding="utf-8")
    run([sys.executable, sign_tool, "sign", signer, str(feed / "qt-update.json"), "version:" + version])


def setup(name, fail_health=False, enabled=True, feed_url=FEED):
    base = require_fenced(work / name)
    base.mkdir()
    install = base / ("Room237.app" if platform.startswith("darwin") else "Room237.AppImage" if "appimage" in platform else "room237")
    if source.is_dir():
        if sys.platform == "darwin":
            run(["/bin/cp", "-cR", str(source), str(install)])
        else:
            shutil.copytree(source, install, symlinks=True)
    else:
        shutil.copy2(source, install)
    if platform.startswith("darwin"):
        binary, core = install / "Contents/MacOS/Room237", install / "Contents/MacOS/room237-core"
    elif "appimage" in platform:
        binary, core = install, None
    else:
        binary = install / ("room237.exe" if platform.startswith("windows") else "room237")
        core = install / ("room237-core.exe" if platform.startswith("windows") else "room237-core")
    root, profile, legacy, home = [base / x for x in ("root", "profile", "legacy", "home")]
    for d in (root, profile, legacy, home):
        d.mkdir()
    (root / "Album").mkdir()
    shutil.copy2(fixture_image, root / "Album/keep.jpg")
    (profile / "ui-state.json").write_text(json.dumps(dict(rootDir=path_map(root), columns=5, language="ru", layout="masonry", sortKey="name", sortDir="desc")))
    config = dict(feed=feed_url, install=path_map(install), executable=path_map(binary),
                  staging=path_map(install.with_name("." + install.name + ".update-new")),
                  root=path_map(root), profile=path_map(profile), legacy=path_map(legacy),
                  home=path_map(home), publicKey=public_key)
    if fail_health:
        config["failHealthVersion"] = N1
    env = dict(QT_QPA_PLATFORM="offscreen", HOME=path_map(home), CFFIXED_USER_HOME=path_map(home),
               ROOM237_UPDATE_HEALTH_TIMEOUT="15" if fail_health else "90")
    if enabled:
        env["ROOM237_QA_UPDATE_FIXTURE"] = json.dumps(config)
    if "appimage" in platform:
        # The type-2 runtime extracts into $TMPDIR; keep that inside this case's fence (no FUSE, no shared /tmp).
        (base / "tmp").mkdir()
        env.update(APPIMAGE_EXTRACT_AND_RUN="1", APPIMAGE=path_map(binary), TMPDIR=path_map(base / "tmp"))
    # Explicit legacy/HOME for native and Wine alike. Windows APPDATA stays synthetic too.
    env.update(APPDATA=path_map(home / "AppData/Roaming"), LOCALAPPDATA=path_map(home / "AppData/Local"))
    app = App(binary, path_map(root), path_map(profile), log=base / "app.log", env=env,
              argv=["--legacy-profile", path_map(legacy)], wrapper=wrapper, wait=120)
    apps.append(app)
    app.wait_albums(120)
    return app, install, binary, core, root, profile, config


def state(app, wanted, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = app.op("update_state")
        if value["state"] in wanted:
            return value
        time.sleep(.05)
    raise TimeoutError((wanted, value))


def offered(app):
    app.op("update_check")
    value = state(app, {"available"})
    check("signed N+1 offered", value["version"] == N1 and value["fixtureAllowed"], value)


def preserved(app, root, profile, label):
    value = app.op("state")
    saved = json.loads((profile / "ui-state.json").read_text())
    check(label + ": root/profile readback", value["root"] == path_map(root) and value["profile"] == path_map(profile) and saved.get("rootDir") == path_map(root), value.get("root"))
    check(label + ": settings readback", all(saved.get(k) == v for k, v in dict(columns=5, language="ru", layout="masonry").items()), saved)
    check(label + ": library hash", digest(root / "Album/keep.jpg") == digest(fixture_image))


def rejected(app, name):
    app.op("update_check")
    value = state(app, {"idle", "failed", "available"})
    check(name, value["state"] != "available" and app.proc.poll() is None, value)


def helper_result(app, expected):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        value = app.op("update_state")
        if value["result"]:
            check("helper result readback", value["result"] == expected, value)
            return
        time.sleep(.1)
    check("helper result readback", False, value)


try:
    app, install, binary, core, root, profile, config = setup("disabled", enabled=False)
    app.op("update_check")
    check("QA updater remains disabled without fixture", app.op("update_state")["state"] == "idle" and not (profile / "updates").exists())
    app.close()

    sign(url=(feed / payload.name).as_uri())
    app, install, binary, core, root, profile, config = setup("file-feed", feed_url=(feed / "qt-update.json").as_uri())
    offered(app)
    check("file feed trust chain accepted", app.op("update_state")["fixtureAllowed"])
    app.close()
    sign()
    app, install, binary, core, root, profile, config = setup("positive")
    check("N version readback", app.op("update_state")["current"] == N)
    # IPC rejects arbitrary installs, relative probes and launch argv before dispatch.
    sentinel = work / "sentinel"
    sentinel.write_bytes(b"unchanged")
    bad = app.op("core_call", method="update_cleanup", params={"install": path_map(sentinel)}, check=False)
    check("Qt fence rejects install substitution", not bad["ok"] and sentinel.read_bytes() == b"unchanged", bad)
    bad = app.op("core_call", method="update_stage", params={"plan": dict(
        payload=path_map(sentinel), install=path_map(install), probe="../sentinel", sha256=digest(sentinel), size=9, kind="tar.gz", version=N1)}, check=False)
    check("Qt fence rejects payload/probe substitution", not bad["ok"] and sentinel.read_bytes() == b"unchanged", bad)
    bad = app.op("core_call", method="update_launch_helper", params={"plan": dict(
        install=path_map(install), launch=["/bin/sh", "-c", "exit 0"])}, check=False)
    check("Qt fence rejects arbitrary helper argv", not bad["ok"] and not (profile / "update-plan.json").exists(), bad)
    if core is not None:
        direct = work / "direct-core"
        direct.mkdir()
        env = dict(os.environ, HOME=config["home"], CFFIXED_USER_HOME=config["home"],
                   ROOM237_QA_PORT="1", ROOM237_QA_FENCE=path_map(fence),
                   ROOM237_QA_UPDATE_FIXTURE=json.dumps(config), QT_QPA_PLATFORM="offscreen")
        def rpc(method, params, environment=env):
            proc = run((wrapper or []) + [str(core), "--profile", path_map(direct)],
                       input=json.dumps(dict(id=1, method=method, params=params))+"\n",
                       text=True, capture_output=True, env=environment)
            return next(json.loads(line) for line in proc.stdout.splitlines() if json.loads(line).get("id") == 1)
        valid_params = dict(manifest=(feed / "qt-update.json").read_text(), signature=(feed / "qt-update.json.minisig").read_text(), currentVersion=N, platform=platform)
        valid = rpc("update_check", valid_params)
        check("direct core verifies fixture trust chain", valid.get("result", {}).get("available"), valid)
        bad = rpc("update_cleanup", dict(install=path_map(sentinel)))
        check("direct core rejects install substitution", "error" in bad and sentinel.read_bytes() == b"unchanged", bad)
        invalid = dict(config, staging="relative")
        bad = rpc("update_check", valid_params, dict(env, ROOM237_QA_UPDATE_FIXTURE=json.dumps(invalid)))
        check("direct core rejects noncanonical fixture", "error" in bad, bad)
        production = dict(env)
        production.pop("ROOM237_QA_PORT")
        bad = rpc("update_check", valid_params, production)
        check("fixture key inert without QA mode", "error" in bad, bad)
    before = digest(binary)
    preserved(app, root, profile, "N")
    offered(app)
    old_pid = app.op("update_state")["pid"]
    app.op("update_install")
    app.reconnect(180)
    value = app.op("update_state")
    check("helper relaunched N+1", value["current"] == N1 and value["pid"] != old_pid, value)
    app.wait_albums(120)
    preserved(app, root, profile, "N+1")
    helper_result(app, "updated")
    check("installed binary changed", digest(binary) != before, digest(binary))
    check("staging and backup removed", not any(".update-" in x.name for x in install.parent.iterdir()))
    app.close()

    app, install, binary, core, root, profile, config = setup("negative")
    before = digest(binary)
    sign()
    (feed / "qt-update.json.minisig").write_text("garbage")
    rejected(app, "corrupt signature refused")
    other = work / "wrong.key"
    run([sys.executable, sign_tool, "keygen", str(other), str(work / "wrong.pub")])
    sign(signer=str(other))
    rejected(app, "wrong key refused")
    sign(version="6.9.0")
    rejected(app, "downgrade refused")
    sign(version=N)
    rejected(app, "same version refused")
    sign(url="https://evil.example/payload")
    rejected(app, "signed nonlocal URL refused")
    sign()
    (feed / "qt-update.json").unlink()
    rejected(app, "offline feed refused")
    # Tampered archive is downloaded then rejected against the signed hash.
    corrupt = feed / "corrupt.tar.gz"
    corrupt.write_bytes(b"corrupt payload")
    sign(data_path=corrupt, sha=digest(payload))
    offered(app)
    app.op("update_install")
    value = state(app, {"failed"})
    check("hash mismatch removes download", not list((profile / "updates").iterdir()), value)
    if "appimage" not in platform:
        evil = feed / "traversal.tar.gz"
        with tarfile.open(evil, "w:gz") as tar:
            info = tarfile.TarInfo("../../escaped")
            info.size = 5
            tar.addfile(info, io.BytesIO(b"owned"))
        sign(data_path=evil)
        offered(app)
        app.op("update_install")
        value = state(app, {"failed"})
        check("archive traversal refused", not (work / "escaped").exists() and not (install.parent / "escaped").exists() and not install.with_name("." + install.name + ".update-new").exists(), value)
    sign()
    offered(app)
    slow.set()
    app.op("update_install")
    value = state(app, {"downloading"})
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and not any(p.stat().st_size > 0 for p in (profile / "updates").iterdir()):
        time.sleep(.02)
    partial = sum(p.stat().st_size for p in (profile / "updates").iterdir())
    app.op("update_cancel")
    value = state(app, {"available"})
    slow.clear()
    check("cancel during partial download removes payload", 0 < partial < payload.stat().st_size and not list((profile / "updates").iterdir()), (partial, value))
    check("all refusals preserve installed bytes", digest(binary) == before)
    preserved(app, root, profile, "refusals/cancel")
    app.close()

    sign()
    app, install, binary, core, root, profile, config = setup("rollback", fail_health=True)
    before = digest(binary)
    offered(app)
    app.op("update_install")
    app.reconnect(180)
    value = app.op("update_state")
    check("unhealthy N+1 actually launched", value["current"] == N1, value)
    app.reconnect(180)
    app.wait_albums(120)
    value = app.op("update_state")
    check("failed health mark rolls back and relaunches N", value["current"] == N and digest(binary) == before, value)
    helper_result(app, "rolled-back")
    preserved(app, root, profile, "rollback")
    check("rollback removes temporary installs", not any(".update-" in x.name for x in install.parent.iterdir()))
    app.close()
except Exception as error:
    check("suite completed", False, repr(error))
    import traceback
    traceback.print_exc()
finally:
    for app in apps:
        app.close()
    server.shutdown()
    server.server_close()
    (work / "results.json").write_text(json.dumps(dict(platform=platform, source=str(source), payload=str(payload), payloadSha256=digest(payload), results=results), indent=2))
failed = [r["name"] for r in results if not r["ok"]]
print(f"{len(results)-len(failed)}/{len(results)} passed", failed)
sys.exit(bool(failed))
