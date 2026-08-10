"""Convert the menu backdrop movies (.bik) to Theora siblings (.ogv).

The runtime's MenuVideoUnderlay plays the witnessed backdrop slots (engine
policy: engine/runtime/menu/menu_video.h — main/header/footer.bik with the
expansion copy preferred), but Godot has no Bink decoder, so playback rides a
converted `.ogv` sibling next to each `.bik`. This command produces those
siblings for every copy in the install (root and each expansion/<name>/), and
the runtime keeps the witnessed expansion-first selection at play time.

Conversion needs an ffmpeg with Bink decode + Theora/Vorbis encode; the
optional `imageio-ffmpeg` package (extra: `video`) bundles one.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

# The three witnessed backdrop movie basenames (the slot table lives at the
# engine home, engine/runtime/menu/menu_video.h).
MENU_MOVIE_NAMES = ("main.bik", "header.bik", "footer.bik")


def find_menu_movie_sources(game_dir: str | Path) -> list[Path]:
    """Every backdrop .bik in the install: root copies plus each expansion's."""
    root = Path(game_dir)
    sources: list[Path] = []
    for name in MENU_MOVIE_NAMES:
        candidate = root / name
        if candidate.is_file():
            sources.append(candidate)
    expansion_root = root / "expansion"
    if expansion_root.is_dir():
        for exp_dir in sorted(expansion_root.iterdir()):
            if not exp_dir.is_dir():
                continue
            for name in MENU_MOVIE_NAMES:
                candidate = exp_dir / name
                if candidate.is_file():
                    sources.append(candidate)
    return sources


def _ffmpeg_exe() -> str | None:
    try:
        import imageio_ffmpeg
    except ImportError:
        return None
    return imageio_ffmpeg.get_ffmpeg_exe()


def convert_menu_movies(game_dir: str | Path, *, force: bool = False) -> int:
    """Convert every found backdrop .bik to an .ogv sibling. Returns exit code."""
    sources = find_menu_movie_sources(game_dir)
    if not sources:
        print(f"No menu movies (main/header/footer.bik) under {game_dir}.")
        return 1
    exe = _ffmpeg_exe()
    if exe is None:
        print(
            "menu-movies needs the bundled ffmpeg: install the importer's"
            " 'video' extra (imageio-ffmpeg), e.g."
            " `uv sync --extra video` or `pip install imageio-ffmpeg`."
        )
        return 1
    failed = 0
    converted = 0
    for source in sources:
        target = source.with_suffix(".ogv")
        if target.exists() and not force:
            print(f"  keep   {target} (exists; --force to redo)")
            continue
        cmd = [
            exe,
            "-y",
            "-i",
            str(source),
            "-c:v",
            "libtheora",
            "-q:v",
            "7",
            "-c:a",
            "libvorbis",
            "-q:a",
            "4",
            str(target),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0 or not target.is_file():
            failed += 1
            tail = (result.stderr or "").strip().splitlines()
            detail = tail[-1] if tail else "unknown ffmpeg failure"
            print(f"  FAIL   {source}: {detail}")
            continue
        converted += 1
        print(f"  wrote  {target}")
    print(f"{converted} converted, {failed} failed, {len(sources)} found.")
    return 0 if failed == 0 else 1
