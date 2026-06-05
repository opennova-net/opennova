"""Validate bad_build.assemble_clip_from_max_world without 3ds Max.

Simulates the Max import keying forward (the exact transforms in
opennova_max/animation.py), then inverts it via assemble_clip_from_max_world and
checks the written .bad re-imports to the same skeleton motion. This pins the
Max exporter's coordinate math even though pymxs cannot run in CI.
"""
from __future__ import annotations

import pytest

from pyopennova import bad_build, bad_ffi, coords
from pyopennova.animation_build import (
    mat_mul,
    quat_xyzw_to_matrix_rows,
    sample_bad_clip,
    vec_add,
)

try:
    bad_ffi._bind()
except Exception as exc:  # pragma: no cover - skip when the native lib is absent
    pytest.skip(f"native opennova library unavailable: {exc}", allow_module_level=True)

_GLOBAL = bad_build._MAX_GLOBAL_MATRIX_ROWS
_CORRECTION = bad_build._MAX_GLOBAL_CORRECTION_ROWS
_IDENTITY_ROWS = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def _max_rows(source_xyzw):
    """opennova_max.animation.max_rows_from_source_quat, replicated."""
    return mat_mul(mat_mul(_GLOBAL, quat_xyzw_to_matrix_rows(source_xyzw)), _CORRECTION)


def _bone_infos(bf):
    infos = []
    for i in range(int(bf.num_bones)):
        b = bf.bones[i]
        name = b.name.decode("utf-8", "replace") if isinstance(b.name, bytes) else str(b.name)
        infos.append((
            name,
            int(b.parent_index),
            coords.bone_space((b.position[0], b.position[1], b.position[2])),
        ))
    return infos


def _approx(a, b, eps=1e-4):
    return all(abs(float(x) - float(y)) <= eps for x, y in zip(a, b))


def test_root_position_zeroed_by_root_status_not_index():
    """The BAD root bone is zeroed by parent status, not by array index 0."""
    # bone 0 is a CHILD of bone 1; bone 1 is the root.
    bones_meta = [("BNchild", 1, 0.5), ("BNroot", -1, 1.0)]
    rows = [[_IDENTITY_ROWS, _IDENTITY_ROWS]]
    node_pos = [[(1.0, 2.0, 3.0), (0.0, 0.0, 0.0)]]
    root_pos = [(0.0, 0.0, 0.0)]

    clip = bad_build.assemble_clip_from_max_world(
        bones_meta=bones_meta,
        per_frame_rows=rows,
        per_frame_node_pos=node_pos,
        per_frame_root_pos=root_pos,
        frame_count=1,
        flags=0,
        fps=30,
    )
    # The root (index 1) is at the origin; the non-root index-0 bone is not.
    assert clip.bones[1].position_bad == (0.0, 0.0, 0.0)
    assert clip.bones[0].position_bad != (0.0, 0.0, 0.0)


def test_max_world_roundtrip(tmp_path):
    bones = [
        bad_build.BadBoneOut("BN01", -1, 1.0, (0.0, 0.0, 0.0), _IDENTITY_ROWS),
        bad_build.BadBoneOut("BN02", 0, 0.5, (0.5, 0.0, 0.0), _IDENTITY_ROWS),
        bad_build.BadBoneOut("BN03", 1, 0.5, (0.0, 0.5, 0.0), _IDENTITY_ROWS),
    ]
    frame_count = 3
    source = [
        [(0.0, 0.0, 0.0, 1.0), (0.1, 0.0, 0.0, 0.99499), (0.0, 0.2, 0.0, 0.9798)],
        [(0.05, 0.0, 0.0, 0.99875), (0.15, 0.0, 0.0, 0.98869), (0.0, 0.25, 0.0, 0.96825)],
        [(0.0, 0.1, 0.0, 0.99499), (0.2, 0.0, 0.0, 0.9798), (0.0, 0.3, 0.0, 0.95394)],
    ]
    # Per-frame per-bone BAD translations; frame 0 is zero (the rest reference).
    trans = [
        [(0.0, 0.0, 0.0)] * 3,
        [(0.1, -0.2, 0.05), (0.0, 0.1, 0.0), (-0.05, 0.0, 0.1)],
        [(0.2, -0.1, 0.0), (0.1, 0.0, -0.1), (0.0, 0.2, 0.0)],
    ]
    # Root-motion events: x,y accumulate, z=bottom.
    events = [
        bad_build.BadEventOut((0.0, 0.0, 0.0), 1.0, 1.0, 0),
        bad_build.BadEventOut((0.1, 0.05, 0.0), 1.0, 1.0, 0),
        bad_build.BadEventOut((0.1, 0.05, 0.0), 1.1, 1.1, 0),
    ]

    clip0 = bad_build.assemble_bad_clip(
        bones=bones,
        per_frame_rotations_xyzw=source,
        frame_count=frame_count,
        flags=bad_build.ANIM_FLAG_TRANSLATION,
        fps=30,
        per_frame_translations=trans,
        events=events,
    )
    out0 = str(tmp_path / "orig.bad")
    bad_build.write_bad(out0, clip0)
    bf0 = bad_ffi.parse_bad(out0)

    try:
        infos0 = _bone_infos(bf0)
        sampled0 = sample_bad_clip(bf0, infos0, animation_name="x")
        assert len(sampled0.frames) == frame_count

        # --- Simulate the 3ds Max import keying forward ---
        per_frame_rows = []
        per_frame_node_pos = []
        per_frame_root_pos = []
        for fr in sampled0.frames:
            root_pos = fr.max_root_motion_position
            per_frame_root_pos.append(root_pos)
            rows = []
            node_pos = []
            for bone in fr.bones:
                rows.append(_max_rows(bone.source_rotation_xyzw))
                node_pos.append(vec_add(bone.world_position, root_pos))
            per_frame_rows.append(rows)
            per_frame_node_pos.append(node_pos)

        # --- Invert via the Max exporter math ---
        clip1 = bad_build.assemble_clip_from_max_world(
            bones_meta=[("BN01", -1, 1.0), ("BN02", 0, 0.5), ("BN03", 1, 0.5)],
            per_frame_rows=per_frame_rows,
            per_frame_node_pos=per_frame_node_pos,
            per_frame_root_pos=per_frame_root_pos,
            frame_count=frame_count,
            flags=bad_build.ANIM_FLAG_TRANSLATION,
            fps=30,
        )
        out1 = str(tmp_path / "rt.bad")
        bad_build.write_bad(out1, clip1)
        bf1 = bad_ffi.parse_bad(out1)
        try:
            infos1 = _bone_infos(bf1)
            sampled1 = sample_bad_clip(bf1, infos1, animation_name="x")

            # Skeleton motion must match: world rotation (sign-agnostic) + world
            # position + accumulated root motion.
            for f in range(frame_count):
                f0 = sampled0.frames[f]
                f1 = sampled1.frames[f]
                assert _approx(f0.max_root_motion_position, f1.max_root_motion_position)
                for b in range(len(bones)):
                    assert _approx(f0.bones[b].world_position, f1.bones[b].world_position, eps=1e-3)
                    q0 = f0.bones[b].world_rotation
                    q1 = f1.bones[b].world_rotation
                    dot = sum(q0[i] * q1[i] for i in range(4))
                    assert abs(abs(dot) - 1.0) <= 1e-3
        finally:
            bad_ffi.free_bad(bf1)
    finally:
        bad_ffi.free_bad(bf0)
