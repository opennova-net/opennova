#!/usr/bin/env python3
"""Encode the homepage reel from numbered 1920x1080 PNG takes (30 fps).

Usage: python scripts/media/encode_editor_showcase.py <takes> --font <font.ttf>
Raw footage and project checkpoints stay outside Git. Requires ffmpeg and ffprobe.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FPS = 30
SHOTS = [
    ("01-overview", 90, "Build your own missions"),
    ("02-terrain", 90, "Shape terrain. Build worlds."),
    ("03-placement", 60, "Shape terrain. Build worlds."),
    ("04-patrols", 120, "Give units their orders"),
    ("05-logic", 120, "Create mission logic"),
    ("06-animation", 120, "Preview models and animations"),
    ("07-menu", 120, "Design game menus"),
    ("08-play", 180, "Bring it all into the game"),
]


def run(*args: str, cwd: Path | None = None) -> None:
    subprocess.run(args, cwd=cwd, check=True)


def probe(path: Path) -> dict:
    return json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path),
    ]))


def encode(takes: Path, output: Path, font: Path) -> None:
    # Fail before replacing any published asset if a take is missing or has the wrong size.
    for name, frames, _ in SHOTS:
        for i in range(frames):
            if not (takes / name / f"{i:04d}.png").is_file():
                raise ValueError(f"Missing frame: {name}/{i:04d}.png")
        stream = probe(takes / name / "0000.png")["streams"][0]
        if (stream["width"], stream["height"]) != (1920, 1080):
            raise ValueError(f"{name}: capture at 1920x1080")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="editor-showcase-") as temporary:
        work = Path(temporary)
        # Relative filter paths avoid FFmpeg's drive-letter and quoting rules.
        shutil.copyfile(font, work / "caption.ttf")
        for index, (name, frames, caption) in enumerate(SHOTS):
            (work / "caption.txt").write_text(caption, encoding="utf-8")
            filters = (
                "scale=1760:990:flags=lanczos,pad=1920:1080:80:0:color=0x10141b,"
                "drawbox=x=80:y=1018:w=4:h=38:color=0x8bba91:t=fill,"
                "drawtext=fontfile=caption.ttf:textfile=caption.txt:"
                "fontcolor=0xf0f3f6:fontsize=42:x=106:y=1015,setsar=1,format=yuv420p"
            )
            run("ffmpeg", "-v", "error", "-y", "-framerate", str(FPS), "-i",
                str(takes / name / "%04d.png"), "-frames:v", str(frames), "-an",
                "-vf", filters, "-c:v", "libx264", "-preset", "fast", "-crf", "14",
                f"part-{index}.mp4", cwd=work)
        (work / "parts.txt").write_text(
            "".join(f"file 'part-{i}.mp4'\n" for i in range(len(SHOTS))), encoding="utf-8")
        run("ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", "parts.txt",
            "-an", "-c", "copy", "master.mp4", cwd=work)
        run("ffmpeg", "-v", "error", "-y", "-i", "master.mp4", "-an", "-c:v", "libx264",
            "-preset", "slow", "-crf", "23", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
            "editor-showcase.mp4", cwd=work)
        run("ffmpeg", "-v", "error", "-y", "-i", "master.mp4", "-an", "-c:v", "libvpx-vp9",
            "-b:v", "0", "-crf", "32", "-row-mt", "1", "-cpu-used", "2",
            "editor-showcase.webm", cwd=work)
        run("ffmpeg", "-v", "error", "-y", "-ss", "1.5", "-i", "editor-showcase.mp4",
            "-frames:v", "1", "-c:v", "libwebp", "-quality", "88",
            "editor-showcase-poster.webp", cwd=work)
        for suffix in ("mp4", "webm"):
            video = work / f"editor-showcase.{suffix}"
            info = probe(video)
            streams = info["streams"]
            if len(streams) != 1 or streams[0]["codec_type"] != "video":
                raise ValueError(f"{suffix}: expected silent video")
            if abs(float(info["format"]["duration"]) - 30) > 0.05:
                raise ValueError(f"{suffix}: expected a 30-second reel")
            if video.stat().st_size > 8_000_000:
                raise ValueError(f"{suffix}: exceeds the 8 MB budget; adjust encoding quality")
        for name in ("editor-showcase.mp4", "editor-showcase.webm", "editor-showcase-poster.webp"):
            shutil.copyfile(work / name, output / name)
            print(f"{output / name}: {(output / name).stat().st_size:,} bytes")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("takes", type=Path)
    parser.add_argument("--font", type=Path, required=True, help="TrueType font for the captions")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "web/public/media")
    args = parser.parse_args()
    for binary in ("ffmpeg", "ffprobe"):
        if not shutil.which(binary):
            parser.error(f"{binary} must be on PATH")
    if not args.font.is_file():
        parser.error("--font must name an existing font file")
    encode(args.takes.resolve(), args.output_dir.resolve(), args.font.resolve())


if __name__ == "__main__":
    main()
