#!/usr/bin/env python
"""Retail first-person bone oracle: the original's FP rig builders, transliterated.

Reproduces the per-bone world matrices retail draws the first-person gun + arms
with, for one clip frame, from the same three files the runtime consumes:

* the FP gun model (`gfx1`, e.g. ``m16_1st.3di``) -- the model bone table
  supplies the row count, parent indices and ABSOLUTE pivots
  [orig: BoneAnim_BuildWorldMatrices @0x40c400 walks modelDef+56 rows: parent
  @+20, pivot @+36/40/44 (the file's ``abs`` vec3: file +24 -> memory +36)];
* the reset/skeleton ``.bad`` named by the weapon's ``animadm`` slot 0 -- its
  per-bone 3x3 is the bind every clip composes against
  [orig: AnimMap_RegisterEntity @0x40bb60 pins channel+44 once];
* the clip ``.bad`` (e.g. the ``anim_wpn_idle`` clip) and a frame index.

Per bone i (all 3x3 matrices row-major, D3D row-vector convention):

    ch_i   = Math_QuaternionToMatrix3x3(clip quaternion x,y,z,w)      @0x615a70
    A_i    = (bind_i^T * ch_i)^T                                      @0x410da0
    W_i    = S * A_i^T * S,  T_i = S * t_i,  S = diag(-1, 1, 1)       @0x40c4d1..0x40c582
    world_i.rot = W_i
    world_i.t   = abs_i * W_parent + world_parent.t + T_i - abs_i * W_i   @0x40c5ef..0x40c721

so the pivot of bone i lands at ``abs_i * W_parent + world_parent.t + T_i`` --
the standard parent-relative FK once the absolute pivots are read. Rows past
the clip's channel count take bone 0's composed matrix (the padding loop
@0x40c5a1). The FP pass then multiplies every row by the view root
[orig: Entity_BuildBoneWorldMatrices @0x4b25e0], which this oracle leaves as
identity: positions are in the model/render frame, root at the origin.

Checked 2026-08-22 against the live OpenNova rig (godot/tests/game/vm_bone_probe.gd
dump): every pivot of the M16/M4 FP rig agrees to <= 0.6 mm at the idle hold
under the model->render x-flip (ADR 0007 convention 1: bones engine-native, the
mesh carries the flip).

Pull the inputs from a retail mount with pyopennova::

    from pyopennova.vfs_ffi import Vfs
    v = Vfs(); v.mount_game(r"C:\\GAMES\\JOTAC\\Game\\JO", "revx02")
    open("m16_1st.3di", "wb").write(v.read_file("m16_1st.3di"))

Usage::

    uv run python scripts/render/fp_bone_oracle.py m16_1st.3di m4_RST.bad m4_1i.bad --frame 15
    uv run python scripts/render/fp_bone_oracle.py ... --probe-log vm_bone_probe.log
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from pyopennova import bad_ffi, threedi_ffi  # noqa: E402

S = [[-1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]


def quat_to_m3(q):
    """Math_QuaternionToMatrix3x3 @0x615a70 (row-major 3x3; q = x, y, z, w)."""
    x, y, z, w = q
    tx, ty, tz = 2 * x, 2 * y, 2 * z
    xx, xy, xz = x * tx, x * ty, x * tz
    yy, yz, zz = y * ty, y * tz, z * tz
    wx, wy, wz = tx * w, ty * w, tz * w
    return [[1 - (zz + yy), wz + xy, xz - wy],
            [xy - wz, 1 - (zz + xx), wx + yz],
            [xz + wy, yz - wx, 1 - (xx + yy)]]


def m3_mul(a, b):
    """Math_MultiplyMatrix3x3_Float @0x616090: a * b, row-major."""
    return [[sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def m3_t(a):
    return [[a[c][r] for c in range(3)] for r in range(3)]


def row_vec_mul(v, m):
    """v * M for a row vector and a row-major 3x3 (the D3D convention)."""
    return [sum(v[k] * m[k][c] for k in range(3)) for c in range(3)]


def bone_rot9(bf, i):
    r = bf.bones[i].rotation
    return [[r[0], r[1], r[2]], [r[3], r[4], r[5]], [r[6], r[7], r[8]]]


def channel_quat(bf, i, frame):
    ch = bf.channels[i]
    q = ch.rotations[min(frame, ch.frame_count - 1)]
    return (q.x, q.y, q.z, q.w)


def channel_trans(bf, i, frame):
    if not (bf.flags & 2) or bf.num_translations == 0:
        return [0.0, 0.0, 0.0]
    t = bf.translations[frame * bf.num_bones + i]
    return [t[0], t[1], t[2]]


def retail_pose(lod, bind_bf, clip_bf, frame):
    """Per-row (W_i, world_t_i) for the model's LOD-0 bone table."""
    n_model = lod.render_object_count
    n_anim = min(bind_bf.num_bones, clip_bf.num_bones)
    rows = []
    for i in range(n_model):
        if i < n_anim:
            ch = quat_to_m3(channel_quat(clip_bf, i, frame))
            bind = bone_rot9(bind_bf, i)
            combined = m3_mul(m3_t(bind), ch)        # transpose(bind) * ch
            a = m3_t(combined)                       # the 4x4's upper 3x3
            w = m3_mul(m3_mul(S, m3_t(a)), S)        # the S*A^T*S copy loop
            t = channel_trans(clip_bf, i, frame)
            tt = [-t[0], t[1], t[2]]                 # S * t
        else:
            w, tt = rows[0][0], [0.0, 0.0, 0.0]      # padding rows = bone 0
        rows.append((w, tt))
    world = []
    for i in range(n_model):
        ro = lod.render_objects[i]
        piv = [ro.abs[0], ro.abs[1], ro.abs[2]]
        parent = ro.parent_index
        w, tt = rows[i]
        trans = row_vec_mul([-piv[0], -piv[1], -piv[2]], w)      # T(-pivot) * W
        if parent == i or parent < 0:
            pw, pt = w, [0.0, 0.0, 0.0]
        else:
            pw, pt = world[parent]
        moved = row_vec_mul(piv, pw)
        world.append((w, [moved[k] + pt[k] + tt[k] + trans[k] for k in range(3)]))
    return world


def pivot_world(world, lod, i):
    ro = lod.render_objects[i]
    piv = [ro.abs[0], ro.abs[1], ro.abs[2]]
    w, t = world[i]
    return [sum(piv[k] * w[k][c] for k in range(3)) + t[c] for c in range(3)]


def read_probe_log(path: Path) -> dict[int, tuple[str, tuple[float, float, float]]]:
    """Bone index -> (name, live origin) from a vm_bone_probe.gd dump."""
    out = {}
    pat = re.compile(r"\[vmbone\]\s+bone\s+(\d+)\s+(\S.*?)\s+parent=\s*(-?\d+)\s+live: origin=\(([^)]*)\)")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pat.match(line)
        if m:
            out[int(m.group(1))] = (m.group(2).strip(), tuple(float(v) for v in m.group(4).split(",")))
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("model", type=Path, help="the FP gun .3di (gfx1)")
    parser.add_argument("bind", type=Path, help="the animadm slot-0 reset .bad")
    parser.add_argument("clip", type=Path, help="the clip .bad to pose")
    parser.add_argument("--frame", type=int, default=0)
    parser.add_argument("--probe-log", type=Path,
                        help="vm_bone_probe.gd output to compare against (x-flip map)")
    args = parser.parse_args(argv)

    model = threedi_ffi.read_model_3di3(str(args.model))
    lod = model.lods[0]
    bind_bf = bad_ffi.parse_bad(str(args.bind))
    clip_bf = bad_ffi.parse_bad(str(args.clip))
    world = retail_pose(lod, bind_bf, clip_bf, args.frame)
    ours = read_probe_log(args.probe_log) if args.probe_log else {}
    worst = 0.0
    print(f"# {args.model.name} bind={args.bind.name} clip={args.clip.name} frame={args.frame} rows={lod.render_object_count}")
    for i in range(lod.render_object_count):
        name = bind_bf.bones[i].name.decode("latin-1") if i < bind_bf.num_bones else f"MDL{i}"
        w, _t = world[i]
        pv = pivot_world(world, lod, i)
        line = (f"{i:2d} {name:18s} parent={lod.render_objects[i].parent_index:2d} "
                f"pivot=({pv[0]:+.5f},{pv[1]:+.5f},{pv[2]:+.5f}) "
                f"rows=[{w[0][0]:+.4f} {w[0][1]:+.4f} {w[0][2]:+.4f} | {w[1][0]:+.4f} {w[1][1]:+.4f} {w[1][2]:+.4f} | {w[2][0]:+.4f} {w[2][1]:+.4f} {w[2][2]:+.4f}]")
        if i in ours:
            o = ours[i][1]
            d = (-pv[0] - o[0], pv[1] - o[1], pv[2] - o[2])
            worst = max(worst, max(abs(v) for v in d))
            line += f" ours=({o[0]:+.5f},{o[1]:+.5f},{o[2]:+.5f}) diff(x-flip)=({d[0]:+.4f},{d[1]:+.4f},{d[2]:+.4f})"
        print(line)
    if ours:
        print(f"# worst |diff| vs probe (x-flip map): {worst:.4f} u")
    bad_ffi.free_bad(bind_bf)
    bad_ffi.free_bad(clip_bf)
    threedi_ffi.free_model_3di3(model)
    return 0


if __name__ == "__main__":
    sys.exit(main())
