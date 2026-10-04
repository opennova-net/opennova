#!/usr/bin/env python3
"""Record the OpenNova editor demo (scripts/demo/README.md).

A packaged Windows editor runs under Godot's Movie Maker, launched behind every other window and
driven through its MCP alone; the takes' frames are cut by storyboard.json into a captioned MP4 and a
README GIF. Everything a run writes lands under --work.

    python scripts/demo/record_demo.py prep     --work <work> --editor <editor.exe> --install <JO install>
    python scripts/demo/record_demo.py record a --work <work> --editor <editor.exe> --install <JO install>
    python scripts/demo/record_demo.py tidy     --work <work> --editor <editor.exe>
    python scripts/demo/record_demo.py record b --work <work> --editor <editor.exe> --rehearse
    python scripts/demo/record_demo.py record b --work <work> --editor <editor.exe>
    python scripts/demo/record_demo.py record b --work <work> --editor <editor.exe> --only build,card
    python scripts/demo/record_demo.py cut      --work <work> --ffmpeg <ffmpeg.exe>
    python scripts/demo/record_demo.py gif      --work <work> --ffmpeg <ffmpeg.exe>
    python scripts/demo/record_demo.py clean    --work <work>
    python scripts/demo/record_demo.py reset    --work <work> --editor <editor.exe>   # before retaking take A

Stdlib Python plus Pillow (the cut); a portable ffmpeg passed by path.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cut as cutter  # noqa: E402
import prep as preparation  # noqa: E402
import takes  # noqa: E402
from demo_lib import STORYBOARD, Storyboard, Workspace  # noqa: E402


def existing_file(text: str) -> str:
    path = Path(text).resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"no such file: {path}")
    return str(path)


def existing_dir(text: str) -> str:
    path = Path(text).resolve()
    if not path.is_dir():
        raise argparse.ArgumentTypeError(f"no such folder: {path}")
    return path.as_posix()  # as the editor's requests take a folder


def ffmpeg_of(text: str) -> str:
    found = shutil.which(text) or (str(Path(text).resolve()) if Path(text).is_file() else None)
    if not found:
        raise SystemExit(f"ffmpeg not found: {text} (pass --ffmpeg <ffmpeg executable>)")
    return found


def parts_of(text: str) -> set[str]:
    return {part.strip() for part in text.split(",") if part.strip()}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="record_demo.py", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    stages = parser.add_subparsers(dest="stage", required=True, metavar="stage")

    def stage(name: str, help_text: str, editor: bool = False, install: bool | None = None,
              ffmpeg: bool = False) -> argparse.ArgumentParser:
        sub = stages.add_parser(name, help=help_text, description=help_text)
        sub.add_argument("--work", required=True,
                         help="the work folder: the editor's APPDATA and TMP, the projects, the frames, the cut and "
                              "the outputs all land under it")
        sub.add_argument("--storyboard", default=str(STORYBOARD), help="the storyboard (default: %(default)s)")
        if editor:
            sub.add_argument("--editor", required=True, type=existing_file,
                             help="the packaged editor executable (editor/opennova-editor.exe of the "
                                  "opennova-editor-windows zip)")
        if install is not None:
            sub.add_argument("--install", required=install, type=existing_dir,
                             help="the retail game install whose data is imported (read, never written)")
        if ffmpeg:
            sub.add_argument("--ffmpeg", required=True,
                             help="the ffmpeg executable (a portable build with libx264 and libass)")
        return sub

    sub = stage("prep", "earlier projects for the welcome page's recents, made from the game install",
                editor=True, install=True)
    sub.add_argument("--port", type=int, default=preparation.PREP_PORT)

    sub = stage("reset", "before a retake of take A: the demo project off the recents and its folder deleted",
                editor=True)
    sub.add_argument("--port", type=int, default=preparation.PREP_PORT)

    sub = stage("tidy", "before a take B: the demo project's open documents closed, unsaved edits discarded",
                editor=True)
    sub.add_argument("--port", type=int, default=preparation.PREP_PORT)

    sub = stage("record", "record one take into <work>/frames/<name> (a rehearsal: <work>/rehearse/<name>)",
                editor=True, install=False)
    sub.add_argument("take", help="the storyboard's take: a (welcome, new project, import; needs --install) or b "
                                  "(the imported project's scenes)")
    sub.add_argument("--rehearse", action="store_true",
                     help="no movie: the editor's own screenshot at each mark instead, unsaved edits discarded at "
                          "the quit")
    sub.add_argument("--only", type=parts_of, default=set(),
                     help="comma-separated parts of the take to record (take b: files, mission, defs, menus, "
                          "models, textures, build, card)")
    sub.add_argument("--name", help="the take's folder (default: the take, plus the parts with --only)")
    sub.add_argument("--replace", action="store_true", help="delete an earlier take in that folder first")
    sub.add_argument("--port", type=int, help="the editor's MCP port (default: the storyboard's for the take)")

    sub = stage("cut", "the recorded takes cut by the storyboard into the captioned MP4 (<work>/out)", ffmpeg=True)
    sub.add_argument("--partial", action="store_true", help="skip the scenes no take has recorded")
    sub.add_argument("--fonts-dir", help="where the caption font's files are (default: the system's fonts folder)")

    stage("gif", "the README GIF from the MP4 (<work>/out), by the storyboard's ranges", ffmpeg=True)

    sub = stage("clean", "the raw frames and the cut's linked sequence deleted; the outputs kept")
    sub.add_argument("--projects", action="store_true", help="the projects deleted too")
    return parser


def main(argv: list[str] | None = None) -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = build_parser().parse_args(argv)
    sb = Storyboard(args.storyboard)
    work = Workspace(args.work)
    if args.stage == "prep":
        preparation.prep(sb, work, args.editor, args.install, args.port)
    elif args.stage == "reset":
        preparation.reset(sb, work, args.editor, args.port)
    elif args.stage == "tidy":
        preparation.tidy(sb, work, args.editor, args.port)
    elif args.stage == "record":
        takes.record(sb, work, args.take, args.editor, args.install, args.rehearse, args.only, args.name,
                     args.replace, args.port)
    elif args.stage == "cut":
        cutter.encode(sb, work, ffmpeg_of(args.ffmpeg), args.partial, args.fonts_dir)
    elif args.stage == "gif":
        cutter.gif(sb, work, ffmpeg_of(args.ffmpeg))
    elif args.stage == "clean":
        cutter.clean(work, args.projects)
    return 0


if __name__ == "__main__":
    sys.exit(main())
