#!/usr/bin/env python3
"""The perf cell (docs/mcp.md "The perf cell"): one repeatable measurement of the game's frame.

Stages a CI run's debug GDExtensions into godot/bin (the gdext_windows_template_debug artifact,
with the PDBs of gdext_windows_template_debug_pdb beside them), then runs the perf_mission_rows
probe on a named cell with game.cfg's lock_framerate off and then on, and prints one table: the
frame and its legs (mean / p50 / p95 ms), the ticks per frame and the frame lock's sleeps. With
--trace, each cell's measured windows are sampled: the Visual Studio collector (vsdiag) needs no
elevation, and WPR (wpr) needs an elevated shell. --top then prints the main thread's heaviest
functions by inclusive samples, resolved through the staged PDBs (xperf, Windows Performance
Toolkit). Standard library only; staging and tracing are Windows-only.

    python scripts/mcp/perf_cell.py --run 40429260952 --resource-dir "<JO install>"
    python scripts/mcp/perf_cell.py --resource-dir "<JO install>" --lock off --trace vsdiag --top 40
    python scripts/mcp/perf_cell.py --run 40429260952 --stage-only

A perf commit quotes this table from before and after the change, on the same cell. The staged
DLLs must be built from the checkout's own head, or its GDScript and the bindings disagree. The
script warns when the run's head is not HEAD.

Exit codes: 0 ok; 1 usage, staging or launch failure; 5 a cell's probe did not pass.
"""

from __future__ import annotations

import argparse
import ctypes
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import game_mcp  # noqa: E402
from game_mcp import GameMcp, GameMcpError  # noqa: E402

REPO_ROOT = game_mcp.REPO_ROOT
PROJECT_DIR = game_mcp.PROJECT_DIR
BIN_DIR = PROJECT_DIR / "bin"
GAME_CFG = PROJECT_DIR / "game.cfg"
GAME_MCP = Path(__file__).resolve().parent / "game_mcp.py"
DEFAULT_SCRATCH = REPO_ROOT / "build" / "perf-cell"

ARTIFACT = "gdext_windows_template_debug"
PDB_ARTIFACT = ARTIFACT + "_pdb"
PROBE = "perf_mission_rows"

EXIT_OK = 0
EXIT_USAGE = 1
EXIT_PROBE = 5

# The named cells. A cell is a mission, the game it mounts and the window size; the retail
# install is the caller's (--resource-dir), never a path written here.
CELLS = {
    "01tr-sp-1080p": {"mission": "01TR.bms", "game": "jo", "resolution": "1920x1080",
                      "about": "JO 01TR single player, 1920x1080 windowed"},
}

# The table's skeleton: (probe metric, depth). A metric the record lacks (an older DLL's board)
# prints no row. "@world" stands for the World tick's other legs (every world_* metric past
# world_runtime), heaviest first.
SKELETON = (
    ("frame", 0),
    ("process_callbacks", 1),
    ("frame_player_before", 2),
    ("frame_world", 2),
    ("world_runtime", 3),
    ("sim_advance", 4),
    ("sim_frame_bank", 5),
    ("sim_frame_updates", 5),
    ("sim_step", 6),
    ("sim_sink", 6),
    ("sim_frame_input_rows", 6),
    ("sim_frame_input_drain", 6),
    ("sim_frame_input_apply", 6),
    ("sim_frame_render_legs", 5),
    ("sim_frame_marshal", 5),
    ("present", 4),
    ("@world", 3),
    ("frame_player_after", 2),
    ("frame_hud", 2),
    ("frame_stats_sample", 2),
    ("frame_physics_callbacks", 1),
    ("deferred_flush", 1),
    ("draw", 1),
    ("pacing_input", 1),
)
# The world legs print at this mean or more in any cell (ms).
WORLD_LEG_MIN_MS = 0.05
# The per-frame counts under the table: (probe metric, label).
COUNTS = (("sim_ticks", "ticks per frame"), ("sim_frame_lock_sleeps", "lock sleeps per frame"))

LOCK_LINE = re.compile(rb"^(lock_framerate[ \t]*=[ \t]*)(\d+)", re.MULTILINE)


class CellError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


# --- game.cfg -----------------------------------------------------------------


def with_lock(cfg: bytes, on: bool) -> bytes:
    """game.cfg's bytes with lock_framerate set, every other byte kept."""
    if not LOCK_LINE.search(cfg):
        raise CellError(EXIT_USAGE, "game.cfg has no lock_framerate line")
    return LOCK_LINE.sub(lambda m: m.group(1) + (b"1" if on else b"0"), cfg, count=1)


# --- staging ------------------------------------------------------------------


def run_head(run_id: str) -> str:
    result = subprocess.run(["gh", "run", "view", run_id, "--json", "headSha", "--jq", ".headSha"],
                            capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


def local_head() -> str:
    result = subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"], capture_output=True, text=True)
    return result.stdout.strip()


def download(run_id: str, name: str, target: Path, required: bool) -> bool:
    if target.is_dir() and any(target.iterdir()):
        return True
    target.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(["gh", "run", "download", run_id, "-n", name, "-D", str(target)],
                            capture_output=True, text=True)
    if result.returncode == 0:
        return True
    if required:
        raise CellError(EXIT_USAGE, f"gh run download {run_id} -n {name} failed: {result.stderr.strip()}")
    print(f"perf_cell: no {name} on run {run_id} ({result.stderr.strip()}); --top resolves no project frames")
    return False


def stage(run_id: str, scratch: Path, godot: Path, skip_import: bool) -> None:
    if not (PROJECT_DIR / "addons" / "imgui-godot").is_dir():
        raise CellError(EXIT_USAGE, "godot/addons/imgui-godot is missing: run scripts/bootstrap_godot.sh first")
    dest = scratch / f"run-{run_id}"
    download(run_id, ARTIFACT, dest / ARTIFACT, required=True)
    download(run_id, PDB_ARTIFACT, dest / PDB_ARTIFACT, required=False)
    files = sorted((dest / ARTIFACT).rglob("*.dll")) + sorted((dest / PDB_ARTIFACT).rglob("*.pdb"))
    if not any(f.suffix == ".dll" for f in files):
        raise CellError(EXIT_USAGE, f"{ARTIFACT} of run {run_id} holds no DLL")
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    for source in files:
        try:
            shutil.copy2(source, BIN_DIR / source.name)
        except PermissionError as error:
            raise CellError(EXIT_USAGE, f"cannot replace {BIN_DIR / source.name} (a Godot process holds it?): {error}")
    print(f"staged run {run_id}: " + ", ".join(f.name for f in files))
    head, here = run_head(run_id), local_head()
    if head and here and head != here:
        print(f"perf_cell: WARNING run {run_id} built {head[:10]}, this checkout is at {here[:10]}: "
              "its GDScript must match the staged bindings")
    if skip_import:
        return
    result = subprocess.run([str(godot), "--headless", "--path", str(PROJECT_DIR), "--import"],
                            stdin=subprocess.DEVNULL, capture_output=True, text=True, errors="replace", timeout=1800)
    if result.returncode != 0:
        raise CellError(EXIT_USAGE, f"the import failed ({result.returncode}):\n{result.stdout[-4000:]}")
    print("imported the project")


# --- the process's main thread ---------------------------------------------------


class ThreadEntry(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_uint32), ("cntUsage", ctypes.c_uint32), ("th32ThreadID", ctypes.c_uint32),
                ("th32OwnerProcessID", ctypes.c_uint32), ("tpBasePri", ctypes.c_long),
                ("tpDeltaPri", ctypes.c_long), ("dwFlags", ctypes.c_uint32)]


def main_thread(pid: int) -> int:
    """The process's first thread (the earliest created): Godot's main thread, which runs the frame."""
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    kernel32.OpenThread.restype = ctypes.c_void_p
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000004, 0)  # TH32CS_SNAPTHREAD
    if not snapshot or snapshot == ctypes.c_void_p(-1).value:
        return 0
    best, best_created = 0, None
    try:
        entry = ThreadEntry()
        entry.dwSize = ctypes.sizeof(ThreadEntry)
        ok = kernel32.Thread32First(ctypes.c_void_p(snapshot), ctypes.byref(entry))
        while ok:
            if entry.th32OwnerProcessID == pid:
                handle = kernel32.OpenThread(0x0800, False, entry.th32ThreadID)  # QUERY_LIMITED_INFORMATION
                if handle:
                    times = (ctypes.c_uint64 * 4)()
                    if kernel32.GetThreadTimes(ctypes.c_void_p(handle), ctypes.byref(times, 0),
                                               ctypes.byref(times, 8), ctypes.byref(times, 16),
                                               ctypes.byref(times, 24)):
                        if best_created is None or times[0] < best_created:
                            best, best_created = entry.th32ThreadID, times[0]
                    kernel32.CloseHandle(ctypes.c_void_p(handle))
            ok = kernel32.Thread32Next(ctypes.c_void_p(snapshot), ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(snapshot))
    return best


# --- tracing ------------------------------------------------------------------


# The standard install roots (the tools' own installers' defaults).
PROGRAM_FILES = (r"C:\Program Files", r"C:\Program Files (x86)")


def vsdiag_exe() -> Path | None:
    for root in PROGRAM_FILES:
        hits = sorted(glob.glob(os.path.join(root, "Microsoft Visual Studio", "*", "*", "Team Tools",
                                             "DiagnosticsHub", "Collector", "VSDiagnostics.exe")), reverse=True)
        if hits:
            return Path(hits[0])
    return None


def xperf_exe() -> Path | None:
    path = Path(PROGRAM_FILES[1]) / "Windows Kits" / "10" / "Windows Performance Toolkit" / "xperf.exe"
    return path if path.is_file() else None


class Trace:
    """One cell's CPU sample: started over the measured windows, stopped before the game quits."""

    def __init__(self, kind: str, pid: int, out: Path):
        self.kind, self.pid, self.out = kind, pid, out
        self.session = str(os.getpid() % 200 + 20)
        if kind == "vsdiag":
            self.exe = vsdiag_exe()
            if self.exe is None:
                raise CellError(EXIT_USAGE, "--trace vsdiag: no Visual Studio Standard Collector (VSDiagnostics.exe)")
        elif kind == "wpr":
            if not ctypes.windll.shell32.IsUserAnAdmin():
                raise CellError(EXIT_USAGE, "--trace wpr needs an elevated shell (--trace vsdiag needs none)")
        self.etl = out.with_suffix(".etl")
        self.running = False

    def start(self) -> None:
        if self.kind == "vsdiag":
            config = self.exe.parent / "AgentConfigs" / "CpuUsageHigh.json"
            command = [str(self.exe), "start", self.session, f"/attach:{self.pid}", f"/loadConfig:{config}"]
        else:
            command = ["wpr", "-start", "CPU", "-filemode"]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace")
        if result.returncode != 0:
            raise CellError(EXIT_USAGE, f"{self.kind} start failed: {result.stdout}{result.stderr}")
        self.running = True

    def stop(self) -> Path:
        self.running = False
        if self.kind == "vsdiag":
            session = self.out.with_suffix("")
            result = subprocess.run([str(self.exe), "stop", self.session, f"/output:{session}"],
                                    capture_output=True, text=True, errors="replace")
            package = session.with_suffix(".diagsession")
            if result.returncode != 0 or not package.is_file():
                raise CellError(EXIT_USAGE, f"vsdiag stop failed: {result.stdout}{result.stderr}")
            with zipfile.ZipFile(package) as archive:
                member = max((i for i in archive.infolist() if i.filename.endswith(".etl")),
                             key=lambda i: i.file_size)
                with archive.open(member) as source, open(self.etl, "wb") as sink:
                    shutil.copyfileobj(source, sink)
            package.unlink()
        else:
            result = subprocess.run(["wpr", "-stop", str(self.etl)], capture_output=True, text=True, errors="replace")
            if result.returncode != 0:
                raise CellError(EXIT_USAGE, f"wpr stop failed: {result.stdout}{result.stderr}")
        return self.etl


ROW = re.compile(r"<tr><td>(.*?)</td><td>(\d+)</td><td>([\d.]+)%</td><td>(\d+)</td>")
TAG = re.compile(r"<[^>]+>")


def parse_butterfly(html: str) -> list[tuple[str, int, float, int]]:
    """xperf's stack report, its "Functions by UniInclusive Hits" table: (module!function,
    inclusive hits, percent of the selection, exclusive hits), heaviest first."""
    start = html.find("id='TblSI'")
    if start < 0:
        return []
    end = html.find("</table>", start)
    rows = []
    for match in ROW.finditer(html, start, end if end > 0 else len(html)):
        rows.append((TAG.sub("", match.group(1)), int(match.group(2)), float(match.group(3)), int(match.group(4))))
    return rows


def top_functions(etl: Path, tid: int, scratch: Path, count: int) -> list[tuple[str, int, float, int]]:
    xperf = xperf_exe()
    if xperf is None:
        raise CellError(EXIT_USAGE, "--top needs xperf (Windows Performance Toolkit)")
    env = dict(os.environ)
    env["_NT_SYMBOL_PATH"] = f"{BIN_DIR};srv*{scratch / 'symbols'}*https://msdl.microsoft.com/download/symbols"
    report = etl.with_suffix(".stacks.html")
    command = [str(xperf), "-i", str(etl), "-symbols", "-o", str(report), "-a", "stack", "-butterfly", "10"]
    if tid:
        command += ["-tid", str(tid)]
    result = subprocess.run(command, env=env, capture_output=True, text=True, errors="replace", timeout=7200)
    if result.returncode != 0 or not report.is_file():
        raise CellError(EXIT_USAGE, f"xperf failed: {result.stdout[-2000:]}{result.stderr[-2000:]}")
    return parse_butterfly(report.read_text(encoding="utf-8", errors="replace"))[:count]


# --- one cell -----------------------------------------------------------------


def run_cell(args: argparse.Namespace, cell: dict, lock: str, out_dir: Path) -> dict:
    label = f"{args.cell}-lock-{lock}"
    pid_file = out_dir / f"{label}.pid"
    launch = [sys.executable, str(GAME_MCP), "launch", "--windowed", "--resolution", cell["resolution"],
              "--resource-dir", args.resource_dir, "--game", cell["game"], "--mission", cell["mission"],
              "--port", str(args.port), "--pid-file", str(pid_file), "--stdout-log", str(out_dir / f"{label}.log")]
    if args.godot:
        launch += ["--godot", args.godot]
    if subprocess.run(launch).returncode != 0:
        raise CellError(EXIT_USAGE, f"{label}: the launch failed")
    pid, _ = game_mcp.read_pid_file(pid_file)
    tid = main_thread(pid) if os.name == "nt" and pid else 0
    trace = Trace(args.trace, pid, out_dir / label) if args.trace else None
    client = GameMcp.for_port(args.port)
    status: dict = {}
    try:
        started = client.structured("game_probe", {"op": "run", "name": PROBE, "args": {
            "warmup_seconds": args.warmup, "window_seconds": args.window_seconds, "windows": args.windows,
            "label": label}})
        run_id, cursor, trace_at = started["run_id"], 0, None
        deadline = time.monotonic() + args.timeout
        while True:
            status = client.structured("game_probe", {"op": "status", "run_id": run_id, "cursor": cursor,
                                                      "wait_ms": 250}, timeout=60)
            for line in status.get("lines", []):
                text = line.get("text", "") if isinstance(line, dict) else str(line)
                print(f"  [{label}] {text}")
                # The windows open once the warmup past this line ends: the trace covers them alone.
                if trace is not None and trace_at is None and "warming" in text:
                    trace_at = time.monotonic() + args.warmup
            cursor = int(status.get("next_cursor", cursor))
            if trace is not None and trace_at is not None and not trace.running and time.monotonic() >= trace_at:
                trace.start()
            if status.get("state") != "running":
                break
            if time.monotonic() >= deadline:
                raise CellError(EXIT_PROBE, f"{label}: the probe still runs after {args.timeout:.0f} s")
        etl = trace.stop() if trace is not None and trace.running else None
    finally:
        # A trace still running here (the probe failed under it) stops ahead of the game's quit.
        if trace is not None and trace.running:
            try:
                trace.stop()
            except CellError as error:
                print(f"perf_cell: {error}", file=sys.stderr)
        subprocess.run([sys.executable, str(GAME_MCP), "stop", "--pid-file", str(pid_file)])
    if status.get("state") != "passed":
        raise CellError(EXIT_PROBE, f"{label}: the probe ended {status.get('state')}: {status.get('error')}")
    record_path = next((Path(a["path"]) for a in status.get("artifacts", [])
                        if str(a.get("path", "")).endswith(".json")), None)
    if record_path is None:
        raise CellError(EXIT_PROBE, f"{label}: the probe wrote no record")
    record = json.loads(record_path.read_text(encoding="utf-8"))
    result = {"lock": lock, "record": str(record_path), "metrics": record["aggregate"]["metrics"],
              "frames": record["aggregate"].get("mission_frames"), "main_thread": tid}
    if etl is not None:
        result["trace"] = str(etl)
        if args.top:
            result["top"] = top_functions(etl, tid, args.scratch, args.top)
    return result


# --- the table ----------------------------------------------------------------


def stat(metrics: dict, name: str) -> tuple[float, float, float] | None:
    value = metrics.get(name)
    if not isinstance(value, dict) or "mean_ms" not in value:
        return None
    return float(value["mean_ms"]), float(value.get("p50_ms", 0.0)), float(value.get("p95_ms", 0.0))


def world_legs(cells: list[dict]) -> list[str]:
    names = set()
    for cell in cells:
        for name, value in cell["metrics"].items():
            if name.startswith("world_") and name != "world_runtime" and isinstance(value, dict) \
                    and float(value.get("mean_ms", 0.0)) >= WORLD_LEG_MIN_MS:
                names.add(name)
    first = cells[0]["metrics"]
    return sorted(names, key=lambda n: (-(stat(first, n) or (0.0,))[0], n))


def summary_table(cells: list[dict]) -> str:
    rows = []
    for name, depth in SKELETON:
        names = world_legs(cells) if name == "@world" else [name]
        for leg in names:
            values = [stat(cell["metrics"], leg) for cell in cells]
            if all(v is None for v in values):
                continue
            cols = " | ".join("%7.2f %7.2f %7.2f" % v if v is not None else "%23s" % "-" for v in values)
            rows.append(f"{'  ' * depth + leg:<34} {cols}")
    head = " | ".join(f"{'lock ' + cell['lock'] + ': mean     p50     p95':>23}" for cell in cells)
    lines = [f"{'leg (ms)':<34} {head}"] + rows
    for name, label in COUNTS:
        values = [cell["metrics"].get(name) for cell in cells]
        if all(not isinstance(v, dict) for v in values):
            continue
        cols = " | ".join("%23.2f" % float(v.get("mean", 0.0)) if isinstance(v, dict) else "%23s" % "-"
                          for v in values)
        lines.append(f"{label:<34} {cols}")
    lines.append(f"{'frames sampled':<34} " + " | ".join("%23s" % cell.get("frames") for cell in cells))
    return "\n".join(lines)


# --- the command --------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--run", default=None, help="stage this CI run's debug GDExtensions (and PDBs) first")
    parser.add_argument("--stage-only", action="store_true", help="stage --run and stop")
    parser.add_argument("--skip-import", action="store_true", help="stage without the project import")
    parser.add_argument("--resource-dir", default=None, help="the retail install the cell mounts")
    parser.add_argument("--cell", default="01tr-sp-1080p", choices=sorted(CELLS))
    parser.add_argument("--lock", default="off,on", help="game.cfg lock_framerate states, in order (off,on)")
    parser.add_argument("--warmup", type=float, default=4.0, help="seconds past the reveal before the windows")
    parser.add_argument("--window-seconds", type=float, default=5.0)
    parser.add_argument("--windows", type=int, default=2)
    parser.add_argument("--trace", choices=("vsdiag", "wpr"), default=None, help="CPU-sample the measured windows")
    parser.add_argument("--top", type=int, default=0, help="with --trace: the main thread's N heaviest functions")
    parser.add_argument("--port", type=int, default=game_mcp.DEFAULT_PORT)
    parser.add_argument("--godot", default=None, help="Godot 4.6.1 binary (default: GODOT_BIN, else .godot-bin/)")
    parser.add_argument("--scratch", type=Path, default=DEFAULT_SCRATCH, help="downloads, logs and traces")
    parser.add_argument("--json", default=None, help="also write the cells' summary here")
    parser.add_argument("--timeout", type=float, default=600.0, help="seconds one cell's probe may run")
    return parser


def run(args: argparse.Namespace) -> int:
    args.scratch = args.scratch.resolve()
    godot = game_mcp.find_godot(args.godot)
    if args.run:
        stage(args.run, args.scratch, godot, args.skip_import)
    if args.stage_only:
        return EXIT_OK
    if not args.resource_dir:
        raise CellError(EXIT_USAGE, "--resource-dir names the retail install the cell mounts")
    locks = [s.strip() for s in args.lock.split(",") if s.strip()]
    if not locks or any(s not in ("off", "on") for s in locks):
        raise CellError(EXIT_USAGE, f"--lock takes off and on, comma separated (got {args.lock!r})")
    if not GAME_CFG.is_file():
        raise CellError(EXIT_USAGE, f"{GAME_CFG} is missing: start the game once (its first menu writes it)")
    cell = CELLS[args.cell]
    out_dir = args.scratch / time.strftime("cell-%Y%m%d-%H%M%S")
    out_dir.mkdir(parents=True, exist_ok=True)
    original = GAME_CFG.read_bytes()
    cells = []
    try:
        for lock in locks:
            GAME_CFG.write_bytes(with_lock(original, lock == "on"))
            cells.append(run_cell(args, cell, lock, out_dir))
    finally:
        GAME_CFG.write_bytes(original)
    print(f"\nperf cell {args.cell} ({cell['about']}); DLL {BIN_DIR / 'oneditord.dll'}"
          + (f" from run {args.run}" if args.run else ""))
    print(summary_table(cells))
    for result in cells:
        if result.get("top"):
            print(f"\nlock {result['lock']}: main thread {result['main_thread']}, heaviest by inclusive samples "
                  f"({result['trace']})")
            for name, inclusive, percent, exclusive in result["top"]:
                print(f"  {percent:6.2f}%  {inclusive:8d} incl  {exclusive:8d} excl  {name}")
    if args.json:
        Path(args.json).write_text(json.dumps({"cell": args.cell, "run": args.run, "head": local_head(),
                                               "cells": cells}, indent=1), encoding="utf-8")
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(errors="replace")
    args = build_parser().parse_args(argv)
    try:
        return run(args)
    except (CellError, GameMcpError) as error:
        print(f"perf_cell: {error}", file=sys.stderr)
        return error.code
    except KeyboardInterrupt:
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
