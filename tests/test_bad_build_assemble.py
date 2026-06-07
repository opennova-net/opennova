"""assemble_bad_clip + sample/assemble inverse, validated through the C writer/parser."""
from __future__ import annotations

import pytest

from pyopennova import bad_build, bad_ffi, coords
from pyopennova.animation_build import sample_bad_clip

try:
    bad_ffi._bind()
except Exception as exc:  # pragma: no cover - skip when the native lib is absent
    pytest.skip(f"native opennova library unavailable: {exc}", allow_module_level=True)

_IDENTITY_ROWS = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def _approx(a, b, eps=1e-5):
    return all(abs(float(x) - float(y)) <= eps for x, y in zip(a, b))


def _sign_agnostic(a_quat, exp_xyzw, eps=1e-4):
    dot = a_quat.x * exp_xyzw[0] + a_quat.y * exp_xyzw[1] + a_quat.z * exp_xyzw[2] + a_quat.w * exp_xyzw[3]
    return abs(abs(dot) - 1.0) <= eps


def test_assemble_direct_roundtrip(tmp_path):
    bones = [
        bad_build.BadBoneOut("BN01", -1, 1.0, (0.0, 0.0, 0.0), _IDENTITY_ROWS),
        bad_build.BadBoneOut("BN02", 0, 0.5, (1.0, 2.0, 3.0), _IDENTITY_ROWS),
    ]
    frame_count = 2
    rots = [
        [(0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0, 1.0)],
        [(0.1, 0.0, 0.0, 0.99499), (0.2, 0.0, 0.0, 0.9798)],
    ]
    trans = [
        [(0.0, 0.0, 0.0), (0.0, 0.0, 0.0)],
        [(1.5, -2.5, 0.25), (-3.0, 4.0, 5.0)],
    ]
    events = [
        bad_build.BadEventOut((0.1, 0.2, 0.3), 0.5, 1.5, 0),
        bad_build.BadEventOut((0.0, 0.1, 0.0), 0.5, 1.5, 0),
    ]
    clip = bad_build.assemble_bad_clip(
        bones=bones,
        per_frame_rotations_xyzw=rots,
        frame_count=frame_count,
        flags=bad_build.ANIM_FLAG_TRANSLATION,
        fps=30,
        per_frame_translations=trans,
        events=events,
        name="anim_walk",
        bad_name="walk",
    )
    # terminal-duplicate convention
    assert len(clip.channels[0].rotations_xyzw) == frame_count + 1
    assert len(clip.translations) == frame_count + 1
    assert len(clip.events) == len(events) + 1

    out = str(tmp_path / "assemble.bad")
    bad_build.write_bad(out, clip)
    rt = bad_ffi.parse_bad(out)
    try:
        assert rt.flags == bad_build.ANIM_FLAG_TRANSLATION
        assert rt.frame_count == frame_count
        assert rt.num_bones == 2
        assert rt.num_translations == 4  # bone_count * frame_count (parser read window)
        for f in range(frame_count):
            for b in range(2):
                v = rt.translations[f * 2 + b]
                assert _approx((v[0], v[1], v[2]), trans[f][b])
        for b in range(2):
            for f in range(frame_count):
                assert _sign_agnostic(rt.channels[b].rotations[f], rots[f][b])
        for i in range(len(events)):
            assert _approx(list(rt.events[i].velocity), list(events[i].velocity_bad))
    finally:
        bad_ffi.free_bad(rt)


def test_sample_then_assemble_channels(tmp_path):
    """sample_bad_clip.source_rotation_xyzw must feed back through assemble unchanged."""
    bones = [
        bad_build.BadBoneOut("BN01", -1, 1.0, (0.0, 0.0, 0.0), _IDENTITY_ROWS),
        bad_build.BadBoneOut("BN02", 0, 0.5, (0.5, 0.0, 0.0), _IDENTITY_ROWS),
        bad_build.BadBoneOut("BN03", 1, 0.5, (0.0, 0.5, 0.0), _IDENTITY_ROWS),
    ]
    frame_count = 3
    rots = [
        [(0.0, 0.0, 0.0, 1.0), (0.1, 0.0, 0.0, 0.99499), (0.0, 0.2, 0.0, 0.9798)],
        [(0.05, 0.0, 0.0, 0.99875), (0.15, 0.0, 0.0, 0.98869), (0.0, 0.25, 0.0, 0.96825)],
        [(0.0, 0.1, 0.0, 0.99499), (0.2, 0.0, 0.0, 0.9798), (0.0, 0.3, 0.0, 0.95394)],
    ]
    clip0 = bad_build.assemble_bad_clip(
        bones=bones, per_frame_rotations_xyzw=rots, frame_count=frame_count, flags=0, fps=30,
    )
    bf, keep = bad_build._badfile_from_clip(clip0)
    bone_infos = [
        (b.name, b.parent_index, coords.bone_space(b.position_bad)) for b in bones
    ]
    sampled = sample_bad_clip(bf, bone_infos, animation_name="x")
    keep.clear()

    assert len(sampled.frames) == frame_count
    per_frame = [
        [fr.bones[i].source_rotation_xyzw for i in range(len(bones))]
        for fr in sampled.frames
    ]
    clip1 = bad_build.assemble_bad_clip(
        bones=bones, per_frame_rotations_xyzw=per_frame, frame_count=frame_count, flags=0, fps=30,
    )
    out = str(tmp_path / "sampled.bad")
    bad_build.write_bad(out, clip1)
    rt = bad_ffi.parse_bad(out)
    try:
        for b in range(len(bones)):
            for f in range(frame_count):
                assert _sign_agnostic(rt.channels[b].rotations[f], rots[f][b])
    finally:
        bad_ffi.free_bad(rt)
