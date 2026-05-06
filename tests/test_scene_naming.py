"""Unit tests for the host-neutral scene naming module."""
from __future__ import annotations

import pytest

from pyopennova.scene_naming import (
    CollisionType,
    OcclusionType,
    blinkbox_enabled_suffix,
    build_occlusion_name,
    build_volume_name,
    format_duplicate_suffix,
)


def test_occlusion_type_prefix_mapping():
    assert OcclusionType.OB.prefix == "OB"
    assert OcclusionType.OS.prefix == "OS"
    assert OcclusionType.OP.prefix == "OP"
    assert OcclusionType.OP2.prefix == "OP"


def test_occlusion_type_color_unique_per_member():
    colors = {t: t.color for t in OcclusionType}
    assert len(set(colors.values())) == len(OcclusionType)
    assert OcclusionType.OB.color == (1.0, 0.2, 0.2)


def test_collision_type_color_covers_every_member():
    for t in CollisionType:
        c = t.color
        assert isinstance(c, tuple)
        assert len(c) == 3


def test_build_occlusion_name_basic():
    assert build_occlusion_name(OcclusionType.OB, 0, -1) == "OB01"
    assert build_occlusion_name(OcclusionType.OS, 4, -1) == "OS05"


def test_build_occlusion_name_op_with_connecting_subobject():
    assert build_occlusion_name(OcclusionType.OP, 0, 2) == "OP01-03"
    assert build_occlusion_name(OcclusionType.OP2, 0, 5) == "OP01-06"


def test_build_occlusion_name_unknown_falls_back_to_OX():
    assert build_occlusion_name(99, 0, -1) == "OX01"


def test_blinkbox_suffix_bit_letters():
    assert blinkbox_enabled_suffix(~0x02 & 0xFF) == "V"
    assert blinkbox_enabled_suffix(~0x04 & 0xFF) == "S"
    assert blinkbox_enabled_suffix(~0x08 & 0xFF) == "W"
    assert blinkbox_enabled_suffix(~0x10 & 0xFF) == "L"
    assert blinkbox_enabled_suffix(~0x20 & 0xFF) == "O"


def test_blinkbox_suffix_all_disabled_returns_empty():
    assert blinkbox_enabled_suffix(0xFF) == ""


def test_blinkbox_suffix_combined_letters_in_VSWLO_order():
    assert blinkbox_enabled_suffix(~0x3E & 0xFF) == "VSWLO"


@pytest.mark.parametrize(
    "occurrence,expected",
    [
        (1, ""),
        (2, "a"),
        (3, "b"),
        (27, "z"),
        (28, "aa"),
        (29, "ab"),
        (53, "az"),
        (54, "ba"),
        (55, "bb"),
    ],
)
def test_format_duplicate_suffix_wraparound(occurrence, expected):
    assert format_duplicate_suffix(occurrence) == expected


def test_build_volume_name_basic():
    assert build_volume_name(CollisionType.CB, 0, 0, 1) == "CB01"
    assert build_volume_name(CollisionType.CS, 0, 4, 1) == "CS05"


def test_build_volume_name_blinkbox_appends_letter_suffix():
    flags = ~0x02 & 0xFF
    assert build_volume_name(CollisionType.BB, flags, 0, 1) == "BBV01"


def test_build_volume_name_duplicate_suffix():
    assert build_volume_name(CollisionType.CB, 0, 0, 2) == "CB01a"
    assert build_volume_name(CollisionType.CB, 0, 0, 27) == "CB01z"
    assert build_volume_name(CollisionType.CB, 0, 0, 28) == "CB01aa"


def test_build_volume_name_unknown_falls_back_to_CX():
    assert build_volume_name(99, 0, 0, 1) == "CX01"


def test_collision_type_int_values_match_protocol():
    assert int(CollisionType.CB) == 1
    assert int(CollisionType.BB) == 8
    assert int(CollisionType.CP) == 19
