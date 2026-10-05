"""The cut (scripts/demo/README.md): the edit list from storyboard.json and the recorded takes' marks,
the frames linked in order into one numbered sequence, an ASS caption track, and ffmpeg's x264 encode
with the file card's wave from its take's own audio; then the GIF from the captioned MP4, and the raw
frames deleted once encoded.

Each scene is cut from the newest recorded take (frames/<name>/take.json) that recorded its part and
holds every mark its pieces name, so a retake of some parts (`record b --only build,card`) replaces
just those scenes.
"""

from __future__ import annotations

import ctypes
import json
import math
import os
import shutil
import subprocess
from pathlib import Path

from demo_lib import EM_DASH, Storyboard, Workspace, mark_ref

FONT_FILES = ("seguisb.ttf", "segoeui.ttf")  # the captions' Segoe UI Semibold, and its regular
CAPTION_STYLE = ("Style: Cap,Segoe UI Semibold,31,&H00FFFFFF,&H00FFFFFF,&H38101418,&H38101418,0,0,0,0,100,100,0,0,"
                 "3,9,0,2,40,40,26,1")  # white on a translucent dark box, bottom centre
CSIDL_FONTS = 0x0014


class Recorded:
    """A recorded take's folder: its take.json and the first frame of each mark."""

    def __init__(self, folder: Path):
        self.dir = folder
        self.meta = json.loads((folder / "take.json").read_text(encoding="utf-8"))
        self.marks: dict[str, int] = {}
        marks_path = folder / "marks.json"
        if marks_path.is_file():
            for entry in json.loads(marks_path.read_text(encoding="utf-8")):
                self.marks.setdefault(entry["label"], entry["frame"])

    def frame(self, ref: str) -> int | None:
        label, offset = mark_ref(ref)
        return self.marks[label] + offset if label in self.marks else None

    def has_frame(self, frame: int) -> bool:
        return (self.dir / f"frame{frame:08d}.png").is_file()


def recorded_takes(work: Workspace) -> list[Recorded]:
    """Every recorded (not rehearsed) take, newest first."""
    takes = []
    if work.frames.is_dir():
        for folder in work.frames.iterdir():
            if (folder / "take.json").is_file():
                take = Recorded(folder)
                if not take.meta.get("rehearse"):
                    takes.append(take)
    takes.sort(key=lambda take: take.meta.get("started", 0), reverse=True)
    return takes


def source_of(scene: dict, takes: list[Recorded]) -> tuple[Recorded | None, str]:
    """The newest take that recorded the scene's part and holds every mark, frame and still its cut
    names; else None and why."""
    why = f"no recorded take {scene['take']} has the part {scene['part']}"
    for take in takes:
        if take.meta.get("take") != scene["take"] or scene["part"] not in take.meta.get("parts", []):
            continue
        lacking = []
        for piece in scene["cut"]:
            if "still" in piece:
                if not (take.dir / piece["still"]).is_file():
                    lacking.append(piece["still"])
                continue
            start, end = take.frame(piece["from"]), take.frame(piece["to"])
            for ref, frame in ((piece["from"], start), (piece["to"], end)):
                if frame is None:
                    lacking.append(f"the mark {mark_ref(ref)[0]}")
            if start is not None and end is not None and not (take.has_frame(start) and take.has_frame(end - 1)):
                lacking.append(f"frames {start}..{end - 1} (cleaned?)")
        if not lacking:
            return take, ""
        why = f"{take.dir.name} lacks {', '.join(lacking)}"
    return None, why


def edit_list(sb: Storyboard, work: Workspace, partial: bool = False) -> dict:
    """The storyboard's scenes as frame ranges of the recorded takes: each piece with its step (a
    speed-up to the piece's `seconds`), its hold and its scene's caption."""
    takes = recorded_takes(work)
    segments: list[dict] = []
    sources: dict[str, Recorded] = {}
    skipped: list[str] = []
    for scene in sb.scenes:
        take, why = source_of(scene, takes)
        if take is None:
            if not partial:
                raise SystemExit(f"scene {scene['id']}: {why} (record it, or cut with --partial)")
            print(f"skipped scene {scene['id']}: {why}")
            skipped.append(scene["id"])
            continue
        sources[scene["id"]] = take
        for piece in scene["cut"]:
            segment: dict = {"scene": scene["id"], "caption": scene["caption"]}
            if "still" in piece:
                segment.update(still=str(take.dir / piece["still"]), seconds=float(piece["seconds"]))
            else:
                start, end = take.frame(piece["from"]), take.frame(piece["to"])
                if end <= start:
                    raise SystemExit(f"scene {scene['id']}: {piece['to']} (frame {end}) is not after {piece['from']} "
                                     f"(frame {start}) in {take.dir.name}")
                segment.update({"dir": str(take.dir), "from": start, "to": end})
                if "seconds" in piece:
                    segment["step"] = max(1, math.ceil((end - start) / (float(piece["seconds"]) * sb.fps)))
                if piece.get("hold"):
                    segment["hold_last"] = float(piece["hold"])
            segments.append(segment)
        print(f"{scene['id']:12s} <- {take.dir.name}")
    cut = {"fps": sb.fps, "size": list(sb.size), "crf": sb.data.get("crf", 20), "mp4": sb.data["mp4"],
           "segments": segments, "skipped": skipped}
    audio = sb.data.get("audio")
    if audio and audio["scene"] in sources:
        take = sources[audio["scene"]]
        frame = take.frame(audio["mark"])
        wav = take.dir / "frame.wav"
        if frame is not None and wav.is_file():
            cut["audio"] = {"wav": str(wav), "dir": str(take.dir), "frame": frame,
                            "lead": float(audio.get("lead", 0.3)), "seconds": float(audio["seconds"])}
        else:
            print(f"no audio: {take.dir.name} has no {audio['mark']} mark or no frame.wav")
    return cut


def link(src: Path, dst: Path) -> None:
    """A hard link (no copy of the frame); a copy where links are refused."""
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def ass_time(seconds: float) -> str:
    cs = int(round(seconds * 100))
    h, cs = divmod(cs, 360000)
    m, cs = divmod(cs, 6000)
    s, cs = divmod(cs, 100)
    return f"{h}:{m:02d}:{s:02d}.{cs:02d}"


def link_sequence(cut: dict, work: Workspace) -> dict:
    """The segments' frames linked in order into cut/seq/NNNNNN.png. A segment's caption shows over the
    whole segment; a still is scaled to the frame. Returns the captions, each scene's span in the MP4
    and where each source frame landed."""
    from PIL import Image  # the cut's one non-stdlib need

    fps = cut["fps"]
    width, height = cut["size"]
    seq = work.cut / "seq"
    if seq.exists():
        work.remove(seq)
    seq.mkdir(parents=True)
    n = 0
    where: dict[tuple[str, int], int] = {}  # (take folder, source frame) -> the output frame it first landed at
    captions: list[list] = []  # [start_s, end_s, text]
    scenes: dict[str, list[float]] = {}
    for index, segment in enumerate(cut["segments"]):
        start = n / fps
        if "still" in segment:
            image = Image.open(segment["still"]).convert("RGB")
            if image.size != (width, height):
                image = image.resize((width, height), Image.Resampling.LANCZOS)
            still = work.cut / f"still_{index}.png"
            image.save(still)
            for _ in range(int(round(segment["seconds"] * fps))):
                link(still, seq / f"{n:06d}.png")
                n += 1
        else:
            source = Path(segment["dir"])
            step = segment.get("step", 1)
            frame = segment["from"]
            while frame < segment["to"]:
                link(source / f"frame{frame:08d}.png", seq / f"{n:06d}.png")
                for k in range(frame, frame + step):
                    where.setdefault((segment["dir"], k), n)
                n += 1
                frame += step
            last = source / f"frame{segment['to'] - 1:08d}.png"
            for _ in range(int(round(segment.get("hold_last", 0) * fps))):
                link(last, seq / f"{n:06d}.png")
                n += 1
        end = n / fps
        text = segment.get("caption")
        if text and captions and captions[-1][2] == text and abs(captions[-1][1] - start) < 1e-6:
            captions[-1][1] = end
        elif text:
            captions.append([start, end, text])
        span = scenes.setdefault(segment["scene"], [start, end])
        span[1] = end
    print(f"{n} frames, {n / fps:.1f} s")
    return {"frames": n, "captions": captions, "scenes": scenes, "where": where}


def write_captions(cut: dict, work: Workspace, captions: list[list]) -> Path:
    """The ASS caption track: one line per scene, white on a translucent dark box."""
    width, height = cut["size"]
    lines = ["[Script Info]", "ScriptType: v4.00+", f"PlayResX: {width}", f"PlayResY: {height}", "WrapStyle: 2", "",
             "[V4+ Styles]",
             "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
             "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
             "Alignment, MarginL, MarginR, MarginV, Encoding", CAPTION_STYLE, "", "[Events]",
             "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text"]
    for start, end, text in captions:
        if EM_DASH in text:
            raise SystemExit(f"no em dashes in captions: {text}")
        lines.append(f"Dialogue: 0,{ass_time(start)},{ass_time(end)},Cap,,0,0,0,,{text}")
    path = work.cut / "captions.ass"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def system_fonts_dir() -> Path | None:
    """The system's fonts folder (Windows: the shell's CSIDL_FONTS); None elsewhere."""
    if os.name != "nt":
        return None
    buffer = ctypes.create_unicode_buffer(260)
    if ctypes.windll.shell32.SHGetFolderPathW(None, CSIDL_FONTS, None, 0, buffer) != 0:
        return None
    return Path(buffer.value)


def caption_fonts(work: Workspace, fonts_dir: str | None) -> bool:
    """The caption font's files copied into cut/fonts for libass; False when none was found (libass then
    picks a font itself)."""
    source = Path(fonts_dir) if fonts_dir else system_fonts_dir()
    target = work.cut / "fonts"
    target.mkdir(parents=True, exist_ok=True)
    copied = 0
    for name in FONT_FILES:
        if source and (source / name).is_file():
            shutil.copy(source / name, target)
            copied += 1
    if not copied:
        print(f"caption font files {', '.join(FONT_FILES)} not found; pass --fonts-dir")
    return copied > 0


def encode(sb: Storyboard, work: Workspace, ffmpeg: str, partial: bool = False, fonts_dir: str | None = None) -> Path:
    """The captioned MP4: x264, the storyboard's CRF, yuv420p, the card's wave from its take's audio."""
    work.cut.mkdir(parents=True, exist_ok=True)
    work.out.mkdir(parents=True, exist_ok=True)
    cut = edit_list(sb, work, partial)
    sequence = link_sequence(cut, work)
    fps = cut["fps"]
    write_captions(cut, work, sequence["captions"])
    (work.cut / "captions.json").write_text(json.dumps(sequence["captions"], indent=1), encoding="utf-8")
    (work.cut / "scenes.json").write_text(json.dumps({"fps": fps, "seconds": sequence["frames"] / fps,
                                                      "scenes": sequence["scenes"]}, indent=1), encoding="utf-8")
    mp4 = work.out / cut["mp4"]
    cmd = [ffmpeg, "-y", "-hide_banner", "-loglevel", "warning", "-framerate", str(fps), "-i",
           os.path.join("seq", "%06d.png")]
    audio = cut.get("audio")
    if audio:
        landed = sequence["where"].get((audio["dir"], audio["frame"]))
        if landed is None:
            print(f"no audio: frame {audio['frame']} of {audio['dir']} is not in the cut")
            audio = None
        else:
            audio["at"] = max(0.0, landed / fps - audio["lead"])
            audio["ss"] = max(0.0, audio["frame"] / fps - audio["lead"])
            print(f"audio at {audio['at']:.2f} s, from {audio['ss']:.2f} s of the take")
            cmd += ["-ss", f"{audio['ss']:.3f}", "-t", str(audio["seconds"]), "-i", audio["wav"]]
    (work.cut / "cut.json").write_text(json.dumps(cut, indent=1), encoding="utf-8")
    video_filter = "ass=captions.ass" + (":fontsdir=fonts" if caption_fonts(work, fonts_dir) else "")
    cmd += ["-vf", video_filter, "-c:v", "libx264", "-preset", "slow", "-crf", str(cut["crf"]), "-pix_fmt", "yuv420p",
            "-r", str(fps)]
    if audio:
        delay = int(audio["at"] * 1000)
        cmd += ["-map", "0:v", "-map", "1:a", "-af", f"adelay={delay}|{delay},apad", "-c:a", "aac", "-b:a", "96k",
                "-shortest"]
    cmd += ["-movflags", "+faststart", str(mp4)]
    print(" ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=str(work.cut), check=True)
    print(f"{mp4}: {mp4.stat().st_size / 1e6:.1f} MB, {sequence['frames'] / fps:.1f} s")
    return mp4


def gif_ranges(sb: Storyboard, work: Workspace) -> list[tuple[float, float]]:
    """The storyboard's GIF ranges as seconds of the MP4, from the spans the cut wrote."""
    spans_path = work.cut / "scenes.json"
    if not spans_path.is_file():
        raise SystemExit(f"{spans_path} is missing: run the cut stage first")
    spans = json.loads(spans_path.read_text(encoding="utf-8"))["scenes"]
    ranges = []
    for span in sb.data["gif"]["ranges"]:
        first, last = span["from"], span.get("to", span["from"])
        if first not in spans or last not in spans:
            print(f"skipped the GIF range {first}..{last}: not in the cut")
            continue
        start = spans[first][0] + float(span.get("skip", 0))
        end = start + float(span["seconds"]) if "seconds" in span else spans[last][1]
        ranges.append((round(start, 3), round(end, 3)))
    return ranges


def gif(sb: Storyboard, work: Workspace, ffmpeg: str) -> Path:
    """The README GIF: the ranges of the captioned MP4 joined, scaled, on one palette."""
    spec = sb.data["gif"]
    mp4 = work.out / sb.data["mp4"]
    if not mp4.is_file():
        raise SystemExit(f"{mp4} is missing: run the cut stage first")
    ranges = gif_ranges(sb, work)
    if not ranges:
        raise SystemExit("no GIF range is in the cut")
    parts = [f"[0:v]trim=start={a}:end={b},setpts=PTS-STARTPTS[v{i}]" for i, (a, b) in enumerate(ranges)]
    chain = "".join(f"[v{i}]" for i in range(len(ranges)))
    graph = (";".join(parts) + f";{chain}concat=n={len(ranges)}:v=1:a=0,fps={spec['fps']},"
             f"scale={spec['width']}:-1:flags=lanczos,split[s0][s1];"
             f"[s0]palettegen=max_colors={spec['colors']}:stats_mode=diff[p];"
             f"[s1][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle")
    out = work.out / spec["file"]
    subprocess.run([ffmpeg, "-y", "-hide_banner", "-loglevel", "warning", "-i", str(mp4), "-filter_complex", graph,
                    "-loop", "0", str(out)], check=True)
    total = sum(b - a for a, b in ranges)
    print(f"{out}: {out.stat().st_size / 1e6:.1f} MB, {total:.1f} s")
    return out


def clean(work: Workspace, projects: bool = False) -> None:
    """The raw frames and waves deleted (each take's marks.json, take.json and game.png kept), and the
    cut's linked sequence, stills and fonts; with `projects`, the projects too. The outputs stay."""
    freed = 0
    if work.frames.is_dir():
        for folder in work.frames.iterdir():
            if not folder.is_dir():
                continue
            work.inside(folder)
            for path in list(folder.glob("frame*.png")) + list(folder.glob("frame*.wav")):
                freed += path.stat().st_size
                path.unlink()
    if work.cut.is_dir():
        freed += work.remove(work.cut / "seq") + work.remove(work.cut / "fonts")
        for still in work.cut.glob("still_*.png"):
            freed += work.remove(still)
    if projects:
        freed += work.remove(work.projects)
    print(f"freed {freed / 1e9:.2f} GB")
    if work.out.is_dir():
        for path in sorted(work.out.iterdir()):
            print(f"kept {path}: {path.stat().st_size / 1e6:.1f} MB")
