"""The recorded takes (scripts/demo/README.md). Take A: the welcome page, a new project on jox01 and
one mission's closure imported. Take B, on the imported project: Files, the mission editor,
definitions, menus, models and animation, textures, build and Play, and the file card (closed at its end
through the workspace, as its X closes it).

Each part is a function; storyboard.json names a take's parts in the order it records them, and its
scenes are cut from the marks these functions lay down. What a part asks the editor for (the mission,
the truck, the menu label) is here; captions and cut points are the storyboard's.
"""

from __future__ import annotations

import base64
import json
import math
import time
from pathlib import Path

from demo_lib import Editor, Storyboard, Take, Workspace
from game_mcp import GameMcp, GameMcpError  # scripts/mcp, on the path demo_lib sets

MISSION_NAME = "CP10.bms"
MISSION = "missions/CP10.bms"
SCRIPT = "missions/CP10.wac"  # the mission's script when script_assist names none
TRUCK_SUFFIX = "#108"  # Drivable Transport Truck #108, in the village
VILLAGE_CAMERA = {"target": [252.0, -298.0, 2.0], "yaw": 35.0, "pitch": 22.0, "distance": 40.0}
EVENT_PREFIX = "Event 6:"
TYPED_LINES = ["if ssndead(108) then", "\tssnwave 108 radio1.wav 50", "endif"]
UNSERVED_TERRAIN = "nightisle"
ITEMS = "defs/ITEMS.DEF"
ITEM_NAME = "Drivable Transport Truck"
COPY_NAME = "Drivable Desert Truck"
WEAPONS = "defs/weapon.def"
WEAPON_PREFIX = "M4 - Auto"
MENU = "menus/options.mnu"
MENU_WINDOW = "BACK"
MENU_WORD = "RETURN"
MODEL = "models/Dblkhwk1.3di"
ANIMATION = "anims/FSldr01.adm"
CLIP_PREFIX = "run forward"
TEXTURED_MODEL = "models/mveg4b.3di"
TEXTURE = "textures/mveg4.dds"
CARD_FILE = "sounds/cmpv503.wav"


# --- helpers over the editor's queries -------------------------------------------------------------
def first(rows, what: str) -> dict:
    for row in rows:
        return row
    raise RuntimeError(f"not found: {what}")


def rows_of(ed: Editor, path: str) -> list[dict]:
    """Every row of a document, page by page."""
    out, offset = [], 0
    while offset is not None:
        page = ed.query("document", path=path, offset=offset, limit=200)
        out += page.get("rows", [])
        offset = page.get("next_offset")
    return out


def items_of(ed: Editor, path: str) -> list[dict]:
    """Every item a document's viewport draws, page by page."""
    out, offset = [], 0
    while offset is not None:
        page = ed.vp("items", path=path, offset=offset, limit=200)
        out += page.get("items", [])
        offset = page.get("next_offset")
    return out


def address(ed: Editor, path: str, record_id) -> dict:
    record = ed.query("record", path=path, id=record_id)
    return {"row": record["row"], "kind": record["kind"], "child": record["child"]}


def select(ed: Editor, path: str, record_id) -> None:
    ed.req("select_record", path=path, address=address(ed, path, record_id))


def ready(ed: Editor, path: str, kind: str | None = None) -> dict:
    return ed.wait_viewport_ready(path=path, kind=kind)


def ease(t: float) -> float:
    return 0.5 - 0.5 * math.cos(math.pi * t)


def glide(ed: Editor, path: str, start: dict, end: dict, steps: int) -> None:
    """The camera from `start` to `end` over `steps` requests (a frame or more each)."""
    for i in range(1, steps + 1):
        t = ease(i / steps)
        camera = {}
        for key in end:
            a, b = start[key], end[key]
            if isinstance(a, list):
                camera[key] = [x + (y - x) * t for x, y in zip(a, b)]
            else:
                camera[key] = a + (b - a) * t
        ed.vp("camera", path=path, camera=camera)


# --- take A ----------------------------------------------------------------------------------------
def part_project(t: Take) -> None:
    """The welcome page, its New project form filled (the workspace's new_project), the new project, the
    chooser, one mission's plan and its import."""
    ed, project = t.ed, t.sb.project
    t.mark("start", shot=False)
    t.hold(0.5)
    t.mark("welcome")
    t.hold(2.0)
    extra = {key: project[key] for key in ("expansion", "builds_on") if key in project}
    form = {"open": True, "title": project["title"], "dir": t.project_dir.as_posix(), "game_install": t.install,
            **extra}
    if "expansion" in project:
        form["as_expansion"] = True
    ed.req("set_workspace", workspace={"new_project": form})
    t.mark("form")
    t.hold(2.5)
    t.mark("new_project", shot=False)
    ed.req("new_project", wait=True, dir=t.project_dir.as_posix(), title=project["title"], game_install=t.install,
           **extra)
    t.mark("opened")
    t.hold(2.5)
    ed.req("apply_project_settings", settings={"mission": True})
    t.hold(0.3)
    t.mark("chooser", shot=False)
    ed.req("preview_install_import", wait=True)
    t.mark("chooser_shown")
    t.hold(2.5)
    t.mark("plan", shot=False)
    ed.req("preview_install_import", wait=True, names=[MISSION_NAME], with_dependencies=True)
    t.mark("plan_shown")
    t.hold(5.0)
    t.mark("import_start", shot=False)
    started = time.monotonic()
    ed.req("import_files", planned=True)
    operation = ed.follow_operation("import_phase", interval=0.5)
    t.mark("import_end", wall=round(time.monotonic() - started, 1), status=ed.state("status")["status"]["status"],
           last=str(operation.get("last_operation"))[:300])
    ed.wait_idle(timeout=3600)
    t.mark("idle", wall=round(time.monotonic() - started, 1))
    t.hold(2.5)


# --- take B ----------------------------------------------------------------------------------------
def part_files(t: Take) -> None:
    """The take's opening: the imported project's Files by kind, held while the view settles."""
    t.mark("start")
    t.hold(3.0)
    t.mark("files_by_kind")


def part_mission(t: Take) -> None:
    """A retail mission in 3D: daylight, a glide into the village, a truck picked and dragged, an event
    and the script edited, and the terrain set to one nothing serves (Problems), then undone."""
    ed = t.ed
    t.mark("mission_open", shot=False)
    ed.req("open_document", path=MISSION)
    ready(ed, MISSION)
    t.mark("mission_ready")
    t.hold(1.8)
    state = ed.vp("state", path=MISSION, limit=1)
    start_camera = {key: state["camera"][key] for key in ("target", "yaw", "pitch", "distance")}
    t.mark("daylight", shot=False)
    ed.vp("options", path=MISSION, options={"time": 11, "marks": {
        "buildings": False, "markers": False, "organics": False, "paths": False, "areas": False}})
    ready(ed, MISSION)
    t.hold(1.2)
    t.mark("glide", shot=False)
    glide(ed, MISSION, start_camera, VILLAGE_CAMERA, 50)
    ready(ed, MISSION)
    t.mark("glided")
    t.hold(0.6)
    truck = first((i for i in items_of(ed, MISSION) if i["kind"] == "item" and i["name"].endswith(TRUCK_SUFFIX)),
                  f"the item {TRUCK_SUFFIX} in {MISSION}")
    sx, sy = truck["screen"]
    # The canvas's own click at the truck's mark (the viewport's click command), as a person's click picks.
    t.mark("pick", shot=False)
    ed.vp("command", path=MISSION, command={"name": "click", "at": [sx, sy - 12]})
    primary = (ed.state("selection")["selection"].get("primary") or {}).get("row")
    if primary != address(ed, MISSION, truck["id"])["row"]:
        select(ed, MISSION, truck["id"])  # the click landed on another mark: the truck chosen by its record
    t.hold(1.8)
    t.mark("picked")
    t.mark("drag", shot=False)
    # One gesture: 18 samples, one undo step.
    begun = ed.vp("drag", path=MISSION, drag={"id": truck["id"], "handle": "move", "by": [7, 1], "end": False})
    gesture = begun["outcome"]["gesture"]
    for i in range(17):
        ed.vp("drag", path=MISSION, drag={"id": truck["id"], "handle": "move", "by": [7, 1], "gesture": gesture,
                                          "end": i == 16})
    t.hold(1.2)
    t.mark("dragged")
    rows = rows_of(ed, MISSION)
    event = first((r for r in rows if r["kind_label"] == "Event" and (r.get("title") or "").startswith(EVENT_PREFIX)),
                  f"{EVENT_PREFIX} in {MISSION}")
    t.mark("event", shot=False, title=event.get("title"))
    select(ed, MISSION, event["id"])
    t.hold(1.6)
    t.mark("event_selected")
    ed.req("edit_record", path=MISSION, edits=[{"op": "set", "id": event["id"], "field": "delay", "value": 10}])
    t.hold(1.8)
    t.mark("event_edited")
    script = ed.query("script_assist", path=MISSION, op="mission_script").get("path") or SCRIPT
    t.mark("script", shot=False)
    ed.req("open_document", path=script)
    t.hold(1.0)
    at_line = ed.query("document", path=script, limit=1).get("line_count") or 18
    for text_line in TYPED_LINES:
        # Typed three characters to an edit, coalesced into one undo step.
        at_column = 1
        for i in range(0, len(text_line), 3):
            piece = text_line[i:i + 3]
            ed.req("edit_record", path=script, edits=[{"op": "apply", "payload": "text.span", "line": at_line,
                                                       "column": at_column, "length": 0, "text": piece,
                                                       "coalesce": True}])
            at_column += len(piece)
        ed.req("edit_record", path=script, edits=[{"op": "apply", "payload": "text.span", "line": at_line,
                                                   "column": at_column, "length": 0, "text": "\r\n",
                                                   "coalesce": True}])
        at_line += 1
    ed.req("end_edit", path=script)
    ed.wait_idle()
    t.hold(1.2)
    # The words of the command typed, shown at its place as a hover shows them (the script viewport's assist).
    ed.vp("options", path=script, options={"assist": {"op": "hover", "line": at_line - 2, "column": 3}}, check=False)
    t.hold(1.8)
    t.mark("script_typed")
    ed.vp("options", path=script, options={"assist": {"op": "none"}}, check=False)
    t.mark("blocks", shot=False)
    ed.req("open_document", path=MISSION)
    ed.req("edit_record", path=MISSION, edits=[{"op": "set", "id": rows[0]["id"], "field": "terrain",
                                                "value": UNSERVED_TERRAIN}])
    ed.wait_idle()
    t.hold(2.8)
    t.mark("blocking", gate=ed.query("build_gate", limit=1).get("refusal"))
    ed.req("undo", path=MISSION)
    ed.wait_idle()
    t.hold(0.6)
    t.mark("mission_end")


def part_defs(t: Take) -> None:
    """An item in words, weapons by their game text, then the item duplicated, renamed and saved."""
    ed = t.ed
    t.mark("defs", shot=False)
    ed.req("open_document", path=ITEMS)
    item = first((r for r in rows_of(ed, ITEMS) if r.get("name") == ITEM_NAME), f"{ITEM_NAME} in {ITEMS}")
    select(ed, ITEMS, item["id"])
    t.hold(2.4)
    t.mark("item_selected")
    ed.req("open_document", path=WEAPONS)
    weapons = [r for r in rows_of(ed, WEAPONS)
               if (r.get("name") or "").startswith(WEAPON_PREFIX) or (r.get("title") or "").startswith(WEAPON_PREFIX)]
    if weapons:
        select(ed, WEAPONS, weapons[0]["id"])
    t.hold(2.2)
    t.mark("weapons")
    ed.req("open_document", path=ITEMS)
    select(ed, ITEMS, item["id"])
    copy = ed.req("duplicate", path=ITEMS)["outcome"]["added"][0]
    t.hold(0.8)
    ed.req("edit_record", path=ITEMS, edits=[{"op": "set", "id": copy, "field": "display_name", "value": COPY_NAME}])
    t.hold(1.2)
    t.mark("duplicated")
    ed.req("save", path=ITEMS)
    ed.wait_idle()
    t.hold(1.6)
    t.mark("saved")


def part_menus(t: Take) -> None:
    """A menu on the canvas, a window's style variables resolved, its label typed letter by letter."""
    ed = t.ed
    t.mark("menus", shot=False)
    ed.req("open_document", path=MENU)
    ready(ed, MENU)
    tree = ed.query("menu_tree", path=MENU, limit=200)
    window = first((w for w in tree["screens"][0]["windows"] if w["name"] == MENU_WINDOW), f"{MENU_WINDOW} in {MENU}")
    select(ed, MENU, window["id"])
    ready(ed, MENU)
    t.hold(2.4)
    t.mark("menu_selected")
    window_address = address(ed, MENU, window["id"])
    for n in range(1, len(MENU_WORD) + 1):
        answer = ed.req("set_string_text", check=False, path=MENU, field="string.value", address=window_address,
                        values={"text": "{hot}" + MENU_WORD[:n]})
        if n == 1:
            t.mark("set_string_text", shot=False, status=answer.get("status"))
        t.hold(0.18)
    ready(ed, MENU)
    t.hold(1.8)
    t.mark("menu_typed")


def part_models(t: Take) -> None:
    """A model orbited, a material picked, its collision layers shown; then an animation clip."""
    ed = t.ed
    t.mark("model", shot=False)
    ed.req("open_document", path=MODEL)
    ed.vp("options", path=MODEL, options={"overlays": {"user_points": False, "lights": False}})
    ed.vp("camera", path=MODEL, camera={"distance": 23.0, "yaw": 0.6, "pitch": 0.3})
    ready(ed, MODEL)
    t.mark("model_ready")
    glide(ed, MODEL, {"yaw": 0.6}, {"yaw": 1.3}, 30)
    materials = ed.query("model_surfaces", path=MODEL, op="materials")
    select(ed, MODEL, materials["materials"][0]["id"])
    glide(ed, MODEL, {"yaw": 1.3}, {"yaw": 1.7}, 24)
    t.mark("material")
    ed.vp("options", path=MODEL, options={"overlays": {"volumes": True, "sections": True}})
    ready(ed, MODEL)
    glide(ed, MODEL, {"yaw": 1.7}, {"yaw": 2.6}, 45)
    t.mark("collision")
    ed.req("open_document", path=ANIMATION)
    clip = first((r for r in rows_of(ed, ANIMATION) if (r.get("title") or "").startswith(CLIP_PREFIX)),
                 f"{CLIP_PREFIX} in {ANIMATION}")
    select(ed, ANIMATION, clip["id"])
    ready(ed, ANIMATION)
    ed.vp("camera", path=ANIMATION, camera={"distance": 2.7, "yaw": 0.9, "pitch": 0.25})
    t.hold(3.2)
    t.mark("animation")


def part_textures(t: Take) -> None:
    """A texture field's thumbnail, the texture as the game reads it, then as drawn."""
    ed = t.ed
    t.mark("textures", shot=False)
    ed.req("open_document", path=TEXTURED_MODEL)
    materials = ed.query("model_surfaces", path=TEXTURED_MODEL, op="materials")
    material = ed.query("record", path=TEXTURED_MODEL, id=materials["materials"][0]["id"])
    select(ed, TEXTURED_MODEL, material["collections"][0]["records"][0]["id"])
    ready(ed, TEXTURED_MODEL)
    t.hold(2.0)
    t.mark("thumbnail")
    ed.req("open_document", path=TEXTURE)
    ready(ed, TEXTURE)
    t.hold(1.8)
    t.mark("texture")
    ed.vp("camera", path=TEXTURE, camera={"fit": False, "scale": 3.0, "x": 128, "y": 300})
    ready(ed, TEXTURE)
    t.hold(1.4)
    t.mark("zoomed")
    ed.vp("options", path=TEXTURE, options={"as_used": 0})
    ready(ed, TEXTURE)
    t.hold(2.0)
    t.mark("as_drawn")


def part_build(t: Take) -> None:
    """Save all, build (its phases marked), the result held; then Play, the game's own screenshot."""
    ed = t.ed
    t.mark("build", shot=False)
    ed.req("save_all")
    ed.wait_idle()
    ed.req("build")
    operation = ed.follow_operation("build_phase", interval=0.3)
    build = operation.get("build", {})
    t.hold(2.4)
    t.mark("built", ok=build.get("ok"), dir=build.get("dir"), status=ed.state("status")["status"]["status"])
    # The build result's panel closed (the workspace's build_result), the editor as it was under it.
    ed.req("set_workspace", check=False, workspace={"build_result": {"open": False}})
    t.hold(0.5)
    play(t)


def play(t: Take) -> None:
    """Play behind (`play {behind}`): the editor starts the game's window behind every other and keeps the
    foreground, so nothing of the person's work moves; this client waits for the `run` section to name
    its endpoint and for its world to load. The shot is the game's own `game_screenshot`."""
    ed = t.ed
    run: dict = {}
    shell: dict = {}
    ed.req("play", mission=MISSION_NAME, behind=True)
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        run = ed.state("run")["run"]
        if run.get("pid") and run.get("state") == "running" and run.get("mcp_port"):
            break
        time.sleep(0.1)
    else:
        raise RuntimeError(f"the game did not start: {json.dumps(run)[:500]}")
    game = GameMcp.for_port(run["mcp_port"], timeout=60)
    waited = time.monotonic()
    while time.monotonic() - waited < 180:
        try:
            shell = game.structured("game_state", {}, timeout=10).get("shell") or {}
        except GameMcpError:
            shell = {}
        if shell.get("world_loaded"):
            break
        time.sleep(0.5)
    t.mark("playing", mission=shell.get("mission_file"), loaded=bool(shell.get("world_loaded")))
    t.hold(1.5)
    shot = game.call("game_screenshot", {"format": "png", "max_dim": t.sb.size[0]})
    images = [block for block in shot.get("content", []) if block.get("type") == "image"]
    if images:
        (t.out_dir / "game.png").write_bytes(base64.b64decode(images[0]["data"]))
    t.mark("game_shot", shot=False, saved=bool(images))
    ed.call("editor_play", {"op": "stop"})
    t.hold(1.0)
    t.mark("play_end")


def part_card(t: Take) -> None:
    """A wave's file card, the wave playing. The open documents close first (saved by now), so the card
    stands over the workspace as it stood after the import."""
    ed = t.ed
    for doc in ed.query("documents", limit=100).get("documents", []):
        ed.req("close_document", check=False, path=doc["path"])
        if (ed.state("dialogs")["dialogs"].get("unsaved_prompt") or {}).get("open"):
            ed.req("resolve_unsaved", choice="discard" if t.rehearse else "save")
    t.hold(1.0)
    t.mark("card", shot=False)
    ed.req("about_file", path=CARD_FILE)
    t.hold(1.2)
    t.mark("play_sound")
    ed.req("play_sound", check=False, path=CARD_FILE)
    t.hold(3.0)
    t.mark("card_end")
    # The card closed (the workspace's card, "" closing it), which stops its sound.
    ed.req("set_workspace", check=False, workspace={"card": {"path": ""}})


PARTS = {
    "project": part_project,
    "files": part_files,
    "mission": part_mission,
    "defs": part_defs,
    "menus": part_menus,
    "models": part_models,
    "textures": part_textures,
    "build": part_build,
    "card": part_card,
}
# Take B's opening runs in every take (the opened project held while its view settles); it counts as
# the recorded `files` part only when asked for.
ALWAYS_RUN = {"files"}


def take_folder(sb: Storyboard, take_id: str, only: set[str], name: str | None) -> str:
    if name:
        return name
    if not only:
        return take_id
    return "-".join([take_id] + [part for part in sb.takes[take_id]["parts"] if part in only])


def record(sb: Storyboard, work: Workspace, take_id: str, editor_exe: str, install: str | None, rehearse: bool,
           only: set[str], name: str | None, replace: bool, port: int | None) -> Path:
    """Record (or rehearse) one take into frames/<name> (rehearse/<name>): the frames, marks.json and
    take.json, which the cut reads."""
    if take_id not in sb.takes:
        raise SystemExit(f"no take {take_id!r} in {sb.path}")
    take = sb.takes[take_id]
    unknown = only - set(take["parts"])
    if unknown:
        raise SystemExit(f"take {take_id} has no part {', '.join(sorted(unknown))}; its parts: "
                         f"{', '.join(take['parts'])}")
    missing = [part for part in take["parts"] if part not in PARTS]
    if missing:
        raise SystemExit(f"{sb.path} names parts no recorder has: {', '.join(missing)}")
    project_dir = work.projects / sb.project["dir"]
    makes_project = "project" in take["parts"]  # take A makes it; take B opens what take A made
    if makes_project and project_dir.exists():
        raise SystemExit(f"{project_dir} exists: run the reset stage before take {take_id}")
    if makes_project and not install:
        raise SystemExit(f"take {take_id} makes the project from the game install: pass --install")
    if not makes_project and not project_dir.is_dir():
        raise SystemExit(f"{project_dir} does not exist: record take a first")

    folder = take_folder(sb, take_id, only, name)
    out_dir = (work.rehearse if rehearse else work.frames) / folder
    if out_dir.exists() and any(out_dir.iterdir()):
        if not replace:
            raise SystemExit(f"{out_dir} holds an earlier take: pass --replace to delete it first, or --name "
                             f"for another folder")
        work.remove(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    editor = Editor(editor_exe, port or take["port"], work, name=f"take-{folder}",
                    frames_dir=None if rehearse else out_dir, size=sb.size, fps=sb.fps,
                    project=None if makes_project else project_dir, marks_path=out_dir / "marks.json",
                    discard_on_quit=rehearse)
    t = Take(editor, sb, work, out_dir, rehearse, only, install)
    wanted = [part for part in take["parts"] if t.want(part)]
    meta = {"take": take_id, "parts": wanted, "rehearse": rehearse, "fps": sb.fps, "size": list(sb.size),
            "started": time.time(), "complete": False}
    meta_path = out_dir / "take.json"
    meta_path.write_text(json.dumps(meta, indent=1), encoding="utf-8")
    try:
        editor.launch(timeout=240 if makes_project else 3600)
        for part in take["parts"]:
            if part in wanted or part in ALWAYS_RUN:
                print(f"--- {part}", flush=True)
                PARTS[part](t)
        t.mark("end", shot=False)
        meta["complete"] = True
    finally:
        editor.stop()
        meta.update(ended=time.time(), frames=editor.frames(), window_changes=editor.disturbed)
        meta_path.write_text(json.dumps(meta, indent=1), encoding="utf-8")
    print(f"take {take_id} -> {out_dir}: {editor.frames()} frames, {len(editor.disturbed)} window change(s)")
    if editor.disturbed:
        print("the window moved during the take (minimized, maximized or resized): check the marks before "
              "cutting from it")
    return out_dir
