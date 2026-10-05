"""The unrecorded stages around the takes (scripts/demo/README.md): the welcome page's recents made,
the demo project reset before a retake of take A, and its open documents closed before a take B.
Each runs its own editor (no movie) in the work folder's isolated APPDATA, quits it and returns."""

from __future__ import annotations

from pathlib import Path

from demo_lib import Editor, Storyboard, Workspace

PREP_PORT = 8993


def same_path(a: str | Path, b: str | Path) -> bool:
    return Path(a).resolve() == Path(b).resolve()


def prep(sb: Storyboard, work: Workspace, editor_exe: str, install: str, port: int = PREP_PORT) -> None:
    """Earlier projects for the welcome page's recents, each made from the game install (which the
    editor then remembers), in the demo's own settings folder. A recent whose folder exists is kept."""
    ed = Editor(editor_exe, port, work, name="prep", size=sb.size, fps=sb.fps)
    try:
        ed.launch()
        for recent in sb.recents:
            folder = work.projects / recent["dir"]
            if folder.exists():
                print(f"kept {folder}")
                continue
            extra = {key: recent[key] for key in ("expansion", "builds_on") if key in recent}
            ed.req("new_project", wait=True, dir=folder.as_posix(), title=recent["title"], game_install=install,
                   **extra)
            ed.req("close_project")
        print("recents:", [r["title"] for r in ed.state("preferences")["preferences"]["recent_projects"]])
    finally:
        ed.stop()


def reset(sb: Storyboard, work: Workspace, editor_exe: str, port: int = PREP_PORT) -> None:
    """Before a retake of take A: the demo project off the recents and its folder deleted."""
    project_dir = work.projects / sb.project["dir"]
    ed = Editor(editor_exe, port, work, name="prep", size=sb.size, fps=sb.fps)
    try:
        ed.launch()
        if ed.state("project")["project"].get("open"):
            ed.req("close_project")
        for recent in ed.state("preferences")["preferences"]["recent_projects"]:
            if same_path(recent["root"], project_dir):
                ed.req("forget_recent", dir=recent["root"])
        print("recents:", [r["title"] for r in ed.state("preferences")["preferences"]["recent_projects"]])
    finally:
        ed.stop()
    freed = work.remove(project_dir)
    print(f"reset: {project_dir} deleted ({freed / 1e6:.0f} MB)")


def tidy(sb: Storyboard, work: Workspace, editor_exe: str, port: int = PREP_PORT) -> None:
    """Before a take B: the demo project's open documents closed and their unsaved edits discarded, so
    the take's launch opens on an empty workspace."""
    project_dir = work.projects / sb.project["dir"]
    if not project_dir.is_dir():
        raise SystemExit(f"{project_dir} does not exist: record take a first")
    ed = Editor(editor_exe, port, work, name="prep", size=sb.size, fps=sb.fps, project=project_dir,
                discard_on_quit=True)
    try:
        ed.launch(timeout=1800)
        docs = ed.query("documents", limit=100).get("documents", [])
        print("open:", [(doc["path"], doc["dirty"]) for doc in docs])
        for doc in docs:
            ed.req("close_document", check=False, path=doc["path"])
            if (ed.state("dialogs")["dialogs"].get("unsaved_prompt") or {}).get("open"):
                ed.req("resolve_unsaved", choice="discard")
        print("left open:", [doc["path"] for doc in ed.query("documents", limit=100).get("documents", [])])
    finally:
        ed.stop()
