"""Measure every source clip the packs offer, so clips.json can be chosen by
numbers: its length, rate, how far and which way the hips travel, and how
high they ride.

    blender -b --factory-startup --python art/on_person/index_sources.py -- [<pack>=<folder> ...]

A pack is a folder of FBX files (or, for `motus`, MocapOnline's .blend
files), scanned recursively:

    kine=<KINEMATION>/Shared/Character/Animations/Generic
    infima=<src>/infima/anims   kubold=<src>/kubold/EXPORT   motus=<BLD_Rifle_Basic>/Animation

Merges into src/source_index.json beside this file (a pack named again is
measured again; the others are kept).
"""
import json
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
INDEX = os.path.join(SRC, "source_index.json")
HIPS = ("pelvis", "Hips", "mixamorig:Hips")


def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def measure(pack, path):
    arm = next((o for o in bpy.data.objects if o.type == "ARMATURE" and o.animation_data
                and o.animation_data.action), None)
    if arm is None:
        return None
    action = arm.animation_data.action
    hips = next((arm.pose.bones[n] for n in HIPS if n in arm.pose.bones), None)
    if hips is None:
        return None
    scene = bpy.context.scene
    fps = scene.render.fps / scene.render.fps_base
    start, end = (int(round(v)) for v in action.frame_range)
    path_xy, heights = [], []
    for f in range(start, end + 1):
        scene.frame_set(f)
        at = arm.matrix_world @ hips.head
        path_xy.append((at.x, at.y))
        heights.append(at.z)
    dx, dy = path_xy[-1][0] - path_xy[0][0], path_xy[-1][1] - path_xy[0][1]
    seconds = (end - start) / fps
    reach = max(((x - path_xy[0][0]) ** 2 + (y - path_xy[0][1]) ** 2) ** 0.5 for x, y in path_xy)
    rest = (arm.matrix_world @ arm.data.bones[hips.name].head_local).z
    return {
        "pack": pack, "path": path, "fps": fps, "start": start, "end": end, "seconds": round(seconds, 4),
        # Blender axes: the sources face -Y, their left is +X.
        "travel": [round(dx, 4), round(dy, 4)], "reach": round(reach, 4),
        "speed": round((dx * dx + dy * dy) ** 0.5 / seconds, 3) if seconds > 0 else 0.0,
        "hips": [round(min(heights), 3), round(max(heights), 3)], "hips_rest": round(rest, 3),
    }


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out = {}
    if os.path.exists(INDEX):
        with open(INDEX, encoding="utf-8") as f:
            out = json.load(f)
    for arg in args:
        pack, folder = arg.split("=", 1)
        out = {k: v for k, v in out.items() if v["pack"] != pack}
        ext = ".blend" if pack == "motus" else ".fbx"
        jobs = []
        for top, _, files in os.walk(folder):
            jobs += [os.path.join(top, n) for n in sorted(files) if n.lower().endswith(ext)]
        for path in jobs:
            name = os.path.splitext(os.path.basename(path))[0]
            try:
                if ext == ".fbx":
                    clear()
                    bpy.ops.import_scene.fbx(filepath=path)
                else:
                    bpy.ops.wm.open_mainfile(filepath=path)
                row = measure(pack, path)
            except Exception as e:  # one unreadable file does not stop the scan
                print(f"index: {name}: {type(e).__name__}: {e}")
                continue
            if row is None:
                print(f"index: {name}: no armature action")
                continue
            out[f"{pack}/{name}"] = row
            print(f"index: {pack}/{name} {row['seconds']}s speed {row['speed']} travel {row['travel']}")
            sys.stdout.flush()
    os.makedirs(SRC, exist_ok=True)
    with open(INDEX, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1, sort_keys=True)
    print(f"index: {len(out)} clips")


main()
