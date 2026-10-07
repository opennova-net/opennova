#!/usr/bin/env python3
"""The mission editor end to end, over the editor's and the game's MCP endpoints (docs/mcp.md).

A new project on a game install; a shipped mission imported with its dependencies (its
closure: ADR 0046 S14), or the whole install with --whole-install; the mission opened, an
entity moved, the move undone and redone; saved, built; Play started in the mission; the
game's own endpoint read until the mission is loaded and the moved entity stands where it
was put. Every process it starts is asked to stop before it returns, whatever the outcome, and
ended (the editor with its tree) when it does not: only the pids this run recorded.

    python scripts/mcp/editor_mission_e2e.py --install "C:/Games/Joint Operations"
    python scripts/mcp/editor_mission_e2e.py --install "C:/Games/Joint Operations" --mission ASH_I1gA.bms --windowed
    python scripts/mcp/editor_mission_e2e.py --install "C:/Games/Joint Operations" --whole-install
    python scripts/mcp/editor_mission_e2e.py --install "C:/Games/Joint Operations Combined Arms" \
        --builds-on jox01 --expansion jxm --mission 07TR.bms

With --expansion (ADR 0046 S16) the project builds as that expansion, on the installed one
--builds-on names when given: its mission imported from what /exp <builds-on> serves, its build
the expansion's folder (the base game's own files left out: same_as_base), Play running the game
over the install's base game with /exp <expansion>, and the game read until the mission is loaded
and in OpenNova's mission catalog, the build saying nothing of the stock game's list showing it
untitled (build.expansion.mission_untitled), and the game's strings showing the expansion's own table
(its EXP_NAME set by the run, read from the loose <expansion>.bin the build placed).

Local only: it needs a game install and starts the game, so no CI job runs it and it reads
no environment variable. Standard library only; the clients are editor_mcp.py's and
game_mcp.py's. Exit codes are editor_mcp.py's: 0 every step held; 1 a step was not done or
an expectation failed (said on stderr); 2 not read (a usage error, no endpoint, a tool
error); 7 the editor did not launch.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import editor_mcp  # noqa: E402
from editor_mcp import EXIT_NOT_DONE, EXIT_NOT_READ, raise_and_wait  # noqa: E402
from game_mcp import EXIT_OK, GameMcp, GameMcpError, pid_alive, port_open, read_pid_file, text_of  # noqa: E402


class StepFailed(Exception):
    """An expectation of the run that did not hold: said, then the run stops (exit 1)."""


def say(text: str) -> None:
    print(text, flush=True)


def expect(holds: bool, what: str) -> None:
    if not holds:
        raise StepFailed(what)


def query(client: GameMcp, name: str, **params) -> dict:
    return client.structured("editor_query", {"query": name, **params}, timeout=120)


def request(client: GameMcp, kind: str, wait_s: float = 0.0, **fields) -> dict:
    """One request; with wait_s, the operation it starts and the validation after it awaited.
    Its answer; a request the editor did not read, did not do, or whose operation did not end
    done fails the step."""
    body = {"kind": kind, **fields}
    if wait_s > 0:
        body["wait"] = True
        body["wait_ms"] = int(wait_s * 1000)
    payload = client.call("editor_request", body, timeout=wait_s + 60 if wait_s > 0 else 120)
    if payload.get("isError"):
        raise StepFailed(f"{kind} was not read: {text_of(payload)}")
    answer = payload.get("structuredContent", {})
    outcome = answer.get("outcome", {})
    expect(outcome.get("done", False), f"{kind} was not done: {json.dumps(outcome)}")
    expect(not answer.get("timed_out"), f"{kind} did not end within {wait_s:.0f} s")
    ended = answer.get("operation") or {}
    expect(ended.get("end", "done") == "done", f"{kind}'s operation ended {ended.get('end')}: {json.dumps(ended)}")
    return answer


def fields_of(record: dict) -> dict:
    """A record's fields by id: {id: value}."""
    return {field.get("id", ""): field.get("value") for field in record.get("fields", [])}


def position_record(client: GameMcp, path: str) -> tuple[int, dict]:
    """The first record of the document that has a position (the fields x, y and z: an entity):
    its id and its fields. The rows are paged; each row and each record its collections hold is
    asked for its fields."""
    offset = 0
    while True:
        page = query(client, "document", path=path, offset=offset, limit=50)
        rows = page.get("rows", [])
        candidates: list[int] = []

        def walk(node: dict) -> None:
            if "id" in node:
                candidates.append(int(node["id"]))
            for collection in node.get("collections", []):
                for child in collection.get("records", []):
                    walk(child)

        for row in rows:
            walk(row)
        for record_id in candidates:
            record = query(client, "record", path=path, id=record_id)
            values = fields_of(record)
            if all(name in values and isinstance(values[name], (int, float)) for name in ("x", "y", "z")):
                return record_id, values
        next_offset = page.get("next_offset")
        if not rows or next_offset is None or next_offset <= offset:
            break
        offset = next_offset
    raise StepFailed(f"{path} holds no record with a position (the fields x, y, z)")


def string_record(client: GameMcp, path: str, section: str, key: str) -> int | None:
    """The id of a string table's entry `key` in its section `section` (compared without case, as the
    game compares them); None when the table has none."""
    page = query(client, "document", path=path, offset=0, limit=200)
    for row in page.get("rows", []):
        if "id" not in row:
            continue
        if str(fields_of(query(client, "record", path=path, id=int(row["id"]))).get("name", "")).lower() != section.lower():
            continue
        for collection in row.get("collections", []):
            for child in collection.get("records", []):
                values = fields_of(query(client, "record", path=path, id=int(child["id"])))
                if str(values.get("key", "")).lower() == key.lower():
                    return int(child["id"])
    return None


def document_state(client: GameMcp, path: str) -> dict:
    return query(client, "document", path=path, limit=1)


def as_xyz(value) -> tuple[float, float, float] | None:
    """A position the game's entity page gives, as a list or as {x, y, z}."""
    if isinstance(value, dict) and all(axis in value for axis in ("x", "y", "z")):
        return float(value["x"]), float(value["y"]), float(value["z"])
    if isinstance(value, (list, tuple)) and len(value) == 3:
        return float(value[0]), float(value[1]), float(value[2])
    return None


def game_entities(game: GameMcp) -> list:
    rows: list = []
    offset = 0
    while True:
        page = game.structured("game_entities", {"op": "list", "offset": offset, "limit": 128})
        batch = page.get("entities", [])
        rows.extend(batch)
        if not batch or len(rows) >= int(page.get("total", len(rows))):
            return rows
        offset += len(batch)


def nearest(rows: list, point: tuple[float, float, float]) -> float:
    """How far the nearest entity stands from the point on the ground plane (x, y): the game
    settles a placed entity's height on the terrain, so z is its own."""
    best = float("inf")
    for row in rows:
        at = as_xyz(row.get("mission_position"))
        if at is None:
            continue
        best = min(best, ((at[0] - point[0]) ** 2 + (at[1] - point[1]) ** 2) ** 0.5)
    return best


def read_pid(pid_file: Path) -> int:
    return read_pid_file(pid_file)[0]  # the launch's {"pid", "port"}


def terminate(pid: int, what: str) -> None:
    """Ends a process this run started that asking did not stop (its tree on Windows: the
    editor's Play child with it). Only ever a pid this run recorded."""
    if pid <= 0 or not pid_alive(pid):
        return
    say(f"   ending the {what} (pid {pid}), which did not stop when asked")
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True, check=False)
    else:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            return
    deadline = time.monotonic() + 15.0
    while pid_alive(pid) and time.monotonic() < deadline:
        time.sleep(0.25)
    if pid_alive(pid) and os.name != "nt":
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def launch_namespace(args: argparse.Namespace, pid_file: Path) -> argparse.Namespace:
    """editor_mcp's launch arguments for this run: the launch parser's defaults (every flag cmd_launch
    reads, --front among them), then this run's."""
    launch = editor_mcp.build_parser().parse_args(["launch"])
    for name, value in dict(port=args.port, editor=args.editor, godot=args.godot, project_dir=str(editor_mcp.PROJECT_DIR),
                            headless=not args.windowed, windowed=args.windowed, pid_file=str(pid_file),
                            timeout=240.0).items():
        setattr(launch, name, value)
    return launch


def run(args: argparse.Namespace, project: Path, pid_file: Path, started_pids: dict) -> None:
    install = str(Path(args.install).resolve())
    launch = launch_namespace(args, pid_file)
    say("1. launching the editor")
    try:
        code = editor_mcp.cmd_launch(launch)
    finally:
        # Written as the process starts, whether or not its endpoint ever answers.
        started_pids["editor"] = read_pid(pid_file)
    expect(code == EXIT_OK, f"the editor did not launch (exit {code})")
    client = GameMcp.for_port(args.port)

    say(f"2. a new project in {project}, on the game install {install}")
    request(client, "new_project", wait_s=120, dir=str(project), title="Mission e2e")
    request(client, "apply_project_settings", wait_s=120, settings={"game_install": install, "mission": True})
    if args.expansion:
        # The project as the expansion (project.opennova's expansion, ADR 0046 S16), then opened again,
        # so the install's listing and the import read through /exp <builds-on>.
        say(f"   building as the expansion {args.expansion}" +
            (f" on {args.builds_on}" if args.builds_on else " on the base game"))
        project_file = project / "project.opennova"
        document = json.loads(project_file.read_text(encoding="utf-8"))
        document["expansion"] = {"name": args.expansion, "builds_on": args.builds_on or ""}
        project_file.write_text(json.dumps(document, indent=2), encoding="utf-8")
        request(client, "open_project", wait_s=600, dir=str(project))
    state = client.structured("editor_state", {"sections": ["project", "import"]})
    expect(state.get("project", {}).get("open", False), "the project did not open")
    say(f"   the install lists {state.get('import', {}).get('install_files', 0)} files")

    started = time.monotonic()
    if args.whole_install:
        say("3. importing the whole game install")
        request(client, "preview_install_import", wait_s=args.timeout, all=True)
    else:
        say(f"3. importing {args.mission} with its dependencies")
        request(client, "preview_install_import", wait_s=args.timeout, names=[args.mission], with_dependencies=True)
    plan = query(client, "import_preview", limit=1)
    expect(plan.get("open", False) and plan.get("count", 0) > 0, f"nothing was planned: {json.dumps(plan)[:400]}")
    say(f"   the plan: {plan.get('count')} files, {plan.get('total_bytes', 0) / 1e6:.1f} MB, "
        f"{plan.get('not_found_count', 0)} not found, planned in {time.monotonic() - started:.1f} s")
    for line in plan.get("summary", [])[:8]:
        say(f"     {line.get('kind')}: {line.get('files')} files, {line.get('bytes', 0) / 1e6:.1f} MB")
    started = time.monotonic()
    request(client, "import_files", wait_s=args.timeout, planned=True, plan=plan.get("plan", 0))
    held = query(client, "files", limit=1).get("count", 0)
    say(f"   imported in {time.monotonic() - started:.1f} s; the project holds {held} files")
    expect(held > 0, "the import wrote nothing")

    title = ""
    if args.expansion:
        # The expansion's own text table (<b>.bin: the installed expansion's imported under the project's name,
        # else a blank one made), its Mods-list name set to one of this run's, which the game must show.
        table = f"{args.expansion}.bin"
        client.call("editor_request", {"kind": "create_missing", "roles": ["expansion_table"], "wait": True,
                                       "wait_ms": 60000}, timeout=120)
        request(client, "open_document", path=table)
        table_path = client.structured("editor_state", {"sections": ["documents"]}).get("documents", {}).get("active", "")
        expect(table_path.lower().endswith(table.lower()), f"{table} did not open (active: {table_path!r})")
        entry = string_record(client, table_path, "exp_info", "EXP_NAME")
        expect(entry is not None, f"{table} has no [exp_info] EXP_NAME")
        title = f"OpenNova e2e {args.expansion}"
        request(client, "edit_record", path=table_path, edits=[{"op": "set", "id": entry, "field": "text", "value": title}])
        say(f"   {table_path}'s EXP_NAME set to {title!r}")

    say(f"4. opening {args.mission}")
    request(client, "open_document", path=args.mission)
    path = client.structured("editor_state", {"sections": ["documents"]}).get("documents", {}).get("active", "")
    expect(path.lower().endswith(args.mission.lower()), f"{args.mission} did not open (active: {path!r})")
    errors = query(client, "problems", severities=["error"], text=args.mission, limit=5)
    for problem in errors.get("problems", []):
        say(f"   error: {problem.get('code')}: {problem.get('message')}")
    record_id, before = position_record(client, path)
    old = (float(before["x"]), float(before["y"]), float(before["z"]))
    new = (old[0] + args.move, old[1], old[2])
    say(f"5. moving record {record_id} from x={old[0]:g} to x={new[0]:g}")
    request(client, "edit_record", path=path, edits=[{"op": "set", "id": record_id, "field": "x", "value": new[0]}])
    moved = fields_of(query(client, "record", path=path, id=record_id))
    expect(abs(float(moved["x"]) - new[0]) < 1e-3 and document_state(client, path).get("dirty", False),
           f"the record did not move (x={moved.get('x')})")
    say("6. undo, then redo")
    request(client, "undo", path=path)
    undone = fields_of(query(client, "record", path=path, id=record_id))
    expect(abs(float(undone["x"]) - old[0]) < 1e-3 and not document_state(client, path).get("dirty", True),
           f"the undo did not take the move back (x={undone.get('x')})")
    request(client, "redo", path=path)
    redone = fields_of(query(client, "record", path=path, id=record_id))
    expect(abs(float(redone["x"]) - new[0]) < 1e-3, f"the redo did not move it again (x={redone.get('x')})")

    say("7. saving and building")
    request(client, "save_all")
    expect(not document_state(client, path).get("dirty", True), "the mission is still unsaved")
    started = time.monotonic()
    outcome, ended = raise_and_wait(client, {"kind": "build"}, args.timeout)
    expect(bool(ended) and ended.get("end") == "done", f"the build did not land: {json.dumps(outcome)} {json.dumps(ended)}")
    build = query(client, "operation").get("build", {})
    expect(build.get("ok", False), f"the build failed: {json.dumps(build)[:600]}")
    say(f"   built {build.get('dir')} in {time.monotonic() - started:.1f} s")
    if args.expansion:
        built = Path(build.get("dir", "")) / "expansion" / args.expansion
        expect(build.get("expansion") == args.expansion and (built / f"{args.expansion}.pff").is_file() and
               (built / f"{args.expansion}L.pff").is_file(), f"no expansion {args.expansion} was built: {built}")
        same = build.get("same_as_base", {})
        size = sum(f.stat().st_size for f in built.rglob("*") if f.is_file())
        say(f"   expansion/{args.expansion}: {size / 1e6:.1f} MB; {same.get('files', 0)} files "
            f"({same.get('bytes', 0) / 1e6:.1f} MB) left out as the base game's own, "
            f"{same.get('base_bytes_read', 0) / 1e6:.1f} MB of the base read")

    say(f"8. Play in {args.mission}")
    outcome, ended = raise_and_wait(client, {"kind": "play", "mission": args.mission}, args.timeout)
    expect(bool(ended) and ended.get("end") == "done", f"Play's build did not land: {json.dumps(outcome)}")
    run_section = client.structured("editor_state", {"sections": ["run"]}).get("run", {})
    started_pids["game"] = int(run_section.get("pid", 0) or 0)
    expect(run_section.get("state") == "running", f"the game is not running: {json.dumps(run_section)}")
    expect(run_section.get("mission", "").lower() == args.mission.lower(),
           f"the game was not started in {args.mission}: {json.dumps(run_section)}")
    game_port = int(run_section.get("mcp_port", 0))
    expect(game_port > 0, "the game has no MCP endpoint")
    say(f"   the game runs (pid {run_section.get('pid')}), its endpoint on port {game_port}")
    if args.expansion:
        command = run_section.get("command_line", "")
        expect(f"/exp {args.expansion}" in command, f"the game was not started with /exp {args.expansion}: {command}")
        say(f"   on its run directory {run_section.get('run_dir')} with /exp {args.expansion}")

    say("9. reading the game")
    game = GameMcp.for_port(game_port)
    deadline = time.monotonic() + args.game_timeout
    shell: dict = {}
    while time.monotonic() < deadline:
        if port_open(game_port):
            try:
                shell = game.structured("game_state").get("shell", {})
            except GameMcpError:
                shell = {}
            if shell.get("world_loaded") and str(shell.get("mission_file", "")).lower() == args.mission.lower():
                break
        failed = [p for p in query(client, "problems", text="play.", limit=20).get("problems", [])
                  if str(p.get("code", "")).startswith("play.")]
        expect(not failed, "the game reported: " + "; ".join(f"{p.get('code')}: {p.get('message')}" for p in failed))
        expect(client.structured("editor_state", {"sections": ["run"]}).get("run", {}).get("state") == "running",
               "the game stopped before the mission loaded (the editor's output has its log)")
        time.sleep(1.0)
    expect(bool(shell.get("world_loaded")) and str(shell.get("mission_file", "")).lower() == args.mission.lower(),
           f"the mission did not load within {args.game_timeout:.0f} s: {json.dumps(shell)}")
    say(f"   loaded {shell.get('mission_file')}")
    if args.expansion:
        expect(shell.get("expansion") == args.expansion,
               f"the game did not mount the expansion {args.expansion}: {json.dumps(shell)}")
        expect(bool(shell.get("mission_in_catalog")), f"OpenNova's mission catalog does not hold {args.mission}")
        # The catalog reads a mission's text through the mount stack, not retail's archive pairs: the
        # build is what says the stock game's list would show it untitled (no text table in its pair).
        untitled = [p for p in query(client, "problems", text="build.expansion.mission_untitled", limit=50).get("problems", [])
                    if str(p.get("code", "")) == "build.expansion.mission_untitled"
                    and args.mission.lower() in str(p.get("message", "")).lower()]
        expect(not untitled, f"the build says the game's mission list shows {args.mission} untitled: "
               + "; ".join(str(p.get("message")) for p in untitled))
        # The expansion's strings: the game's lookup answers from the loose <b>.bin the build placed.
        expect(shell.get("expansion_title") == title,
               f"the game's strings do not show the expansion's table: EXP_NAME is {shell.get('expansion_title')!r}, "
               f"not {title!r}")
        say(f"   the game's string lookup shows the expansion's EXP_NAME {title!r}")
        say(f"   mounted /exp {shell.get('expansion')}; {args.mission} in OpenNova's mission catalog, its text table in "
            f"the expansion's pair")
    entities = game_entities(game)
    expect(len(entities) > 0, "the game lists no entity")
    to_new, to_old = nearest(entities, new), nearest(entities, old)
    say(f"   {len(entities)} entities; on the ground plane the nearest to where the record was put is {to_new:.2f} away, "
        f"to where it was {to_old:.2f}")
    expect(to_new <= args.tolerance, f"no entity stands where the record was moved to (nearest {to_new:.2f})")
    if args.windowed and args.screenshots:
        shots = Path(args.screenshots)
        shots.mkdir(parents=True, exist_ok=True)
        editor_mcp.cmd_screenshot(argparse.Namespace(port=args.port, url=None, out=str(shots / "editor.png"),
                                                     format="png", max_dim=1600, quality=None))
    say("10. every step held")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install", required=True, help="the game install (its folder holds the three archives)")
    parser.add_argument("--mission", default="04TR.bms", help="the shipped mission to import, edit and play")
    parser.add_argument("--project", default=None,
                        help="where the project is made (must not exist; default: a temp folder, removed after)")
    parser.add_argument("--whole-install", dest="whole_install", action="store_true",
                        help="import every file of the install instead of the mission's closure")
    parser.add_argument("--expansion", default=None,
                        help="the project builds as this expansion (ADR 0046 S16), played with /exp")
    parser.add_argument("--builds-on", dest="builds_on", default=None,
                        help="with --expansion: the installed expansion the project's files come from")
    parser.add_argument("--move", type=float, default=64.0, help="how far the record moves along x")
    parser.add_argument("--tolerance", type=float, default=1.0,
                        help="how near, on the ground plane, the game's entity must stand to where the record was put")
    parser.add_argument("--port", type=int, default=editor_mcp.DEFAULT_PORT, help="the editor's --mcp-port")
    parser.add_argument("--editor", default=None, help="a packaged opennova-editor.exe (default: the source project)")
    parser.add_argument("--godot", default=None, help="Godot 4.6.1 binary (default: GODOT_BIN, else .godot-bin/)")
    parser.add_argument("--windowed", action="store_true", help="a windowed editor (headless by default)")
    parser.add_argument("--screenshots", default=None, help="with --windowed: a folder for the editor's screenshot")
    parser.add_argument("--timeout", type=float, default=1800.0, help="seconds an import, a build or a plan may take")
    parser.add_argument("--game-timeout", dest="game_timeout", type=float, default=300.0,
                        help="seconds the mission may take to load in the game")
    parser.add_argument("--keep", action="store_true", help="keep the project folder (a temp one is removed otherwise)")
    args = parser.parse_args(argv)

    made_temp = args.project is None
    holder = Path(tempfile.mkdtemp(prefix="opennova-mission-e2e-")) if made_temp else None
    project = (holder / "project") if holder else Path(args.project).resolve()
    if project.exists() and any(project.iterdir()):
        print(f"editor_mission_e2e: {project} is not empty", file=sys.stderr)
        return EXIT_NOT_READ
    pid_file = (holder if holder else project.parent) / "editor_mission_e2e.pid"
    code = EXIT_OK
    started_pids: dict = {}
    try:
        run(args, project, pid_file, started_pids)
    except StepFailed as failure:
        print(f"editor_mission_e2e: FAILED: {failure}", file=sys.stderr)
        code = EXIT_NOT_DONE
    except GameMcpError as error:
        print(f"editor_mission_e2e: {error}", file=sys.stderr)
        code = error.code if error.code != EXIT_NOT_DONE else EXIT_NOT_READ
    finally:
        # The game, then the editor, asked to stop; what does not is ended: nothing it started is
        # left running, whatever the exit path (an editor still starting when the launch gave up
        # has no endpoint to ask).
        editor_pid = started_pids.get("editor") or read_pid(pid_file)
        try:
            editor_mcp.cmd_stop(argparse.Namespace(port=args.port, url=None, pid=editor_pid, pid_file=str(pid_file),
                                                   timeout=60.0))
        except GameMcpError as error:
            print(f"editor_mission_e2e: stopping the editor: {error}", file=sys.stderr)
            code = code or error.code
            terminate(editor_pid, "editor")
        except KeyboardInterrupt:
            terminate(editor_pid, "editor")
            raise
        terminate(started_pids.get("game", 0), "game")
        if made_temp and not args.keep:
            shutil.rmtree(holder, ignore_errors=True)
        elif made_temp:
            say(f"kept {project}")
    return code


if __name__ == "__main__":
    sys.exit(main())
