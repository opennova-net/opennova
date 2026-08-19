import hashlib
import json
import os
from pathlib import Path
import re

import pytest


ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "docs" / "render" / "render-fixtures-v1.json"
RETAIL_CATALOG_PATH = (
    ROOT / "docs" / "render" / "render-fixtures-retail-v2.json"
)
RETAIL_ARCHIVE_CATALOG_PATH = (
    ROOT / "docs" / "render" / "render-fixtures-retail-v1.json"
)


def _catalog(path: Path = CATALOG_PATH) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def test_render_fixture_catalog_pins_three_missions_and_capture_contract() -> None:
    catalog = _catalog()

    assert catalog["schema"] == "opennova.render-fixtures.v1"
    assert catalog["capture"] == {
        "resolution": [1600, 900],
        "load_settle_frames": 132,
        "visibility_settle_frames": 3,
        "vertical_fov_deg": 50.534,
        "freeze_simulation": True,
        "freeze_weather": True,
    }
    assert catalog["missions"] == {
        "00TRa.bms": "64dd18b3122a6c4ee4e173cac1b9a12d19004c32572bd2f1ad113a475fe57407",
        "CP01.bms": "476219276170c1b26bcc5c2d9eb5aafdcfaccb583aad843f89867cb1f93b9863",
        "CP12.bms": "24d498c38f694bcddecfe71567f64a5c12d7164f3566554fc5cde66e42216459",
    }
    for digest in catalog["missions"].values():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)


@pytest.mark.parametrize("path", [CATALOG_PATH, RETAIL_CATALOG_PATH])
def test_render_fixture_mission_hashes_match_configured_loose_assets(
    path: Path,
) -> None:
    mission_root_value = os.environ.get("NOVA_MISSION_RESOURCE_DIR", "")
    if not mission_root_value:
        return
    mission_root = Path(mission_root_value)
    assert mission_root.is_dir()
    for name, expected in _catalog(path)["missions"].items():
        source = mission_root / name
        assert source.is_file()
        assert hashlib.sha256(source.read_bytes()).hexdigest() == expected


@pytest.mark.parametrize(
    ("path", "schema", "resolution"),
    [
        (CATALOG_PATH, "opennova.render-fixtures.v1", [1600, 900]),
        (RETAIL_CATALOG_PATH, "opennova.render-fixtures.v2", [2000, 1200]),
    ],
)
def test_fixture_catalog_common_contract(
    path: Path,
    schema: str,
    resolution: list[int],
) -> None:
    catalog = _catalog(path)
    assert catalog["schema"] == schema
    assert catalog["capture"]["resolution"] == resolution
    assert len(catalog["fixtures"]) == len({
        fixture["id"] for fixture in catalog["fixtures"]
    })
    for name, digest in catalog["missions"].items():
        assert name.lower().endswith(".bms")
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
    for fixture in catalog["fixtures"]:
        assert fixture["mission"] in catalog["missions"]
        assert len(fixture["camera_bms"]["position"]) == 3
        assert fixture["minutes_of_day"]


def test_render_fixture_catalog_has_every_agreed_pose_and_unique_identity() -> None:
    fixtures = _catalog()["fixtures"]
    by_id = {fixture["id"]: fixture for fixture in fixtures}
    assert len(by_id) == len(fixtures)
    assert set(by_id) == {
        "00tra-courtyard",
        "00tra-armory-glass",
        "00tra-fire-barrel",
        "cp01-water-wide",
        "cp01-waterline-above",
        "cp01-waterline-below",
        "cp01-suv-glass",
        "cp12-night-wreck",
        "cp12-night-sky-up",
        "cp12-night-sky-ground",
        "cp12-truck-material",
    }
    expected_records = {
        "00tra-courtyard": (949, "marker", 8),
        "00tra-armory-glass": (35, "building", 20),
        "00tra-fire-barrel": (348, "building", 333),
        "cp01-water-wide": (76, "building", 62),
        "cp01-waterline-above": (76, "building", 62),
        "cp01-waterline-below": (76, "building", 62),
        "cp01-suv-glass": (5, "item", 5),
        "cp12-night-wreck": (28, "item", 28),
        "cp12-night-sky-up": (1215, "marker", 4),
        "cp12-night-sky-ground": (1215, "marker", 4),
        "cp12-truck-material": (32, "item", 32),
    }
    for fixture in fixtures:
        selector = fixture["entity"]
        assert selector["bms_id"] > 0
        record = selector["bms_record"]
        assert (
            record["write_order_index"],
            record["mission_kind"],
            record["kind_index"],
        ) == expected_records[fixture["id"]]
        assert selector["item_id"] == selector["raw_type"] + 100000
        assert len(selector.get("model_sha256", "0" * 64)) == 64
        assert len(fixture["camera_bms"]["position"]) == 3
        assert fixture["camera_bms"]["vertical_fov_deg"] == 50.534

    barrel = by_id["00tra-fire-barrel"]
    assert barrel["entity"]["bms_id"] == 1099
    assert barrel["entity"]["graphic"].casefold() == "firebrl3.3di"
    assert barrel["entity"]["lght_count"] == 1
    assert barrel["minutes_of_day"] == [900, 1320]
    assert by_id["cp01-waterline-above"]["camera_bms"]["position"][2] == 21.25
    assert by_id["cp01-waterline-below"]["camera_bms"]["position"][2] == 20.75


def test_wide_fixtures_cover_the_four_time_of_day_witnesses() -> None:
    by_id = {fixture["id"]: fixture for fixture in _catalog()["fixtures"]}
    expected = [350, 720, 1125, 1320]
    assert by_id["00tra-courtyard"]["minutes_of_day"] == expected
    assert by_id["cp01-water-wide"]["minutes_of_day"] == expected
    assert by_id["cp12-night-sky-up"]["minutes_of_day"] == expected


@pytest.mark.parametrize("path", [CATALOG_PATH, RETAIL_CATALOG_PATH])
def test_catalog_records_its_canonical_payload_hash(path: Path) -> None:
    catalog = _catalog(path)
    claimed = catalog.pop("catalog_sha256")
    canonical = json.dumps(catalog, sort_keys=True, separators=(",", ":")).encode()
    assert claimed == hashlib.sha256(canonical).hexdigest()


def test_retail_catalog_declares_every_agreed_static_comparison() -> None:
    catalog = _catalog(RETAIL_CATALOG_PATH)
    assert catalog["retail_video_profile_contract"] == {
        "id": "retail_reference_highest_retail_selectable_v2",
        "video_option_catalog_id": "joint-operations-revx02-video-options-v1",
        "video_option_catalog_sha256": (
            "371d95a8caccb85b89ca0e0a10bc87e60ba37f632f9724c7f14036b06a2f8ff4"
        ),
        "required_values": {
            "windowed": 0,
            "video_res": "1920x1200",
            "gamma": 0.8,
            "terrain_polydetail": 3,
            "terrain_texdetail": 3,
            "object_polydetail": 3,
            "object_texdetail": 3,
            "display_16x9": 1,
            "water_quality": 3,
            "shadow_quality": 3,
            "particle_density": 2,
            "antialias_mode": 2,
            "texfilter_level": 3,
            "fbeffects_level": 3,
            "shader_usage_level": 2,
            "texcompression_level": 2,
            "lock_framerate": 0,
            "force_vsync": 0,
            "reduce_mouselag": 1,
            "NoBlood": 0,
            "NoCasings": 0,
            "NoSmoke": 0,
            "no_anim": 0,
        },
    }
    assert catalog["comparison_contract"] == {
        "capture_mode": "hud_hidden",
        "equipped_weapon": "WPN_M16BURST",
        "weapon_clip": 30,
        "weapon_reserve": 270,
        "character_id": 0x0402,
        "arms_graphic": "IndoArms.3di",
        "arms_camo": [1, 0, 0],
        "hud_detail_level": 3,
        "gameplay_hud_visible": False,
        "hud_canvas_layer_active": True,
        "player_view_effects_active": True,
        "terrain_enabled": True,
        "viewmodel_enabled": True,
        "ads_active": False,
        "big_map_active": False,
        "player_pose_source": "retail_player_bms.applied",
    }
    assert catalog["retail_profile_contract"] == {
        "path": "expansion/revx02/weapon.sav",
        "sha256": (
            "f4907820a58505a6988f140a27638dae7dbaaea2cc446642f0bc44c868905623"
        ),
        "profile_slot": 0,
        "blue": {
            "player_class": 9,
            "avatar_a": 2,
            "avatar_b": 0,
            "character_id": 0x0402,
            "primary_weapon": "WPN_M16BURST",
        },
        "red": {
            "player_class": 9,
            "avatar_a": 7,
            "avatar_b": 0,
            "character_id": 0x8207,
        },
    }
    assert catalog["retail_eye_model"]["provenance_status"] \
        == "legacy_unverified"
    assert "frame-correlated" not in catalog["retail_eye_model"]["derivation"]
    assert "registered capture" in catalog["retail_eye_model"]["derivation"]
    assert catalog["capture"]["camera_registration"] \
        == "inverse_frame_correlated_row_major_view_matrix"
    fixtures = catalog["fixtures"]
    by_id = {fixture["id"]: fixture for fixture in fixtures}
    assert len(by_id) == len(fixtures) == 15
    assert set(by_id) == {
        "00tra-courtyard-retail",
        "00tra-armory-glass-retail",
        "00tra-fire-barrel-east-retail",
        "00tra-fire-barrel-close-retail",
        "cp01-water-wide-retail",
        "cp01-water-shallow-retail",
        "cp01-water-steep-retail",
        "cp01-waterline-above-retail",
        "cp01-waterline-below-retail",
        "cp01-water-oblique-retail",
        "cp12-yard-road-retail",
        "cp12-truck-material-retail",
        "cp12-yard-tanks-retail",
        "cp04-checkpoint-fires-retail",
        "00tra-fire-barrel-full-composite-retail",
    }
    assert {fixture["capture_mode"] for fixture in fixtures} == {"hud_hidden"}
    for fixture_id, fixture in by_id.items():
        assert fixture["capture_mode"] == "hud_hidden"
        assert fixture["minutes_of_day"] == [
            catalog["clock"]["start_minutes"][fixture["mission"]]
        ]
        assert len(fixture["camera_bms"]["position"]) == 3
        assert fixture["camera_bms"]["vertical_fov_deg"] == 53.4468
        pose = fixture["retail_player_bms"]
        assert pose["provenance_status"] in {
            "required_at_capture",
            "legacy_unverified",
        }
        assert "observed" not in pose
        assert "witnessed" not in pose["derivation"]
        assert "camera_bms minus" not in pose["derivation"]
        assert "inverse view matrix" in pose["derivation"]

    assert {
        fixture_id for fixture_id, fixture in by_id.items()
        if fixture["retail_player_bms"]["provenance_status"]
        == "legacy_unverified"
    } == {
        "00tra-fire-barrel-east-retail",
        "00tra-fire-barrel-close-retail",
        "cp01-water-wide-retail",
        "cp04-checkpoint-fires-retail",
        "00tra-fire-barrel-full-composite-retail",
    }

    assert by_id["00tra-courtyard-retail"]["camera_bms"] == {
        "position": [297.881214309941, -409.028974271988, 28.660125732422],
        "yaw_deg": 0.0,
        "pitch_deg": 0.0,
        "vertical_fov_deg": 53.4468,
    }
    assert by_id["cp01-water-shallow-retail"]["camera_bms"] == {
        "position": [
            -738.746882924068,
            -754.372438413281,
            22.042848805241,
        ],
        "yaw_deg": 0.0,
        "pitch_deg": -2.0,
        "vertical_fov_deg": 53.4468,
    }
    assert by_id["cp01-water-steep-retail"]["camera_bms"]["pitch_deg"] \
        == -20.0
    assert by_id["cp01-waterline-above-retail"]["camera_bms"]["position"][2] \
        == 20.995712280273
    assert by_id["cp01-waterline-below-retail"]["camera_bms"]["position"][2] \
        == 20.94172668457
    assert by_id["cp01-water-oblique-retail"]["camera_bms"] == {
        "position": [
            -738.525954097758,
            -754.470230922642,
            22.237902722705,
        ],
        "yaw_deg": 30.0,
        "pitch_deg": -8.0,
        "vertical_fov_deg": 53.4468,
    }
    assert by_id["cp12-yard-road-retail"]["camera_bms"] == {
        "position": [-429.618780784416, 1539.726867761563, 62.225941856971],
        "yaw_deg": 0.0,
        "pitch_deg": -5.0,
        "vertical_fov_deg": 53.4468,
    }
    assert by_id["cp12-yard-tanks-retail"]["camera_bms"] == {
        "position": [-429.701783556709, 1539.431518554688, 62.234282787589],
        "yaw_deg": 90.0,
        "pitch_deg": -5.0,
        "vertical_fov_deg": 53.4468,
    }


def test_previous_retail_catalog_remains_an_explicit_v1_archive() -> None:
    assert _catalog(RETAIL_ARCHIVE_CATALOG_PATH)["schema"] \
        == "opennova.render-fixtures.v1"
