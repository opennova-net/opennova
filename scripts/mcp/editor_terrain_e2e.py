#!/usr/bin/env python3
"""The terrain editing round end to end, over the editor's and the game's MCP endpoints (ADR 0046 S25, docs/mcp.md).

The acceptance scenario of the round's plan on a shipped mission: a new project on a game install; the mission
imported with its dependencies; the mission opened and an entity of it taken as the tank (unlocked: it follows the
ground); the Terrain pane shown with Raise, the lock op counting the tank among what follows the ground; a stroke of
the heights across the tank, the tank's height following the ground once the stroke's bake lands; Paint with surface
class 12 beside it; a tile stamped beside the hill on the mission's map and turned; four Undos in the mission
taking the four files back to what was loaded, four Redos making them again, and one Save writing them; then built
and played: OpenNova Play's game read through its terrain_ground probe (the raised height, surface 12, the tile), or
with --play-mode strict the game install's own program over the build in its run directory, its file log read with
no error (the stock game names no mission on its command line, so the terrain's heights, its surface map and the
mission's .til are named there once a player starts the mission within --install-run). Every process it starts is
asked to stop before it returns, whatever the outcome, and ended when it does not: only the pids this run recorded.

    python scripts/mcp/editor_terrain_e2e.py --install "C:/Games/Joint Operations Combined Arms" \\
        --mission 00TRa.bms --port 8983
    python scripts/mcp/editor_terrain_e2e.py --install "C:/Games/Joint Operations Combined Arms" \\
        --mission 00TRa.bms --port 8983 --windowed --screenshots shots/
    python scripts/mcp/editor_terrain_e2e.py --install "C:/Games/Joint Operations Combined Arms" \\
        --mission 00TRa.bms --port 8983 --play-mode strict

With --windowed and --screenshots the editor's window is captured: the brush's ring over the tank (the workspace's
terrain_brush.cursor placing it with no pointer), the Terrain pane, the map's Delta background and the hill.

Local only: it needs a game install and starts the game, so no CI job runs it and it reads no environment variable.
Standard library only; the clients are editor_mcp.py's and game_mcp.py's, the steps editor_mission_e2e.py's. Exit
codes are editor_mcp.py's: 0 every step held; 1 a step was not done or an expectation failed (said on stderr); 2 not
read (a usage error, no endpoint, a tool error); 7 the editor did not launch.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import editor_mcp  # noqa: E402
from editor_mcp import EXIT_NOT_DONE, EXIT_NOT_READ, raise_and_wait  # noqa: E402
from editor_mission_e2e import (StepFailed, document_state, expect, fields_of, launch_namespace,  # noqa: E402
                                mission_rows, position_record, query, read_pid, request, say, terminate)
from game_mcp import EXIT_OK, GameMcp, GameMcpError, port_open  # noqa: E402

SURFACE = 12  # the surface class Paint lays (Stone in the plan's scenario)


def terrain(client: GameMcp, path: str, op: str, **params) -> dict:
    """The terrain query's op of the view `path`: a refusal fails the step."""
    payload = client.call("editor_query", {"query": "terrain", "path": path, "op": op, **params}, timeout=120)
    if payload.get("isError"):
        raise StepFailed(f"the terrain query's {op} was refused: {payload.get('content')}")
    return payload.get("structuredContent", {})


def sample(client: GameMcp, path: str, x: float, y: float) -> dict:
    return terrain(client, path, "sample", x=x, y=y)


def set_workspace(client: GameMcp, change: dict) -> None:
    request(client, "set_workspace", workspace=change)


def stroke(client: GameMcp, path: str, plane: str, points: list, brush: dict | None = None) -> dict:
    """One stroke of the brush over `points` (mission metres): its samples in one gesture, ended by its token."""
    body = {"plane": plane, "points": points[:1], "gesture": 0, "end": False}
    if brush:
        body["brush"] = brush
    first = request(client, "terrain_stroke", path=path, stroke=body)
    token = int(first.get("outcome", {}).get("gesture", 0))
    expect(token > 0, f"the stroke named no gesture: {json.dumps(first.get('outcome'))}")
    last = first
    for at in range(1, len(points), 32):
        body = {"plane": plane, "points": points[at:at + 32], "gesture": token, "end": at + 32 >= len(points)}
        if brush:
            body["brush"] = brush
        last = request(client, "terrain_stroke", path=path, stroke=body)
    if len(points) == 1:
        request(client, "end_edit", path=path, gesture=token)
    return last.get("outcome", {}).get("stroke", {})


def line_across(x: float, y: float, half: float, step: float = 1.0) -> list:
    """Points on a line west to east through (x, y), `half` metres each way."""
    count = max(1, int(2 * half / step))
    return [[x - half + i * (2 * half) / count, y] for i in range(count + 1)]


def await_bake(client: GameMcp, path: str, timeout: float) -> None:
    """The heights' bake landed: the terrain query's bake op idle, every region baked."""
    deadline = time.monotonic() + timeout
    answer: dict = {}
    while time.monotonic() < deadline:
        answer = terrain(client, path, "bake")
        if not answer.get("due", True):
            return
        time.sleep(0.5)
    raise StepFailed(f"the bake did not land within {timeout:.0f} s: {json.dumps(answer)[:400]}")


def record_xyz(client: GameMcp, path: str, record: int) -> tuple[float, float, float]:
    fields = fields_of(query(client, "record", path=path, id=record))
    return float(fields["x"]), float(fields["y"]), float(fields["z"])


def map_pin(client: GameMcp, path: str, record: int) -> tuple[float, float]:
    """The record's pin on the mission's 2D map, in the map's pixels (the state's items paged, 200 a page)."""
    offset = 0
    while True:
        state = client.structured("editor_viewport", {"op": "state", "path": path, "kind": "map", "offset": offset,
                                                      "limit": 200})
        items = state.get("items", [])
        for item in items:
            if int(item.get("id", 0)) == record:
                screen = item.get("screen", [0, 0])
                return float(screen[0]), float(screen[1])
        if len(items) < 200:
            break
        offset += len(items)
    raise StepFailed(f"the map shows no pin of record {record}")


def probe_run(game: GameMcp, name: str, args: dict, timeout: float) -> tuple[bool, dict, dict]:
    """A game_probe run to its verdict: whether it passed, its data and its status."""
    started = game.structured("game_probe", {"op": "run", "name": name, "args": args}, timeout=60)
    run_id = started.get("run_id", "")
    expect(bool(run_id), f"the game did not start the {name} probe: {json.dumps(started)}")
    deadline = time.monotonic() + timeout
    status: dict = {}
    while time.monotonic() < deadline:
        status = game.structured("game_probe", {"op": "status", "run_id": run_id, "wait_ms": 2000}, timeout=60)
        if status.get("state") in ("passed", "failed", "cancelled", "error"):
            break
    verdict = status.get("verdict") or {}
    return status.get("state") == "passed", verdict.get("data", {}), status


def probe(game: GameMcp, name: str, args: dict, timeout: float) -> dict:
    """A game_probe run to its verdict: its data (a failed verdict fails the step)."""
    passed, data, status = probe_run(game, name, args, timeout)
    expect(passed, f"the {name} probe did not pass: {json.dumps(status)[:800]}")
    return data


def await_ready(client: GameMcp, path: str, kind: str, timeout: float = 180.0) -> None:
    """The viewport's picture built (its state's status ready), for a screenshot of it."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = client.structured("editor_viewport", {"op": "state", "path": path, "kind": kind, "limit": 1})
        if state.get("status") == "ready":
            return
        time.sleep(1.0)
    say(f"   the {kind} picture was not ready within {timeout:.0f} s")


def shoot(args: argparse.Namespace, client: GameMcp, path: str, tank: int, at: tuple, name: str,
          map_background: str = "") -> None:
    """A screenshot of the editor's window: the 3D view framed on the tank with the brush's ring at `at` (the
    workspace's cursor, no pointer), the Terrain pane beside it; with `map_background`, the 2D map brought forward
    with that background."""
    if not (args.windowed and args.screenshots):
        return
    request(client, "select_record", path=path, address={"row": tank, "child": 0})
    request(client, "edit_in_viewport", path=path, command={"name": "frame", "kind": "mission"})
    set_workspace(client, {"terrain_brush": {"cursor": [at[0], at[1]]}})
    if map_background:
        request(client, "set_viewport", path=path, viewport={"kind": "map", "options": {"background": map_background}})
        set_workspace(client, {"document": {"path": path, "map": True}, "focus": "preview"})
        await_ready(client, path, "map")
    await_ready(client, path, "mission")
    time.sleep(1.0)
    screenshot(args, name)


def screenshot(args: argparse.Namespace, name: str) -> None:
    if not (args.windowed and args.screenshots):
        return
    out = Path(args.screenshots) / f"{name}.png"
    editor_mcp.cmd_screenshot(argparse.Namespace(port=args.port, url=None, out=str(out), format="png", max_dim=1600,
                                                 quality=None))
    say(f"   screenshot {out}")


def run(args: argparse.Namespace, project: Path, pid_file: Path, started_pids: dict) -> None:
    install = str(Path(args.install).resolve())
    say("1. launching the editor")
    try:
        code = editor_mcp.cmd_launch(launch_namespace(args, pid_file))
    finally:
        started_pids["editor"] = read_pid(pid_file)
    expect(code == EXIT_OK, f"the editor did not launch (exit {code})")
    client = GameMcp.for_port(args.port)

    say(f"2. a new project in {project}, on {install}; {args.mission} imported with its dependencies")
    request(client, "new_project", wait_s=120, dir=str(project), title="Terrain e2e")
    settings = {"game_install": install, "mission": True}
    if args.play_mode:
        settings["play_mode"] = args.play_mode
    request(client, "apply_project_settings", wait_s=120, settings=settings)
    request(client, "preview_install_import", wait_s=args.timeout, names=[args.mission], with_dependencies=True)
    plan = query(client, "import_preview", limit=1)
    expect(plan.get("open", False) and plan.get("count", 0) > 0, f"nothing was planned: {json.dumps(plan)[:400]}")
    request(client, "import_files", wait_s=args.timeout, planned=True, plan=plan.get("plan", 0))

    say(f"3. opening {args.mission}; its tank, unlocked")
    request(client, "open_document", path=args.mission)
    path = client.structured("editor_state", {"sections": ["documents"]}).get("documents", {}).get("active", "")
    expect(path.lower().endswith(args.mission.lower()), f"{args.mission} did not open (active: {path!r})")
    tank, fields = position_record(client, path)
    if int(fields.get("height_lock", 0) or 0):
        request(client, "edit_record", path=path, edits=[{"op": "set", "id": tank, "field": "height_lock", "value": 0}])
        request(client, "save", path=path)
    x, y, z = float(fields["x"]), float(fields["y"]), float(fields["z"])
    ground = sample(client, path, x, y)
    expect(ground.get("height") is not None, f"no terrain under the tank: {json.dumps(ground)[:400]}")
    clearance = z - float(ground["height"])
    family = terrain(client, path, "family")
    files = {plane: family.get("planes", {}).get(plane, {}).get("path", "") for plane in ("heights", "surface")}
    files["tiles"] = family.get("tiles", {}).get("path", "")
    say(f"   the tank: record {tank} at ({x:.1f}, {y:.1f}), {clearance:.2f} m over the ground; the files: {files}")
    expect(all(files.values()), f"the family names no file for a plane: {json.dumps(family)[:600]}")

    say("4. the Terrain pane, Raise; a stroke across the tank")
    set_workspace(client, {"document": {"path": path, "terrain": True, "map": True},
                           "terrain_brush": {"tool": "raise", "radius": args.radius, "strength": 1.0}})
    brush = terrain(client, path, "brush")
    expect(brush.get("live") and brush.get("plane") == "heights", f"the brush is not live on the heights: {brush}")
    before = float(ground["height"])
    under = terrain(client, path, "lock", x=x, y=y, radius=args.radius)
    expect(int(under.get("follow", 0)) >= 1, f"the lock op counts no follower at the tank: {json.dumps(under)[:400]}")
    say(f"   under the brush: {under.get('follow')} follow the ground, {under.get('locked')} height locked")
    stroke(client, path, "heights", line_across(x, y, args.radius))
    await_bake(client, path, args.timeout)
    raised = float(sample(client, path, x, y).get("height") or before)
    expect(raised > before + 0.01, f"the stroke did not raise the ground under the tank ({before:.2f} -> {raised:.2f})")
    deadline = time.monotonic() + 60.0
    now = record_xyz(client, path, tank)
    while time.monotonic() < deadline and abs(now[2] - (raised + clearance)) > 0.05:
        time.sleep(0.5)
        now = record_xyz(client, path, tank)
    expect(abs(now[2] - (raised + clearance)) <= 0.05,
           f"the tank did not follow the ground: z {now[2]:.2f}, the ground {raised:.2f} + {clearance:.2f}")
    say(f"   the ground under the tank {before:.2f} -> {raised:.2f}; the tank followed to z {now[2]:.2f}")
    shoot(args, client, path, tank, (x, y), "1_raise")

    say(f"5. Paint, surface {SURFACE}, over the hill beside the tank")
    beside = (x + args.radius * 0.5, y)
    set_workspace(client, {"terrain_brush": {"tool": "surface", "surface": SURFACE}})
    stroke(client, path, "surface", line_across(beside[0], beside[1], args.radius * 0.25))
    painted = sample(client, path, beside[0], beside[1]).get("ground", {})
    expect(int(painted.get("surface", -1)) == SURFACE, f"the ground beside the tank is not surface {SURFACE}: {painted}")

    say("6. Tiles: a stamp beside the hill on the map, turned")
    set_workspace(client, {"terrain_brush": {"tool": "stamp", "cell": args.cell}})
    px, py = map_pin(client, path, tank)
    laid_before = int(terrain(client, path, "tiles").get("count", 0))
    request(client, "edit_in_viewport", path=path,
            drop={"reference": "tile", "name": str(args.cell), "at": [px + 40.0, py], "kind": "map"})
    laid = int(terrain(client, path, "tiles").get("count", 0))
    expect(laid == laid_before + 1, f"no tile was laid ({laid_before} -> {laid})")
    # The stamp is the placement's last tile (drawn over every other), read through the .til's own document (the
    # tiles op's list of a shipped placement is long).
    rows = mission_rows(client, files["tiles"], "Tile")
    expect(len(rows) == laid, f"the .til's document shows {len(rows)} tiles, not {laid}")
    stamped = int(rows[-1]["id"])
    fields = fields_of(query(client, "record", path=files["tiles"], id=stamped))
    expect(int(fields.get("tile", -1)) == args.cell, f"the laid tile is not cell {args.cell}: {fields}")
    orientation = int(fields.get("orientation", 0))
    request(client, "edit_in_viewport", path=path,
            command={"name": "tile_rotate", "ids": [stamped], "by": [1], "kind": "map"})
    turned = fields_of(query(client, "record", path=files["tiles"], id=stamped))
    expect(int(turned.get("orientation", 0)) != orientation, f"the tile did not turn: {turned}")
    # The middle of the square the game draws it over: the record's corner its north-west (the .til's y the north edge).
    tile_at = (float(fields["x"]) + 8.0, float(fields["y"]) - 8.0)
    shoot(args, client, path, tank, tile_at, "2_tiles", map_background="colour")

    say("7. four Undos in the mission: the four files as loaded")
    paths = [files["heights"], files["surface"], files["tiles"], path]
    for _ in range(4):
        undone = request(client, "undo", path=path)
        say(f"   undid in {undone.get('outcome', {}).get('documents', [])}")
    for each in paths:
        expect(not document_state(client, each).get("dirty", False), f"{each} is not back to what was loaded")
    say("8. four Redos, one Save")
    for _ in range(4):
        request(client, "redo", path=path)
    saved = request(client, "save", path=path)
    say(f"   {saved.get('status', '')}")
    for each in paths:
        expect(not document_state(client, each).get("dirty", True), f"{each} was not saved with the mission")

    # Points on the raised ground about the tank, off its body, with the editor's height of each: the game's ground is
    # read at the first its ray finds the terrain at (an entity may stand on another).
    ground_points = []
    for dx, dy in ((-0.5, 0.0), (0.0, 0.4), (0.0, -0.4), (-0.3, 0.3), (-0.3, -0.3)):
        px_, py_ = x + dx * args.radius, y + dy * args.radius
        height_there = sample(client, path, px_, py_).get("height")
        if height_there is not None:
            ground_points.append((px_, py_, float(height_there)))

    if args.windowed and args.screenshots:
        set_workspace(client, {"terrain_brush": {"tool": "raise"}})
        shoot(args, client, path, tank, (x, y), "3_ring")
        shoot(args, client, path, tank, (x, y), "4_delta", map_background="delta")

    say("9. built")
    outcome, ended = raise_and_wait(client, {"kind": "build"}, args.timeout)
    expect(bool(ended) and ended.get("end") == "done", f"the build did not land: {json.dumps(outcome)} {json.dumps(ended)}")
    if args.play_mode == "strict":
        play_strict(args, client, files, started_pids)
    else:
        play_runtime(args, client, (x, y), beside, tile_at, ground_points, started_pids)
    say("11. every step held")


def play_runtime(args: argparse.Namespace, client: GameMcp, tank: tuple, beside: tuple, tile_at: tuple,
                 ground_points: list, started_pids: dict) -> None:
    say(f"10. OpenNova Play in {args.mission}; the game's ground read")
    play = {"kind": "play", "mission": args.mission}
    if os.name == "nt":
        play["behind"] = True
    outcome, ended = raise_and_wait(client, play, args.timeout)
    expect(bool(ended) and ended.get("end") == "done", f"Play's build did not land: {json.dumps(outcome)}")
    run_section = client.structured("editor_state", {"sections": ["run"]}).get("run", {})
    started_pids["game"] = int(run_section.get("pid", 0) or 0)
    game_port = int(run_section.get("mcp_port", 0))
    expect(run_section.get("state") == "running" and game_port > 0, f"the game is not running: {json.dumps(run_section)}")
    deadline = time.monotonic() + args.game_timeout
    while time.monotonic() < deadline and not port_open(game_port):
        time.sleep(1.0)
    game = GameMcp.for_port(game_port)
    # The mission loaded (its world and its ground) before the ground is read.
    shell: dict = {}
    while time.monotonic() < deadline:
        try:
            shell = game.structured("game_state").get("shell", {})
        except GameMcpError:
            shell = {}
        if shell.get("world_loaded") and str(shell.get("mission_file", "")).lower() == args.mission.lower():
            break
        time.sleep(1.0)
    expect(bool(shell.get("world_loaded")), f"the mission did not load within {args.game_timeout:.0f} s: {json.dumps(shell)}")
    under = probe(game, "terrain_ground", {"x": beside[0], "y": beside[1]}, args.game_timeout)
    expect(int(under.get("surface", -1)) == SURFACE, f"the game reads surface {under.get('surface')} beside the tank")
    height = None
    editor_height = None
    met: list = []
    for gx, gy, eh in ground_points:
        passed, data, _status = probe_run(game, "terrain_ground", {"x": gx, "y": gy}, args.game_timeout)
        if passed:
            height, editor_height = data.get("height"), eh
            break
        met.append(data.get("entity") or data.get("blocked") or "?")
    expect(height is not None, f"the game's ray met no terrain about the tank (it met {met})")
    expect(abs(float(height) - editor_height) < 1.0,
           f"the game's ground by the tank is {float(height):.2f}, the editor's {editor_height:.2f}")
    # The tiles the game loaded at the stamp, whatever its ray meets there.
    _passed, at_tile, _status = probe_run(game, "terrain_ground", {"x": tile_at[0], "y": tile_at[1]}, args.game_timeout)
    tiles = at_tile.get("tiles", [])
    expect(any(int(t.get("tile", -1)) == args.cell for t in tiles), f"the game reads no tile of cell {args.cell}: {tiles}")
    say(f"   the game: surface {under.get('surface')} beside the tank, the ground {float(height):.2f} by it "
        f"(the editor's {editor_height:.2f}), the tile there")


def play_strict(args: argparse.Namespace, client: GameMcp, files: dict, started_pids: dict) -> None:
    say("10. strict Play from the game install's executable over the build; its file log read")
    play = {"kind": "play"}
    if os.name == "nt":
        play["behind"] = True
    outcome, ended = raise_and_wait(client, play, args.timeout)
    expect(bool(ended) and ended.get("end") == "done", f"Play's build did not land: {json.dumps(outcome)}")
    run_section = client.structured("editor_state", {"sections": ["run"]}).get("run", {})
    started_pids["game"] = int(run_section.get("pid", 0) or 0)
    expect(run_section.get("ran_mode") == "strict" and "/FRISK" in run_section.get("command_line", ""),
           f"the game was not started as a player starts it: {json.dumps(run_section)}")
    deadline = time.monotonic() + args.install_run
    while time.monotonic() < deadline:
        if client.structured("editor_state", {"sections": ["run"]}).get("run", {}).get("state") != "running":
            break
        time.sleep(1.0)
    request(client, "stop_play")
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        run_section = client.structured("editor_state", {"sections": ["run"]}).get("run", {})
        if run_section.get("state") == "stopped" and run_section.get("file_log") is not None:
            break
        time.sleep(0.5)
    log = run_section.get("file_log") or {}
    read = [str(name).replace("\\", "/").lower() for name in log.get("from_archives", []) + log.get("from_disk", [])]
    expect(bool(read), f"the game's file log names nothing: {json.dumps(run_section)[:600]}")
    failed = [p for p in query(client, "problems", text="play.", limit=20).get("problems", [])
              if str(p.get("code", "")).startswith("play.") and p.get("severity") == "error"]
    expect(not failed, "the game reported: " + "; ".join(f"{p.get('code')}: {p.get('message')}" for p in failed))
    # The stock game names no mission on its command line (ADR 0046: "Neither game takes a place on its command
    # line"), so it reads the mission's files once a player starts the mission (and, on a run directory's first run,
    # answers its device dialog): within --install-run they are asserted where the log names any of them.
    names = [Path(each).name.lower() for each in files.values()]
    named = [name for name in names if any(entry.endswith(name) for entry in read)]
    if named:
        for name in names:
            expect(name in named, f"the game's file log names {named} but not {name}: {read[:40]}")
        say(f"   the game read {', '.join(names)} with no error")
    else:
        say(f"   the game booted on the build with no error ({len(read)} files read); the mission's files wait on a "
            f"player starting it (no command line names a mission)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install", required=True, help="the game install (its folder holds the three archives)")
    parser.add_argument("--mission", default="00TRa.bms", help="the shipped mission to import, edit and play")
    parser.add_argument("--project", default=None, help="where the project is made (default: a temp folder, removed after)")
    parser.add_argument("--radius", type=float, default=20.0, help="the brush's radius, metres")
    parser.add_argument("--cell", type=int, default=3, help="the tile set's cell the stamp lays")
    parser.add_argument("--port", type=int, default=editor_mcp.DEFAULT_PORT, help="the editor's --mcp-port")
    parser.add_argument("--editor", default=None, help="a packaged opennova-editor.exe (default: the source project)")
    parser.add_argument("--godot", default=None, help="Godot 4.6.1 binary (default: GODOT_BIN, else .godot-bin/)")
    parser.add_argument("--windowed", action="store_true", help="a windowed editor (headless by default)")
    parser.add_argument("--screenshots", default=None, help="with --windowed: a folder for the editor's screenshots")
    parser.add_argument("--timeout", type=float, default=1800.0, help="seconds an import, a bake, a build may take")
    parser.add_argument("--game-timeout", dest="game_timeout", type=float, default=300.0,
                        help="seconds the mission may take to load in the game")
    parser.add_argument("--keep", action="store_true", help="keep the project folder (a temp one is removed otherwise)")
    parser.add_argument("--play-mode", dest="play_mode", choices=("runtime", "strict"), default=None,
                        help="how Play runs the game (default: the OpenNova runtime)")
    parser.add_argument("--install-run", dest="install_run", type=float, default=30.0,
                        help="with --play-mode strict: seconds the game runs before it is stopped")
    args = parser.parse_args(argv)

    made_temp = args.project is None
    holder = Path(tempfile.mkdtemp(prefix="opennova-terrain-e2e-")) if made_temp else None
    project = (holder / "project") if holder else Path(args.project).resolve()
    if project.exists() and any(project.iterdir()):
        print(f"editor_terrain_e2e: {project} is not empty", file=sys.stderr)
        return EXIT_NOT_READ
    pid_file = (holder if holder else project.parent) / "editor_terrain_e2e.pid"
    code = EXIT_OK
    started_pids: dict = {}
    try:
        run(args, project, pid_file, started_pids)
    except StepFailed as failure:
        print(f"editor_terrain_e2e: FAILED: {failure}", file=sys.stderr)
        code = EXIT_NOT_DONE
    except GameMcpError as error:
        print(f"editor_terrain_e2e: {error}", file=sys.stderr)
        code = error.code if error.code != EXIT_NOT_DONE else EXIT_NOT_READ
    finally:
        editor_pid = started_pids.get("editor") or read_pid(pid_file)
        try:
            editor_mcp.cmd_stop(argparse.Namespace(port=args.port, url=None, pid=editor_pid, pid_file=str(pid_file),
                                                   timeout=60.0))
        except GameMcpError as error:
            print(f"editor_terrain_e2e: stopping the editor: {error}", file=sys.stderr)
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
