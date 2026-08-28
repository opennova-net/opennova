#!/usr/bin/env python3
"""The opennova-game MCP client (docs/mcp.md).

Launches the OpenNova runtime with `--mcp-port`, speaks JSON-RPC to its
loopback Streamable-HTTP endpoint, and wraps the everyday tools as
subcommands. Standard library only. PowerShell callers use
scripts/mcp/game_mcp.ps1 instead; both talk to the same endpoint.

    python scripts/mcp/game_mcp.py launch --windowed --mission 00TRa.bms
    python scripts/mcp/game_mcp.py tools
    python scripts/mcp/game_mcp.py call game_state
    python scripts/mcp/game_mcp.py probe run perf_sample '{"sample_ms": 5000}' --wait
    python scripts/mcp/game_mcp.py stop --pid-file .scratch/game.pid

Exit codes: 0 ok; 1 usage or transport failure; 2 the tool reported
isError; 3 a JSON-RPC error; 4 the probe was cancelled; 5 the probe
errored; 6 stop: the process outlived the quit; 7 launch failed.
"""

from __future__ import annotations

import argparse
import base64
import ctypes
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_PORT = 8975
PROTOCOL_VERSION = "2025-06-18"
REPO_ROOT = Path(__file__).resolve().parents[2]
PROJECT_DIR = REPO_ROOT / "godot"
RUNTIME_SCENE = "res://game/game_runtime_root.tscn"
GODOT_BINARIES = (
    "Godot_v4.6.1-stable_win64.exe",
    "Godot_v4.6.1-stable_win64_console.exe",
    "Godot_v4.6.1-stable_linux.x86_64",
    "Godot_v4.6.1-stable_macos.universal",
)

EXIT_OK = 0
EXIT_USAGE = 1
EXIT_TOOL_ERROR = 2
EXIT_RPC_ERROR = 3
EXIT_PROBE_CANCELLED = 4
EXIT_PROBE_ERROR = 5
EXIT_STOP_SURVIVED = 6
EXIT_LAUNCH_FAILED = 7

# The game flags `launch` knows by name. Anything else rides after `--`.
GAME_VALUE_FLAGS = (
    ("resource_dir", "--resource-dir"),
    ("exp", "/exp"),
    ("game", "/game"),
    ("loose_mission", "--loose-mission"),
    ("mission", "--mission"),
    ("lan_host", "--lan-host"),
    ("lan_join", "--lan-join"),
    ("lan_port", "--lan-port"),
    ("callsign", "--callsign"),
    ("lan_gametype", "--lan-gametype"),
    ("lan_mode", "--lan-mode"),
    ("lan_max_players", "--lan-max-players"),
    ("integrity_profile", "--integrity-profile"),
    ("capture_pcap", "--capture-pcap"),
)


class GameMcpError(Exception):
    """A failure with a process exit code attached."""

    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


class GameMcp:
    """One JSON-RPC session against a running game's endpoint."""

    def __init__(self, url: str, timeout: float = 120.0):
        self.url = url
        self.timeout = timeout
        self.session_id = ""
        self._next_id = 1
        self.server_info: dict = {}

    @classmethod
    def for_port(cls, port: int, timeout: float = 120.0) -> "GameMcp":
        return cls(f"http://127.0.0.1:{port}/mcp", timeout)

    def _post(self, payload: dict, timeout: float | None = None) -> dict | None:
        body = json.dumps(payload).encode("utf-8")
        headers = {"Content-Type": "application/json", "Accept": "application/json"}
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        request = urllib.request.Request(self.url, data=body, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=timeout or self.timeout) as response:
                session = response.headers.get("Mcp-Session-Id")
                if session:
                    self.session_id = session
                raw = response.read()
        except urllib.error.HTTPError as error:
            raise GameMcpError(EXIT_USAGE, f"HTTP {error.code} from {self.url}: {error.reason}") from error
        except (urllib.error.URLError, ConnectionError, socket.timeout, OSError) as error:
            raise GameMcpError(EXIT_USAGE, f"no game endpoint at {self.url}: {error}") from error
        if not raw:
            return None
        try:
            return json.loads(raw.decode("utf-8"))
        except ValueError as error:
            raise GameMcpError(EXIT_RPC_ERROR, f"invalid JSON from {self.url}: {error}") from error

    def rpc(self, method: str, params: dict | None = None, timeout: float | None = None):
        request_id = self._next_id
        self._next_id += 1
        envelope = self._post(
            {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}, timeout)
        if envelope is None:
            raise GameMcpError(EXIT_RPC_ERROR, f"{method}: empty response")
        if "error" in envelope:
            error = envelope["error"]
            raise GameMcpError(
                EXIT_RPC_ERROR,
                f"{method}: JSON-RPC error {error.get('code')}: {error.get('message')}"
                + (f" {json.dumps(error['data'])}" if "data" in error else ""))
        return envelope.get("result")

    def initialize(self, timeout: float | None = None) -> dict:
        result = self.rpc("initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "game_mcp.py", "version": "1"},
        }, timeout)
        self._post({"jsonrpc": "2.0", "method": "notifications/initialized"}, timeout)
        self.server_info = result.get("serverInfo", {}) if isinstance(result, dict) else {}
        return result

    def ensure_session(self) -> None:
        if not self.session_id:
            self.initialize()

    def tools(self) -> list:
        self.ensure_session()
        return self.rpc("tools/list").get("tools", [])

    def call(self, name: str, arguments: dict | None = None, timeout: float | None = None) -> dict:
        """tools/call; returns the raw payload {content, isError[, structuredContent]}."""
        self.ensure_session()
        return self.rpc("tools/call", {"name": name, "arguments": arguments or {}}, timeout)

    def structured(self, name: str, arguments: dict | None = None, timeout: float | None = None):
        """tools/call that must succeed; returns structuredContent (or the parsed text block)."""
        payload = self.call(name, arguments, timeout)
        if payload.get("isError"):
            raise GameMcpError(EXIT_TOOL_ERROR, f"{name} failed: {text_of(payload)}")
        if "structuredContent" in payload:
            return payload["structuredContent"]
        text = text_of(payload)
        try:
            return json.loads(text)
        except ValueError:
            return text


def text_of(payload: dict) -> str:
    return "\n".join(
        block.get("text", "") for block in payload.get("content", []) if block.get("type") == "text")


def images_of(payload: dict) -> list:
    return [block for block in payload.get("content", []) if block.get("type") == "image"]


# --- process helpers ----------------------------------------------------------


def port_open(port: int, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        # os.kill(pid, 0) TERMINATES on Windows; ask the kernel instead.
        kernel32 = ctypes.windll.kernel32
        synchronize = 0x00100000
        handle = kernel32.OpenProcess(synchronize, False, pid)
        if not handle:
            return False
        try:
            wait_timeout = 0x00000102
            return kernel32.WaitForSingleObject(handle, 0) == wait_timeout
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def find_godot(explicit: str | None) -> Path:
    candidate = explicit or os.environ.get("GODOT_BIN", "")
    if candidate:
        path = Path(candidate)
        if not path.is_file():
            raise GameMcpError(EXIT_LAUNCH_FAILED, f"Godot binary not found: {path}")
        return runtime_sibling(path)
    directory = REPO_ROOT
    while True:
        for name in GODOT_BINARIES:
            path = directory / ".godot-bin" / name
            if path.is_file():
                return runtime_sibling(path)
        if directory.parent == directory:
            break
        directory = directory.parent
    raise GameMcpError(
        EXIT_LAUNCH_FAILED,
        "Godot 4.6.1 not found: pass --godot, set GODOT_BIN, or place the binary under .godot-bin/")


def runtime_sibling(path: Path) -> Path:
    """The console wrapper starts the real exe as a child; launch the real one so the PID is the game's."""
    if path.name.endswith("_console.exe"):
        sibling = path.with_name(path.name.replace("_console.exe", ".exe"))
        if sibling.is_file():
            return sibling
    return path


def tail(path: Path, lines: int = 40) -> str:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    return "\n".join(text.splitlines()[-lines:])


def retail_install() -> str:
    """The packed retail install (OPENNOVA_JO_DIR, docs/asset-gated-tests.md); '' when unset."""
    value = os.environ.get("OPENNOVA_JO_DIR", "").strip()
    return value if value and Path(value).is_dir() else ""


# --- subcommands --------------------------------------------------------------


def cmd_launch(args: argparse.Namespace) -> int:
    port = args.port
    if port_open(port):
        raise GameMcpError(
            EXIT_LAUNCH_FAILED,
            f"port {port} already answers; stop that game first or pass another --port")
    godot = find_godot(args.godot)
    project = Path(args.project).resolve()
    if not (project / "project.godot").is_file():
        raise GameMcpError(EXIT_LAUNCH_FAILED, f"no project.godot under {project}")

    argv = [str(godot), "--path", str(project)]
    if args.headless:
        argv.append("--headless")
    elif args.windowed:
        argv.append("--windowed")
    if args.resolution:
        argv += ["--resolution", args.resolution]
    if args.rendering_method:
        argv += ["--rendering-method", args.rendering_method]
    log_file = Path(args.log_file).resolve() if args.log_file else None
    if log_file:
        argv += ["--log-file", str(log_file)]
    argv += [args.scene, "--"]
    if args.loose:
        argv.append("/d")
    if args.loose_root:
        argv.append("--loose-root")
    resource_dir = args.resource_dir or retail_install()
    if resource_dir and not args.resource_dir:
        setattr(args, "resource_dir", resource_dir)
    for attribute, flag in GAME_VALUE_FLAGS:
        value = getattr(args, attribute, None)
        if value not in (None, ""):
            argv += [flag, str(value)]
    argv += list(args.game_args or [])
    argv += ["--mcp-port", str(port)]

    stdout_log = Path(args.stdout_log) if args.stdout_log else \
        Path(tempfile.gettempdir()) / f"opennova-game-mcp-{port}.log"
    stdout_log.parent.mkdir(parents=True, exist_ok=True)
    creation = 0
    popen_kwargs = {}
    if os.name == "nt":
        creation = subprocess.CREATE_NEW_PROCESS_GROUP | getattr(subprocess, "DETACHED_PROCESS", 0x00000008)
        popen_kwargs["creationflags"] = creation
    else:
        popen_kwargs["start_new_session"] = True
    with open(stdout_log, "wb") as sink:
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT,
                                 cwd=str(project), **popen_kwargs)
    if args.pid_file:
        Path(args.pid_file).write_text(str(child.pid), encoding="utf-8")

    deadline = time.monotonic() + args.timeout
    client = GameMcp.for_port(port)
    while time.monotonic() < deadline:
        exit_code = child.poll()
        if exit_code is not None:
            raise GameMcpError(
                EXIT_LAUNCH_FAILED,
                f"the game exited with code {exit_code} before its endpoint answered\n"
                f"--- {stdout_log} ---\n{tail(stdout_log)}"
                + (f"\n--- {log_file} ---\n{tail(log_file)}" if log_file else ""))
        if port_open(port):
            try:
                client.initialize(timeout=5)
                break
            except GameMcpError:
                pass
        time.sleep(0.25)
    else:
        raise GameMcpError(
            EXIT_LAUNCH_FAILED,
            f"the endpoint on port {port} did not answer within {args.timeout:.0f} s (pid {child.pid} "
            f"still running; stop it with `stop --pid {child.pid}`)\n--- {stdout_log} ---\n{tail(stdout_log)}")
    print(f"url={client.url} pid={child.pid} log={stdout_log}"
          + (f" godot_log={log_file}" if log_file else ""))
    return EXIT_OK


def cmd_wait(args: argparse.Namespace) -> int:
    deadline = time.monotonic() + args.timeout
    client = client_of(args)
    while time.monotonic() < deadline:
        if port_open(port_of(args)):
            try:
                client.initialize(timeout=5)
                print(f"ready url={client.url} server={client.server_info.get('name', '?')}")
                return EXIT_OK
            except GameMcpError:
                pass
        time.sleep(0.25)
    raise GameMcpError(EXIT_USAGE, f"no endpoint answered at {client.url} within {args.timeout:.0f} s")


def cmd_tools(args: argparse.Namespace) -> int:
    tools = client_of(args).tools()
    if args.json:
        print(json.dumps(tools, indent=2))
        return EXIT_OK
    for tool in tools:
        description = str(tool.get("description", "")).split(". ")[0]
        print(f"{tool['name']:<26} {description}")
    return EXIT_OK


def parse_json_arg(text: str | None, path: str | None) -> dict:
    if path:
        text = Path(path).read_text(encoding="utf-8")
    if not text:
        return {}
    try:
        value = json.loads(text)
    except ValueError as error:
        raise GameMcpError(EXIT_USAGE, f"arguments are not JSON: {error}") from error
    if not isinstance(value, dict):
        raise GameMcpError(EXIT_USAGE, "arguments must be a JSON object")
    return value


def emit_payload(payload: dict, args: argparse.Namespace) -> None:
    if getattr(args, "json", False):
        print(json.dumps(payload, indent=2))
        return
    text = text_of(payload)
    if text:
        print(text)
    for index, image in enumerate(images_of(payload)):
        data = base64.b64decode(image.get("data", ""))
        out = getattr(args, "out", None)
        if out:
            target = Path(out) if index == 0 else Path(out).with_name(f"{Path(out).stem}-{index}{Path(out).suffix}")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            print(f"image -> {target} ({len(data)} bytes, {image.get('mimeType')})")
        else:
            print(f"[image {image.get('mimeType')} {len(data)} bytes; pass --out to save it]")


def cmd_call(args: argparse.Namespace) -> int:
    payload = client_of(args).call(args.tool, parse_json_arg(args.arguments, args.args_file),
                                   timeout=args.timeout)
    emit_payload(payload, args)
    return EXIT_TOOL_ERROR if payload.get("isError") else EXIT_OK


def cmd_state(args: argparse.Namespace) -> int:
    print(json.dumps(client_of(args).structured("game_state"), indent=2))
    return EXIT_OK


def cmd_logs(args: argparse.Namespace) -> int:
    client = client_of(args)
    cursor = args.cursor
    sources = [s for s in (args.sources or "").split(",") if s]
    while True:
        request = {"limit": args.limit}
        if cursor > 0:
            request["cursor"] = cursor
        if sources:
            request["sources"] = sources
        page = client.structured("game_logs", request)
        for entry in page.get("entries", []):
            print(f"{entry.get('seq', '')}\t{entry.get('source', '')}\t{entry.get('level', '')}\t{entry.get('text', '')}")
        cursor = int(page.get("next_cursor", cursor))
        if not args.follow:
            print(f"next_cursor={cursor}", file=sys.stderr)
            return EXIT_OK
        time.sleep(args.interval)


def cmd_screenshot(args: argparse.Namespace) -> int:
    request = {"format": args.format, "max_dim": args.max_dim}
    if args.quality is not None:
        request["quality"] = args.quality
    payload = client_of(args).call("game_screenshot", request, timeout=90)
    if payload.get("isError"):
        raise GameMcpError(EXIT_TOOL_ERROR, f"game_screenshot failed: {text_of(payload)}")
    images = images_of(payload)
    if not images:
        raise GameMcpError(EXIT_TOOL_ERROR, "game_screenshot returned no image block")
    target = Path(args.out)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(base64.b64decode(images[0]["data"]))
    print(f"{target} ({images[0].get('mimeType')})")
    return EXIT_OK


def cmd_entities(args: argparse.Namespace) -> int:
    client = client_of(args)
    sink = open(args.out, "a", encoding="utf-8") if args.out else None
    deadline = time.monotonic() + args.duration if args.duration else None
    try:
        while True:
            rows = []
            offset = 0
            while True:
                page = client.structured("game_entities", {"op": "list", "offset": offset, "limit": 128})
                batch = page.get("entities", page.get("rows", []))
                rows.extend(batch)
                if not batch or len(batch) < 128 or not page.get("has_more", False):
                    break
                offset += len(batch)
            record = {"t": time.strftime("%Y-%m-%dT%H:%M:%S"), "count": len(rows), "entities": rows}
            line = json.dumps(record)
            if sink:
                sink.write(line + "\n")
                sink.flush()
                print(f"{record['t']} {len(rows)} entities", file=sys.stderr)
            else:
                print(line)
            if not args.watch:
                return EXIT_OK
            if deadline and time.monotonic() >= deadline:
                return EXIT_OK
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return EXIT_OK
    finally:
        if sink:
            sink.close()


PROBE_EXIT = {"passed": EXIT_OK, "failed": EXIT_USAGE, "cancelled": EXIT_PROBE_CANCELLED, "error": EXIT_PROBE_ERROR}


def print_probe_lines(status: dict) -> None:
    for line in status.get("lines", []):
        print(f"[{line.get('t_ms', 0) / 1000.0:8.3f}] {line.get('text', '')}")


def print_probe_end(status: dict) -> int:
    state = str(status.get("state", "error"))
    verdict = status.get("verdict") or {}
    summary = verdict.get("summary", "") if isinstance(verdict, dict) else ""
    print(f"probe {status.get('name', '?')} {status.get('run_id', '?')}: {state}"
          + (f" - {summary}" if summary else "")
          + (f" ({status.get('error')})" if status.get("error") else ""))
    for artifact in status.get("artifacts", []):
        print(f"  artifact {artifact.get('label', '')}: {artifact.get('path', '')} ({artifact.get('bytes', 0)} bytes)")
    if isinstance(verdict, dict) and verdict.get("data") not in (None, {}):
        print("  data: " + json.dumps(verdict["data"]))
    return PROBE_EXIT.get(state, EXIT_PROBE_ERROR)


def wait_probe(client: GameMcp, run_id: str, poll_ms: int, timeout: float) -> int:
    deadline = time.monotonic() + timeout
    cursor = 0
    while True:
        status = client.structured("game_probe", {
            "op": "status", "run_id": run_id, "cursor": cursor, "wait_ms": min(poll_ms, 30000)},
            timeout=max(60.0, poll_ms / 1000.0 + 30.0))
        print_probe_lines(status)
        cursor = int(status.get("next_cursor", cursor))
        if status.get("state") != "running":
            return print_probe_end(status)
        if time.monotonic() >= deadline:
            raise GameMcpError(EXIT_PROBE_ERROR, f"probe {run_id} still running after {timeout:.0f} s (cancel it with `probe cancel {run_id}`)")


def cmd_probe(args: argparse.Namespace) -> int:
    client = client_of(args)
    if args.probe_op == "list":
        listing = client.structured("game_probe", {"op": "list"})
        if args.json:
            print(json.dumps(listing, indent=2))
            return EXIT_OK
        for probe in listing.get("probes", []):
            flags = []
            if probe.get("needs_window"):
                flags.append("window")
            if probe.get("needs_mission"):
                flags.append("mission")
            if not probe.get("available", True):
                flags.append("NOT SHIPPED")
            print(f"{probe['name']:<28} {probe.get('description', '')}" + (f"  [{', '.join(flags)}]" if flags else ""))
        return EXIT_OK
    if args.probe_op == "run":
        started = client.structured("game_probe", {
            "op": "run", "name": args.name, "args": parse_json_arg(args.arguments, args.args_file)})
        print(f"run_id={started.get('run_id')} artifact_dir={started.get('artifact_dir', '')}")
        if not args.wait:
            return EXIT_OK
        return wait_probe(client, started["run_id"], args.poll_ms, args.timeout)
    if args.probe_op == "status":
        request = {"op": "status", "cursor": args.cursor, "wait_ms": args.wait_ms}
        if args.run_id:
            request["run_id"] = args.run_id
        status = client.structured("game_probe", request, timeout=max(60.0, args.wait_ms / 1000.0 + 30.0))
        if args.json:
            print(json.dumps(status, indent=2))
            return EXIT_OK
        print_probe_lines(status)
        if status.get("state") == "running":
            print(f"probe {status.get('name')} {status.get('run_id')}: running ({status.get('elapsed_ms', 0)} ms) next_cursor={status.get('next_cursor')}")
            return EXIT_OK
        return print_probe_end(status)
    if args.probe_op == "wait":
        return wait_probe(client, args.run_id, args.poll_ms, args.timeout)
    if args.probe_op == "cancel":
        request = {"op": "cancel"}
        if args.run_id:
            request["run_id"] = args.run_id
        result = client.structured("game_probe", request)
        print(f"probe {result.get('run_id')}: {result.get('state')}")
        return EXIT_OK
    raise GameMcpError(EXIT_USAGE, f"unknown probe op {args.probe_op}")


def cmd_stop(args: argparse.Namespace) -> int:
    port = port_of(args)
    pid = args.pid or 0
    if args.pid_file and not pid:
        try:
            pid = int(Path(args.pid_file).read_text(encoding="utf-8").strip() or "0")
        except (OSError, ValueError):
            pid = 0
    client = client_of(args)
    if port_open(port):
        try:
            client.call("game_probe", {"op": "cancel"}, timeout=10)
        except GameMcpError:
            pass  # no probe tool, or nothing running
        try:
            client.call("game_control", {"action": "quit"}, timeout=10)
        except GameMcpError:
            pass  # the game may drop the connection while quitting
    else:
        print(f"no endpoint on port {port}; waiting for the process only")
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        alive = pid_alive(pid) if pid else port_open(port)
        if not alive:
            print(f"stopped" + (f" pid={pid}" if pid else f" port={port}"))
            if args.pid_file:
                try:
                    Path(args.pid_file).unlink()
                except OSError:
                    pass
            return EXIT_OK
        time.sleep(0.25)
    raise GameMcpError(
        EXIT_STOP_SURVIVED,
        f"the game did not exit within {args.timeout:.0f} s after game_control quit"
        + (f" (pid {pid} is still alive; this client never kills)" if pid else ""))


# --- argument parsing ---------------------------------------------------------


def port_of(args: argparse.Namespace) -> int:
    return int(args.port)


def client_of(args: argparse.Namespace) -> GameMcp:
    if getattr(args, "url", None):
        return GameMcp(args.url)
    return GameMcp.for_port(port_of(args))


def add_endpoint_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"the game's --mcp-port (default {DEFAULT_PORT})")
    parser.add_argument("--url", default=None, help="the full endpoint URL instead of --port")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    launch = commands.add_parser("launch", help="start the game with --mcp-port and wait for its endpoint")
    launch.add_argument("--godot", default=None, help="Godot 4.6.1 binary (default: GODOT_BIN, else .godot-bin/)")
    launch.add_argument("--project", default=str(PROJECT_DIR), help="the Godot project directory")
    launch.add_argument("--scene", default=RUNTIME_SCENE, help=argparse.SUPPRESS)
    launch.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"the endpoint port (default {DEFAULT_PORT})")
    display = launch.add_mutually_exclusive_group()
    display.add_argument("--headless", action="store_true", help="no window (tools that need one refuse)")
    display.add_argument("--windowed", action="store_true", help="force a window over the project's fullscreen setting")
    launch.add_argument("--resolution", default=None, help="WxH for the window")
    launch.add_argument("--rendering-method", default=None, help="Godot rendering method (gl_compatibility, forward_plus)")
    launch.add_argument("--log-file", default=None, help="Godot --log-file; game_logs follows it")
    launch.add_argument("--stdout-log", default=None, help="where the child's stdout/stderr go (default: the temp dir)")
    launch.add_argument("--pid-file", default=None, help="write the game's PID here (stop --pid-file reads it)")
    launch.add_argument("--timeout", type=float, default=240.0, help="seconds to wait for the endpoint")
    launch.add_argument("--loose", action="store_true", help="/d: loose files beside the archives override them")
    launch.add_argument("--loose-root", action="store_true", help="--loose-root: mount an archive-less directory as loose files")
    launch.add_argument("--resource-dir", default=None, help="game data directory (default: OPENNOVA_JO_DIR)")
    launch.add_argument("--exp", default=None, help="/exp <name>: mount an expansion")
    launch.add_argument("--game", default=None, help="/game <code>: jo, jodemo, ...")
    launch.add_argument("--loose-mission", default=None, help="boot this loose .bms from --resource-dir")
    launch.add_argument("--mission", default=None, help="boot straight into this single-player mission")
    launch.add_argument("--lan-host", default=None, metavar="MISSION", help="host this mission as a LAN listen host")
    launch.add_argument("--lan-join", default=None, metavar="IP[:PORT]", help="join that LAN host")
    launch.add_argument("--lan-port", default=None, help="host bind port")
    launch.add_argument("--callsign", default=None, help="the local player's callsign")
    launch.add_argument("--lan-gametype", default=None, help="numeric g_GameType override (0x ok)")
    launch.add_argument("--lan-mode", default=None, help="host LAN rate mode 1..4")
    launch.add_argument("--lan-max-players", default=None, help="listen-host capacity 1..64")
    launch.add_argument("--integrity-profile", default=None, help="the session's integrity profile")
    launch.add_argument("--capture-pcap", default=None, help="record the session's datagrams to this pcap")
    launch.add_argument("game_args", nargs="*", help="further game flags, after --")
    launch.set_defaults(func=cmd_launch)

    wait = commands.add_parser("wait", help="wait until an endpoint answers")
    add_endpoint_options(wait)
    wait.add_argument("--timeout", type=float, default=240.0)
    wait.set_defaults(func=cmd_wait)

    tools = commands.add_parser("tools", help="list the tools")
    add_endpoint_options(tools)
    tools.add_argument("--json", action="store_true")
    tools.set_defaults(func=cmd_tools)

    call = commands.add_parser("call", help="call one tool with JSON arguments")
    add_endpoint_options(call)
    call.add_argument("tool")
    call.add_argument("arguments", nargs="?", default=None, help="a JSON object")
    call.add_argument("--args-file", default=None, help="read the JSON object from this file")
    call.add_argument("--json", action="store_true", help="print the raw result payload")
    call.add_argument("--out", default=None, help="save the first image block here")
    call.add_argument("--timeout", type=float, default=120.0)
    call.set_defaults(func=cmd_call)

    state = commands.add_parser("state", help="print game_state")
    add_endpoint_options(state)
    state.set_defaults(func=cmd_state)

    logs = commands.add_parser("logs", help="print game_logs entries")
    add_endpoint_options(logs)
    logs.add_argument("--cursor", type=int, default=0)
    logs.add_argument("--limit", type=int, default=200)
    logs.add_argument("--sources", default="", help="comma list: server,script,engine,probe")
    logs.add_argument("--follow", action="store_true")
    logs.add_argument("--interval", type=float, default=1.0)
    logs.set_defaults(func=cmd_logs)

    screenshot = commands.add_parser("screenshot", help="save a game_screenshot")
    add_endpoint_options(screenshot)
    screenshot.add_argument("--out", required=True)
    screenshot.add_argument("--format", choices=("webp", "png"), default="png")
    screenshot.add_argument("--max-dim", type=int, default=1280)
    screenshot.add_argument("--quality", type=float, default=None)
    screenshot.set_defaults(func=cmd_screenshot)

    entities = commands.add_parser("entities", help="dump game_entities, once or as a watched JSONL stream")
    add_endpoint_options(entities)
    entities.add_argument("--watch", action="store_true", help="poll until Ctrl+C or --duration")
    entities.add_argument("--interval", type=float, default=1.0)
    entities.add_argument("--duration", type=float, default=None, help="seconds to watch")
    entities.add_argument("--out", default=None, help="append JSON lines here instead of stdout")
    entities.set_defaults(func=cmd_entities)

    probe = commands.add_parser("probe", help="drive the game_probe tool")
    add_endpoint_options(probe)
    ops = probe.add_subparsers(dest="probe_op", required=True)
    probe_list = ops.add_parser("list")
    probe_list.add_argument("--json", action="store_true")
    probe_run = ops.add_parser("run")
    probe_run.add_argument("name")
    probe_run.add_argument("arguments", nargs="?", default=None, help="a JSON object")
    probe_run.add_argument("--args-file", default=None)
    probe_run.add_argument("--wait", action="store_true", help="stream lines until the probe ends")
    probe_run.add_argument("--poll-ms", type=int, default=2000)
    probe_run.add_argument("--timeout", type=float, default=900.0)
    probe_status = ops.add_parser("status")
    probe_status.add_argument("run_id", nargs="?", default=None)
    probe_status.add_argument("--cursor", type=int, default=0)
    probe_status.add_argument("--wait-ms", type=int, default=0)
    probe_status.add_argument("--json", action="store_true")
    probe_wait = ops.add_parser("wait")
    probe_wait.add_argument("run_id")
    probe_wait.add_argument("--poll-ms", type=int, default=2000)
    probe_wait.add_argument("--timeout", type=float, default=900.0)
    probe_cancel = ops.add_parser("cancel")
    probe_cancel.add_argument("run_id", nargs="?", default=None)
    probe.set_defaults(func=cmd_probe)

    stop = commands.add_parser("stop", help="game_control quit, then wait for the process to exit (never kills)")
    add_endpoint_options(stop)
    stop.add_argument("--pid", type=int, default=None)
    stop.add_argument("--pid-file", default=None)
    stop.add_argument("--timeout", type=float, default=20.0)
    stop.set_defaults(func=cmd_stop)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except GameMcpError as error:
        print(f"game_mcp: {error}", file=sys.stderr)
        return error.code
    except KeyboardInterrupt:
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
