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


def _forward_to_max(sampled):
    """Replicate the Max import keying: per-frame node rows + world positions + root."""
    rows, node_pos, root_pos = [], [], []
    for fr in sampled.frames:
        rp = fr.max_root_motion_position
        root_pos.append(rp)
        rows.append([_max_rows(b.source_rotation_xyzw) for b in fr.bones])
        node_pos.append([vec_add(b.world_position, rp) for b in fr.bones])
    return rows, node_pos, root_pos


def test_translation_anchored_to_reset_not_clip_frame0(tmp_path):
    """An animated clip whose frame 0 != the reset pose must export translations
    anchored to the RESET rest (what the importer rebuilds from), not the clip's
    own frame 0. This is the regression for the 'plays wrong on re-import' bug."""
    bones = [
        bad_build.BadBoneOut("BN01", -1, 1.0, (0.0, 0.0, 0.0), _IDENTITY_ROWS),
        bad_build.BadBoneOut("BN02", 0, 0.5, (0.5, 0.0, 0.0), _IDENTITY_ROWS),
        bad_build.BadBoneOut("BN03", 1, 0.5, (0.0, 0.5, 0.0), _IDENTITY_ROWS),
    ]
    TR = bad_build.ANIM_FLAG_TRANSLATION

    reset = bad_build.assemble_bad_clip(
        bones=bones,
        per_frame_rotations_xyzw=[[(0, 0, 0, 1)] * 3],
        frame_count=1,
        flags=TR,
        per_frame_translations=[[(0.0, 0.0, 0.0)] * 3],
    )
    anim_rots = [
        [(0, 0, 0, 1), (0.1, 0, 0, 0.99499), (0, 0.2, 0, 0.9798)],
        [(0.05, 0, 0, 0.99875), (0.15, 0, 0, 0.98869), (0, 0.25, 0, 0.96825)],
        [(0, 0.1, 0, 0.99499), (0.2, 0, 0, 0.9798), (0, 0.3, 0, 0.95394)],
    ]
    # Translations are NONZERO at frame 0 -> clip frame 0 differs from the reset.
    anim_trans = [
        [(0.3, -0.2, 0.1), (0.1, 0.2, 0.0), (-0.1, 0.0, 0.2)],
        [(0.35, -0.1, 0.1), (0.15, 0.25, 0.0), (-0.05, 0.05, 0.2)],
        [(0.4, -0.15, 0.0), (0.2, 0.2, -0.1), (0.0, 0.3, 0.1)],
    ]
    anim = bad_build.assemble_bad_clip(
        bones=bones,
        per_frame_rotations_xyzw=anim_rots,
        frame_count=3,
        flags=TR,
        per_frame_translations=anim_trans,
    )

    reset_path = str(tmp_path / "reset.bad")
    anim_path = str(tmp_path / "anim.bad")
    bad_build.write_bad(reset_path, reset)
    bad_build.write_bad(anim_path, anim)
    bf_reset = bad_ffi.parse_bad(reset_path)
    bf_anim = bad_ffi.parse_bad(anim_path)
    try:
        # The importer samples EVERY clip against the reset skeleton's rest.
        reset_infos = _bone_infos(bf_reset)
        s_reset = sample_bad_clip(bf_reset, reset_infos, animation_name="reset")
        s_anim = sample_bad_clip(bf_anim, reset_infos, animation_name="anim")

        r_rows, r_node, r_root = _forward_to_max(s_reset)
        a_rows, a_node, a_root = _forward_to_max(s_anim)

        bones_meta = [("BN01", -1, 1.0), ("BN02", 0, 0.5), ("BN03", 1, 0.5)]
        parents = [-1, 0, 1]
        reset_rest = bad_build.rest_origins_from_max_world(r_rows[0], r_node[0], r_root[0], parents)

        def export_and_resample(reset_rest_origins):
            clip = bad_build.assemble_clip_from_max_world(
                bones_meta=bones_meta,
                per_frame_rows=a_rows,
                per_frame_node_pos=a_node,
                per_frame_root_pos=a_root,
                frame_count=3,
                flags=TR,
                reset_rest_origins=reset_rest_origins,
            )
            path = str(tmp_path / ("rt_%s.bad" % ("fixed" if reset_rest_origins else "buggy")))
            bad_build.write_bad(path, clip)
            bf = bad_ffi.parse_bad(path)
            try:
                return sample_bad_clip(bf, reset_infos, animation_name="rt")
            finally:
                bad_ffi.free_bad(bf)

        s_fixed = export_and_resample(reset_rest)
        s_buggy = export_and_resample(None)  # pre-fix: per-clip frame-0 anchoring

        fixed_ok = True
        buggy_matches = True
        for f in range(3):
            for b in range(3):
                orig = s_anim.frames[f].bones[b].world_position
                if not _approx(orig, s_fixed.frames[f].bones[b].world_position, eps=1e-3):
                    fixed_ok = False
                if not _approx(orig, s_buggy.frames[f].bones[b].world_position, eps=1e-3):
                    buggy_matches = False
        assert fixed_ok, "reset-anchored export must reproduce the original world positions"
        assert not buggy_matches, "without reset anchoring the round-trip should diverge"
    finally:
        bad_ffi.free_bad(bf_reset)
        bad_ffi.free_bad(bf_anim)


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
