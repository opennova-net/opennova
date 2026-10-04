"""What retail's US01 clip set asks of each clip, read from the retail files
at build time (nothing here is stored in the repo):

    python art/on_person/retail_spec.py <opennova-3di> <reference dir> [--table]

`spec(cli, reference)` returns the table's rows and, per clip, its frames,
rate, flags, the mean ground step per frame (mission axes: x forward, y left)
and the frames its trigger bits fall on. build_clips.py fits every recreated
clip to it; --table prints it.
"""
import os
import subprocess
import sys
import tempfile

FLAG_LOOP = 0x1
FLAG_BIT3 = 0x8


def read_o3a(path):
    """The rows and clips of an .o3a clip-set text (opennova-3di anim scene)."""
    rows, clips, clip = [], [], None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            parts = _tokens(raw)
            if not parts:
                continue
            key = parts[0]
            if key == "row":
                rows.append((parts[1], parts[2:]))
            elif key == "clip":
                clip = {"name": parts[1], "fps": 30, "flags": 0, "frames": 0, "bones": 0, "events": []}
                clips.append(clip)
            elif clip is None:
                continue
            elif key in ("fps", "frames", "flags"):
                clip[key] = int(parts[1], 0)
            elif key == "bone":
                clip["bones"] += 1
            elif key == "event":
                clip["events"].append((tuple(float(x) for x in parts[1:4]), int(parts[4], 0),
                                       float(parts[5]), float(parts[6])))
    return rows, clips


def _tokens(line):
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i]
        if c in " \t\r\n":
            i += 1
        elif c == "#":
            break
        elif c == '"':
            j = line.index('"', i + 1)
            out.append(line[i + 1:j])
            i = j + 1
        else:
            j = i
            while j < n and line[j] not in " \t\r\n":
                j += 1
            out.append(line[i:j])
            i = j
    return out


def find(reference, name):
    wanted = name.lower()
    for entry in os.listdir(reference):
        if entry.lower() == wanted:
            return os.path.join(reference, entry)
    raise FileNotFoundError(f"{name} is not in {reference}")


def spec(cli, reference, table="US01.adm"):
    """(rows, clips): rows as [(key, [clip stem, ...])], clips keyed by lower
    stem: frames, fps, loop, bit3, bones, step (mean ground step per frame),
    speed (m/s), bottom and top ranges, triggers {bit: [frames]}."""
    with tempfile.TemporaryDirectory(prefix="us01_spec_") as tmp:
        o3a = os.path.join(tmp, "set.o3a")
        subprocess.run([cli, "anim", "scene", find(reference, table), "-o", o3a], check=True,
                       capture_output=True, text=True)
        rows, clips = read_o3a(o3a)
    out = {}
    for c in clips:
        frames = max(1, c["frames"])
        events = c["events"]
        steps = [e[0] for e in events[:frames]]
        sx = sum(s[0] for s in steps) / frames
        sy = sum(s[1] for s in steps) / frames
        triggers = {}
        for f, e in enumerate(events):
            bit = 1
            while bit <= e[1]:
                if e[1] & bit:
                    triggers.setdefault(bit, []).append(f)
                bit <<= 1
        out[c["name"].lower()] = {
            "name": c["name"], "frames": c["frames"], "fps": c["fps"],
            "loop": bool(c["flags"] & FLAG_LOOP), "bit3": bool(c["flags"] & FLAG_BIT3), "bones": c["bones"],
            "step": (sx, sy), "speed": (sx * sx + sy * sy) ** 0.5 * c["fps"],
            "bottom": (min(e[2] for e in events), max(e[2] for e in events)) if events else (0.0, 0.0),
            "top": (min(e[3] for e in events), max(e[3] for e in events)) if events else (0.0, 0.0),
            "triggers": triggers,
        }
    rows = [(key, [os.path.splitext(v)[0] for v in variants]) for key, variants in rows]
    return rows, out


def main():
    cli, reference = sys.argv[1], sys.argv[2]
    rows, clips = spec(cli, reference)
    if "--table" in sys.argv:
        used = {}
        for key, variants in rows:
            for v in variants:
                used.setdefault(v.lower(), []).append(key)
        for stem, c in clips.items():
            trig = " ".join(f"0x{b:x}@{','.join(map(str, fr))}" for b, fr in sorted(c["triggers"].items()))
            print(f"{c['name']:<12} fr {c['frames']:>3} {'L' if c['loop'] else '1'}{'+b3' if c['bit3'] else '   '} "
                  f"step {c['step'][0]:+.4f} {c['step'][1]:+.4f} speed {c['speed']:5.2f} "
                  f"bottom {c['bottom'][0]:.2f}..{c['bottom'][1]:.2f} rows {len(used.get(stem, []))} {trig}")
    print(f"{len(rows)} rows, {len(clips)} clips")


if __name__ == "__main__":
    main()
