from __future__ import annotations

from dataclasses import dataclass

import pytest

from pyopennova.animation_build import (
    IDENTITY_QUAT,
    quat_xyzw_to_matrix_rows,
    sample_animation_context,
    sample_bad_clip,
)
from pyopennova.model import AnimationContext, AnimationMeta


@dataclass
class FakeQuat:
    x: float
    y: float
    z: float
    w: float


class FakeChannel:
    def __init__(self, rotations):
        self.rotations = rotations
        self.frame_count = len(rotations)


class FakeEvent:
    def __init__(self, velocity, bottom=0.0, top=0.0):
        self.velocity = velocity
        self.bottom = bottom
        self.top = top


class FakeBad:
    def __init__(
        self,
        *,
        frame_count: int,
        flags: int = 0,
        rotations=None,
        translations=None,
        events=None,
    ):
        self.frame_count = frame_count
        self.flags = flags
        self.fps = 30
        self.num_bones = 1
        self.num_channels = 1
        self.channels = [FakeChannel(rotations or [FakeQuat(0.0, 0.0, 0.0, 1.0)] * frame_count)]
        self.num_translations = len(translations or [])
        self.translations = translations or []
        self.num_events = len(events or [])
        self.events = [
            FakeEvent(*v) if len(v) == 3 and isinstance(v[0], tuple) else FakeEvent(v)
            for v in (events or [])
        ]


def test_sample_bad_clip_ignores_terminal_event_and_accumulates_root_motion() -> None:
    bad = FakeBad(
        frame_count=2,
        events=[
            (1.0, 2.0, 3.0),
            (4.0, 5.0, 6.0),
            (99.0, 99.0, 99.0),
        ],
    )

    clip = sample_bad_clip(
        bad,
        [("BN01", -1, (0.0, 0.0, 0.0))],
        animation_name="walk",
    )

    assert clip.frame_count == 2
    assert [frame.frame for frame in clip.frames] == [1, 2]
    assert clip.frames[0].root_motion_position == pytest.approx((1.0, -3.0, 2.0))
    assert clip.frames[1].root_motion_position == pytest.approx((5.0, -9.0, 7.0))


def test_sample_bad_clip_tracks_max_root_motion_horizontal_velocity_and_bottom() -> None:
    bad = FakeBad(
        frame_count=2,
        events=[
            ((1.0, 2.0, 3.0), 4.5, 8.0),
            ((4.0, 5.0, 6.0), 9.5, 12.0),
            ((99.0, 99.0, 99.0), 99.0, 99.0),
        ],
    )

    clip = sample_bad_clip(
        bad,
        [("BN01", -1, (0.0, 0.0, 0.0))],
        animation_name="walk",
    )

    assert clip.frames[0].max_root_motion_position == pytest.approx((1.0, -3.0, 4.5))
    assert clip.frames[1].max_root_motion_position == pytest.approx((5.0, -9.0, 9.5))


def test_sample_bad_clip_applies_translations_only_when_flagged() -> None:
    translations = [(2.0, 3.0, 4.0)]
    bad = FakeBad(frame_count=1, flags=0x02, translations=translations)

    clip = sample_bad_clip(
        bad,
        [("BN01", -1, (10.0, 20.0, 30.0))],
        animation_name="translated",
    )

    bone = clip.frames[0].bones[0]
    assert bone.local_position == pytest.approx((12.0, 16.0, 33.0))
    assert bone.local_rotation == pytest.approx(IDENTITY_QUAT)


def test_quat_xyzw_to_matrix_rows_matches_bad_row_convention() -> None:
    half_sqrt = 2.0 ** -0.5

    rows = quat_xyzw_to_matrix_rows((half_sqrt, 0.0, 0.0, half_sqrt))

    expected = (
        (1.0, 0.0, 0.0),
        (0.0, 0.0, 1.0),
        (0.0, -1.0, 0.0),
    )
    for row, expected_row in zip(rows, expected):
        assert row == pytest.approx(expected_row)


def test_sample_animation_context_orders_reset_first_and_sets_clip_ranges() -> None:
    reset = AnimationMeta("anim_reset", "reset.bad", 30, 1, bad_name="reset", flags=0)
    run = AnimationMeta("run", "run.bad", 30, 2, bad_name="run", flags=0)
    context = AnimationContext(reset_animation=reset, animations=[run])
    bads = {
        "reset.bad": FakeBad(frame_count=1),
        "run.bad": FakeBad(frame_count=2),
    }

    result = sample_animation_context(
        context,
        [("BN01", -1, (0.0, 0.0, 0.0))],
        parse_bad=lambda path: bads[path],
        free_bad=lambda _bad: None,
    )

    assert result.warnings == ()
    assert [(clip.name, clip.start_frame, clip.end_frame, clip.is_reset) for clip in result.clips] == [
        ("anim_reset", 1, 1, True),
        ("run", 2, 3, False),
    ]


def test_sample_animation_context_skips_failed_clip_without_aborting() -> None:
    reset = AnimationMeta("anim_reset", "reset.bad", 30, 1)
    broken = AnimationMeta("broken", "broken.bad", 30, 1)
    context = AnimationContext(reset_animation=reset, animations=[broken])

    def parse_bad(path: str):
        if path == "broken.bad":
            raise RuntimeError("bad parse")
        return FakeBad(frame_count=1)

    result = sample_animation_context(
        context,
        [("BN01", -1, (0.0, 0.0, 0.0))],
        parse_bad=parse_bad,
        free_bad=lambda _bad: None,
    )

    assert [clip.name for clip in result.clips] == ["anim_reset"]
    assert result.warnings == ("broken: bad parse",)
