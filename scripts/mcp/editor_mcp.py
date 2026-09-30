#!/usr/bin/env python3
"""The opennova-editor MCP client (docs/mcp.md).

Launches the OpenNova Editor with `--mcp-port`, speaks JSON-RPC to its
loopback Streamable-HTTP endpoint, and wraps its tools as subcommands that
print JSON. Standard library only; the transport class is game_mcp.py's, so a
`play start` here hands its `mcp_port` straight to `game_mcp.py --port`.

    python scripts/mcp/editor_mcp.py launch --headless --open "C:/mods/My Game" --pid-file build/editor.pid
    python scripts/mcp/editor_mcp.py state --sections documents,problem_counts  # the view by section
    python scripts/mcp/editor_mcp.py state --since 42                         # only what moved since view_revision 42
    python scripts/mcp/editor_mcp.py query catalog                  # every request and query, with its params
    python scripts/mcp/editor_mcp.py query files --limit 5          # a page of the files; next_offset the next page
    python scripts/mcp/editor_mcp.py query files --limit 5 --offset 5
    python scripts/mcp/editor_mcp.py query problems --arg severities='["error"]' --arg group=kind
    python scripts/mcp/editor_mcp.py query record --path main.mnu --arg symbol=MAIN
    python scripts/mcp/editor_mcp.py query references --path main.mnu
    python scripts/mcp/editor_mcp.py query output --cursor 0 --limit 50
    python scripts/mcp/editor_mcp.py request new_project --dir "C:/mods/My Game" --title "My Game"
    python scripts/mcp/editor_mcp.py request create_missing --roles main_menu,gametext
    python scripts/mcp/editor_mcp.py request open_document --path main.mnu
    python scripts/mcp/editor_mcp.py request edit_record --path main.mnu --edits '[{"op": "add", "kind": "window",
        "parent": 3, "as": "w"}, {"op": "set", "id": "w", "field": "name", "value": "HELLO"}]'   # one undo step
    python scripts/mcp/editor_mcp.py request apply_project_settings --settings '{"game_install": "C:/Games/JO"}'
    python scripts/mcp/editor_mcp.py build                # waits on the build's operation, its progress on stderr
    python scripts/mcp/editor_mcp.py play start           # the run section: state, pid, mcp_port
    python scripts/mcp/game_mcp.py call game_menu '{"op": "state"}' --port <that port>
    python scripts/mcp/editor_mcp.py call editor_menu_preview '{"op": "state"}'
    python scripts/mcp/editor_mcp.py stop --pid-file build/editor.pid

Exit codes: 0 ok; 1 usage or transport failure; 2 the tool reported isError, a
`request` was not done (its outcome: refused, did not finish, or waits on the
unsaved-changes prompt), or a build or play did not land; 3 a JSON-RPC error;
6 stop: the process outlived the quit; 7 launch failed.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import game_mcp  # noqa: E402  (the shared transport and process helpers)
from game_mcp import (  # noqa: E402
    EXIT_LAUNCH_FAILED, EXIT_OK, EXIT_STOP_SURVIVED, EXIT_TOOL_ERROR, EXIT_USAGE, GameMcp, GameMcpError,
    emit_payload, find_godot, images_of, parse_json_arg, pid_alive, port_open, tail, text_of,
)

# 8975 is the game, 8976 a LAN joiner (docs/mcp.md); the editor takes the next one.
DEFAULT_PORT = 8977
EDITOR_SCENE = "res://editor/editor_root.tscn"
PROJECT_DIR = game_mcp.PROJECT_DIR


# --- subcommands --------------------------------------------------------------


def cmd_launch(args: argparse.Namespace) -> int:
    port = args.port
    if port_open(port):
        raise GameMcpError(
            EXIT_LAUNCH_FAILED,
            f"port {port} already answers; stop that editor first or pass another --port")
    if args.editor:
        executable = Path(args.editor).resolve()
        if not executable.is_file():
            raise GameMcpError(EXIT_LAUNCH_FAILED, f"editor executable not found: {executable}")
        argv = [str(executable)]
        cwd = executable.parent
    else:
        godot = find_godot(args.godot)
        project = Path(args.project_dir).resolve()
        if not (project / "project.godot").is_file():
            raise GameMcpError(EXIT_LAUNCH_FAILED, f"no project.godot under {project}")
        argv = [str(godot), "--path", str(project)]
        cwd = project
    if args.headless:
        argv.append("--headless")
    elif args.windowed:
        argv.append("--windowed")
    if args.resolution:
        argv += ["--resolution", args.resolution]
    log_file = Path(args.log_file).resolve() if args.log_file else None
    if log_file:
        argv += ["--log-file", str(log_file)]
    if not args.editor:
        argv.append(EDITOR_SCENE)
    argv.append("--")
    if args.open:
        argv += ["--project", str(Path(args.open).resolve())]
    argv += ["--mcp-port", str(port)]

    stdout_log = Path(args.stdout_log) if args.stdout_log else \
        Path(tempfile.gettempdir()) / f"opennova-editor-mcp-{port}.log"
    stdout_log.parent.mkdir(parents=True, exist_ok=True)
    popen_kwargs = {}
    if os.name == "nt":
        popen_kwargs["creationflags"] = (
            subprocess.CREATE_NEW_PROCESS_GROUP | getattr(subprocess, "DETACHED_PROCESS", 0x00000008))
    else:
        popen_kwargs["start_new_session"] = True
    with open(stdout_log, "wb") as sink:
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT,
                                 cwd=str(cwd), **popen_kwargs)
    if args.pid_file:
        Path(args.pid_file).write_text(str(child.pid), encoding="utf-8")

    deadline = time.monotonic() + args.timeout
    client = GameMcp.for_port(port)
    while time.monotonic() < deadline:
        exit_code = child.poll()
        if exit_code is not None:
            raise GameMcpError(
                EXIT_LAUNCH_FAILED,
                f"the editor exited with code {exit_code} before its endpoint answered\n"
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


def cmd_call(args: argparse.Namespace) -> int:
    payload = client_of(args).call(args.tool, parse_json_arg(args.arguments, args.args_file),
                                   timeout=args.timeout)
    emit_payload(payload, args)
    return EXIT_TOOL_ERROR if payload.get("isError") else EXIT_OK


def print_json(value) -> None:
    print(json.dumps(value, indent=2))


def cmd_state(args: argparse.Namespace) -> int:
    request: dict = {}
    if args.sections:
        request["sections"] = [name.strip() for name in args.sections.split(",") if name.strip()]
    if args.since is not None:
        request["since"] = args.since
    print_json(client_of(args).structured("editor_state", request))
    return EXIT_OK


def parse_value(text: str):
    """A --arg value: JSON when it reads as JSON (a number, true, a list), else the text itself."""
    try:
        return json.loads(text)
    except ValueError:
        return text


def cmd_query(args: argparse.Namespace) -> int:
    params = parse_json_arg(args.params, args.params_file) if (args.params or args.params_file) else {}
    for name in ("offset", "limit", "cursor", "path", "id", "field", "text"):
        if getattr(args, name) is not None:
            params[name] = getattr(args, name)
    for pair in args.arg or []:
        name, sep, value = pair.partition("=")
        if not sep or not name:
            raise GameMcpError(EXIT_USAGE, f"--arg takes NAME=VALUE, not {pair!r}")
        params[name] = parse_value(value)
    payload = client_of(args).call("editor_query", {"query": args.name, **params}, timeout=args.timeout)
    if payload.get("isError"):
        print(text_of(payload), file=sys.stderr)
        return EXIT_TOOL_ERROR
    print_json(payload.get("structuredContent", {}))
    return EXIT_OK


def parse_list(text: str, flag: str, shape: str) -> list:
    """--edits / --imports: a JSON array of objects."""
    try:
        value = json.loads(text)
    except ValueError as error:
        raise GameMcpError(EXIT_USAGE, f"{flag} is not JSON: {error}") from error
    if not isinstance(value, list) or not all(isinstance(item, dict) for item in value):
        raise GameMcpError(EXIT_USAGE, f"{flag} must be a JSON array of {shape} objects")
    return value


# The request's fields (engine/editor/session/request_fields.cpp), one flag each: the text fields,
# the lists (comma-separated), the objects (JSON) and the switches. The kind's row says which it
# takes; the editor refuses the rest, naming what the kind takes (`query catalog` lists them).
REQUEST_TEXTS = ("dir", "title", "game", "path", "locator", "field", "new_name", "role", "file_kind", "out_dir",
                 "mode", "choice", "purpose")
REQUEST_LISTS = ("roles", "names")
REQUEST_SWITCHES = ("with_dependencies", "replace", "force", "ask_name", "open_first", "import_pass")


def request_of(args: argparse.Namespace) -> dict:
    request: dict = {"kind": args.kind}
    for field in REQUEST_TEXTS:
        if getattr(args, field) is not None:
            request[field] = getattr(args, field)
    for field in REQUEST_LISTS:
        if getattr(args, field):
            request[field] = [name.strip() for name in getattr(args, field).split(",") if name.strip()]
    for field in REQUEST_SWITCHES:
        if getattr(args, field) is not None:
            request[field] = getattr(args, field) == "true"
    if args.paths:
        request["paths"] = args.paths
    if args.imports:
        request["imports"] = parse_list(args.imports, "--imports", "{path, entry?, install?, native?}")
    if args.edits:
        request["edits"] = parse_list(args.edits, "--edits", "edit")
    for field in ("address", "paste_at", "settings"):
        if getattr(args, field):
            request[field] = parse_json_arg(getattr(args, field), None)
    return request


def cmd_request(args: argparse.Namespace) -> int:
    payload = client_of(args).call("editor_request", request_of(args), timeout=args.timeout)
    if payload.get("isError"):
        print(text_of(payload), file=sys.stderr)
        return EXIT_TOOL_ERROR
    answer = payload.get("structuredContent", {})
    print_json(answer)
    # `ok` only says the request read; the outcome says whether it happened.
    outcome = answer.get("outcome", {})
    return EXIT_OK if not outcome or outcome.get("done", False) else EXIT_TOOL_ERROR


def progress_line(operation: dict) -> str:
    """The running operation as the editor's menu bar says it, with what it works on."""
    done, total = operation.get("done", 0), operation.get("total", 0)
    amount = (f"{done * 100 // total}%" if operation.get("unit") == "bytes" else f"{done}/{total}") if total else ""
    return " ".join(part for part in (operation.get("kind", ""), amount, operation.get("label", "")) if part)


def raise_and_wait(client: GameMcp, kind: str, timeout: float) -> tuple[dict, dict]:
    """Raise `kind` (build, play) and wait on the operation its outcome names, polling `query
    operation` while the editor steps it frame by frame (its progress on stderr): the outcome, and
    what the operation came to (last_operation; {} when it was not the one to end, or none ran)."""
    payload = client.call("editor_request", {"kind": kind}, timeout=60)
    if payload.get("isError"):
        raise GameMcpError(EXIT_TOOL_ERROR, f"editor_request {kind} failed: {text_of(payload)}")
    outcome = payload.get("structuredContent", {}).get("outcome", {})
    if not outcome.get("done", False):
        return outcome, {}
    operation = outcome.get("operation", 0)
    deadline = time.monotonic() + timeout
    shown = ""
    while True:
        state = client.structured("editor_query", {"query": "operation"})
        running = state.get("operation") or {}
        if not operation or not running.get("running") or running.get("id") != operation:
            last = state.get("last_operation") or {}
            return outcome, last if last.get("id") == operation else {}
        line = progress_line(running)
        if line != shown:
            print(line, file=sys.stderr)
            shown = line
        if time.monotonic() >= deadline:
            raise GameMcpError(EXIT_TOOL_ERROR, f"the {kind}'s operation {operation} did not end within {timeout:.0f} s "
                                                f"(`request cancel_operation` stops it)")
        time.sleep(0.2)


def cmd_build(args: argparse.Namespace) -> int:
    client = client_of(args)
    outcome, ended = raise_and_wait(client, "build", args.timeout)
    if not outcome.get("done", False):
        print_json({"outcome": outcome})
        return EXIT_TOOL_ERROR
    build = client.structured("editor_query", {"query": "operation"}).get("build", {})
    build["operation"] = ended
    print_json(build)
    return EXIT_OK if ended.get("end") == "done" and build.get("ok") else EXIT_TOOL_ERROR


def run_section(client: GameMcp) -> dict:
    return client.structured("editor_state", {"sections": ["run"]}).get("run", {})


def cmd_play(args: argparse.Namespace) -> int:
    client = client_of(args)
    if args.op == "start":
        # Play builds first: its build's operation is waited on as `build` waits, then the run
        # section read (the game started on the poll the build landed).
        outcome, ended = raise_and_wait(client, "play", args.timeout)
        if not outcome.get("done", False):
            print_json({"outcome": outcome})
            return EXIT_TOOL_ERROR
        run = run_section(client)
        run["operation"] = ended
        print_json(run)
        return EXIT_OK if run.get("state") == "running" else EXIT_TOOL_ERROR
    payload = client.call("editor_play", {"op": args.op}, timeout=args.timeout)
    if payload.get("isError"):
        print(text_of(payload), file=sys.stderr)
        return EXIT_TOOL_ERROR
    print_json(payload.get("structuredContent", {}))
    return EXIT_OK


def cmd_screenshot(args: argparse.Namespace) -> int:
    request = {"format": args.format, "max_dim": args.max_dim}
    if args.quality is not None:
        request["quality"] = args.quality
    payload = client_of(args).call("editor_screenshot", request, timeout=90)
    if payload.get("isError"):
        raise GameMcpError(EXIT_TOOL_ERROR, f"editor_screenshot failed: {text_of(payload)}")
    images = images_of(payload)
    if not images:
        raise GameMcpError(EXIT_TOOL_ERROR, "editor_screenshot returned no image block")
    target = Path(args.out)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(base64.b64decode(images[0]["data"]))
    print_json({"out": str(target), "mime": images[0].get("mimeType")})
    return EXIT_OK


def cmd_logs(args: argparse.Namespace) -> int:
    request = {"limit": args.limit}
    if args.cursor > 0:
        request["cursor"] = args.cursor
    print_json(client_of(args).structured("editor_logs", request))
    return EXIT_OK


def cmd_stop(args: argparse.Namespace) -> int:
    port = int(args.port)
    pid = args.pid or 0
    if args.pid_file and not pid:
        try:
            pid = int(Path(args.pid_file).read_text(encoding="utf-8").strip() or "0")
        except (OSError, ValueError):
            pid = 0
    client = client_of(args)
    if port_open(port):
        try:
            client.call("editor_play", {"op": "stop"}, timeout=30)
        except GameMcpError:
            pass  # no game running, or the editor is already leaving
        try:
            client.call("editor_request", {"kind": "quit"}, timeout=10)
        except GameMcpError:
            pass  # the editor may drop the connection while quitting
    else:
        print(f"no endpoint on port {port}; waiting for the process only")
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        alive = pid_alive(pid) if pid else port_open(port)
        if not alive:
            print("stopped" + (f" pid={pid}" if pid else f" port={port}"))
            if args.pid_file:
                try:
                    Path(args.pid_file).unlink()
                except OSError:
                    pass
            return EXIT_OK
        time.sleep(0.25)
    raise GameMcpError(
        EXIT_STOP_SURVIVED,
        f"the editor did not exit within {args.timeout:.0f} s after editor_request quit"
        + (f" (pid {pid} is still alive; this client never kills)" if pid else ""))


# --- argument parsing ---------------------------------------------------------


def client_of(args: argparse.Namespace) -> GameMcp:
    if getattr(args, "url", None):
        return GameMcp(args.url)
    return GameMcp.for_port(int(args.port))


def add_endpoint_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"the editor's --mcp-port (default {DEFAULT_PORT})")
    parser.add_argument("--url", default=None, help="the full endpoint URL instead of --port")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    launch = commands.add_parser("launch", help="start the editor with --mcp-port and wait for its endpoint")
    launch.add_argument("--editor", default=None,
                        help="a packaged opennova-editor.exe (default: the Godot binary at the source project)")
    launch.add_argument("--godot", default=None, help="Godot 4.6.1 binary (default: GODOT_BIN, else .godot-bin/)")
    launch.add_argument("--project-dir", default=str(PROJECT_DIR), help="the Godot project directory (source runs)")
    launch.add_argument("--open", default=None, metavar="DIR", help="open this OpenNova project at boot (--project)")
    launch.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"the endpoint port (default {DEFAULT_PORT})")
    display = launch.add_mutually_exclusive_group()
    display.add_argument("--headless", action="store_true", help="no window (the screenshot refuses)")
    display.add_argument("--windowed", action="store_true", help="force a window")
    launch.add_argument("--resolution", default=None, help="WxH for the window")
    launch.add_argument("--log-file", default=None, help="Godot --log-file for the editor process")
    launch.add_argument("--stdout-log", default=None, help="where the child's stdout/stderr go (default: the temp dir)")
    launch.add_argument("--pid-file", default=None, help="write the editor's PID here (stop --pid-file reads it)")
    launch.add_argument("--timeout", type=float, default=240.0, help="seconds to wait for the endpoint")
    launch.set_defaults(func=cmd_launch)

    call = commands.add_parser("call", help="call one tool with JSON arguments")
    add_endpoint_options(call)
    call.add_argument("tool")
    call.add_argument("arguments", nargs="?", default=None, help="a JSON object")
    call.add_argument("--args-file", default=None, help="read the JSON object from this file")
    call.add_argument("--json", action="store_true", help="print the raw result payload")
    call.add_argument("--out", default=None, help="save the first image block here")
    call.add_argument("--timeout", type=float, default=300.0)
    call.set_defaults(func=cmd_call)

    state = commands.add_parser("state", help="editor_state: the view by section, as JSON")
    add_endpoint_options(state)
    state.add_argument("--sections", default=None,
                       help="comma-separated: the sections to read (query catalog lists them; every one by default)")
    state.add_argument("--since", type=int, default=None,
                       help="a view_revision an earlier answer carried: the sections that have not moved since are "
                            "left out (0 or left out: every section)")
    state.set_defaults(func=cmd_state)

    query = commands.add_parser("query", help="editor_query: one query by name, its answer as JSON")
    add_endpoint_options(query)
    query.add_argument("name", help="files, document, record, problems, references, output, operation, catalog, ... "
                                    "(`query catalog` lists every query with its params)")
    query.add_argument("params", nargs="?", default=None, help="the params as a JSON object")
    query.add_argument("--params-file", default=None, help="read the params' JSON object from this file")
    query.add_argument("--offset", type=int, default=None, help="a page's first entry")
    query.add_argument("--limit", type=int, default=None, help="how many entries a page holds, 1 to 200")
    query.add_argument("--cursor", type=int, default=None, help="the output's or the events' page from this cursor")
    query.add_argument("--path", default=None, help="a document, a file or a menu, as the query takes it")
    query.add_argument("--id", type=int, default=None, help="a record's identity")
    query.add_argument("--field", default=None, help="a record's field")
    query.add_argument("--text", default=None, help="what a search finds")
    query.add_argument("--arg", action="append", default=None, metavar="NAME=VALUE",
                       help="any other param (VALUE read as JSON when it is, else as text); repeat for more")
    query.add_argument("--timeout", type=float, default=120.0)
    query.set_defaults(func=cmd_query)

    request = commands.add_parser("request", help="editor_request: one request by kind, its answer as JSON")
    add_endpoint_options(request)
    request.add_argument("kind", help="new_project, open_project, create_missing, edit_record, save_all, quit, ... "
                                      "(`query catalog` lists every kind with its fields)")
    request.add_argument("--dir", default=None, help="a project's directory (new_project, open_project, forget_recent)")
    request.add_argument("--title", default=None, help="a new project's title")
    request.add_argument("--game", default=None, help="a new project's game, a gameprofile code (jo when left out)")
    request.add_argument("--path", default=None, help="a file: a project file or open document ('' the active one)")
    request.add_argument("--locator", default=None, help="a record's locator (open_document, the renames)")
    request.add_argument("--field", default=None, help="a field of that record")
    request.add_argument("--new-name", dest="new_name", default=None, help="the name a rename gives")
    request.add_argument("--role", default=None, help="a requirement's role (assign_requirement)")
    request.add_argument("--file-kind", dest="file_kind", default=None,
                         help="create_file: an asset kind token, for a name that cannot say its kind")
    request.add_argument("--out-dir", dest="out_dir", default=None,
                         help="build: where it lands, each build a directory under it (left out: the project's "
                              ".opennova/build/play)")
    request.add_argument("--roles", default=None, help="comma-separated: create_missing's requirement roles")
    request.add_argument("--names", default=None, help="comma-separated: preview_install_import's files")
    request.add_argument("--paths", action="append", default=None,
                         help="a file picked for preview_import (repeat for more)")
    request.add_argument("--imports", default=None,
                         help="plan_import's files chosen, import_files' rows kept: a JSON array of "
                              "{path, entry?, install?, native?} (query import_preview's rows carry each as source)")
    request.add_argument("--edits", default=None,
                         help="edit_record: the batch form as a JSON array of {op, id, parent, kind, field, value, "
                              "position, as, ...}, one undo step; revert_to_saved: [{id, field}]")
    request.add_argument("--address", default=None, help="a record's address as a JSON object {row, kind, child}")
    request.add_argument("--paste-at", dest="paste_at", default=None,
                         help="paste: where, as a JSON object {row, parent, position} (left out: after the selection)")
    request.add_argument("--mode", choices=("replace", "add", "toggle"), default=None,
                         help="how select_record joins the selection")
    request.add_argument("--choice", choices=("save", "discard", "cancel"), default=None,
                         help="resolve_unsaved: save writes the files the prompt lists, then what waited runs")
    request.add_argument("--purpose", default=None, help="the pickers' purpose (refused over MCP: pass paths)")
    request.add_argument("--settings", default=None,
                         help="apply_project_settings: the settings to set as a JSON object (title, mission, "
                              "multiplayer, game_install, runtime_executable, play_in_install; one left out stays)")
    switch = ("true", "false")
    request.add_argument("--with-dependencies", dest="with_dependencies", choices=switch, default=None,
                         help="preview_import, plan_import, preview_install_import: with the files they need; "
                              "set_import_dependencies: the setting")
    request.add_argument("--replace", choices=switch, default=None,
                         help="import_files: replace the project's files of the names")
    request.add_argument("--force", choices=switch, default=None, help="reimport: import again even when unchanged")
    request.add_argument("--ask-name", dest="ask_name", choices=switch, default=None,
                         help="show_in_files, preview_rename: and ask the new name")
    request.add_argument("--open-first", dest="open_first", choices=switch, default=None,
                         help="edit_record: open the document first when it is not")
    request.add_argument("--import-pass", dest="import_pass", choices=switch, default=None,
                         help="open_project: false opens it on its files as they are, nothing written")
    request.add_argument("--timeout", type=float, default=300.0)
    request.set_defaults(func=cmd_request)

    build = commands.add_parser("build", help="build the project and wait on its operation (progress on stderr)")
    add_endpoint_options(build)
    build.add_argument("--timeout", type=float, default=300.0)
    build.set_defaults(func=cmd_build)

    play = commands.add_parser("play", help="start (build, waiting on its operation, then run), stop or read the game")
    add_endpoint_options(play)
    play.add_argument("op", choices=("start", "stop", "state"))
    play.add_argument("--timeout", type=float, default=300.0)
    play.set_defaults(func=cmd_play)

    screenshot = commands.add_parser("screenshot", help="save an editor_screenshot")
    add_endpoint_options(screenshot)
    screenshot.add_argument("--out", required=True)
    screenshot.add_argument("--format", choices=("webp", "png"), default="png")
    screenshot.add_argument("--max-dim", type=int, default=1280)
    screenshot.add_argument("--quality", type=float, default=None)
    screenshot.set_defaults(func=cmd_screenshot)

    logs = commands.add_parser("logs", help="editor_logs: a page of the transport's own log, as JSON")
    add_endpoint_options(logs)
    logs.add_argument("--cursor", type=int, default=0)
    logs.add_argument("--limit", type=int, default=200)
    logs.set_defaults(func=cmd_logs)

    stop = commands.add_parser("stop", help="stop the game, quit the editor, wait for the process")
    add_endpoint_options(stop)
    stop.add_argument("--pid", type=int, default=None)
    stop.add_argument("--pid-file", default=None)
    stop.add_argument("--timeout", type=float, default=30.0)
    stop.set_defaults(func=cmd_stop)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except GameMcpError as error:
        print(f"editor_mcp: {error}", file=sys.stderr)
        return error.code
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
