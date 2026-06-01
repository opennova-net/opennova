from __future__ import annotations

from pathlib import Path

from pyopennova.animation_build import sample_animation_context
from pyopennova.asset_resolver import AssetResolver
from pyopennova.lw_anim import build_lw_animation_context


FIXTURE = Path(__file__).resolve().parents[1] / "fixtures" / "lw" / "dflw" / "badguy"

PARENTS = (-1, 0, 1, 1, 1, 3, 4, 5, 6, 0, 0, 9, 10, 11, 12)
BONE_INFOS = tuple((f"BN{i + 1:02d}", parent, (0.0, 0.0, 0.0)) for i, parent in enumerate(PARENTS))


def test_lw_animation_context_samples_ksa_clips_without_bad_files() -> None:
    with AssetResolver(str(FIXTURE)) as resolver:
        context = build_lw_animation_context("enemy00", "player01", resolver=resolver)

    assert context is not None

    def unexpected_bad_parse(path: str) -> object:
        raise AssertionError(f"LW sampling should not parse BAD files: {path}")

    sampled = sample_animation_context(
        context,
        BONE_INFOS,
        parse_bad=unexpected_bad_parse,
        free_bad=lambda _bad: None,
    )

    walking = next(clip for clip in sampled.clips if clip.name == "Walking0_2HW")
    running = next(clip for clip in sampled.clips if clip.name == "Running0_2HW")

    assert walking.source_format == "lw"
    assert walking.fps == 30
    assert walking.frame_count == 140
    assert walking.start_frame > 1
    assert len(walking.frames) == 140
    assert len(walking.frames[0].bones) == 15
    assert running.frame_count == 22
    assert running.start_frame > walking.start_frame
    assert sampled.warnings == ()


def test_lw_sampled_frames_decode_non_identity_part_rotations() -> None:
    with AssetResolver(str(FIXTURE)) as resolver:
        context = build_lw_animation_context("enemy00", "player01", resolver=resolver)

    assert context is not None

    sampled = sample_animation_context(
        context,
        BONE_INFOS,
        parse_bad=lambda path: (_ for _ in ()).throw(AssertionError(path)),
        free_bad=lambda _bad: None,
    )
    running = next(clip for clip in sampled.clips if clip.name == "Running0_2HW")
    first_frame = running.frames[0]
    second_frame = running.frames[1]

    assert first_frame.bones[0].source_rotation_xyzw != (0.0, 0.0, 0.0, 1.0)
    assert first_frame.bones[7].world_rotation != second_frame.bones[7].world_rotation
