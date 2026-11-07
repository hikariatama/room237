"""Launches a built Room237 under the QA driver and talks to it. Root and profile must be inside FENCE."""
import hashlib, json, os, socket, subprocess, time
from pathlib import Path
FENCE = os.environ.get("ROOM237_QA_FENCE", "")  # Required: an existing absolute disposable directory.


def canonical_absolute(path, *, strict=True):
    if not isinstance(path, (str, os.PathLike)) or not path or not Path(path).is_absolute() or ".." in Path(path).parts:
        raise ValueError(f"expected an absolute path without traversal: {path}")
    return Path(path).resolve(strict=strict)


def require_fenced(path):
    """Validate containment within the explicit absolute boundary before any launch/write."""
    text = str(path) if path else ""
    if text.startswith("Z:"):
        text = text[2:].replace("\\", "/")
    resolved = canonical_absolute(text, strict=False)
    # A dangling symlink must not become an apparently safe missing output.
    for parent in [Path(text), *Path(text).parents]:
        if parent.is_symlink() and not parent.exists():
            raise ValueError(f"unresolvable QA path: {path}")
    ancestor = Path(text)
    while not ancestor.exists():
        ancestor = ancestor.parent
    if ancestor != Path(text) and not ancestor.is_dir():
        raise ValueError(f"missing destination below a non-directory: {path}")
    boundary = canonical_absolute(FENCE)
    if not boundary.is_dir() or boundary == Path(boundary.anchor):
        raise ValueError("QA fence must be an existing absolute disposable directory")
    if resolved != boundary and boundary not in resolved.parents:
        raise ValueError(f"outside the QA fence: {path}")
    return resolved


def scope_check(path, roots):
    """Prewrite check: return the canonical absolute destination only within an explicit root."""
    destination = canonical_absolute(path, strict=False)
    require_fenced(path)
    for root in roots:
        allowed = canonical_absolute(root)
        if destination == allowed or allowed in destination.parents:
            return destination
    raise ValueError(f"outside write scope: {path}")


class NativeDirectoryPanel:
    """Actual macOS AppKit bridge. The driver must own a live invocation of the real picker.

    bind/read/confirm/cancel address only its retained NSOpenPanel and window ID. No global input.
    The backend reads NSOpenPanel.URLs and rechecks the path, folder identity and 2s ticket at confirm.
    """
    def __init__(self, app, session):
        self.app = app
        self.session = session
        self.closed = False
        if not isinstance(session, str) or not session:
            raise ValueError("native dialog session from native_directory_open is required")
        try:
            self.binding = app.op("native_directory_panel", action="bind", session=session)
            if (self.binding.get("pid") != app.proc.pid or not self.binding.get("window") or
                    not isinstance(self.binding.get("token"), str) or not self.binding["token"]):
                raise RuntimeError("native panel ownership could not be established")
        except Exception as failure:
            # The Qt dialog is pinned to this session even if its native window cannot be bound/read.
            app.op("native_directory_abort", session=session)
            self.closed = True
            raise failure

    def _call(self, action, **params):
        if self.closed:
            raise RuntimeError("native panel adapter already consumed")
        return self.app.op("native_directory_panel", action=action, session=self.session, token=self.binding["token"], **params)

    def read_selected_path(self):
        value = self._call("read")
        if any(value.get(key) != self.binding[key] for key in ("pid", "window", "token")):
            raise RuntimeError("stale native panel identity")
        path = value.get("path")
        require_fenced(path)
        selected = canonical_absolute(path)
        if not selected.is_dir():
            raise ValueError("native selection is not an existing directory")
        return selected

    def confirm(self, expected):
        result = self._call("confirm", expected=str(expected))
        self.confirmation = result  # includes how the backend confirmed ("via") for the report
        if result.get("confirmed") is not True:
            raise RuntimeError("native confirmation refused")
        self.closed = True

    def cancel(self):
        try:
            result = self._call("cancel")
        except Exception:
            result = self.app.op("native_directory_abort", session=self.session)
        if result.get("cancelled") is not True:
            raise RuntimeError("owned native cancellation refused")
        self.closed = True

    def confirm_expected(self, expected):
        def settled_root():
            # One readback for the contract. The app applies the root after its modal dialog returns, so this waits
            # (bounded 10 s) for the confirmed folder to become active and then reports whatever is active.
            wanted = str(canonical_absolute(expected))
            deadline = time.monotonic() + 10
            root = self.app.op("state")["root"]
            while str(canonical_absolute(root)) != wanted and time.monotonic() < deadline:
                time.sleep(0.1)
                root = self.app.op("state")["root"]
            return root
        return confirm_native_directory(self.read_selected_path, expected,
                                        lambda: self.confirm(expected), self.cancel, settled_root)


def read_native_directory_path(panel):
    """Read the full actual selected URL through a bound native panel adapter."""
    return panel.read_selected_path()


def confirm_native_directory(read_selected_path, expected, confirm, cancel, read_active_root):
    """Confirm once on an exact fenced match. Cancel this panel on any pre-confirm failure; never retry."""
    try:
        wanted = canonical_absolute(expected)
        require_fenced(expected)
        if not wanted.is_dir():
            raise ValueError("expected directory disappeared")
        identity = (wanted.stat().st_dev, wanted.stat().st_ino)
        selected = read_selected_path()
        require_fenced(selected)
        actual = canonical_absolute(selected)
        if actual != wanted or not actual.is_dir() or (actual.stat().st_dev, actual.stat().st_ino) != identity:
            raise ValueError("native directory selection differs from expected or is stale")
        confirm()  # The real backend rechecks its pinned panel and selected URL before addressed action.
    except Exception as failure:
        try:
            cancel()
        except Exception as cancellation:
            raise RuntimeError(f"native panel refused; scoped cancellation failed: {cancellation}") from failure
        raise
    # Confirmation already consumed this panel. Readback failure must not cancel any later dialog.
    active = read_active_root()
    require_fenced(active)
    if canonical_absolute(active) != wanted or (wanted.stat().st_dev, wanted.stat().st_ino) != identity:
        raise RuntimeError("active root differs from confirmed directory")
    return wanted


class GuardRefused(RuntimeError):
    """A synthetic event would not reach the app under test. The run aborts; nothing is retried or activated."""


def guard(pid, x=None, y=None, window=None, allow=()):
    """Checks foreground, target window and the top window at the point through the qa_guard helper named by QA_GUARD."""
    command = [os.environ["QA_GUARD"], str(pid)]
    if x is not None:
        command += [str(x), str(y)]
    if window:
        command += ["--window", str(window)]
    for trusted in allow:
        command += ["--allow", str(trusted)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise GuardRefused(result.stdout.strip() or result.stderr.strip() or f"guard exit {result.returncode}")
    return result.stdout.strip()


def guarded_send(send_raw, pid, command, window=None, allow=()):
    """One gesture through the guard: every point it touches is checked first; keys and wheel check foreground too."""
    if os.environ.get("ROOM237_SLOT_AUTHORIZATION") and command.get("op") != "quit":
        from slot_contract import authorization, pressure
        authorization(os.environ["ROOM237_SLOT_AUTHORIZATION"], os.environ["ROOM237_SLOT_BINARY"])
        pressure()
    if "x" in command:
        guard(pid, command["x"], command["y"], window, allow)
        if "x2" in command:
            guard(pid, command["x2"], command["y2"], window, allow)
    elif command.get("op") != "quit":
        guard(pid, window=window, allow=allow)
    if command.get("op") != "quit":
        command = dict(command, guardPid=pid, guardWindow=int(window or window_id(pid)), guardAllow=list(allow))
    reply = send_raw(**command)
    if not isinstance(reply, str) or not reply.startswith("ok"):
        raise GuardRefused("input helper refused or disconnected: " + str(reply))
    return reply


def window_id(pid):
    return subprocess.run([os.environ["QA_WINID"], str(pid)], capture_output=True, text=True, check=True).stdout.strip()


def capture_window(pid, path):
    """A still of exactly the target window."""
    subprocess.run(["screencapture", "-x", "-o", f"-l{window_id(pid)}", str(path)], check=True)


def app_panels(pid):
    """The app's own smaller windows that the window server stacks above its main window, front first."""
    out = subprocess.run([os.environ["QA_WINID"], str(pid), "--panels"], capture_output=True, text=True, check=True).stdout
    rows = [line.split("\t") for line in out.splitlines()]
    return [{"id": r[0], "x": float(r[1]), "y": float(r[2]), "w": float(r[3]), "h": float(r[4])} for r in rows]


def capture_app(pid, path):
    """The main window with the app's own panels composited at their window-server positions; no other owner's pixels."""
    from PIL import Image
    capture_window(pid, path)
    base = Image.open(path).convert("RGBA")
    main = subprocess.run([os.environ["QA_WINID"], str(pid), "--main-frame"], capture_output=True, text=True, check=True).stdout.split()
    origin_x, origin_y, width = float(main[0]), float(main[1]), float(main[2])
    scale = base.width / width
    for panel in reversed(app_panels(pid)):
        piece = str(path) + ".panel.png"
        subprocess.run(["screencapture", "-x", "-o", f"-l{panel['id']}", piece], check=True)
        layer = Image.open(piece).convert("RGBA")
        base.alpha_composite(layer, (int((panel["x"] - origin_x) * scale), int((panel["y"] - origin_y) * scale)))
        os.remove(piece)
    base.save(path)


def build_id(binary):
    """Identifies the exact binaries a result belongs to."""
    binary = Path(binary)
    entries = {}
    for candidate in [binary, binary.parent / "room237-core", binary.parent / "room237-core.exe"]:
        if candidate.is_file():
            entries[candidate.name] = hashlib.sha256(candidate.read_bytes()).hexdigest()
    return entries


class App:
    def __init__(self, binary, root, profile, log=None, env=None, argv=None, wrapper=None, wait=30, legacy_discovery=False):
        require_fenced(root)
        require_fenced(profile)
        argv = argv or []
        if "--legacy-profile" in argv:
            require_fenced(argv[argv.index("--legacy-profile") + 1])
        self.binary = str(binary)
        self.build = build_id(binary)
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1" if wrapper is None else "0.0.0.0", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        environment = dict(os.environ, ROOM237_QA_PORT=str(self.port), ROOM237_QA_FENCE=FENCE)
        environment.update(env or {})
        # Caller environment cannot silently disable or widen the driver's configured boundary.
        environment.update(ROOM237_QA_PORT=str(self.port), ROOM237_QA_FENCE=FENCE)
        if wrapper is None:
            Path(profile).mkdir(parents=True, exist_ok=True)
            home = (env or {}).get("HOME", str(Path(profile).parent / (Path(profile).name + "-home")))
            require_fenced(home)
            Path(home).mkdir(parents=True, exist_ok=True)
            environment.update(HOME=str(home), CFFIXED_USER_HOME=str(home), XDG_CONFIG_HOME=str(Path(home) / ".config"))
            # legacy_discovery leaves the platform default location (under the synthetic HOME) to the app itself.
            if "--legacy-profile" not in argv and not legacy_discovery:
                legacy = Path(profile).parent / (Path(profile).name + "-legacy")
                require_fenced(legacy)
                legacy.mkdir(exist_ok=True)
                argv = [*argv, "--legacy-profile", str(legacy)]
        if log:
            require_fenced(log)
        self.log = open(log, "ab") if log else subprocess.DEVNULL
        command = (wrapper or []) + [str(binary), "--root", str(root), "--profile", str(profile)] + argv
        self.proc = subprocess.Popen(command, env=environment, stdout=self.log, stderr=subprocess.STDOUT)
        self.pid = self.proc.pid
        if wrapper is None:
            (Path(profile).parent / (Path(profile).name + '-launch.json')).write_text(json.dumps({'pid':self.proc.pid,'command':command,'binarySha256':self.build,'root':str(root),'profile':str(profile),'fence':FENCE,'platform':environment.get('QT_QPA_PLATFORM')},indent=2))
        self.server.settimeout(wait)
        self.seq = 0
        self.events = []
        try:
            self.reconnect(wait)
        except BaseException:
            self.proc.terminate()
            try: self.proc.wait(5)
            except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()
            self.server.close()
            if self.log is not subprocess.DEVNULL: self.log.close()
            raise

    def reconnect(self, wait=30):
        """Waits for a process to dial in. Local port scanners do connect too; only the app sends the boot line."""
        self.server.settimeout(wait)
        while True:
            self.conn, _ = self.server.accept()
            self.conn.settimeout(wait)
            self.buf = b""
            try:
                if self._read().get("event") == "boot":
                    break
            except (RuntimeError, OSError):
                pass
            self.conn.close()
        self.conn.settimeout(120)

    def _read(self):
        while b"\n" not in self.buf:
            data = self.conn.recv(1 << 16)
            if not data:
                raise RuntimeError("driver connection closed")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        try:
            return json.loads(line)
        except ValueError:
            raise RuntimeError(f"bad driver line: {line[:200]!r}")

    def op(self, operation, /, check=True, **params):
        self.seq += 1
        self.conn.sendall(json.dumps({"id": self.seq, "op": operation, **params}).encode() + b"\n")
        while True:
            message = self._read()
            if message.get("id") == self.seq:
                if check and not message.get("ok"):
                    raise RuntimeError(f"{operation}: {message.get('error')}")
                return message.get("result") if check else message
            self.events.append(message)

    def wait_albums(self, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            state = self.op("state")
            if state.get("albumsReady"):
                return state
            time.sleep(0.1)
        raise TimeoutError("albums")

    def wait_idle(self, timeout=300):
        end = time.time() + timeout
        while time.time() < end:
            if not self.op("state").get("preloading"):
                return True
            time.sleep(0.5)
        raise TimeoutError("preload")

    def close(self):
        if getattr(self, "_closed", False):
            return
        self._closed = True
        try:
            self.op("quit_now", check=False)
        except Exception:
            pass
        try:
            self.exit_code = self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(10)
            self.exit_code = "killed after timeout"
        finally:
            self.conn.close()
            self.server.close()
            if self.log is not subprocess.DEVNULL:
                self.log.close()
