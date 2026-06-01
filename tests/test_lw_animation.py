from __future__ import annotations

import struct
from pathlib import Path

import pytest

from pyopennova.definitions import build_animation_context


def _saf_frame(root: tuple[float, ...], records: list[tuple[int, int]]) -> bytes:
    assert len(root) == 13
    data = bytearray(struct.pack("<13f", *root))
    for part_byte, angle in records:
        data.extend(struct.pack("<Bhb", part_byte, angle, 0))
    return bytes(data)


def _runtime_frame(
    *,
    root_values: tuple[int, int, int, int, int, int, int, int, int],
    records: list[tuple[int, int]],
) -> bytes:
    data = bytearray(88)
    for i, (part_token, angle) in enumerate(records[:15]):
        off = i * 4
        data[off] = part_token & 0xFF
        struct.pack_into("<h", data, off + 1, angle)
    for i, value in enumerate(root_values):
        struct.pack_into("<h", data, 60 + i * 2, value)
    return bytes(data)


def test_parse_saf_normalizes_to_game_runtime_frames() -> None:
    from pyopennova.lw_animation import parse_saf_bytes

    root = (
        2.0,   # part record count
        0.0,
        1.0,   # root rotation component -> negative scale
        -2.0,  # root rotation component -> positive scale
        3.0,   # root rotation component -> negative scale
        0.25,
        -0.5,
        1.0,   # clamped to -30 after game scaling
        1.5,
        -2.5,
        0.125,
        0.0,
        0.0,
    )
    blob = (
        b"SAF1"
        + struct.pack("<III", 100, 1, 0x34)
        + _saf_frame(root, [(0x7D, 1234), (0x80, -2345)])
    )

    saf = parse_saf_bytes(blob, name="TEST.SAF")

    assert saf.frame_count == 1
    frame = saf.frames[0]
    assert frame.part_records == (
        (0xFD, 1234),
        (0x00, -2345),
    )
    assert frame.root_values == (
        21,    # root[5] * 85.333336
        128,   # root[8] * 85.333336
        -42,   # root[6] * 85.333336
        -213,  # root[9] * 85.333336
        -30,   # root[7] * 85.333336, game clamp
        10,    # root[10] * 85.333336
        -2730, # root[3] * 1365.3334
        -1365, # root[2] * -1365.3334
        -4096, # root[4] * -1365.3334
    )


def test_parse_ksa_slot_table_and_embedded_frames() -> None:
    from pyopennova.lw_animation import parse_ksa_bytes

    frame_a = _runtime_frame(
        root_values=(1, 2, 3, 4, 5, 6, 7, 8, 9),
        records=[(0xFD, 111)],
    )
    frame_b = _runtime_frame(
        root_values=(9, 8, 7, 6, 5, 4, 3, 2, 1),
        records=[(0xFE, -222), (0xFF, 333)],
    )
    table = (
        struct.pack("<7I", 1, 0x009F7400, 4, 0, 0, 0, 0)
        + struct.pack("<7I", 1, 0x009F7458, 9, 1, 0, 0, 0)
    )
    header = bytearray(100)
    header[:4] = b"KSA\0"
    struct.pack_into("<I", header, 4, 1)
    struct.pack_into("<I", header, 44, len(table) + len(frame_a) + len(frame_b))
    struct.pack_into("<I", header, 52, 2)
    blob = bytes(header) + table + frame_a + frame_b

    ksa = parse_ksa_bytes(blob, name="PLAYER01.KSA")

    assert ksa.entry_count == 2
    assert [entry.slot_id for entry in ksa.entries] == [4, 9]
    assert ksa.entries[1].loop_frame == 1
    assert ksa.entries[1].frames[0].part_records == ((0xFE, -222), (0xFF, 333))


def test_parse_lw_anm_and_aca_text() -> None:
    from pyopennova.lw_animation import parse_aca_text, parse_anm_text

    anm = parse_anm_text(
        """
        // move, animation number, velocity
        Null 0 0.0 override
        Walking0_2HW 88 1.5 // forward
        Standing_2HW 6 0.0
        """,
        name="PLAYANIM.ANM",
    )
    aca = parse_aca_text(
        """
        slot 88 3walk09a.saf 0
        slot 6 3shoot03.saf
        """,
        name="PLAYER01.ACA",
    )

    assert anm.movements["Walking0_2HW"].slot_id == 88
    assert anm.movements["Null"].override is True
    assert aca.slots[88].saf_name == "3walk09a.saf"
    assert aca.slots[6].loop_frame == 0


def test_build_animation_context_returns_lw_context_for_anm(tmp_path: Path) -> None:
    from pyopennova.lw_animation import LwAnimationContext

    (tmp_path / "PLAYANIM.ANM").write_text("Walking0_2HW 88 1.5\n", encoding="ascii")
    (tmp_path / "PLAYER01.ACA").write_text("slot 88 3walk09a.saf 0\n", encoding="ascii")
    (tmp_path / "3WALK09A.SAF").write_bytes(
        b"SAF1"
        + struct.pack("<III", 100, 1, 0x34)
        + _saf_frame((1.0,) + (0.0,) * 12, [(0x7D, 1)])
    )

    class Resolver:
        def resolve(self, filename: str):
            p = tmp_path / filename
            if p.exists():
                return str(p)
            upper = tmp_path / filename.upper()
            return str(upper) if upper.exists() else None

    ctx = build_animation_context("playanim", resolver=Resolver())

    assert isinstance(ctx, LwAnimationContext)
    assert ctx.anm_name.lower() == "playanim.anm"
    assert ctx.clips[88].source_name.lower() == "3walk09a.saf"
    assert ctx.movement_clips["Walking0_2HW"].slot_id == 88


@pytest.mark.skipif(
    not Path(r"C:\Users\taylor\Desktop\DFLW").exists(),
    reason="local DFLW loose asset directory is not available",
)
def test_real_lw_animation_assets_parse() -> None:
    from pyopennova.asset_resolver import AssetResolver
    from pyopennova.lw_animation import build_lw_animation_context

    with AssetResolver(r"C:\Users\taylor\Desktop\DFLW") as resolver:
        ctx = build_lw_animation_context("playanim", resolver=resolver, chr_file="player01")

    assert ctx is not None
    assert ctx.anm is not None
    assert len(ctx.saf_clips) == 53
    assert 88 in ctx.clips
    assert ctx.clips[88].frame_count > 0
