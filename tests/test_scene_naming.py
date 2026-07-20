"""Canonical collision-volume names shared by every DCC scene builder."""

from __future__ import annotations

import pytest

from pyopennova.scene_naming import CollisionType, build_volume_name


@pytest.mark.parametrize(
    ("type_code", "prefix"),
    [
        (9, "CD"),
        (10, "CT"),
        (13, "CF"),
        (16, "DH"),
        (17, "DM"),
        (18, "DL"),
        (19, "CP"),
    ],
)
def test_extended_gameplay_volume_names(type_code: int, prefix: str) -> None:
    assert build_volume_name(type_code, 0, 0, 1) == f"{prefix}01"


def test_damage_volume_colors_are_supported_and_distinct() -> None:
    colors = {CollisionType.DH.color, CollisionType.DM.color, CollisionType.DL.color}
    assert len(colors) == 3
    assert (1.0, 1.0, 1.0) not in colors
