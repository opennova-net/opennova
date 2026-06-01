from __future__ import annotations

from pathlib import Path

from pyopennova.lw_anim import parse_aca, parse_anm, parse_ksa, parse_saf


FIXTURE = Path(__file__).resolve().parents[1] / "fixtures" / "lw" / "dflw" / "badguy"


def test_enemy00_anm_maps_move_names_to_slots_and_velocity() -> None:
    anim = parse_anm(FIXTURE / "ENEMY00.ANM")

    assert anim.entries["Null"].slot == 0
    assert anim.entries["Null"].override is True
    assert anim.entries["Walking0_2HW"].slot == 18
    assert anim.entries["Walking0_2HW"].velocity == 1.0
    assert anim.entries["Running0_2HW"].slot == 61
    assert anim.entries["Running0_2HW"].velocity == 5.0
    assert len(anim.entries) == 124


def test_player01_ksa_covers_every_enemy00_slot_by_slot_index() -> None:
    anim = parse_anm(FIXTURE / "ENEMY00.ANM")
    ksa = parse_ksa(FIXTURE / "PLAYER01.KSA")

    missing = sorted(slot for slot in anim.unique_slots() if slot not in ksa.slots)

    assert ksa.version == 1
    assert len(ksa.slots) == 255
    assert ksa.slots[18].frame_count == 140
    assert ksa.slots[18].loop_frame == 1
    assert missing == []


def test_player01_aca_and_saf_parse_source_slot_list() -> None:
    aca = parse_aca(FIXTURE / "PLAYER01.ACA")
    saf = parse_saf(FIXTURE / "3WALK09A.SAF")

    assert len(aca.slots) == 53
    assert aca.slots[88].filename.lower() == "3walk09a.saf"
    assert aca.slots[88].loop_frame == 0
    assert saf.version == 100
    assert saf.frame_count == 31
    assert len(saf.frames) == 31
    assert len(saf.frames[0].bone_records) == 15
