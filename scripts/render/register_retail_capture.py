"""Register a raw retail capture only after every reproducibility check passes.

This is intentionally a preflight/manifest assembler, not an onHook launcher.
The onHook MCP server owns process launch and frame capture. Use this sequence:

1. Launch ``Jointops.exe /exp revx02 /w`` through the owned mission session
   and retain its exact PID/process-start ``instance_id``.
2. Save onhook_instances() structuredContent for that instance.
3. Save onhook_apply_render_fixture({fixture_id, position_bms, yaw_degrees,
   pitch_degrees, camera_mode: "first_person", vertical_fov_degrees,
   time_of_day_seconds, mission_file, mission_sha256}) structuredContent; all
   values must come from the selected catalog fixture and it must report
   exact=true.
4. onhook_capture_retail_reference({instance_id, path, fixture_id})
5. Pass all three saved tool results, the create-new PNG/state sidecar, and the
   same-process onhook.log here.

Before launch, run ``stage_retail_presentation.py stage``. Registration must
run before that helper restores ``game.cfg`` so this preflight can independently
hash the exhaustive highest-quality profile and active ``weapon.sav``. HUD state
is not staged or mutated: the capture bundle must prove one backbuffer snapshot
at a known pre-retail-HUD boundary, an exact D3D scene split, and one subsequent
unmodified retail UI call. The sanitized stage manifest must sit beside the
output bundle; its local restore token is never accepted or published.

CLI (all paths are explicit; no installed-tool fallback is used):

uv run python scripts/render/register_retail_capture.py \
  --catalog docs/render/render-fixtures-retail-v4.json \
  --fixture-id FIXTURE --raw-state FRAME.state.json --raw-image FRAME.png \
  --instance-status INSTANCES.json --fixture-result FIXTURE-RESULT.json \
  --capture-result CAPTURE-RESULT.json \
  --onhook-log onhook.log --game-dir RETAIL_ROOT --expansion revx02 \
  --retail-executable RETAIL_ROOT/Jointops.exe \
  --onhook-mcp PATH/TO/onhook-mcp.exe \
  --onhook-proxy RETAIL_ROOT/binkw32.dll \
  --onhook-forwarder RETAIL_ROOT/binkw32_.dll \
  --retail-stage-manifest OUTPUT_DIR/retail-stage.json \
  --confirm-retail-presentation-contract \
  --opennova-source-commit FULL_40_HEX_SHA --output REGISTERED.json

The script refuses older hooks, caller-only fixture context without an exact
same-instance fixture result, uncorrelated frames, and mission/build claims
that cannot be independently bound. ``/exp revx02`` remains an operational
launch prerequisite corroborated by the expansion VFS/profile and same-PID
mission log rather than a native argv witness; correlated D3D state proves
effective ``/w`` windowed presentation.

``player_pose_source=retail_player_bms.applied`` names the requested teleport
shared with the OpenNova fixture. Retail may settle after that request; the
correlated inverse view matrix, not equality with the observed player position,
is the authoritative visual camera registration.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import struct
import sys
from typing import Any

from PySide6.QtGui import QImage

from pyopennova.vfs_ffi import Vfs


TOOL_VERSION = "4.1.0"
# The registration floor for how long retail settled after the fixture apply
# (a teleport) before the reference frame was taken. Retail's first-person
# motion lead and ground snap take seconds to decay; the hook's capture-frame
# witness (bridge protocol 1.5) proves the applied clock was still pinned when
# the pixels were taken, so this is a settle rule, not a correlation window.
DEFAULT_MIN_SETTLE_SECONDS = 1.0
RETAIL_SIZE = (1920, 1200)
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
GUID_HEX_PATTERN = re.compile(r"^[0-9a-fA-F]{32}$")
PROFILE_ID = "retail_reference_highest_retail_selectable_v2"
VIDEO_OPTION_CATALOG_ID = "joint-operations-revx02-video-options-v1"
HIGHEST_VIDEO_PROFILE: dict[str, int | float | str] = {
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
}
# The engine exposes these 21 values as runtime globals.  Windowed mode and
# resolution remain required staged settings, but their effective values are
# witnessed by the same-frame D3D device/backbuffer below instead of being
# misrepresented as engine runtime globals.
RUNTIME_VIDEO_PROFILE: dict[str, int | float | str] = {
    key: value
    for key, value in HIGHEST_VIDEO_PROFILE.items()
    if key not in {"windowed", "video_res"}
}
RUNTIME_GAMMA_FLOAT32 = struct.unpack(
    "<f", struct.pack("<f", HIGHEST_VIDEO_PROFILE["gamma"])
)[0]
VIDEO_OPTION_POLICIES = {
    "windowed": "runtime_presentation",
    "hw3d_deviceno": "device_identity",
    "hw3d_name": "device_identity",
    "hw3d_guid": "device_identity",
    "video_res": "comparison_invariant",
    "gamma": "comparison_invariant",
    "terrain_polydetail": "video_quality",
    "terrain_texdetail": "video_quality",
    "object_polydetail": "video_quality",
    "object_texdetail": "video_quality",
    "display_16x9": "comparison_invariant",
    "water_quality": "video_quality",
    "shadow_quality": "video_quality",
    "particle_density": "video_quality",
    "antialias_mode": "highest_retail_selectable",
    "texfilter_level": "video_quality",
    "fbeffects_level": "video_quality",
    "shader_usage_level": "video_quality",
    "texcompression_level": "highest_fidelity",
    "lock_framerate": "frame_control",
    "force_vsync": "frame_control",
    "reduce_mouselag": "frame_control",
    "enable_keyboardtips": "preserved_ui_preference",
    "enable_gameplaytips": "preserved_ui_preference",
    "NoBlood": "visual_effect",
    "NoCasings": "visual_effect",
    "NoSmoke": "visual_effect",
    "no_anim": "visual_effect",
}
PRESERVED_CONFIG_KEYS = {
    "hw3d_deviceno", "hw3d_name", "hw3d_guid",
    "enable_keyboardtips", "enable_gameplaytips", "hud_detail",
}
COMPARISON_CONTRACT = {
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


class RegistrationError(ValueError):
    pass


def _normalized_guid(value: object, label: str) -> str:
    if not isinstance(value, str):
        raise RegistrationError(f"{label} is malformed")
    compact = value.replace("-", "")
    if not GUID_HEX_PATTERN.fullmatch(compact):
        raise RegistrationError(f"{label} is malformed")
    return compact.casefold()


def _same_typed_value(actual: object, expected: object) -> bool:
    if type(actual) is not type(expected):
        return False
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return False
        return set(actual) == set(expected) and all(
            _same_typed_value(actual[key], value)
            for key, value in expected.items()
        )
    if isinstance(expected, list):
        if not isinstance(actual, list):
            return False
        return len(actual) == len(expected) and all(
            _same_typed_value(left, right)
            for left, right in zip(actual, expected, strict=True)
        )
    return actual == expected


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _video_option_catalog_sha256() -> str:
    payload = {
        "id": VIDEO_OPTION_CATALOG_ID,
        "policies": VIDEO_OPTION_POLICIES,
    }
    return hashlib.sha256(json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RegistrationError(f"could not read {label} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise RegistrationError(f"{label} must contain a JSON object")
    return value


def _validate_catalog(catalog: dict[str, Any]) -> str:
    if catalog.get("schema") != "opennova.render-fixtures.v2":
        raise RegistrationError("unsupported fixture catalog schema")
    if not _same_typed_value(
        catalog.get("comparison_contract"), COMPARISON_CONTRACT
    ):
        raise RegistrationError(
            "fixture catalog comparison_contract does not match the required "
            "HUD-hidden M16 Burst and bare-arms presentation"
        )
    payload = dict(catalog)
    claimed = payload.pop("catalog_sha256", "")
    if not isinstance(claimed, str) or not SHA256_PATTERN.fullmatch(claimed):
        raise RegistrationError("catalog_sha256 is absent or malformed")
    canonical = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode()
    actual = hashlib.sha256(canonical).hexdigest()
    if claimed != actual:
        raise RegistrationError(
            f"catalog_sha256 mismatch: declared={claimed} canonical={actual}"
        )
    return claimed


def _validate_video_profile_contract(catalog: dict[str, Any]) -> dict[str, Any]:
    contract = catalog.get("retail_video_profile_contract")
    expected = {
        "id": PROFILE_ID,
        "video_option_catalog_id": VIDEO_OPTION_CATALOG_ID,
        "video_option_catalog_sha256": _video_option_catalog_sha256(),
        "required_values": HIGHEST_VIDEO_PROFILE,
    }
    if not _same_typed_value(contract, expected):
        raise RegistrationError(
            "retail_video_profile_contract is absent, incomplete, or not the "
            "exhaustive highest-quality profile"
        )
    return dict(expected)


def _validate_profile_contract(
    catalog: dict[str, Any], expansion: str
) -> dict[str, Any]:
    contract = catalog.get("retail_profile_contract")
    expected_path = f"expansion/{expansion}/weapon.sav"
    if not isinstance(contract, dict) or set(contract) != {
        "path", "sha256", "profile_slot", "blue", "red",
    }:
        raise RegistrationError("retail_profile_contract is absent or malformed")
    if contract.get("path") != expected_path:
        raise RegistrationError(
            "retail_profile_contract does not select the active expansion weapon.sav"
        )
    if not isinstance(contract.get("sha256"), str) \
            or not SHA256_PATTERN.fullmatch(contract["sha256"]):
        raise RegistrationError("retail_profile_contract sha256 is malformed")
    if type(contract.get("profile_slot")) is not int \
            or contract.get("profile_slot") != 0:
        raise RegistrationError("retail_profile_contract must select profile slot 0")
    expected_blue = {
        "player_class": 9,
        "avatar_a": 2,
        "avatar_b": 0,
        "character_id": COMPARISON_CONTRACT["character_id"],
        "primary_weapon": COMPARISON_CONTRACT["equipped_weapon"],
    }
    expected_red = {
        "player_class": 9,
        "avatar_a": 7,
        "avatar_b": 0,
        "character_id": 0x8207,
    }
    if not _same_typed_value(contract.get("blue"), expected_blue) \
            or not _same_typed_value(contract.get("red"), expected_red):
        raise RegistrationError(
            "retail_profile_contract does not select the witnessed slot-0 profile"
        )
    return dict(contract)


def _validate_stage_manifest(
    path: Path,
    profile_contract: dict[str, Any],
    video_profile_contract: dict[str, Any],
) -> dict[str, Any]:
    stage = _load_json(path, "retail presentation stage manifest")
    if set(stage) != {
        "schema", "tool_version", "profile", "game_config", "weapon_profile",
    } or stage.get("schema") != "opennova.retail-presentation-stage.v3" \
            or stage.get("tool_version") != "3.0.0":
        raise RegistrationError(
            "retail presentation stage manifest schema/version is unsupported"
        )
    if not _same_typed_value(stage.get("profile"), video_profile_contract):
        raise RegistrationError(
            "retail presentation stage does not bind the catalog's exhaustive "
            "highest-quality video profile"
        )
    config = stage.get("game_config")
    if not isinstance(config, dict) or set(config) != {
        "file_name", "original_sha256", "effective_sha256",
        "original_values", "effective_values", "changed_keys",
        "preserved_values", "hud_detail",
    }:
        raise RegistrationError("retail staged game.cfg evidence is malformed")
    if config.get("file_name") != "game.cfg":
        raise RegistrationError("retail staged config must identify game.cfg")
    for field in ("original_sha256", "effective_sha256"):
        value = config.get(field)
        if not isinstance(value, str) or not SHA256_PATTERN.fullmatch(value):
            raise RegistrationError(f"retail staged game.cfg {field} is malformed")
    original_values = config.get("original_values")
    effective_values = config.get("effective_values")
    if not isinstance(original_values, dict) \
            or set(original_values) != set(HIGHEST_VIDEO_PROFILE):
        raise RegistrationError(
            "retail staged game.cfg original_values has an incomplete video key set"
        )
    for key, expected in HIGHEST_VIDEO_PROFILE.items():
        if type(original_values[key]) is not type(expected):
            raise RegistrationError(
                f"retail staged game.cfg original value {key} has the wrong type"
            )
    if not _same_typed_value(effective_values, HIGHEST_VIDEO_PROFILE):
        raise RegistrationError(
            "retail staged game.cfg effective_values is missing or below the "
            "highest-quality profile"
        )
    changed_keys = config.get("changed_keys")
    expected_changed = [
        key for key, required in HIGHEST_VIDEO_PROFILE.items()
        if original_values[key] != required
    ]
    if changed_keys != expected_changed:
        raise RegistrationError(
            "retail staged game.cfg changed_keys does not match the typed maps"
        )
    preserved = config.get("preserved_values")
    if not isinstance(preserved, dict) or set(preserved) != PRESERVED_CONFIG_KEYS:
        raise RegistrationError(
            "retail staged game.cfg preserved_values has an incomplete key set"
        )
    for key in ("hw3d_deviceno", "enable_keyboardtips", "enable_gameplaytips", "hud_detail"):
        if type(preserved.get(key)) is not int:
            raise RegistrationError(
                f"retail staged game.cfg preserved value {key} has the wrong type"
            )
    if preserved["hw3d_deviceno"] < 0 \
            or preserved["enable_keyboardtips"] not in {0, 1} \
            or preserved["enable_gameplaytips"] not in {0, 1}:
        raise RegistrationError("retail staged preserved DISPLAY values are invalid")
    if not isinstance(preserved.get("hw3d_name"), str) \
            or not preserved["hw3d_name"]:
        raise RegistrationError(
            "retail staged game.cfg preserved value hw3d_name is malformed"
        )
    _normalized_guid(
        preserved.get("hw3d_guid"),
        "retail staged game.cfg preserved value hw3d_guid",
    )
    original_detail = preserved["hud_detail"]
    if not 0 <= original_detail <= 3:
        raise RegistrationError(
            "retail staged game.cfg preserved hud_detail is malformed"
        )
    expected_hud = {
        "original": original_detail,
        "effective": original_detail,
        "unchanged": True,
    }
    if not _same_typed_value(config.get("hud_detail"), expected_hud):
        raise RegistrationError(
            "retail staging must prove hud_detail was unchanged"
        )
    profile = stage.get("weapon_profile")
    if not isinstance(profile, dict) or set(profile) != {
        "file_name", "sha256", "slot", "blue", "red",
        "blue_selected_kit_contains_wpn_m16burst",
    }:
        raise RegistrationError("retail staged weapon.sav evidence is malformed")
    if profile.get("file_name") != "weapon.sav" \
            or profile.get("sha256") != profile_contract["sha256"] \
            or type(profile.get("slot")) is not int \
            or profile.get("slot") != profile_contract["profile_slot"]:
        raise RegistrationError(
            "retail staged weapon.sav identity does not match the catalog"
        )
    expected_blue = dict(profile_contract["blue"])
    expected_blue["avatar_packed"] = expected_blue.pop("character_id")
    expected_blue.pop("primary_weapon")
    expected_red = dict(profile_contract["red"])
    expected_red["avatar_packed"] = expected_red.pop("character_id")
    if not _same_typed_value(profile.get("blue"), expected_blue) \
            or not _same_typed_value(profile.get("red"), expected_red) \
            or profile.get("blue_selected_kit_contains_wpn_m16burst") is not True:
        raise RegistrationError(
            "retail staged weapon.sav slot-0 facts do not match the catalog"
        )
    return stage


def _find_fixture(catalog: dict[str, Any], fixture_id: str) -> dict[str, Any]:
    matches = [
        row for row in catalog.get("fixtures", [])
        if isinstance(row, dict) and row.get("id") == fixture_id
    ]
    if len(matches) != 1:
        raise RegistrationError(
            f"catalog must contain fixture {fixture_id!r} exactly once"
        )
    return matches[0]


def _same_vector(actual: object, expected: object) -> bool:
    if not isinstance(actual, (list, tuple)) \
            or not isinstance(expected, (list, tuple)) \
            or len(actual) != len(expected):
        return False
    try:
        return all(
            abs(float(left) - float(right)) <= 1.0e-5
            for left, right in zip(actual, expected, strict=True)
        )
    except (TypeError, ValueError):
        return False


def _camera_matches(actual: object, expected: object) -> bool:
    if not isinstance(actual, dict) or not isinstance(expected, dict):
        return False
    if not _same_vector(actual.get("position"), expected.get("position")):
        return False
    for key in ("yaw_deg", "pitch_deg", "vertical_fov_deg"):
        try:
            if abs(float(actual[key]) - float(expected[key])) > 1.0e-5:
                return False
        except (KeyError, TypeError, ValueError):
            return False
    return True


def _structured(document: dict[str, Any]) -> dict[str, Any]:
    value = document.get("structuredContent", document)
    if not isinstance(value, dict):
        raise RegistrationError("MCP evidence has no structuredContent object")
    return value


def _validate_instance_status(
    document: dict[str, Any],
    instance_id: str,
    pid: int,
) -> dict[str, Any]:
    status = _structured(document)
    matches = [
        row for row in status.get("instances", [])
        if isinstance(row, dict)
        and row.get("instance_id") == instance_id
        and int(row.get("pid", 0)) == pid
    ]
    if len(matches) != 1:
        raise RegistrationError(
            "onhook_instances evidence does not contain the captured instance"
        )
    instance = matches[0]
    if instance.get("capture_bundle_supported") is not True:
        raise RegistrationError(
            "onhook_instances does not prove CAP_CAPTURE_BUNDLE support"
        )
    if instance.get("render_fixture_supported") is not True:
        raise RegistrationError("onhook instance cannot apply exact render fixtures")
    if instance.get("build_flavor") != "proxy":
        raise RegistrationError("registered retail capture requires the pinned proxy build")
    if instance.get("render_state_available") is not True \
            or instance.get("capture_ready") is not True:
        raise RegistrationError("onhook instance was not render/capture ready")
    if (instance.get("frame_width"), instance.get("frame_height")) != RETAIL_SIZE:
        raise RegistrationError("onhook instance backbuffer must be exactly 1920x1200")
    return instance


def _validate_fixture_result(
    document: dict[str, Any],
    fixture: dict[str, Any],
    catalog: dict[str, Any],
    instance_id: str,
) -> dict[str, Any]:
    result = _structured(document)
    if result.get("schema") != "opennova.render_fixture_result.v1" \
            or result.get("instance_id") != instance_id \
            or result.get("fixture_id") != fixture.get("id"):
        raise RegistrationError("fixture application result identity is mismatched")
    if result.get("exact") is not True:
        raise RegistrationError("fixture application was not exact")
    if result.get("frame_serial") in {None, "", "0", 0} \
            or result.get("frame_qpc") in {None, "", "0", 0}:
        raise RegistrationError("fixture application has no frame identity")
    requested = result.get("fixture")
    required_catalog_bindings = {
        "vertical_fov_degrees",
        "time_of_day_seconds",
        "mission_file",
        "mission_sha256",
    }
    if not isinstance(requested, dict) \
            or not required_catalog_bindings.issubset(requested):
        raise RegistrationError(
            "fixture application is missing required catalog bindings"
        )
    position = requested.get("position_bms", {})
    actual_position = [position.get(axis) for axis in ("x", "y", "z")]
    if not _same_vector(
        actual_position, fixture.get("retail_player_bms", {}).get("applied")
    ):
        raise RegistrationError("fixture application position does not match catalog")
    camera = fixture.get("camera_bms", {})
    for result_key, catalog_key in (
        ("yaw_degrees", "yaw_deg"),
        ("pitch_degrees", "pitch_deg"),
    ):
        try:
            if abs(float(requested[result_key]) - float(camera[catalog_key])) > 1.0e-5:
                raise RegistrationError(
                    f"fixture application {result_key} does not match catalog"
                )
        except (KeyError, TypeError, ValueError) as exc:
            raise RegistrationError(
                f"fixture application {result_key} is absent"
            ) from exc
    if requested.get("camera_mode") != "first_person":
        raise RegistrationError("fixture application is not first-person")
    mission_file = fixture.get("mission")
    requested_fov = requested["vertical_fov_degrees"]
    catalog_fov = camera.get("vertical_fov_deg")
    if type(requested_fov) is not float \
            or not math.isfinite(requested_fov) \
            or requested_fov != catalog_fov:
        raise RegistrationError("fixture application vertical FOV is mismatched")
    mission_sha = catalog.get("missions", {}).get(mission_file)
    if type(requested["mission_file"]) is not str \
            or type(requested["mission_sha256"]) is not str \
            or requested["mission_file"] != mission_file \
            or requested["mission_sha256"] != mission_sha:
        raise RegistrationError("fixture application mission identity is mismatched")
    minutes = fixture.get("minutes_of_day")
    if not isinstance(minutes, list) or len(minutes) != 1 \
            or type(minutes[0]) is not int \
            or not 0 <= minutes[0] < 1440:
        raise RegistrationError(
            "catalog fixture must declare exactly one integer minute of day"
        )
    expected_seconds = minutes[0] * 60
    if type(requested["time_of_day_seconds"]) is not int \
            or requested["time_of_day_seconds"] != expected_seconds:
        raise RegistrationError("fixture application time of day is mismatched")
    return result


def _positive_frame_identity(
    document: dict[str, Any], label: str
) -> tuple[int, int]:
    try:
        serial = int(document["frame_serial"])
        qpc = int(document["frame_qpc"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RegistrationError(f"{label} has no frame identity") from exc
    if serial <= 0 or qpc <= 0:
        raise RegistrationError(f"{label} has no frame identity")
    return serial, qpc


def _positive_decimal_string(value: object, label: str) -> int:
    if type(value) is not str or re.fullmatch(r"[1-9][0-9]*", value) is None:
        raise RegistrationError(f"{label} must be a positive decimal string")
    parsed = int(value)
    if parsed > 0xFFFFFFFFFFFFFFFF:
        raise RegistrationError(f"{label} exceeds the uint64 range")
    return parsed


def _nonnegative_integer(value: object, label: str) -> int:
    if type(value) is not int or value < 0:
        raise RegistrationError(f"{label} must be a non-negative integer")
    return value


def _time_of_day_fixed24(seconds: int) -> int:
    # Environment_SetCurrentTime takes unsigned 8.24 hours; the hook converts
    # the wire seconds with the engine's truncating integer semantics.
    return (seconds << 24) // 3600


def _validate_fixture_witness(
    state: dict[str, Any],
    fixture_result: dict[str, Any],
    capture_frame: tuple[int, int],
    fixture_frame: tuple[int, int],
    min_settle_seconds: float,
) -> dict[str, Any]:
    """Prove the capture frame still held the applied fixture, then the settle.

    The hook samples this witness on the capture frame itself (bridge protocol
    1.5): which exact apply it follows, whether the time-advance lease was still
    held, and the live clock against the requested fixed24. A capture taken
    seconds after the apply registers only when the clock never moved.
    """
    witness = state.get("fixture_binding")
    if not isinstance(witness, dict) \
            or witness.get("source") != "capture_frame_fixture_witness.v1":
        raise RegistrationError(
            "retail sidecar has no capture-frame fixture witness; the onHook "
            "build predates bridge protocol 1.5"
        )
    if witness.get("applied") is not True:
        raise RegistrationError(
            "retail capture frame witnessed no exact fixture application"
        )
    apply_frame = (
        _positive_decimal_string(
            witness.get("apply_frame_serial"), "fixture witness apply frame serial"
        ),
        _positive_decimal_string(
            witness.get("apply_frame_qpc"), "fixture witness apply frame qpc"
        ),
    )
    if apply_frame != fixture_frame:
        raise RegistrationError(
            "retail capture frame witnessed a different fixture application"
        )
    frequency = _positive_decimal_string(
        witness.get("qpc_frequency"), "fixture witness qpc frequency"
    )
    time_of_day = witness.get("time_of_day")
    if not isinstance(time_of_day, dict) \
            or time_of_day.get("requested") is not True \
            or time_of_day.get("witnessed") is not True:
        raise RegistrationError(
            "retail capture frame did not witness the applied time of day"
        )
    expected_fixed24 = _time_of_day_fixed24(
        int(fixture_result["fixture"]["time_of_day_seconds"])
    )
    requested_fixed24 = _nonnegative_integer(
        time_of_day.get("requested_fixed24"), "fixture witness requested clock"
    )
    current_fixed24 = _nonnegative_integer(
        time_of_day.get("current_fixed24"), "fixture witness live clock"
    )
    advance_fixed24 = _nonnegative_integer(
        time_of_day.get("advance_fixed24"), "fixture witness live clock advance"
    )
    if requested_fixed24 != expected_fixed24 \
            or current_fixed24 != expected_fixed24 \
            or advance_fixed24 != 0 \
            or time_of_day.get("pinned") is not True:
        raise RegistrationError(
            "retail time of day was not pinned on the capture frame"
        )
    frames_after_apply = capture_frame[0] - fixture_frame[0]
    seconds_after_apply = (capture_frame[1] - fixture_frame[1]) / frequency
    if seconds_after_apply < min_settle_seconds:
        raise RegistrationError(
            f"retail capture settled {seconds_after_apply:.3f} s after the "
            f"fixture application, under the {min_settle_seconds:.3f} s "
            "registration floor (--min-settle-seconds)"
        )
    lease = witness.get("lease", {})
    if not isinstance(lease, dict):
        raise RegistrationError("fixture witness lease block is malformed")
    return {
        "witness": "capture_frame_fixture_witness.v1",
        "apply_frame_serial": str(apply_frame[0]),
        "apply_frame_qpc": str(apply_frame[1]),
        "frames_after_apply": frames_after_apply,
        "seconds_after_apply": seconds_after_apply,
        "min_settle_seconds": min_settle_seconds,
        "qpc_frequency": str(frequency),
        "time_of_day_fixed24": expected_fixed24,
        "time_of_day_pinned": True,
        "advance_lease": {
            "started": lease.get("started") is True,
            "held_on_capture_frame": lease.get("held_on_capture_frame") is True,
        },
    }


def _validate_capture_result(
    document: dict[str, Any],
    instance_id: str,
    fixture_id: str,
    image_path: Path,
    state_path: Path,
    state: dict[str, Any],
    fixture_result: dict[str, Any],
    min_settle_seconds: float,
) -> dict[str, Any]:
    result = _structured(document)
    if result.get("schema") != "opennova.render_capture_bundle.v4" \
            or result.get("instance_id") != instance_id:
        raise RegistrationError("capture result identity is mismatched")
    if type(result.get("profile_id")) is not str \
            or result["profile_id"] != PROFILE_ID:
        raise RegistrationError("capture result profile identity is mismatched")
    if type(result.get("fixture_id")) is not str \
            or result["fixture_id"] != fixture_id:
        raise RegistrationError("capture result fixture identity is mismatched")
    try:
        result_image = Path(str(result["path"])).resolve()
        result_state = Path(str(result["state_path"])).resolve()
    except (KeyError, TypeError, ValueError) as exc:
        raise RegistrationError("capture result has no artifact paths") from exc
    if result_image != image_path or result_state != state_path:
        raise RegistrationError("capture result does not bind the selected raw pair")
    if result.get("mime_type") != "image/png":
        raise RegistrationError("capture result is not a PNG artifact")
    try:
        file_bytes = int(result["file_bytes"])
        state_bytes = int(result["state_bytes"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RegistrationError("capture result has no artifact byte counts") from exc
    if file_bytes != image_path.stat().st_size \
            or state_bytes != state_path.stat().st_size:
        raise RegistrationError("capture result byte counts do not bind the raw pair")
    source = result.get("source", {})
    if (source.get("width"), source.get("height")) != RETAIL_SIZE:
        raise RegistrationError("capture result source is not 1920x1200")
    state_image = state.get("image", {})
    if (state_image.get("width"), state_image.get("height")) != RETAIL_SIZE:
        raise RegistrationError("retail sidecar image is not 1920x1200")
    if state_image.get("sha256") != _sha256(image_path):
        raise RegistrationError("retail sidecar image hash does not bind the raw PNG")
    capture_frame = _positive_frame_identity(result, "capture result")
    state_frame = _positive_frame_identity(state, "retail sidecar")
    fixture_frame = _positive_frame_identity(
        fixture_result, "fixture application"
    )
    if capture_frame != state_frame:
        raise RegistrationError("capture result frame does not match retail sidecar")
    if capture_frame[0] <= fixture_frame[0] \
            or capture_frame[1] <= fixture_frame[1]:
        raise RegistrationError(
            "retail capture must follow the exact fixture application"
        )
    return _validate_fixture_witness(
        state, fixture_result, capture_frame, fixture_frame, min_settle_seconds
    )


def _finite_matrix(value: object) -> bool:
    if not isinstance(value, list) or len(value) != 16:
        return False
    try:
        return all(math.isfinite(float(component)) for component in value)
    except (TypeError, ValueError):
        return False


def _camera_bms_from_row_major_view(view: list[object]) -> list[float]:
    matrix = [float(component) for component in view]
    if any(abs(matrix[index]) > 1.0e-4 for index in (3, 7, 11)) \
            or abs(matrix[15] - 1.0) > 1.0e-4:
        raise RegistrationError("retail view matrix is not a rigid row-major transform")
    rotation = [
        matrix[0:3],
        matrix[4:7],
        matrix[8:11],
    ]
    for row_index, row in enumerate(rotation):
        length_squared = sum(component * component for component in row)
        if abs(length_squared - 1.0) > 0.01:
            raise RegistrationError("retail view matrix rotation is not orthonormal")
        for other_index in range(row_index):
            dot = sum(
                row[column] * rotation[other_index][column]
                for column in range(3)
            )
            if abs(dot) > 0.01:
                raise RegistrationError("retail view matrix rotation is not orthonormal")
    determinant = (
        rotation[0][0] * (
            rotation[1][1] * rotation[2][2]
            - rotation[1][2] * rotation[2][1]
        )
        - rotation[0][1] * (
            rotation[1][0] * rotation[2][2]
            - rotation[1][2] * rotation[2][0]
        )
        + rotation[0][2] * (
            rotation[1][0] * rotation[2][1]
            - rotation[1][1] * rotation[2][0]
        )
    )
    if abs(determinant - 1.0) > 0.01:
        raise RegistrationError("retail view matrix rotation has invalid handedness")
    a, b, c = rotation[0]
    d, e, f = rotation[1]
    g, h, i = rotation[2]
    inverse_rotation = [
        [(e * i - f * h) / determinant,
         (c * h - b * i) / determinant,
         (b * f - c * e) / determinant],
        [(f * g - d * i) / determinant,
         (a * i - c * g) / determinant,
         (c * d - a * f) / determinant],
        [(d * h - e * g) / determinant,
         (b * g - a * h) / determinant,
         (a * e - b * d) / determinant],
    ]
    translation = matrix[12:15]
    camera_render = [
        -sum(
            translation[index] * inverse_rotation[index][axis]
            for index in range(3)
        )
        for axis in range(3)
    ]
    # Retail render coordinates are (-BMS Y, BMS Z, BMS X).
    return [camera_render[2], -camera_render[0], camera_render[1]]


def _validate_render_state(
    state: dict[str, Any],
    fixture: dict[str, Any],
    catalog: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    telemetry = state.get("telemetry", {})
    available = telemetry.get("available", [])
    if not isinstance(available, list) or not {
        "view_matrix", "projection_matrix", "viewport"
    }.issubset(available):
        raise RegistrationError(
            "retail capture lacks required view/projection/viewport telemetry"
        )
    render = state.get("render_state", {})
    view = render.get("view_matrix_row_major")
    projection = render.get("projection_matrix_row_major")
    if not _finite_matrix(view) or not _finite_matrix(projection):
        raise RegistrationError("retail capture matrices are absent or non-finite")
    viewport = render.get("viewport", {})
    if (viewport.get("x"), viewport.get("y"),
            viewport.get("width"), viewport.get("height")) != (0, 0, 1920, 1200):
        raise RegistrationError("retail capture viewport is not the full 1920x1200 frame")

    expected_fov = float(fixture["camera_bms"]["vertical_fov_deg"])
    expected_m11 = 1.0 / math.tan(math.radians(expected_fov) * 0.5)
    declared_frustum = catalog.get("capture", {}).get("resolution", [])
    if not isinstance(declared_frustum, list) or len(declared_frustum) != 2:
        raise RegistrationError("catalog has no declared OpenNova frustum resolution")
    expected_m00 = expected_m11 / (
        float(declared_frustum[0]) / float(declared_frustum[1])
    )
    if abs(float(projection[0]) - expected_m00) > 0.005 \
            or abs(float(projection[5]) - expected_m11) > 0.005:
        raise RegistrationError("retail projection matrix does not match fixture frustum")

    observed = state.get("observed_state", {})
    if observed.get("player_present") is not True \
            or observed.get("capture_ready") is not True \
            or observed.get("camera_available") is not True \
            or observed.get("camera_mode") != "first_person":
        raise RegistrationError("retail observed camera/player state is unavailable")
    position = observed.get("position_bms", {})
    try:
        player = [float(position[axis]) for axis in ("x", "y", "z")]
        yaw = float(fixture["camera_bms"]["yaw_deg"])
        pitch = float(fixture["camera_bms"]["pitch_deg"])
    except (KeyError, TypeError, ValueError) as exc:
        raise RegistrationError("retail observed player pose is absent") from exc
    # Preserve the correlated player telemetry, but do not compare it to the
    # APPLY_POSE request: retail physics can settle the body before CAP. The
    # inverse view matrix below is the frame's authoritative visual camera.
    bam_per_degree = 4294967296.0 / 360.0
    expected_heading_unsigned = int(
        math.fmod(90.0 - yaw, 360.0) % 360.0 * bam_per_degree
    ) & 0xFFFFFFFF
    expected_pitch_unsigned = int(pitch * bam_per_degree) & 0xFFFFFFFF
    try:
        observed_heading = int(observed["heading_bam"]) & 0xFFFFFFFF
        observed_pitch = int(observed["pitch_bam"]) & 0xFFFFFFFF
    except (KeyError, TypeError, ValueError) as exc:
        raise RegistrationError("retail observed yaw/pitch BAM is absent") from exc
    bam_tolerance = int(0.1 * bam_per_degree)
    for label, actual, expected in (
        ("yaw", observed_heading, expected_heading_unsigned),
        ("pitch", observed_pitch, expected_pitch_unsigned),
    ):
        circular_delta = abs(((actual - expected + (1 << 31)) & 0xFFFFFFFF)
                             - (1 << 31))
        if circular_delta > bam_tolerance:
            raise RegistrationError(
                f"retail observed {label} differs from fixture by more than 0.1 degrees"
            )
    observed_camera_position = _camera_bms_from_row_major_view(view)
    expected_position = fixture["camera_bms"]["position"]
    if any(
        abs(actual - float(expected)) > 0.05
        for actual, expected in zip(
            observed_camera_position, expected_position, strict=True
        )
    ):
        raise RegistrationError(
            "retail view matrix camera does not reproduce the fixture within 0.05 units"
        )
    realized_camera = {
        "position": observed_camera_position,
        "yaw_deg": yaw,
        "pitch_deg": pitch,
        "vertical_fov_deg": expected_fov,
    }
    return realized_camera, {
        "view_matrix_row_major": view,
        "projection_matrix_row_major": projection,
        "viewport": viewport,
        "observed_player_bms": player,
        "camera_position_source": "inverse_correlated_view_matrix",
    }


def _find_case_insensitive(directory: Path, name: str) -> Path:
    if not directory.is_dir():
        raise RegistrationError(f"retail directory is absent: {directory}")
    matches = [
        entry for entry in directory.iterdir()
        if entry.is_file() and entry.name.casefold() == name.casefold()
    ]
    if len(matches) != 1:
        raise RegistrationError(
            f"expected exactly one retail provenance file {name} in {directory}"
        )
    return matches[0]


def _relative(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError as exc:
        raise RegistrationError(f"retail provenance path escapes game dir: {path}") \
            from exc


def _portable_relative(path: Path, root: Path, label: str) -> str:
    try:
        relative = path.resolve().relative_to(root.resolve())
    except ValueError as exc:
        raise RegistrationError(
            f"{label} must be colocated under the registered bundle directory"
        ) from exc
    if relative.is_absolute() or ".." in relative.parts:
        raise RegistrationError(f"{label} is not a portable relative path")
    return relative.as_posix()


def _retail_install_provenance(game_dir: Path, expansion: str) -> dict[str, Any]:
    expansion_dir = game_dir / "expansion" / expansion
    ordered = [
        _find_case_insensitive(expansion_dir, f"{expansion}L.pff"),
        _find_case_insensitive(expansion_dir, f"{expansion}.pff"),
        _find_case_insensitive(game_dir, "language.pff"),
        _find_case_insensitive(game_dir, "localres.pff"),
        _find_case_insensitive(game_dir, "resource.pff"),
    ]
    marker = _find_case_insensitive(expansion_dir, f"{expansion}.bin")
    return {
        "expansion": expansion,
        "mounted_archives": [
            {"path": _relative(path, game_dir), "sha256": _sha256(path)}
            for path in ordered
        ],
        "version_marker": {
            "path": _relative(marker, game_dir),
            "sha256": _sha256(marker),
        },
    }


def _resolve_packed_mission(
    game_dir: Path,
    expansion: str,
    mission_file: str,
) -> tuple[bytes, str]:
    try:
        with Vfs() as vfs:
            if not vfs.mount_game(str(game_dir), expansion):
                raise RegistrationError(
                    f"could not mount retail game through engine VFS: {vfs.last_error()}"
                )
            payload = vfs.read_file(mission_file)
            if payload is None:
                raise RegistrationError(
                    f"engine VFS did not resolve logical mission {mission_file}"
                )
            source_archive = ""
            for index in range(vfs.file_count()):
                row = vfs.file_at(index)
                if row is not None and row[0].casefold() == mission_file.casefold():
                    if row[1] == "pff" and row[2]:
                        source_archive = _relative(Path(row[2]), game_dir)
                    break
            if not source_archive:
                raise RegistrationError(
                    "retail mission must resolve from a mounted PFF, not loose caller data"
                )
            return payload, source_archive
    except RegistrationError:
        raise
    except Exception as exc:
        raise RegistrationError(f"engine VFS mission lookup failed: {exc}") from exc


def _validate_runtime_video_witness(
    state: dict[str, Any],
    retail_stage: dict[str, Any],
) -> dict[str, Any]:
    video = state.get("video_settings")
    if not isinstance(video, dict) or set(video) != {
        "profile_id", "source", "available", "unavailable", "values",
    }:
        raise RegistrationError("retail runtime video-settings witness is malformed")
    if video.get("profile_id") != PROFILE_ID \
            or video.get("source") != "engine_runtime_global":
        raise RegistrationError(
            "retail runtime video-settings witness has the wrong profile or provenance"
        )
    available = video.get("available")
    if not isinstance(available, list) \
            or any(not isinstance(key, str) for key in available) \
            or len(available) != len(set(available)) \
            or set(available) != set(RUNTIME_VIDEO_PROFILE) \
            or video.get("unavailable") != []:
        raise RegistrationError(
            "retail runtime video-settings witness does not expose the exact required key set"
        )
    values = video.get("values")
    values_match = isinstance(values, dict) \
        and set(values) == set(RUNTIME_VIDEO_PROFILE)
    if values_match:
        for key, expected in RUNTIME_VIDEO_PROFILE.items():
            actual = values[key]
            if key == "gamma":
                values_match = type(actual) is float \
                    and math.isfinite(actual) \
                    and actual == RUNTIME_GAMMA_FLOAT32
            else:
                values_match = _same_typed_value(actual, expected)
            if not values_match:
                break
    if not values_match:
        raise RegistrationError(
            "retail runtime video setting is missing, type-coerced, or below the "
            "highest-quality profile"
        )

    d3d = state.get("d3d_device")
    if not isinstance(d3d, dict) or set(d3d) != {
        "frame_serial", "frame_qpc", "adapter", "device_generation",
        "reset_generation", "windowed", "backbuffer", "depth_format",
        "multisample",
    }:
        raise RegistrationError("retail D3D device witness is malformed")
    root_frame_serial = _positive_decimal_string(
        state.get("frame_serial"), "retail sidecar frame_serial"
    )
    root_frame_qpc = _positive_decimal_string(
        state.get("frame_qpc"), "retail sidecar frame_qpc"
    )
    d3d_frame_serial = _positive_decimal_string(
        d3d.get("frame_serial"), "retail D3D frame_serial"
    )
    d3d_frame_qpc = _positive_decimal_string(
        d3d.get("frame_qpc"), "retail D3D frame_qpc"
    )
    if d3d_frame_serial != root_frame_serial \
            or d3d_frame_qpc != root_frame_qpc:
        raise RegistrationError(
            "retail D3D frame identity does not match the pre-HUD cutpoint"
        )
    preserved = retail_stage["game_config"]["preserved_values"]
    adapter = d3d.get("adapter")
    if not isinstance(adapter, dict) or set(adapter) != {
        "ordinal", "name", "guid",
    } or type(adapter.get("ordinal")) is not int \
            or not isinstance(adapter.get("name"), str):
        raise RegistrationError("retail D3D adapter witness is malformed")
    adapter_guid = _normalized_guid(
        adapter.get("guid"), "retail D3D adapter GUID"
    )
    expected_guid = _normalized_guid(
        preserved["hw3d_guid"], "retail staged adapter GUID"
    )
    if adapter["ordinal"] != preserved["hw3d_deviceno"] \
            or adapter["name"] != preserved["hw3d_name"] \
            or adapter_guid != expected_guid:
        raise RegistrationError(
            "retail D3D adapter does not match staged device identity"
        )
    if type(d3d.get("device_generation")) is not int \
            or d3d["device_generation"] <= 0 \
            or type(d3d.get("reset_generation")) is not int \
            or d3d["reset_generation"] < 0:
        raise RegistrationError("retail D3D device/reset generation is malformed")
    if d3d.get("windowed") is not True:
        raise RegistrationError("retail D3D device must prove the /w windowed launch")
    expected_backbuffer = {
        "width": RETAIL_SIZE[0],
        "height": RETAIL_SIZE[1],
        "format": "D3DFMT_X8R8G8B8",
    }
    if not _same_typed_value(d3d.get("backbuffer"), expected_backbuffer) \
            or d3d.get("depth_format") != "D3DFMT_D24S8":
        raise RegistrationError(
            "retail D3D backbuffer/depth witness does not match the reference path"
        )
    multisample = d3d.get("multisample")
    multisample_error = (
        "retail D3D multisample witness does not prove effective "
        "highest-retail-selectable 4x MSAA"
    )
    expected_multisample = {
        "mode_token": HIGHEST_VIDEO_PROFILE["antialias_mode"],
        "token_semantics": "one_based_ordinal_into_retail_selectable",
        "type": "D3DMULTISAMPLE_4_SAMPLES",
        "samples": 4,
        "quality": 0,
        "retail_selectable": {
            "source": "engine_runtime_aa_mode_list",
            "maskable_samples": [2, 4],
            "highest_samples": 4,
            "is_selected_highest": True,
        },
        "hardware_usable": {
            "source": "d3d9_runtime_color_depth_intersection",
            "maskable_samples": [2, 4, 8],
            "highest_samples": 8,
        },
    }
    # Validate the two domains relationally before pinning their authoritative
    # values. The config token is a one-based ordinal into retail's runtime
    # list, not a sample count, and hardware support is a separate D3D9 domain.
    if not isinstance(multisample, dict) \
            or set(multisample) != set(expected_multisample):
        raise RegistrationError(multisample_error)
    retail_selectable = multisample.get("retail_selectable")
    hardware_usable = multisample.get("hardware_usable")
    if not isinstance(retail_selectable, dict) \
            or set(retail_selectable) \
            != set(expected_multisample["retail_selectable"]) \
            or not isinstance(hardware_usable, dict) \
            or set(hardware_usable) \
            != set(expected_multisample["hardware_usable"]):
        raise RegistrationError(multisample_error)
    retail_modes = retail_selectable.get("maskable_samples")
    hardware_modes = hardware_usable.get("maskable_samples")
    if not isinstance(retail_modes, list) or not retail_modes \
            or any(type(value) is not int for value in retail_modes) \
            or retail_modes != sorted(set(retail_modes)) \
            or not isinstance(hardware_modes, list) or not hardware_modes \
            or any(type(value) is not int for value in hardware_modes) \
            or hardware_modes != sorted(set(hardware_modes)):
        raise RegistrationError(multisample_error)
    token = multisample.get("mode_token")
    samples = multisample.get("samples")
    if type(token) is not int or type(samples) is not int \
            or type(retail_selectable.get("highest_samples")) is not int \
            or type(hardware_usable.get("highest_samples")) is not int \
            or retail_selectable["highest_samples"] != max(retail_modes) \
            or hardware_usable["highest_samples"] != max(hardware_modes) \
            or not set(retail_modes).issubset(hardware_modes) \
            or token < 1 or token > len(retail_modes) \
            or retail_modes[token - 1] != samples \
            or samples != max(retail_modes):
        raise RegistrationError(multisample_error)
    if not _same_typed_value(multisample, expected_multisample):
        raise RegistrationError(multisample_error)

    presentation = state.get("presentation")
    if not isinstance(presentation, dict) or set(presentation) != {
        "policy", "generation", "armed_frame_serial", "armed_frame_qpc",
        "target_frame_serial", "target_frame_qpc", "snapshot_count",
        "boundary", "before_retail_hud", "d3d_scene_split", "retail_ui",
    }:
        raise RegistrationError(
            "retail pre-HUD snapshot presentation witness is malformed"
        )
    boundary = presentation.get("boundary")
    if presentation.get("policy") \
            != "retail_pre_hud_backbuffer_snapshot.v2" \
            or type(presentation.get("generation")) is not int \
            or presentation["generation"] <= 0 \
            or type(presentation.get("snapshot_count")) is not int \
            or presentation["snapshot_count"] != 1 \
            or type(boundary) is not str \
            or boundary not in {
                "world_labels", "pre_feed",
            } \
            or presentation.get("before_retail_hud") is not True \
            or not _same_typed_value(
                presentation.get("d3d_scene_split"),
                {"end_count": 1, "begin_count": 1, "restored": True},
            ) \
            or not _same_typed_value(
                presentation.get("retail_ui"),
                {"original_call_count": 1, "executed_unmodified": True},
            ):
        raise RegistrationError(
            "retail presentation does not prove one pre-HUD snapshot with "
            "the D3D scene and ordinary retail UI restored"
        )
    armed_serial = _positive_decimal_string(
        presentation.get("armed_frame_serial"),
        "retail presentation armed_frame_serial",
    )
    armed_qpc = _positive_decimal_string(
        presentation.get("armed_frame_qpc"),
        "retail presentation armed_frame_qpc",
    )
    target_serial = _positive_decimal_string(
        presentation.get("target_frame_serial"),
        "retail presentation target_frame_serial",
    )
    target_qpc = _positive_decimal_string(
        presentation.get("target_frame_qpc"),
        "retail presentation target_frame_qpc",
    )
    if armed_serial + 1 != target_serial \
            or target_serial != root_frame_serial \
            or not armed_qpc < root_frame_qpc <= target_qpc:
        raise RegistrationError(
            "retail presentation timing does not bind the pre-HUD cutpoint "
            "to the exact next target frame"
        )
    return {
        "video_settings": video,
        "d3d_device": d3d,
        "presentation": presentation,
    }


def _validate_raw_state(
    state: dict[str, Any],
    fixture: dict[str, Any],
) -> tuple[int, str, dict[str, Any]]:
    if state.get("schema") != "opennova.render_capture_bundle.v4":
        raise RegistrationError("unsupported raw retail capture bundle schema")
    source = state.get("source", {})
    if not isinstance(source, dict) \
            or type(source.get("bridge_version_major")) is not int \
            or source["bridge_version_major"] != 1 \
            or type(source.get("bridge_version_minor")) is not int \
            or source["bridge_version_minor"] != 5 \
            or type(source.get("hook_version")) is not str \
            or source["hook_version"] != "0.6.0":
        raise RegistrationError(
            "retail capture producer version must be bridge 1.5 and hook 0.6.0"
        )
    if source.get("pre_overlay") is not True \
            or source.get("frame_correlated") is not True:
        raise RegistrationError(
            "retail sidecar must be pre-overlay and frame-correlated"
        )
    fixture_context = state.get("fixture_context", {})
    if fixture_context.get("fixture_id") != fixture.get("id"):
        raise RegistrationError("retail sidecar fixture context is mismatched")
    _positive_decimal_string(
        state.get("frame_serial"), "retail sidecar frame_serial"
    )
    _positive_decimal_string(state.get("frame_qpc"), "retail sidecar frame_qpc")
    image = state.get("image", {})
    if (image.get("width"), image.get("height")) != RETAIL_SIZE:
        raise RegistrationError("retail state image must be exactly 1920x1200")
    try:
        pid = int(source.get("pid"))
    except (TypeError, ValueError) as exc:
        raise RegistrationError("retail sidecar source PID is absent") from exc
    if pid <= 0:
        raise RegistrationError("retail sidecar source PID is absent")
    instance_id = source.get("instance_id")
    if not isinstance(instance_id, str) or not instance_id:
        raise RegistrationError("retail sidecar source instance_id is absent")
    try:
        instance_pid, instance_suffix = instance_id.split("-", 1)
        process_start_time = int(str(source["process_start_time"]))
        encoded_start_time = int(instance_suffix, 16)
    except (KeyError, TypeError, ValueError) as exc:
        raise RegistrationError(
            "retail sidecar process-start identity is absent"
        ) from exc
    if instance_pid != str(pid) or process_start_time != encoded_start_time:
        raise RegistrationError(
            "retail sidecar process start does not match instance_id"
        )
    return pid, instance_id, source


def _validate_launch_log(
    log_path: Path, pid: int, instance_id: str, mission_file: str
) -> None:
    try:
        body = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise RegistrationError(f"could not read onHook launch log: {exc}") from exc
    try:
        instance_pid, instance_suffix = instance_id.split("-", 1)
    except ValueError as exc:
        raise RegistrationError("capture instance_id has no process-start suffix") \
            from exc
    if instance_pid != str(pid) or not re.fullmatch(
        r"[0-9A-Fa-f]+", instance_suffix
    ):
        raise RegistrationError("capture instance_id is not bound to its PID")
    pipe_name = f"opennova.onhook.v1.{pid}.{instance_suffix}"
    if pipe_name.casefold() not in body.casefold():
        raise RegistrationError(
            "onHook launch log is not bound to the exact capture instance"
        )
    starts = re.findall(r"Debug LAN host: starting mission '([^']+)'", body)
    if not starts or starts[-1].casefold() != mission_file.casefold():
        raise RegistrationError("onHook launch log does not start the fixture mission")


def _require_file(path: Path, label: str) -> Path:
    path = path.resolve()
    if not path.is_file():
        raise RegistrationError(f"{label} not found: {path}")
    return path


def _require_game_binary(
    path: Path,
    game_dir: Path,
    file_name: str,
    label: str,
) -> Path:
    binary = _require_file(path, label)
    expected = game_dir / file_name
    expected_lexical = os.path.normcase(os.path.abspath(expected))
    if os.path.normcase(str(binary)) != expected_lexical:
        raise RegistrationError(
            f"{label} must be {expected} inside --game-dir"
        )
    return binary


def register_retail_reference(args: argparse.Namespace) -> None:
    output_path = Path(args.output).resolve()
    if output_path.exists():
        raise RegistrationError(f"refusing to overwrite existing output: {output_path}")
    if args.confirm_retail_presentation_contract is not True:
        raise RegistrationError(
            "retail registration requires explicit visual confirmation that the "
            "hashed frame satisfies the comparison presentation contract"
        )
    catalog_path = _require_file(Path(args.catalog), "fixture catalog")
    state_path = _require_file(Path(args.raw_state), "raw retail state")
    instance_status_path = _require_file(
        Path(args.instance_status), "onhook_instances evidence"
    )
    fixture_result_path = _require_file(
        Path(args.fixture_result), "render fixture result"
    )
    capture_result_path = _require_file(
        Path(args.capture_result), "capture bundle result"
    )
    image_path = _require_file(Path(args.raw_image), "raw retail image")
    log_path = _require_file(Path(args.onhook_log), "onHook launch log")
    stage_manifest_path = _require_file(
        Path(args.retail_stage_manifest), "retail presentation stage manifest"
    )
    game_dir = Path(args.game_dir).resolve()
    if not game_dir.is_dir():
        raise RegistrationError(f"retail game dir not found: {game_dir}")

    catalog = _load_json(catalog_path, "fixture catalog")
    catalog_sha = _validate_catalog(catalog)
    profile_contract = _validate_profile_contract(catalog, args.expansion)
    video_profile_contract = _validate_video_profile_contract(catalog)
    retail_stage = _validate_stage_manifest(
        stage_manifest_path, profile_contract, video_profile_contract
    )
    stage_relative = _portable_relative(
        stage_manifest_path,
        output_path.parent,
        "retail presentation stage manifest",
    )
    if len(PurePosixPath(stage_relative).parts) != 1:
        raise RegistrationError(
            "retail presentation stage manifest must be a colocated sibling"
        )
    live_game_config = _require_file(
        game_dir / "game.cfg", "live retail game.cfg"
    )
    if _sha256(live_game_config) \
            != retail_stage["game_config"]["effective_sha256"]:
        raise RegistrationError(
            "live game.cfg hash does not match the staged effective config"
        )
    live_weapon_profile = _require_file(
        game_dir / profile_contract["path"], "live retail weapon.sav"
    )
    if _sha256(live_weapon_profile) != profile_contract["sha256"]:
        raise RegistrationError(
            "live weapon.sav hash does not match the staged catalog profile"
        )
    fixture = _find_fixture(catalog, args.fixture_id)
    if fixture.get("capture_mode") != COMPARISON_CONTRACT["capture_mode"]:
        raise RegistrationError(
            "registered retail fixture capture_mode must be hud_hidden"
        )
    mission_file = fixture.get("mission")
    mission_sha = catalog.get("missions", {}).get(mission_file)
    if not isinstance(mission_file, str) \
            or not isinstance(mission_sha, str) \
            or not SHA256_PATTERN.fullmatch(mission_sha):
        raise RegistrationError("fixture mission identity is absent from catalog")

    state = _load_json(state_path, "raw retail state")
    source_identity = state.get("source", {})
    if not isinstance(source_identity, dict):
        raise RegistrationError(
            "retail capture producer version must be bridge 1.5 and hook 0.6.0"
        )
    try:
        source_pid = int(source_identity.get("pid"))
    except (TypeError, ValueError) as exc:
        raise RegistrationError("retail sidecar source PID is absent") from exc
    source_instance_id = source_identity.get("instance_id")
    if not isinstance(source_instance_id, str) or not source_instance_id:
        raise RegistrationError("retail sidecar source instance_id is absent")
    instance_status = _load_json(
        instance_status_path, "onhook_instances evidence"
    )
    instance = _validate_instance_status(
        instance_status, source_instance_id, source_pid
    )
    fixture_result_document = _load_json(
        fixture_result_path, "render fixture result"
    )
    fixture_result = _validate_fixture_result(
        fixture_result_document, fixture, catalog, source_instance_id
    )
    pid, instance_id, source = _validate_raw_state(state, fixture)
    runtime_video_witness = _validate_runtime_video_witness(state, retail_stage)
    capture_result_document = _load_json(
        capture_result_path, "capture bundle result"
    )
    settle = _validate_capture_result(
        capture_result_document,
        instance_id,
        fixture["id"],
        image_path,
        state_path,
        state,
        fixture_result,
        float(args.min_settle_seconds),
    )
    realized_camera, render_state = _validate_render_state(
        state, fixture, catalog
    )
    _validate_launch_log(log_path, pid, instance_id, mission_file)
    image = QImage(str(image_path))
    if image.isNull() or (image.width(), image.height()) != RETAIL_SIZE:
        raise RegistrationError("raw retail image must be exactly 1920x1200")

    mission_bytes, mission_archive = _resolve_packed_mission(
        game_dir, args.expansion, mission_file
    )
    resolved_sha = hashlib.sha256(mission_bytes).hexdigest()
    if resolved_sha != mission_sha:
        raise RegistrationError(
            f"engine VFS mission hash mismatch: catalog={mission_sha} resolved={resolved_sha}"
        )
    install = _retail_install_provenance(game_dir, args.expansion)

    source_commit = args.opennova_source_commit.lower()
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise RegistrationError(
            "--opennova-source-commit must be a full 40-character Git SHA"
        )
    retail_executable = _require_game_binary(
        Path(args.retail_executable), game_dir, "Jointops.exe",
        "retail executable",
    )
    onhook_mcp = _require_file(Path(args.onhook_mcp), "onHook MCP executable")
    onhook_proxy = _require_game_binary(
        Path(args.onhook_proxy), game_dir, "binkw32.dll",
        "onHook proxy binary",
    )
    onhook_forwarder = _require_game_binary(
        Path(args.onhook_forwarder), game_dir, "binkw32_.dll",
        "onHook proxy forwarder",
    )
    if str(source.get("executable", "")).casefold() \
            != retail_executable.name.casefold() \
            or str(instance.get("executable", "")).casefold() \
            != retail_executable.name.casefold():
        raise RegistrationError("retail executable identity is mismatched")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    image_relative = _portable_relative(
        image_path, output_path.parent, "raw retail image"
    )
    image_sha = _sha256(image_path)
    registration = {
        "schema": "opennova.registered-retail-capture.v5",
        "tool": {"name": "register_retail_capture", "version": TOOL_VERSION},
        "catalog_sha256": catalog_sha,
        "fixture_id": fixture["id"],
        "mission": {
            "file": mission_file,
            "sha256": mission_sha,
            "source_archive": mission_archive,
            "verified": True,
            "verification": [
                "engine_vfs_logical_lookup",
                "same_pid_onhook_launch_log",
            ],
        },
        "capture": {
            "mode": fixture["capture_mode"],
            "retail_presentation": "game_composite_pre_retail_hud",
            "retail_gameplay_hud_visible": False,
            "onhook_overlay_present": False,
            "presentation_policy": "retail_pre_hud_backbuffer_snapshot.v2",
            "video_profile_id": PROFILE_ID,
            "image_path": image_relative,
            "sha256": image_sha,
            "width": RETAIL_SIZE[0],
            "height": RETAIL_SIZE[1],
            "pre_overlay": True,
            "frame_correlated": True,
            "frame_serial": str(state["frame_serial"]),
            "frame_qpc": str(state["frame_qpc"]),
            "settle": settle,
        },
        "comparison_presentation": {
            "contract": dict(COMPARISON_CONTRACT),
            "observed": {
                "equipped_weapon": COMPARISON_CONTRACT["equipped_weapon"],
                "arms_appearance": "bare",
                "gameplay_hud_visible": False,
                "terrain_enabled": COMPARISON_CONTRACT["terrain_enabled"],
                "viewmodel_enabled": COMPARISON_CONTRACT["viewmodel_enabled"],
                "ads_active": COMPARISON_CONTRACT["ads_active"],
                "big_map_active": COMPARISON_CONTRACT["big_map_active"],
            },
            "verification": {
                "method": "frame_correlated_pre_hud_snapshot",
                "bound_image_sha256": image_sha,
                "telemetry_available": True,
            },
        },
        "retail_runtime_witness": runtime_video_witness,
        "retail_presentation_stage": {
            "manifest_path": stage_relative,
            "manifest_sha256": _sha256(stage_manifest_path),
            "schema": retail_stage["schema"],
            "tool_version": retail_stage["tool_version"],
            "profile": retail_stage["profile"],
            "game_config": retail_stage["game_config"],
            "weapon_profile": retail_stage["weapon_profile"],
        },
        "camera_bms": realized_camera,
        "render_state": render_state,
        "build": {
            "opennova_source_commit": source_commit,
            "retail_executable_sha256": _sha256(retail_executable),
            "onhook_mcp_sha256": _sha256(onhook_mcp),
            "onhook_proxy_sha256": _sha256(onhook_proxy),
            "onhook_forwarder_sha256": _sha256(onhook_forwarder),
        },
        "retail_install": install,
        "raw_evidence": {
            "state_name": state_path.name,
            "state_sha256": _sha256(state_path),
            "onhook_log_name": log_path.name,
            "onhook_log_sha256": _sha256(log_path),
            "instance_status_name": instance_status_path.name,
            "instance_status_sha256": _sha256(instance_status_path),
            "fixture_result_name": fixture_result_path.name,
            "fixture_result_sha256": _sha256(fixture_result_path),
            "capture_result_name": capture_result_path.name,
            "capture_result_sha256": _sha256(capture_result_path),
            "retail_stage_manifest_name": stage_manifest_path.name,
            "retail_stage_manifest_sha256": _sha256(stage_manifest_path),
            "source_pid": pid,
            "source_instance_id": instance_id,
            "bridge_version_major": source["bridge_version_major"],
            "bridge_version_minor": source["bridge_version_minor"],
            "hook_version": source.get("hook_version", ""),
            "capture_bundle_supported": True,
        },
    }
    output_path.write_text(
        json.dumps(registration, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--fixture-id", required=True)
    parser.add_argument("--raw-state", required=True)
    parser.add_argument("--instance-status", required=True)
    parser.add_argument("--fixture-result", required=True)
    parser.add_argument("--capture-result", required=True)
    parser.add_argument("--raw-image", required=True)
    parser.add_argument("--onhook-log", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--expansion", required=True)
    parser.add_argument("--retail-executable", required=True)
    parser.add_argument("--onhook-mcp", required=True)
    parser.add_argument("--onhook-proxy", required=True)
    parser.add_argument("--onhook-forwarder", required=True)
    parser.add_argument(
        "--retail-stage-manifest",
        required=True,
        help=(
            "immutable pre-launch stage manifest proving the exhaustive highest "
            "video profile, unchanged HUD, and catalog-pinned weapon.sav profile"
        ),
    )
    parser.add_argument(
        "--confirm-retail-presentation-contract",
        action="store_true",
        help=(
            "attest by visual inspection that the hashed retail frame satisfies "
            "the catalog's HUD-hidden weapon, bare-arms, terrain, and "
            "viewmodel contract"
        ),
    )
    parser.add_argument("--opennova-source-commit", required=True)
    parser.add_argument(
        "--min-settle-seconds",
        type=float,
        default=DEFAULT_MIN_SETTLE_SECONDS,
        help=(
            "registration floor for the seconds retail settled between the "
            "fixture apply and the capture frame, proven by the hook's "
            f"capture-frame witness (default {DEFAULT_MIN_SETTLE_SECONDS})"
        ),
    )
    parser.add_argument("--output", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        register_retail_reference(_parse_args(sys.argv[1:] if argv is None else argv))
    except RegistrationError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
