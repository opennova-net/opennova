#!/usr/bin/env python3
"""The opennova-editor MCP client (docs/mcp.md).

Launches the OpenNova Editor with `--mcp-port`, speaks JSON-RPC to its
loopback Streamable-HTTP endpoint, and wraps the everyday tools as
subcommands. Standard library only; the transport class is game_mcp.py's,
so a `play start` here hands its `mcp_port` straight to `game_mcp.py --port`.

    python scripts/mcp/editor_mcp.py launch --headless --open "C:/mods/My Game" --pid-file build/editor.pid
    python scripts/mcp/editor_mcp.py state
    python scripts/mcp/editor_mcp.py request new_project --path "C:/mods/My Game" --text "My Game"
    python scripts/mcp/editor_mcp.py request create_missing --names main_menu,gametext
    python scripts/mcp/editor_mcp.py request apply_project_settings --settings '{"retail_directory": "C:/Games/JO"}'
    python scripts/mcp/editor_mcp.py request preview_import --paths "C:/art/main.mnu" --flag true  # with what it needs
    python scripts/mcp/editor_mcp.py request import_files --imports '[{"path": "C:/art/main.mnu"},
        {"path": "C:/art/arial.fnt", "native": true}]'   # the rows kept (state's import rows' sources)
    python scripts/mcp/editor_mcp.py call editor_document '{"op": "open", "path": "main.mnu"}'
    python scripts/mcp/editor_mcp.py problems --severity error --group kind --fixable
    python scripts/mcp/editor_mcp.py build
    python scripts/mcp/editor_mcp.py references main.mnu  # what it names, who names it
    python scripts/mcp/editor_mcp.py rename logo.tga logo2.tga
    python scripts/mcp/editor_mcp.py menu-preview              # the previewed screen's widgets
    python scripts/mcp/editor_mcp.py menu-preview --hit 400,200 # the widget the game's hit test finds
    python scripts/mcp/editor_mcp.py menu-preview --notes      # the frame compiler's notes on it
    python scripts/mcp/editor_mcp.py menu-preview --render main.mnu --screen 12  # the render check's
    python scripts/mcp/editor_mcp.py menu-preview --drag 7 --handle right --by 16,0  # resize window 7
    python scripts/mcp/editor_mcp.py menu-preview --nudge 7 --by 0,-1                # move it up a unit
    python scripts/mcp/editor_mcp.py menu-preview --arrange align_left --ids 7,9,12   # 9 and 12 to 7's left edge
    python scripts/mcp/editor_mcp.py model-preview --lod auto --frame   # the open model as the game draws it
    python scripts/mcp/editor_mcp.py model-preview --ctrl HEAT=5 --yaw 1.2  # a register held, the camera turned
    python scripts/mcp/editor_mcp.py menu tree --path main.mnu   # screens and windows, with rects
    python scripts/mcp/editor_mcp.py menu edit --edits '[{"op": "add", "kind": "window", "parent": 2, "as": "w"},
        {"op": "set", "id": "w", "field": "name", "value": "HELLO"}]'   # one undo step
    python scripts/mcp/editor_mcp.py menu list --id 11 --list action --records '[{"type": "POP_SCREEN"}]'
    python scripts/mcp/editor_mcp.py menu analyze --path main.mnu   # its Problems rows by source
    python scripts/mcp/editor_mcp.py play start          # prints the game's mcp_port
    python scripts/mcp/game_mcp.py call game_menu '{"op": "state"}' --port <that port>
    python scripts/mcp/editor_mcp.py stop --pid-file build/editor.pid

Exit codes: 0 ok; 1 usage or transport failure; 2 the tool reported
isError, or a `request` was not done (its outcome: refused, did not finish, or
waits on the unsaved-changes prompt); 3 a JSON-RPC error; 6 stop: the process
outlived the quit; 7 launch failed.
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


def cmd_tools(args: argparse.Namespace) -> int:
    tools = client_of(args).tools()
    if args.json:
        print(json.dumps(tools, indent=2))
        return EXIT_OK
    for tool in tools:
        description = str(tool.get("description", "")).split(". ")[0]
        print(f"{tool['name']:<20} {description}")
    return EXIT_OK


def cmd_call(args: argparse.Namespace) -> int:
    payload = client_of(args).call(args.tool, parse_json_arg(args.arguments, args.args_file),
                                   timeout=args.timeout)
    emit_payload(payload, args)
    return EXIT_TOOL_ERROR if payload.get("isError") else EXIT_OK


def cmd_state(args: argparse.Namespace) -> int:
    request = {"output_cursor": args.output_cursor, "output_limit": args.output_limit,
               "import_offset": args.import_offset, "import_limit": args.import_limit}
    print(json.dumps(client_of(args).structured("editor_state", request), indent=2))
    return EXIT_OK


def parse_edits(text: str) -> list:
    """--edits: a JSON array of edit objects (one batch on one row)."""
    try:
        value = json.loads(text)
    except ValueError as error:
        raise GameMcpError(EXIT_USAGE, f"--edits is not JSON: {error}") from error
    if not isinstance(value, list) or not all(isinstance(edit, dict) for edit in value):
        raise GameMcpError(EXIT_USAGE, "--edits must be a JSON array of edit objects")
    return value


def parse_imports(text: str) -> list:
    """--imports: a JSON array of import sources ({path, entry?, retail?, native?}), as
    editor_state's import rows carry them in `source`."""
    try:
        value = json.loads(text)
    except ValueError as error:
        raise GameMcpError(EXIT_USAGE, f"--imports is not JSON: {error}") from error
    if not isinstance(value, list) or not all(isinstance(source, dict) for source in value):
        raise GameMcpError(EXIT_USAGE, "--imports must be a JSON array of {path, entry?, retail?, native?} objects")
    return value


def cmd_request(args: argparse.Namespace) -> int:
    request: dict = {"kind": args.kind}
    if args.path is not None:
        request["path"] = args.path
    if args.text is not None:
        request["text"] = args.text
    if args.flag is not None:
        request["flag"] = args.flag == "true"
    if args.paths:
        request["paths"] = args.paths
    if args.names:
        request["names"] = [name.strip() for name in args.names.split(",") if name.strip()]
    if args.imports:
        request["imports"] = parse_imports(args.imports)
    if args.edit:
        request["edit"] = parse_json_arg(args.edit, None)
    if args.edits:
        request["edits"] = parse_edits(args.edits)
    if args.mode:
        request["mode"] = args.mode
    if args.unsaved_choice:
        request["unsaved_choice"] = args.unsaved_choice
    if args.settings:
        request["settings"] = parse_json_arg(args.settings, None)
    payload = client_of(args).call("editor_request", request, timeout=args.timeout)
    emit_payload(payload, args)
    if payload.get("isError"):
        return EXIT_TOOL_ERROR
    # `ok` only says the request parsed; the outcome says whether it happened.
    outcome = payload.get("structuredContent", {}).get("outcome", {})
    if outcome and not outcome.get("done", False):
        if outcome.get("unsaved_prompt"):
            print("not done: it waits on unsaved changes (`state` names the files); answer with "
                  "`request resolve_unsaved --unsaved-choice save|discard|cancel` (build and play take "
                  "save or cancel)", file=sys.stderr)
        for finding in outcome.get("findings", []):
            print(f"not done: {finding.get('severity', '')} {finding.get('code', '')}: {finding.get('message', '')}",
                  file=sys.stderr)
        return EXIT_TOOL_ERROR
    return EXIT_OK


def cmd_problems(args: argparse.Namespace) -> int:
    query: dict = {"offset": args.offset, "limit": args.limit}
    if args.severity:
        query["severities"] = args.severity
    if args.text:
        query["text"] = args.text
    if args.scope:
        query["scope"] = args.scope
    if args.fixable:
        query["fixable"] = True
    if args.group:
        query["group"] = args.group
    page = client_of(args).structured("editor_problems", query)
    # The page's groups by key (the transport caps a long list with a marker string: skip it).
    groups = {entry.get("key"): entry for entry in page.get("groups", []) if isinstance(entry, dict)}
    group = None
    for problem in page.get("problems", []):
        if not isinstance(problem, dict):
            continue
        if problem.get("group") is not None and problem["group"] != group:
            group = problem["group"]
            entry = groups.get(group, {})
            print(f"== {entry.get('title', group)} ({entry.get('count', '?')})")
        where = problem.get("asset", "") or problem.get("target", "")
        if problem.get("field"):
            where += f" [{problem['field']}]"
        print(f"{problem.get('severity', ''):<8} {problem.get('code', ''):<32} {where:<40} {problem.get('message', '')}")
        for fix in problem.get("fixes", []):
            if isinstance(fix, dict):
                print(f"{'':<8} fix: {fix.get('label', '')}  {json.dumps(fix.get('request', {}))}")
    groups_note = f" in {page['group_count']} group(s)" if "group_count" in page else ""
    print(f"{page.get('shown', 0)} of {page.get('total', 0)} problem(s){groups_note}", file=sys.stderr)
    return EXIT_OK


def graph_edges(client: GameMcp, request: dict) -> list:
    """Every edge of an editor_graph list op, page after page (the tool answers a page)."""
    edges: list = []
    while True:
        page = client.structured("editor_graph", {**request, "offset": len(edges), "limit": 200})
        edges += page.get("edges", [])
        if not page.get("edges") or len(edges) >= int(page.get("count", 0)):
            return edges


def cmd_references(args: argparse.Namespace) -> int:
    client = client_of(args)
    names = graph_edges(client, {"op": "references", "path": args.path})
    print(f"{args.path} references {len(names)} name(s):")
    for edge in names:
        where = f"{edge.get('record')}: " if edge.get("record") else ""
        print(f"  {where}{edge.get('field')} = {edge.get('value')}  [{edge.get('kind')}, {edge.get('status')}]")
    users = graph_edges(client, {"op": "referrers", "path": args.path})
    print(f"referenced by {len(users)} field(s):")
    for edge in users:
        where = f" ({edge.get('record')})" if edge.get("record") else ""
        print(f"  {edge.get('source')}{where}: {edge.get('field')}")
    return EXIT_OK


def cmd_rename(args: argparse.Namespace) -> int:
    payload = client_of(args).call("editor_graph", {"op": "rename", "path": args.path, "name": args.name},
                                   timeout=args.timeout)
    emit_payload(payload, args)
    return EXIT_TOOL_ERROR if payload.get("isError") else EXIT_OK


def print_notes(notes: list) -> None:
    for note in notes:
        where = note.get("name") or "(screen)"
        print(f"  [{note.get('severity')}/{note.get('basis')}] {where}: {note.get('message')}")


def cmd_model_preview(args: argparse.Namespace) -> int:
    client = client_of(args)
    options = {}
    if args.lod is not None:
        options["lod"] = "auto" if args.lod == "auto" else int(args.lod)
    if args.ctrl is not None:
        held = {}
        for pair in args.ctrl:
            name, _, value = pair.partition("=")
            try:
                held[name] = int(value)
            except ValueError:
                raise GameMcpError(EXIT_USAGE, "--ctrl takes NAME=VALUE (an integer)") from None
        options["ctrl"] = held
    if options:
        client.structured("editor_model_preview", {"op": "options", **options})
    camera = {key: getattr(args, key) for key in ("yaw", "pitch", "distance") if getattr(args, key) is not None}
    if args.frame:
        camera["frame"] = True
    if camera:
        client.structured("editor_model_preview", {"op": "camera", **camera})
    state = client.structured("editor_model_preview", {"op": "state"})
    if args.json:
        print(json.dumps(state, indent=2))
        return EXIT_OK
    print(f"{state.get('status')}: {state.get('message') or state.get('path', '')} (builds {state.get('builds', 0)})")
    lod = state.get("lod", {})
    if lod:
        print(f"level {lod.get('shown')} of {lod.get('count')} (auto {lod.get('auto')}, "
              f"{lod.get('projected_px', 0):.1f} px, thresholds {lod.get('thresholds')})")
    for register in state.get("registers", []):
        print(f"  ctrl {register.get('name')} = {register.get('value')}")
    for point in state.get("user_points", []):
        print(f"  user point {point.get('index')} {point.get('name', ''):<16} {point.get('position')} "
              f"-> {point.get('screen')}")
    return EXIT_OK


def cmd_menu_preview(args: argparse.Namespace) -> int:
    client = client_of(args)
    options = {}
    if args.width:
        options["width"] = args.width
    if args.height:
        options["height"] = args.height
    if args.show_hidden:
        options["show_hidden"] = True
    if args.force_id is not None:
        options["force_id"] = args.force_id
    if args.force_state:
        options["force_state"] = args.force_state
    for flag in ("checked", "popup_open", "focus"):
        if getattr(args, flag):
            options[flag] = True
    if options:
        client.structured("editor_menu_preview", {"op": "options", **options})
    if args.drag is not None or args.nudge is not None:
        try:
            dx, dy = (int(part) for part in args.by.split(","))
        except (AttributeError, ValueError):
            raise GameMcpError(EXIT_USAGE, "--drag and --nudge take --by dx,dy in design units") from None
        if args.drag is not None:
            moved = {"op": "drag", "id": args.drag, "handle": args.handle, "dx": dx, "dy": dy, "snap": not args.no_snap}
        else:
            moved = {"op": "nudge", "id": args.nudge, "dx": dx, "dy": dy}
        payload = client.call("editor_menu_preview", moved, timeout=60)
        if payload.get("isError"):
            emit_payload(payload, args)
            return EXIT_TOOL_ERROR
    if args.arrange:
        try:
            ids = [int(part) for part in (args.ids or "").split(",") if part.strip()]
        except ValueError:
            raise GameMcpError(EXIT_USAGE, "--arrange takes --ids id,id,... (window record ids)") from None
        payload = client.call("editor_menu_preview", {"op": "arrange", "ids": ids, "arrange": args.arrange}, timeout=60)
        if payload.get("isError"):
            emit_payload(payload, args)
            return EXIT_TOOL_ERROR
    if args.hit:
        try:
            x, y = (float(part) for part in args.hit.split(","))
        except ValueError:
            raise GameMcpError(EXIT_USAGE, "--hit takes x,y in 800x600 design units") from None
        hit = client.structured("editor_menu_preview", {"op": "hit", "x": x, "y": y})
        print(json.dumps(hit, indent=2) if args.json else f"index={hit.get('index')} id={hit.get('id')} name={hit.get('name')}")
        return EXIT_OK
    if args.render:
        if args.screen is None:
            raise GameMcpError(EXIT_USAGE, "--render takes --screen (the screen row id)")
        render = client.structured("editor_menu_preview", {"op": "render", "path": args.render, "screen": args.screen,
                                                      "limit": 200})
        notes = client.structured("editor_menu_preview", {"op": "notes", "path": args.render, "screen": args.screen,
                                                     "limit": 200})
        if args.json:
            print(json.dumps({"render": render, "notes": notes.get("notes", [])}, indent=2))
            return EXIT_OK
        print(f"{render.get('status')}: {render.get('path', '')} {render.get('screen', {}).get('name', '')} "
              f"({render.get('widget_count', 0)} widgets, {render.get('note_count', 0)} notes)")
        for widget in render.get("widgets", []):
            print(f"  {widget.get('index'):>3} {widget.get('name', ''):<24} {widget.get('rect')}")
        print_notes(notes.get("notes", []))
        return EXIT_OK
    if args.notes:
        notes = client.structured("editor_menu_preview", {"op": "notes", "limit": 200})
        if args.json:
            print(json.dumps(notes, indent=2))
        else:
            print(f"{notes.get('status')}: {notes.get('count', 0)} note(s)")
            print_notes(notes.get("notes", []))
        return EXIT_OK
    state = client.structured("editor_menu_preview", {"op": "state"})
    rects = client.structured("editor_menu_preview", {"op": "rects", "limit": 200}) if state.get("status") == "ready" else {}
    if args.json:
        print(json.dumps({"state": state, "widgets": rects.get("widgets", [])}, indent=2))
        return EXIT_OK
    print(f"{state.get('status')}: {state.get('message') or state.get('path', '')} {state.get('screen', {}).get('name', '')}")
    if state.get("missing"):
        print("missing: " + ", ".join(state["missing"]))
    if state.get("unreadable"):
        print("did not load: " + ", ".join(state["unreadable"]))
    for widget in rects.get("widgets", []):
        print(f"  {widget.get('index'):>3} {widget.get('name', ''):<24} {widget.get('type', ''):<10} "
              f"{widget.get('rect')} {'' if widget.get('shown') else '(hidden) '}{widget.get('text', '')!r}")
    return EXIT_OK


def parse_json_list(text: str, flag: str) -> list:
    """--edits / --records: a JSON array of objects."""
    try:
        value = json.loads(text)
    except ValueError as error:
        raise GameMcpError(EXIT_USAGE, f"{flag} is not JSON: {error}") from error
    if not isinstance(value, list) or not all(isinstance(item, dict) for item in value):
        raise GameMcpError(EXIT_USAGE, f"{flag} must be a JSON array of objects")
    return value


def print_menu_tree(tree: dict) -> None:
    print(f"{tree.get('path', '')}{' (open)' if tree.get('open') else ''}{' (unsaved)' if tree.get('dirty') else ''}")
    for screen in tree.get("screens", []):
        state = screen.get("status", "")
        if screen.get("status") == "ready" and not screen.get("current"):
            state += ", not current"
        print(f"  screen {screen.get('name', '')} (id {screen.get('id')}, {state}, "
              f"{screen.get('window_count', 0)} windows)")
        for window in screen.get("windows", []):
            indent = "    " + "  " * int(window.get("depth", 0))
            lists = ", ".join(f"{kind} {count}" for kind, count in sorted(window.get("lists", {}).items()))
            text = f" {window['text']!r}" if "text" in window else ""
            rect = f" {window['rect']}" if "rect" in window else ""
            hidden = " (hidden)" if window.get("shown") is False else ""
            print(f"{indent}{window.get('id')} {window.get('name', '')} {window.get('type', '')}{rect}{hidden}{text}"
                  + (f" [{lists}]" if lists else ""))


def cmd_menu(args: argparse.Namespace) -> int:
    request: dict = {"op": args.op}
    if args.path:
        request["path"] = args.path
    if args.op == "tree" and args.screen is not None:
        request["screen"] = args.screen
    if args.op == "edit":
        if not args.edits:
            raise GameMcpError(EXIT_USAGE, "menu edit takes --edits '[...]'")
        request["edits"] = parse_json_list(args.edits, "--edits")
    if args.op == "list":
        if args.id is None or not args.list or args.records is None:
            raise GameMcpError(EXIT_USAGE, "menu list takes --id, --list and --records '[...]'")
        request.update({"id": args.id, "list": args.list, "records": parse_json_list(args.records, "--records")})
    if args.op == "analyze" and args.severity:
        request["severity"] = args.severity
    payload = client_of(args).call("editor_menu", request, timeout=args.timeout)
    if payload.get("isError") or args.json:
        emit_payload(payload, args)
        return EXIT_TOOL_ERROR if payload.get("isError") else EXIT_OK
    out = payload.get("structuredContent", {})
    if args.op == "tree":
        print_menu_tree(out)
    elif args.op == "analyze":
        counts = out.get("counts", {})
        sources = ", ".join(f"{source} {count}" for source, count in sorted(out.get("sources", {}).items()))
        print(f"{out.get('path', '')}: {counts.get('error', 0)} error(s), {counts.get('warning', 0)} warning(s), "
              f"{counts.get('info', 0)} info" + (f" ({sources})" if sources else ""))
        for screen in out.get("screens", []):
            print(f"  screen {screen.get('name', '')}: {screen.get('status', '')}, {screen.get('notes', 0)} note(s), "
                  f"{screen.get('problems', 0)} problem(s)")
        for problem in out.get("problems", []):
            where = problem.get("record", "")
            if problem.get("field"):
                where += f" [{problem['field']}]"
            print(f"  {problem.get('severity', ''):<8} {problem.get('source', ''):<7} {problem.get('code', ''):<32} "
                  f"{where}: {problem.get('message', '')}")
    else:
        made = out.get("made", {})
        print("done" + (": " + ", ".join(f"{label}={id_}" for label, id_ in made.items()) if made else "")
              + (f" (added {out.get('added')})" if out.get("added") else ""))
    return EXIT_OK


def cmd_build(args: argparse.Namespace) -> int:
    payload = client_of(args).call("editor_build", {}, timeout=args.timeout)
    emit_payload(payload, args)
    return EXIT_TOOL_ERROR if payload.get("isError") else EXIT_OK


def cmd_play(args: argparse.Namespace) -> int:
    payload = client_of(args).call("editor_play", {"op": args.op}, timeout=args.timeout)
    if payload.get("isError"):
        print(text_of(payload))
        return EXIT_TOOL_ERROR
    block = payload.get("structuredContent", {})
    if args.json:
        print(json.dumps(block, indent=2))
    else:
        print(f"state={block.get('state')} pid={block.get('pid')} mcp_port={block.get('mcp_port')}"
              + (" exited_on_its_own" if block.get("exited_on_its_own") else ""))
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
    print(f"{target} ({images[0].get('mimeType')})")
    return EXIT_OK


def cmd_logs(args: argparse.Namespace) -> int:
    request = {"limit": args.limit}
    if args.cursor > 0:
        request["cursor"] = args.cursor
    page = client_of(args).structured("editor_logs", request)
    for entry in page.get("entries", []):
        print(f"{entry.get('seq', '')}\t{entry.get('source', '')}\t{entry.get('level', '')}\t{entry.get('text', '')}")
    print(f"next_cursor={page.get('next_cursor', args.cursor)}", file=sys.stderr)
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
    call.add_argument("--timeout", type=float, default=300.0)
    call.set_defaults(func=cmd_call)

    state = commands.add_parser("state", help="print editor_state")
    add_endpoint_options(state)
    state.add_argument("--output-cursor", type=int, default=0)
    state.add_argument("--output-limit", type=int, default=200)
    state.add_argument("--import-offset", type=int, default=0,
                       help="the page of the import lists (choices, roots, rows, not_found) from here")
    state.add_argument("--import-limit", type=int, default=200)
    state.set_defaults(func=cmd_state)

    request = commands.add_parser("request", help="raise one editor_request by kind")
    add_endpoint_options(request)
    request.add_argument("kind", help="new_project, open_project, create_missing, build, play, save_all, quit, ...")
    request.add_argument("--path", default=None)
    request.add_argument("--text", default=None)
    request.add_argument("--flag", choices=("true", "false"), default=None,
                         help="the kind's flag: preview_import, preview_retail_import and plan_import plan with "
                              "the files the chosen ones need; set_import_dependencies sets that setting; "
                              "import_files replaces existing files; reimport forces")
    request.add_argument("--paths", action="append", default=None,
                         help="a file picked for preview_import (repeat for more)")
    request.add_argument("--names", default=None,
                         help="comma-separated: create_missing's requirement roles, preview_retail_import's files")
    request.add_argument("--imports", default=None,
                         help="plan_import's files chosen, import_files' rows kept: a JSON array of "
                              "{path, entry?, retail?, native?} (editor_state's import rows carry each as source)")
    request.add_argument("--edit", default=None, help="the edit as a JSON object (edit_record, select_record, paste)")
    request.add_argument("--edits", default=None,
                         help="a batch of edits on one row as a JSON array (edit_record): one undo step")
    request.add_argument("--mode", choices=("replace", "add", "toggle"), default=None,
                         help="how select_record joins the selection")
    request.add_argument("--unsaved-choice", choices=("save", "discard", "cancel"), default=None,
                         help="resolve_unsaved: save writes the files the prompt lists, then what waited runs")
    request.add_argument("--settings", default=None,
                         help="apply_project_settings: the settings to set as a JSON object (title, mission, "
                              "multiplayer, retail_directory, runtime_executable, play_retail; one left out stays)")
    request.add_argument("--json", action="store_true", help="print the raw result payload")
    request.add_argument("--timeout", type=float, default=300.0)
    request.set_defaults(func=cmd_request)

    problems = commands.add_parser("problems", help="print editor_problems, each with its fixes")
    add_endpoint_options(problems)
    problems.add_argument("--severity", choices=("error", "warning", "info"), action="append", default=None,
                          help="a level to show (repeat for more; every level when left out)")
    problems.add_argument("--text", default=None, help="matched without case against message, file, record, field, code")
    problems.add_argument("--scope", choices=("project", "active_file", "open_files"), default=None)
    problems.add_argument("--fixable", action="store_true", help="only the problems with a fix")
    problems.add_argument("--group", choices=("none", "file", "kind"), default=None)
    problems.add_argument("--offset", type=int, default=0)
    problems.add_argument("--limit", type=int, default=100)
    problems.set_defaults(func=cmd_problems)

    references = commands.add_parser("references", help="editor_graph: what a file names and who names it")
    add_endpoint_options(references)
    references.add_argument("path", help="a project file (project-relative path or logical name)")
    references.set_defaults(func=cmd_references)

    rename = commands.add_parser("rename", help="editor_graph: rename a file with every reference rewritten")
    add_endpoint_options(rename)
    rename.add_argument("path")
    rename.add_argument("name", help="the new logical name (same extension)")
    rename.add_argument("--json", action="store_true", help="print the raw result payload")
    rename.add_argument("--timeout", type=float, default=120.0)
    rename.set_defaults(func=cmd_rename)

    preview = commands.add_parser("menu-preview", help="editor_menu_preview: the previewed menu screen as the game draws it")
    add_endpoint_options(preview)
    preview.add_argument("--hit", default=None, help="x,y in 800x600 design units: the widget the hit test finds")
    preview.add_argument("--width", type=int, default=None)
    preview.add_argument("--height", type=int, default=None)
    preview.add_argument("--show-hidden", action="store_true")
    preview.add_argument("--force-id", type=int, default=None, help="a window record held in --force-state")
    preview.add_argument("--force-state", choices=("normal", "mouseover", "selected", "disabled"), default=None)
    preview.add_argument("--checked", action="store_true", help="the --force-id window checked")
    preview.add_argument("--popup-open", action="store_true", help="the --force-id combo box with its list open")
    preview.add_argument("--focus", action="store_true", help="the --force-id edit box focused, its caret showing")
    preview.add_argument("--drag", type=int, default=None, metavar="ID",
                         help="drag a window of the previewed screen by --handle, --by dx,dy (one undo step)")
    preview.add_argument("--handle", default="move",
                         choices=("move", "left", "right", "top", "bottom", "top_left", "top_right", "bottom_left",
                                  "bottom_right"))
    preview.add_argument("--no-snap", action="store_true", help="--drag without the grid of 8")
    preview.add_argument("--nudge", type=int, default=None, metavar="ID", help="move a window --by dx,dy, no snap")
    preview.add_argument("--by", default=None, metavar="DX,DY", help="how far --drag or --nudge moves, design units")
    preview.add_argument("--arrange", default=None,
                         choices=("align_left", "align_right", "align_top", "align_bottom", "align_horizontal_centers",
                                  "align_vertical_centers", "distribute_horizontally", "distribute_vertically",
                                  "bring_to_front", "bring_forward", "send_backward", "send_to_back"),
                         help="arrange the --ids windows (aligned to the first), one undo step")
    preview.add_argument("--ids", default=None, metavar="ID,ID,...", help="the windows --arrange takes")
    preview.add_argument("--notes", action="store_true", help="the frame compiler's notes on the previewed screen")
    preview.add_argument("--render", default=None, metavar="PATH",
                         help="a menu's screen as the render check compiled it (with --screen)")
    preview.add_argument("--screen", type=int, default=None, help="the screen row id --render reads")
    preview.add_argument("--json", action="store_true", help="print the state and widgets as JSON")
    preview.set_defaults(func=cmd_menu_preview)

    model = commands.add_parser("model-preview", help="editor_model_preview: the open model as the game draws it")
    add_endpoint_options(model)
    model.add_argument("--lod", default=None, help="a level, or auto (the level the game picks at the distance)")
    model.add_argument("--ctrl", action="append", default=None, metavar="NAME=VALUE",
                       help="hold a CTRL register at a value (repeat for more; replaces the held set)")
    model.add_argument("--yaw", type=float, default=None, help="the camera's turn about the vertical, radians")
    model.add_argument("--pitch", type=float, default=None, help="the eye's elevation, radians")
    model.add_argument("--distance", type=float, default=None, help="the camera's distance from its target")
    model.add_argument("--frame", action="store_true", help="look at the whole model")
    model.add_argument("--json", action="store_true", help="print the state as JSON")
    model.set_defaults(func=cmd_model_preview)

    menu = commands.add_parser("menu", help="editor_menu: a menu's tree, a batch edit, a list replaced, its findings")
    add_endpoint_options(menu)
    menu.add_argument("op", choices=("tree", "edit", "list", "analyze"))
    menu.add_argument("--path", default=None,
                      help="the menu (project-relative path or logical name; default the previewed menu, "
                           "else the active document when it is a menu; the same menu for every op)")
    menu.add_argument("--screen", type=int, default=None, help="tree: one screen (its row id)")
    menu.add_argument("--edits", default=None,
                      help="edit: a JSON array of {op, id, field, value, kind, parent, position, as}: one undo step")
    menu.add_argument("--id", type=int, default=None, help="list: the record that holds the list")
    menu.add_argument("--list", default=None, help="list: the list's kind token (action, sound, items.item, ...)")
    menu.add_argument("--records", default=None, help="list: a JSON array of {field: value} records that replace it")
    menu.add_argument("--severity", choices=("error", "warning", "info"), default=None, help="analyze: one severity")
    menu.add_argument("--json", action="store_true", help="print the raw result payload")
    menu.add_argument("--timeout", type=float, default=300.0)
    menu.set_defaults(func=cmd_menu)

    build = commands.add_parser("build", help="editor_build: pack the project and wait")
    add_endpoint_options(build)
    build.add_argument("--json", action="store_true", help="print the raw result payload")
    build.add_argument("--timeout", type=float, default=300.0)
    build.set_defaults(func=cmd_build)

    play = commands.add_parser("play", help="editor_play: start, stop or read the running game")
    add_endpoint_options(play)
    play.add_argument("op", choices=("start", "stop", "state"))
    play.add_argument("--json", action="store_true", help="print the play block as JSON")
    play.add_argument("--timeout", type=float, default=300.0)
    play.set_defaults(func=cmd_play)

    screenshot = commands.add_parser("screenshot", help="save an editor_screenshot")
    add_endpoint_options(screenshot)
    screenshot.add_argument("--out", required=True)
    screenshot.add_argument("--format", choices=("webp", "png"), default="png")
    screenshot.add_argument("--max-dim", type=int, default=1280)
    screenshot.add_argument("--quality", type=float, default=None)
    screenshot.set_defaults(func=cmd_screenshot)

    logs = commands.add_parser("logs", help="print editor_logs entries")
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
