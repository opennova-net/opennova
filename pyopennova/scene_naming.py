"""DCC-neutral OpenNova scene naming and visualization color tables.

Shared by the DCC scene builders (Blender, 3ds Max). Adding a new
collision/occlusion type or changing a name format is a one-file change here.
"""
from __future__ import annotations

import enum


class OcclusionType(enum.IntEnum):
    OB = 0
    OS = 1
    OP = 2
    OP2 = 3

    @property
    def prefix(self) -> str:
        return _OCCLUSION_PREFIXES[self]

    @property
    def color(self) -> tuple[float, float, float]:
        return _OCCLUSION_COLORS[self]


_OCCLUSION_PREFIXES = {
    OcclusionType.OB: "OB",
    OcclusionType.OS: "OS",
    OcclusionType.OP: "OP",
    OcclusionType.OP2: "OP",
}

_OCCLUSION_COLORS = {
    OcclusionType.OB: (1.0, 0.2, 0.2),
    OcclusionType.OS: (0.2, 0.6, 1.0),
    OcclusionType.OP: (1.0, 0.8, 0.2),
    OcclusionType.OP2: (1.0, 0.0, 0.6),
}


class CollisionType(enum.IntEnum):
    CB = 1; CS = 2; CC = 3; CL = 4; CV = 5; CA = 6
    VC = 7; BB = 8; CD = 9; CT = 10; CM = 11; VK = 12
    CF = 13; LP = 14; DH = 16; DM = 17; DL = 18; CP = 19

    @property
    def color(self) -> tuple[float, float, float]:
        return _COLLISION_COLORS[self]


_COLLISION_COLORS = {
    CollisionType.CB: (0.0, 1.0, 0.0),
    CollisionType.CS: (0.0, 0.8, 1.0),
    CollisionType.CC: (1.0, 0.5, 0.0),
    CollisionType.CL: (1.0, 1.0, 0.0),
    CollisionType.CV: (0.5, 0.0, 1.0),
    CollisionType.CA: (0.2, 1.0, 0.5),
    CollisionType.VC: (0.6, 0.6, 0.6),
    CollisionType.BB: (1.0, 0.0, 1.0),
    CollisionType.CD: (0.8, 0.4, 0.0),
    CollisionType.CT: (1.0, 0.2, 0.2),
    CollisionType.CM: (0.4, 0.4, 1.0),
    CollisionType.VK: (1.0, 0.0, 0.0),
    CollisionType.CF: (0.3, 1.0, 0.3),
    CollisionType.LP: (1.0, 0.8, 0.2),
    CollisionType.DH: (1.0, 0.1, 0.1),
    CollisionType.DM: (1.0, 0.4, 0.1),
    CollisionType.DL: (1.0, 0.7, 0.1),
    CollisionType.CP: (0.7, 0.7, 1.0),
}


def build_occlusion_name(
    type_code: int, parent_subobject: int, connecting_subobject: int
) -> str:
    display_index = parent_subobject if parent_subobject >= 0 else 0
    try:
        prefix = OcclusionType(type_code).prefix
    except ValueError:
        prefix = "OX"
    name = f"{prefix}{display_index + 1:02d}"
    if (type_code == 2 or type_code == 3) and connecting_subobject >= 0:
        name += f"-{connecting_subobject + 1:02d}"
    return name


def blinkbox_enabled_suffix(flags: int) -> str:
    enabled = (~flags) & 0x3E
    if enabled == 0:
        return ""
    out = ""
    if enabled & (1 << 1): out += "V"
    if enabled & (1 << 2): out += "S"
    if enabled & (1 << 3): out += "W"
    if enabled & (1 << 4): out += "L"
    if enabled & (1 << 5): out += "O"
    return out


def format_duplicate_suffix(occurrence: int) -> str:
    if occurrence <= 1:
        return ""
    index = occurrence - 1
    out = ""
    while index > 0:
        index -= 1
        out = chr(ord("a") + (index % 26)) + out
        index //= 26
    return out


def build_volume_name(
    type_code: int, flags: int, index: int, occurrence: int
) -> str:
    try:
        base = CollisionType(type_code).name
    except ValueError:
        base = "CX"
    if type_code == 8:
        suffix = blinkbox_enabled_suffix(flags)
        if suffix:
            base += suffix
    display_index = index if index >= 0 else 0
    return f"{base}{display_index + 1:02d}{format_duplicate_suffix(occurrence)}"
