"""The demo's driver: a packaged OpenNova editor under Godot's Movie Maker, through its MCP only
(scripts/demo/README.md).

The window starts behind every other window (game_mcp.BehindLaunch); nothing here moves the cursor,
sends OS input, takes focus or captures the desktop: Movie Maker writes the frames. Movie time is
frame time: a hold counts frames written, never sleeps. An operation is polled through the
`operation` query, never awaited through the tool's own 300 s budget. A watchdog reads (never
changes) the window's show state and size, so a take whose window was minimized, maximized or
resized is known from its marks. The editor child's APPDATA and TMP point into the work folder, so
the user's own editor settings, imgui.ini and recent projects are never touched.
"""

from __future__ import annotations

import base64
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "mcp"))

from game_mcp import BehindLaunch, GameMcp, GameMcpError, port_open, text_of  # noqa: E402

STORYBOARD = HERE / "storyboard.json"
SW_SHOWNORMAL = 1  # WINDOWPLACEMENT.showCmd of a window neither minimized nor maximized
REHEARSE_PAUSE = 0.3  # a rehearsal's hold, in wall seconds (no movie to pace)
MARK_REF = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)([+-]\d+)?$")
EM_DASH = chr(0x2014)  # the project's copy takes none (CLAUDE.md), captions included


def mark_ref(text: str) -> tuple[str, int]:
    """A storyboard mark reference: `label`, or `label+N` / `label-N` frames from it."""
    match = MARK_REF.match(text)
    if not match:
        raise ValueError(f"not a mark reference: {text!r} (a label, or label+N / label-N frames)")
    return match.group(1), int(match.group(2) or 0)


class Workspace:
    """Everything a run writes, under one work folder."""

    def __init__(self, root: str | Path):
        self.root = Path(root).resolve()
        self.appdata = self.root / "appdata"  # the editor child's APPDATA: its settings, imgui.ini, recents
        self.tmp = self.root / "tmp"  # the editor child's TMP/TEMP, and the logs and pid files
        self.projects = self.root / "projects"  # the demo project and the recents' projects
        self.frames = self.root / "frames"  # one folder per recorded take: Movie Maker's frames, marks
        self.rehearse = self.root / "rehearse"  # one folder per rehearsal: screenshots at the marks
        self.cut = self.root / "cut"  # the cut's linked frame sequence, captions and edit list
        self.out = self.root / "out"  # the MP4 and the GIF

    def ensure(self) -> None:
        for folder in (self.appdata, self.tmp, self.projects):
            folder.mkdir(parents=True, exist_ok=True)

    def inside(self, path: str | Path) -> Path:
        """`path`, refused unless it lies under the work folder (everything a stage deletes does)."""
        resolved = Path(path).resolve()
        if resolved == self.root or self.root not in resolved.parents:
            raise RuntimeError(f"{resolved} is not inside the work folder {self.root}")
        return resolved

    def remove(self, path: str | Path) -> int:
        """Delete a file or folder under the work folder; the bytes freed."""
        target = self.inside(path)
        if not target.exists():
            return 0
        if target.is_file():
            size = target.stat().st_size
            target.unlink()
            return size
        size = sum(p.stat().st_size for p in target.rglob("*") if p.is_file())
        shutil.rmtree(target)
        return size


class Storyboard:
    """storyboard.json: the scenes in cut order, the takes and their parts, the GIF's ranges."""

    def __init__(self, path: str | Path = STORYBOARD):
        self.path = Path(path)
        self.data = json.loads(self.path.read_text(encoding="utf-8"))
        self.fps = int(self.data["fps"])
        self.size = (int(self.data["size"][0]), int(self.data["size"][1]))
        self.project = self.data["project"]
        self.recents = self.data.get("recents", [])
        self.takes = self.data["takes"]
        self.scenes = self.data["scenes"]
        self.check()

    def check(self) -> None:
        seen = set()
        for scene in self.scenes:
            sid = scene["id"]
            if sid in seen:
                raise ValueError(f"{self.path}: scene {sid!r} twice")
            seen.add(sid)
            take = self.takes.get(scene["take"])
            if take is None or scene["part"] not in take["parts"]:
                raise ValueError(f"{self.path}: scene {sid!r}: take {scene['take']!r} records no part "
                                 f"{scene['part']!r}")
            if EM_DASH in scene["caption"]:
                raise ValueError(f"{self.path}: scene {sid!r}: no em dashes in captions")
            for piece in scene["cut"]:
                if "still" in piece:
                    float(piece["seconds"])
                    continue
                mark_ref(piece["from"])
                mark_ref(piece["to"])
        audio = self.data.get("audio")
        if audio and audio["scene"] not in seen:
            raise ValueError(f"{self.path}: the audio names no scene {audio['scene']!r}")
        for span in self.data.get("gif", {}).get("ranges", []):
            for key in ("from", "to"):
                if key in span and span[key] not in seen:
                    raise ValueError(f"{self.path}: a GIF range names no scene {span[key]!r}")


class Editor:
    """One packaged editor process, started here and driven through its MCP alone."""

    def __init__(self, exe: str | Path, port: int, work: Workspace, name: str, frames_dir: Path | None = None,
                 size: tuple[int, int] = (1600, 900), fps: int = 30, project: Path | None = None,
                 marks_path: Path | None = None, discard_on_quit: bool = False):
        self.exe = Path(exe).resolve()
        self.port = port
        self.work = work
        self.name = name  # names the logs: <tmp>/<name>-godot.log, <name>.stdout.log, <name>.pid
        self.frames_dir = frames_dir
        self.size = size
        self.fps = fps
        self.project = project
        self.marks_path = marks_path
        self.discard_on_quit = discard_on_quit
        self.marks: list[dict] = []
        self.disturbed: list[dict] = []
        self.child: subprocess.Popen | None = None
        self.client = GameMcp.for_port(port, timeout=600)
        self._last_count = 0

    # --- process -------------------------------------------------------------------------------
    def launch(self, timeout: float = 240.0) -> None:
        if not self.exe.is_file():
            raise RuntimeError(f"editor executable not found: {self.exe}")
        if port_open(self.port):
            raise RuntimeError(f"port {self.port} already answers: stop that editor first or pass --port")
        self.work.ensure()
        argv = [str(self.exe), "--windowed", "--resolution", f"{self.size[0]}x{self.size[1]}"]
        if self.frames_dir:
            self.frames_dir.mkdir(parents=True, exist_ok=True)
            argv += ["--write-movie", str(self.frames_dir / "frame.png"), "--fixed-fps", str(self.fps)]
        argv += ["--log-file", str(self.work.tmp / f"{self.name}-godot.log")]
        argv.append("--")
        if self.project:
            argv += ["--project", str(self.project)]
        argv += ["--mcp-port", str(self.port)]
        # The editor child's own environment: user:// (its settings, imgui.ini, recents) and its
        # temporary files under the work folder, never the user's.
        env = dict(os.environ)
        env["APPDATA"] = str(self.work.appdata)
        env["TMP"] = env["TEMP"] = str(self.work.tmp)
        stdout_log = self.work.tmp / f"{self.name}.stdout.log"
        kwargs: dict = {}
        if os.name == "nt":
            kwargs["creationflags"] = (subprocess.CREATE_NEW_PROCESS_GROUP
                                       | getattr(subprocess, "DETACHED_PROCESS", 0x00000008))
        else:
            kwargs["start_new_session"] = True
        behind = BehindLaunch(False)
        try:
            behind.prepare()
            kwargs.update(behind.popen_kwargs())
            with open(stdout_log, "wb") as sink:
                self.child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT,
                                              cwd=str(self.exe.parent), env=env, **kwargs)
            (self.work.tmp / f"{self.name}.pid").write_text(str(self.child.pid), encoding="utf-8")
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                behind.tend(self.child.pid)
                if self.child.poll() is not None:
                    raise RuntimeError(f"editor exited {self.child.returncode}; see {stdout_log}")
                if port_open(self.port):
                    try:
                        self.client.initialize(timeout=5)
                        break
                    except GameMcpError:
                        pass
                time.sleep(0.05)
            else:
                raise RuntimeError(f"the endpoint on port {self.port} did not answer within {timeout:.0f} s")
            behind.settle(self.child.pid)
        finally:
            behind.release()
        self.start_watchdog()
        if self.project:
            self.wait_idle(timeout)
        print(f"launched pid={self.child.pid} port={self.port}", flush=True)

    def start_watchdog(self) -> None:
        """Note (never change) the window's state as it moves: minimized, maximized, resized or gone.
        A minimized window draws nothing, so its frames are stale."""

        def run() -> None:
            last = None
            while self.child and self.child.poll() is None:
                try:
                    shown = self.window_state()
                    now = (shown[0]["show"], shown[0]["client"]) if shown else None
                except Exception:  # a read that failed is itself a state worth noting
                    now = "?"
                if now != last:
                    if last is not None:
                        entry = {"frame": self.frames(), "label": "WINDOW", "window": str(now)}
                        self.disturbed.append(entry)
                        self.marks.append(entry)
                        print(f"[{entry['frame']:6d}] WINDOW {now}", flush=True)
                    last = now
                time.sleep(0.5)

        threading.Thread(target=run, daemon=True).start()

    def window_state(self, pid: int | None = None) -> list[dict]:
        """Read (never change) the shown top-level windows of the editor (or `pid`): their client
        size, window rect and show state."""
        if os.name != "nt":
            return []
        from ctypes import wintypes
        pid = pid or (self.child.pid if self.child else 0)
        user32 = ctypes.WinDLL("user32")
        proto = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

        class Placement(ctypes.Structure):
            _fields_ = [("length", ctypes.c_uint), ("flags", ctypes.c_uint), ("showCmd", ctypes.c_uint),
                        ("ptMin", wintypes.POINT), ("ptMax", wintypes.POINT), ("rc", wintypes.RECT)]

        found: list[dict] = []

        def visit(hwnd, _):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
            if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
                client, rect = wintypes.RECT(), wintypes.RECT()
                user32.GetClientRect(ctypes.c_void_p(hwnd), ctypes.byref(client))
                user32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(rect))
                placement = Placement()
                placement.length = ctypes.sizeof(Placement)
                user32.GetWindowPlacement(ctypes.c_void_p(hwnd), ctypes.byref(placement))
                found.append({"hwnd": hwnd, "client": (client.right, client.bottom),
                              "rect": (rect.left, rect.top, rect.right, rect.bottom), "show": placement.showCmd})
            return True

        user32.EnumWindows(proto(visit), 0)
        return found

    def window_ok(self) -> tuple[bool, list[dict]]:
        """Shown normally at the take's size (read, never changed)."""
        shown = self.window_state()
        ok = bool(shown) and shown[0]["show"] == SW_SHOWNORMAL and shown[0]["client"] == tuple(self.size)
        return ok, shown

    def stop(self, timeout: float = 60.0) -> None:
        """Quit the editor (a running game stopped first); the unsaved-changes prompt answered as
        asked (`discard_on_quit`). Only a quit the editor did not take ends the process, and only the
        one this script started."""
        if self.child and port_open(self.port):
            try:
                self.client.call("editor_play", {"op": "stop"}, timeout=30)
            except GameMcpError:
                pass
            try:
                self.client.call("editor_request", {"kind": "quit"}, timeout=10)
            except GameMcpError:
                pass
            # A quit with unsaved edits waits on the unsaved-changes prompt.
            started = time.monotonic()
            while self.child.poll() is None and time.monotonic() - started < 4:
                time.sleep(0.25)
            if self.child.poll() is None and port_open(self.port):
                try:
                    dialogs = self.client.structured("editor_state", {"sections": ["dialogs"]}, timeout=10)
                    if (dialogs.get("dialogs", {}).get("unsaved_prompt") or {}).get("open"):
                        choice = "discard" if self.discard_on_quit else "save"
                        self.client.call("editor_request", {"kind": "resolve_unsaved", "choice": choice}, timeout=10)
                except GameMcpError:
                    pass
        deadline = time.monotonic() + timeout
        while self.child and time.monotonic() < deadline:
            if self.child.poll() is not None:
                break
            time.sleep(0.25)
        self.save_marks()
        alive = bool(self.child and self.child.poll() is None)
        print(f"stopped alive={alive}", flush=True)
        if alive and os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(self.child.pid), "/T", "/F"], check=False)
        elif alive:
            self.child.kill()

    # --- frames and marks ----------------------------------------------------------------------
    def frames(self) -> int:
        """Frames Movie Maker has written so far (0 with no movie)."""
        if not self.frames_dir:
            return 0
        count = self._last_count
        # Frames are frameNNNNNNNN.png, numbered from 0: probed forward from the last count.
        while (self.frames_dir / f"frame{count:08d}.png").exists():
            count += 1
        self._last_count = count
        return count

    def hold(self, seconds: float) -> int:
        """Wait until `seconds` of movie time pass (that many frames written), the editor idling."""
        if not self.frames_dir:
            time.sleep(seconds)
            return 0
        target = self.frames() + int(round(seconds * self.fps))
        while self.frames() < target:
            if self.child and self.child.poll() is not None:
                raise RuntimeError("editor exited")
            time.sleep(0.02)
        return self._last_count

    def mark(self, label: str, **extra) -> None:
        entry = {"frame": self.frames(), "label": label, **extra}
        self.marks.append(entry)
        print(f"[{entry['frame']:6d}] {label}", flush=True)
        self.save_marks()

    def save_marks(self) -> None:
        if self.marks_path:
            self.marks_path.write_text(json.dumps(self.marks, indent=1), encoding="utf-8")

    # --- the tools -----------------------------------------------------------------------------
    def call(self, tool: str, args: dict, check: bool = True, timeout: float = 600):
        payload = self.client.call(tool, args, timeout=timeout)
        if payload.get("isError"):
            if check:
                raise RuntimeError(f"{tool} {json.dumps(args)[:300]} -> {text_of(payload)[:2000]}")
            return {"error": text_of(payload)}
        return payload.get("structuredContent", payload)

    def req(self, kind: str, check: bool = True, wait: bool = False, **fields):
        """An `editor_request`. `wait` is polled here (`wait_idle`), never the tool's own wait: its
        budget is 300 s, and under Movie Maker an operation's frames can be slow in wall time."""
        answer = self.call("editor_request", {"kind": kind, **fields}, check=check)
        outcome = answer.get("outcome", {}) if isinstance(answer, dict) else {}
        if check and outcome and not outcome.get("done", False):
            raise RuntimeError(f"request {kind} not done: {json.dumps(answer)[:2000]}")
        if wait:
            state = self.wait_idle(timeout=7200)
            answer["operation"] = state.get("last_operation") or {}
            answer["status"] = self.state("status")["status"]["status"]
        return answer

    def query(self, name: str, check: bool = True, **params):
        return self.call("editor_query", {"query": name, **params}, check=check)

    def vp(self, op: str, check: bool = True, **fields):
        return self.call("editor_viewport", {"op": op, **fields}, check=check)

    def state(self, *sections):
        return self.call("editor_state", {"sections": list(sections)} if sections else {})

    def wait_idle(self, timeout: float = 600.0) -> dict:
        """Poll the `operation` query until no operation (nor validation) runs."""
        deadline = time.monotonic() + timeout
        while True:
            state = self.query("operation")
            running = (state.get("operation") or {}).get("running") or (state.get("validation") or {}).get("running")
            if not running:
                return state
            if time.monotonic() > deadline:
                raise RuntimeError("operation did not end")
            time.sleep(0.25)

    def follow_operation(self, label: str, interval: float = 0.5) -> dict:
        """Poll the running operation to its end, marking each phase it names; the last `operation`
        query."""
        last = None
        while True:
            state = self.query("operation")
            running = state.get("operation") or {}
            if not running.get("running"):
                return state
            phase = running.get("label") or running.get("kind")
            if phase != last:
                self.mark(label, phase=phase, done=running.get("done"), total=running.get("total"))
                last = phase
            time.sleep(interval)

    def wait_viewport_ready(self, path: str | None = None, kind: str | None = None, timeout: float = 300.0) -> dict:
        args = {}
        if path:
            args["path"] = path
        if kind:
            args["kind"] = kind
        deadline = time.monotonic() + timeout
        while True:
            answer = self.vp("state", limit=1, **args)
            if answer.get("status") != "loading":
                return answer
            if time.monotonic() > deadline:
                raise RuntimeError("viewport never ready")
            time.sleep(0.1)

    def screenshot(self, out: Path, max_dim: int = 1920) -> None:
        """The editor's own `editor_screenshot` (never a desktop capture)."""
        payload = self.client.call("editor_screenshot", {"format": "png", "max_dim": max_dim}, timeout=90)
        images = [b for b in payload.get("content", []) if b.get("type") == "image"]
        if not images:
            raise RuntimeError(f"no screenshot: {text_of(payload)}")
        Path(out).write_bytes(base64.b64decode(images[0]["data"]))


class Take:
    """One recording: the editor, the parts asked for, the marks, and a rehearsal's screenshots."""

    def __init__(self, editor: Editor, storyboard: Storyboard, work: Workspace, out_dir: Path, rehearse: bool,
                 only: set[str], install: str | None):
        self.ed = editor
        self.sb = storyboard
        self.work = work
        self.out_dir = out_dir
        self.rehearse = rehearse
        self.only = only
        self.install = install
        self.shots: list[str] = []

    @property
    def project_dir(self) -> Path:
        return self.work.projects / self.sb.project["dir"]

    def want(self, part: str) -> bool:
        return not self.only or part in self.only

    def mark(self, label: str, shot: bool = True, **extra) -> None:
        """A mark the cut can name; in a rehearsal, the editor's own screenshot of it (`shot`). The
        window is checked (read only) at every mark."""
        self.ed.mark(label, **extra)
        if self.rehearse and shot:
            time.sleep(0.4)
            name = f"{len(self.shots):02d}_{label}"
            self.ed.screenshot(self.out_dir / f"{name}.png", max_dim=self.sb.size[0])
            self.shots.append(name)
        ok, shown = self.ed.window_ok()
        if not ok:
            self.ed.mark("WINDOW-CHANGED " + label, window=str(shown))

    def hold(self, seconds: float) -> None:
        """Movie time: `seconds` of frames written. A rehearsal has no movie: a short wall pause."""
        if self.rehearse:
            time.sleep(min(seconds, REHEARSE_PAUSE))
        else:
            self.ed.hold(seconds)
