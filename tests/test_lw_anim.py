"""Land Warrior animation resolution-chain tests (Python/FFI side).

Validates pyopennova.lw_anim_ffi (the SAF/KSA/ACA/ANM parsers + pose sampler
exported from libs/threedi/threedi_lw_anim) and pyopennova.lw_anim.build_lw_animation_context
end-to-end against the checked-in dflw fixtures (chr_file=player01, anim_def=enemy00).

The Blender Action math (scene_builder._build_lw_action) is exercised only in the
onimport UI; this covers everything up to the sampled per-bone transforms.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from pyopennova import lw_anim_ffi
from pyopennova.asset_resolver import AssetResolver
from pyopennova.lw_anim import build_lw_animation_context

FIXTURE = Path(__file__).resolve().parents[1] / "fixtures" / "threedi" / "lw"
KSA = FIXTURE / "PLAYER01.KSA"
ANM = FIXTURE / "ENEMY00.ANM"
MODEL = FIXTURE / "BADGUY.3DI"


def test_ffi_anm_parses_move_table() -> None:
    anm = lw_anim_ffi.read_anm(str(ANM))
    try:
        assert anm.entry_count == 124
        by_name = {}
        for i in range(anm.entry_count):
            e = anm.entries[i]
            by_name[e.name.decode("ascii", "replace").strip()] = e
        assert by_name["Walking0_2HW"].slot == 18
        assert by_name["Walking0_2HW"].velocity == pytest.approx(1.0)
        assert by_name["Running0_2HW"].slot == 61
        assert by_name["Running0_2HW"].velocity == pytest.approx(5.0)
    finally:
        lw_anim_ffi.free_anm(anm)


def test_ffi_ksa_parses_baked_bundle() -> None:
    ksa = lw_anim_ffi.read_ksa(str(KSA))
    try:
        assert ksa.version == 1
        assert ksa.slot_count == 255
        assert ksa.slots[18].frame_count == 140
        assert ksa.slots[18].loop_frame == 1
    finally:
        lw_anim_ffi.free_ksa(ksa)


def test_read_skeleton_matches_badguy_geometry() -> None:
    subs, flags = lw_anim_ffi.read_lw_subobjects(str(MODEL), 0)
    assert len(subs) == 15          # BADGUY LOD0 sub-objects
    assert flags & 1                # skinned LOD
    # Parent hierarchy is a tree rooted at 0 (parents precede children).
    assert subs[0].parent in (-1, 0)


def test_build_context_samples_named_clips() -> None:
    with AssetResolver(str(FIXTURE)) as resolver:
        model_path = resolver.resolve("badguy.3di")
        assert model_path is not None
        ctx = build_lw_animation_context(
            "enemy00", "player01", resolver=resolver, model_path=str(model_path)
        )

    assert ctx is not None
    assert ctx.kind == "lw"
    assert ctx.anim_name == "enemy00"
    assert ctx.chr_name == "player01"
    assert ctx.slot_count == 255
    assert ctx.clips, "expected at least one sampled clip"

    clips = {c.name: c for c in ctx.clips}
    walk = clips["Walking0_2HW"]
    assert walk.slot == 18
    assert walk.frame_count == 140
    assert walk.loop_frame == 1
    assert walk.bone_count == 15
    assert len(walk.frames) == 140

    # Each frame is per-bone row-major 3x4 [R|t].
    frame0 = walk.frames[0]
    assert len(frame0) == 15
    assert len(frame0[0]) == 3 and len(frame0[0][0]) == 4

    # The clip is genuinely animated: some bone rotates between frame 0 and a
    # mid frame (not a static rest pose).
    mid = walk.frames[walk.frame_count // 2]
    delta = max(
        abs(frame0[b][r][c] - mid[b][r][c])
        for b in range(15)
        for r in range(3)
        for c in range(3)
    )
    assert delta > 1e-3
