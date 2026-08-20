"""Build one fail-closed, registered OpenNova/retail comparison sheet.

Example:

uv run python scripts/render/build_retail_side_by_side.py \
  --catalog docs/render/render-fixtures-retail-v4.json \
  --fixture-id FIXTURE --opennova-manifest OPENNOVA-MANIFEST.json \
  --retail-bundle REGISTERED.json --output-dir OUTPUT \
  --opennova-caption "HUD hidden, bare arms, M16 Burst, frozen" \
  --retail-caption "pre-retail-HUD snapshot, bare arms, frame-correlated" \
  --roi world_center=240,180,1320,420 \
  --roi viewmodel_arms=850,700,900,500

Both inputs must prove the catalog's matched HUD-hidden presentation contract:
M16 Burst and bare arms, no gameplay HUD, and active viewmodel, terrain, and
player-view effects. Pixel differences remain qualitative; explicit ROI/mask
rows are descriptive only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import subprocess
import sys
from typing import Any

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QFont,
    QFontDatabase,
    QGuiApplication,
    QImage,
    QPainter,
    QPen,
)

try:
    from scripts.render import register_retail_capture as retail_contract
except ModuleNotFoundError:  # Direct ``python scripts/render/...`` execution.
    import register_retail_capture as retail_contract  # type: ignore[no-redef]


TOOL_NAME = "build_retail_side_by_side"
TOOL_VERSION = "4.0.0"
OPENNOVA_SIZE = (2000, 1200)
RETAIL_SIZE = (1920, 1200)
HEADER_HEIGHT = 72
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
CAPTION_FONT_REGULAR = (
    REPOSITORY_ROOT / "third_party" / "gut" / "addons" / "gut" / "fonts"
    / "CourierPrime-Regular.ttf"
)
CAPTION_FONT_BOLD = (
    REPOSITORY_ROOT / "third_party" / "gut" / "addons" / "gut" / "fonts"
    / "CourierPrime-Bold.ttf"
)
MATCHED_PRESENTATION_CONTRACT = {
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
CANONICAL_RETAIL_PROFILE_FACTS = {
    "path": "expansion/revx02/weapon.sav",
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


class EvidenceError(ValueError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"could not read {label} JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} JSON must contain an object")
    return value


def _canonical_catalog_hash(catalog: dict[str, Any]) -> str:
    payload = dict(catalog)
    claimed = payload.pop("catalog_sha256", "")
    if not isinstance(claimed, str) or not SHA256_PATTERN.fullmatch(claimed):
        raise EvidenceError("catalog_sha256 must be a lowercase SHA-256 digest")
    canonical = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode()
    actual = hashlib.sha256(canonical).hexdigest()
    if claimed != actual:
        raise EvidenceError(
            f"catalog_sha256 mismatch: declared={claimed} canonical={actual}"
        )
    return claimed


def _fixture(catalog: dict[str, Any], fixture_id: str) -> dict[str, Any]:
    matches = [
        row for row in catalog.get("fixtures", [])
        if isinstance(row, dict) and row.get("id") == fixture_id
    ]
    if len(matches) != 1:
        raise EvidenceError(
            f"catalog must contain fixture {fixture_id!r} exactly once"
        )
    return matches[0]


def _comparison_contract(catalog: dict[str, Any]) -> dict[str, Any]:
    contract = catalog.get("comparison_contract")
    if not _same_typed_value(contract, MATCHED_PRESENTATION_CONTRACT):
        raise EvidenceError(
            "catalog comparison presentation contract must require matched "
            "HUD-hidden WPN_M16BURST, bare arms, viewmodel, terrain, and pose"
        )
    return dict(MATCHED_PRESENTATION_CONTRACT)


def _retail_profile_contract(catalog: dict[str, Any]) -> dict[str, Any]:
    contract = catalog.get("retail_profile_contract")
    if not isinstance(contract, dict) or set(contract) != {
        "path", "sha256", "profile_slot", "blue", "red",
    }:
        raise EvidenceError("catalog retail profile contract is malformed")
    sha256 = contract.get("sha256")
    facts = dict(contract)
    facts.pop("sha256")
    if not isinstance(sha256, str) or not SHA256_PATTERN.fullmatch(sha256) \
            or facts != CANONICAL_RETAIL_PROFILE_FACTS:
        raise EvidenceError(
            "catalog retail profile contract must pin the live-validated "
            "slot-0 SASR bare-arms weapon.sav"
        )
    return dict(contract)


def _retail_video_profile_contract(catalog: dict[str, Any]) -> dict[str, Any]:
    contract = catalog.get("retail_video_profile_contract")
    expected = {
        "id": retail_contract.PROFILE_ID,
        "video_option_catalog_id": retail_contract.VIDEO_OPTION_CATALOG_ID,
        "video_option_catalog_sha256": (
            retail_contract._video_option_catalog_sha256()
        ),
        "required_values": retail_contract.HIGHEST_VIDEO_PROFILE,
    }
    if not retail_contract._same_typed_value(contract, expected):
        raise EvidenceError(
            "catalog retail video profile must be the exhaustive highest-quality profile"
        )
    return dict(expected)


def _same_typed_value(actual: object, expected: object) -> bool:
    actual_is_number = isinstance(actual, (int, float)) \
        and not isinstance(actual, bool)
    expected_is_number = isinstance(expected, (int, float)) \
        and not isinstance(expected, bool)
    if actual_is_number and expected_is_number:
        actual_number = float(actual)
        expected_number = float(expected)
        return math.isfinite(actual_number) \
            and math.isfinite(expected_number) \
            and actual_number == expected_number
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


def _same_fixture_payload(actual: object, expected: object) -> bool:
    actual_is_number = isinstance(actual, (int, float)) \
        and not isinstance(actual, bool)
    expected_is_number = isinstance(expected, (int, float)) \
        and not isinstance(expected, bool)
    if actual_is_number and expected_is_number:
        actual_number = float(actual)
        expected_number = float(expected)
        return math.isfinite(actual_number) \
            and math.isfinite(expected_number) \
            and math.isclose(
                actual_number, expected_number,
                rel_tol=5.0e-15, abs_tol=1.0e-12,
            )
    if type(actual) is not type(expected):
        return False
    if isinstance(expected, dict):
        return set(actual) == set(expected) and all(
            _same_fixture_payload(actual[key], value)
            for key, value in expected.items()
        )
    if isinstance(expected, list):
        return len(actual) == len(expected) and all(
            _same_fixture_payload(left, right)
            for left, right in zip(actual, expected, strict=True)
        )
    return actual == expected


def _required_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or not SHA256_PATTERN.fullmatch(value):
        raise EvidenceError(f"{label} must be a lowercase SHA-256 digest")
    return value


def resolve_artifact(path_value: object, manifest_path: Path, label: str) -> Path:
    """Resolve one hash-bound artifact without permitting machine-specific paths."""
    if not isinstance(path_value, str) or not path_value:
        raise EvidenceError(f"{label} path is absent")
    relative = PurePosixPath(path_value)
    if "\\" in path_value \
            or relative.is_absolute() \
            or PureWindowsPath(path_value).is_absolute() \
            or ".." in relative.parts:
        raise EvidenceError(f"{label} must use a portable relative path")
    root = manifest_path.parent.resolve()
    path = root.joinpath(*relative.parts).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise EvidenceError(f"{label} must use a portable relative path") from exc
    if not path.is_file():
        raise EvidenceError(f"{label} file not found: {path}")
    return path


def _image_size(path: Path, label: str) -> tuple[int, int]:
    image = QImage(str(path))
    if image.isNull():
        raise EvidenceError(f"{label} is not a readable image: {path}")
    return image.width(), image.height()


def _same_camera(actual: object, expected: dict[str, Any]) -> bool:
    if not isinstance(actual, dict):
        return False
    try:
        positions_match = len(actual["position"]) == 3 and all(
            abs(float(left) - float(right)) <= 0.05
            for left, right in zip(
                actual["position"], expected["position"], strict=True
            )
        )
        angles_match = all(
            abs(float(actual[key]) - float(expected[key])) <= 0.1
            for key in ("yaw_deg", "pitch_deg")
        )
        fov_matches = abs(
            float(actual["vertical_fov_deg"])
            - float(expected["vertical_fov_deg"])
        ) <= 0.01
    except (KeyError, TypeError, ValueError):
        return False
    return positions_match and angles_match and fov_matches


def _same_position(actual: object, expected: object) -> bool:
    if not isinstance(actual, list) or not isinstance(expected, list) \
            or len(actual) != 3 or len(expected) != 3:
        return False
    try:
        return all(
            abs(float(left) - float(right)) <= 0.001
            for left, right in zip(actual, expected, strict=True)
        )
    except (TypeError, ValueError):
        return False


def _validate_opennova_presentation(
    capture: dict[str, Any],
    contract: dict[str, Any],
    fixture: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    if not _same_typed_value(capture.get("comparison_contract"), contract):
        raise EvidenceError(
            "OpenNova comparison presentation contract does not match catalog"
        )
    witness = capture.get("comparison_contract_witness")
    expected_keys = set(contract) | {
        "observed_at",
        "player_class",
        "player_position_bms",
        "requested_player_pose_bms",
        "hud_canvas_layer_visible",
        "viewmodel_canvas_layer_visible",
        "terrain_data_available",
        "terrain_node_visible",
    }
    if not isinstance(witness, dict) or set(witness) != expected_keys:
        raise EvidenceError(
            "OpenNova comparison presentation witness is absent or malformed"
        )
    if witness.get("observed_at") \
            != "after_pose_settle_before_fixture_freeze":
        raise EvidenceError("OpenNova presentation witness timing is mismatched")
    for field, expected in contract.items():
        if not _same_typed_value(witness.get(field), expected):
            raise EvidenceError(
                f"OpenNova {field.replace('_', ' ')} witness does not match catalog"
            )
    if witness.get("hud_canvas_layer_visible") \
            is not contract["hud_canvas_layer_active"]:
        raise EvidenceError("OpenNova HUD CanvasLayer witness does not match catalog")
    if witness.get("viewmodel_canvas_layer_visible") \
            is not contract["viewmodel_enabled"]:
        raise EvidenceError("OpenNova viewmodel witness does not match catalog")
    if witness.get("terrain_data_available") is not True \
            or witness.get("terrain_node_visible") \
            is not contract["terrain_enabled"]:
        raise EvidenceError("OpenNova terrain witness does not match catalog")
    player_class = witness.get("player_class")
    if isinstance(player_class, bool) or not isinstance(player_class, int):
        raise EvidenceError("OpenNova player-class witness is malformed")
    retail_player_bms = fixture.get("retail_player_bms")
    expected_position = retail_player_bms.get("applied") \
        if isinstance(retail_player_bms, dict) else None
    if not _same_position(witness.get("player_position_bms"), expected_position):
        raise EvidenceError("OpenNova player pose does not match retail fixture")
    requested = witness.get("requested_player_pose_bms")
    camera = fixture.get("camera_bms", {})
    if not isinstance(requested, dict) \
            or set(requested) != {"position", "yaw_deg", "pitch_deg"} \
            or not _same_position(requested.get("position"), expected_position):
        raise EvidenceError("OpenNova requested player pose is mismatched")
    try:
        angles_match = all(
            abs(float(requested[key]) - float(camera[key])) <= 0.001
            for key in ("yaw_deg", "pitch_deg")
        )
    except (KeyError, TypeError, ValueError):
        angles_match = False
    if not angles_match:
        raise EvidenceError("OpenNova requested player pose is mismatched")
    observed = {
        key: witness[key]
        for key in contract
        if key not in {"capture_mode", "player_pose_source"}
    }
    return witness, observed


def _validate_retail_presentation(
    retail: dict[str, Any],
    contract: dict[str, Any],
    image_sha256: str,
) -> dict[str, Any]:
    presentation = retail.get("comparison_presentation")
    if not isinstance(presentation, dict) \
            or set(presentation) != {"contract", "observed", "verification"}:
        raise EvidenceError(
            "retail comparison presentation witness is absent or malformed"
        )
    if not _same_typed_value(presentation.get("contract"), contract):
        raise EvidenceError(
            "retail comparison presentation contract does not match catalog"
        )
    expected_observed = {
        "equipped_weapon": contract["equipped_weapon"],
        "arms_appearance": "bare",
        "gameplay_hud_visible": contract["gameplay_hud_visible"],
        "terrain_enabled": contract["terrain_enabled"],
        "viewmodel_enabled": contract["viewmodel_enabled"],
        "ads_active": contract["ads_active"],
        "big_map_active": contract["big_map_active"],
    }
    observed = presentation.get("observed")
    if not isinstance(observed, dict):
        raise EvidenceError("retail comparison presentation observation is absent")
    for field, expected in expected_observed.items():
        if not _same_typed_value(observed.get(field), expected):
            raise EvidenceError(
                f"retail {field.replace('_', ' ')} witness does not match catalog"
            )
    if not _same_typed_value(observed, expected_observed):
        raise EvidenceError("retail comparison presentation observation is malformed")
    expected_verification = {
        "method": "frame_correlated_pre_hud_snapshot",
        "bound_image_sha256": image_sha256,
        "telemetry_available": True,
    }
    if not _same_typed_value(
        presentation.get("verification"), expected_verification
    ):
        raise EvidenceError(
            "retail visual presentation witness is not bound to the hashed frame"
        )
    return presentation


def _validate_retail_stage(
    retail: dict[str, Any],
    profile_contract: dict[str, Any],
    video_profile_contract: dict[str, Any],
    retail_bundle_path: Path,
) -> tuple[dict[str, Any], Path]:
    stage = retail.get("retail_presentation_stage")
    if not isinstance(stage, dict) or set(stage) != {
        "manifest_path", "manifest_sha256", "schema", "tool_version",
        "profile", "game_config", "weapon_profile",
    }:
        raise EvidenceError("registered retail presentation stage is absent")
    stage_path = resolve_artifact(
        stage.get("manifest_path"), retail_bundle_path, "retail stage manifest"
    )
    declared_sha = _required_sha256(
        stage.get("manifest_sha256"), "retail stage manifest hash"
    )
    if _sha256(stage_path) != declared_sha:
        raise EvidenceError("retail stage manifest hash does not match artifact")
    if stage.get("schema") != "opennova.retail-presentation-stage.v3" \
            or stage.get("tool_version") != "3.0.0":
        raise EvidenceError("registered retail presentation stage is unsupported")
    stage_document = _load_json(stage_path, "retail stage manifest")
    expected_document = {
        "schema": stage["schema"],
        "tool_version": stage["tool_version"],
        "profile": stage["profile"],
        "game_config": stage["game_config"],
        "weapon_profile": stage["weapon_profile"],
    }
    if not _same_typed_value(stage_document, expected_document):
        raise EvidenceError(
            "retail stage manifest artifact does not match registered evidence"
        )
    try:
        retail_contract._validate_stage_manifest(
            stage_path, profile_contract, video_profile_contract
        )
    except retail_contract.RegistrationError as exc:
        raise EvidenceError(f"registered retail stage is invalid: {exc}") from exc
    return stage, stage_path


def _validate_retail_runtime_witness(
    retail: dict[str, Any],
    retail_stage: dict[str, Any],
    retail_capture: dict[str, Any],
) -> dict[str, Any]:
    witness = retail.get("retail_runtime_witness")
    if not isinstance(witness, dict) or set(witness) != {
        "video_settings", "d3d_device", "presentation",
    }:
        raise EvidenceError("registered retail runtime witness is absent or malformed")
    state = {
        **witness,
        "frame_serial": retail_capture.get("frame_serial"),
        "frame_qpc": retail_capture.get("frame_qpc"),
    }
    try:
        retail_contract._validate_runtime_video_witness(state, retail_stage)
    except retail_contract.RegistrationError as exc:
        raise EvidenceError(f"registered retail runtime witness is invalid: {exc}") \
            from exc
    return witness


def _portable_metadata_path(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or "\\" in value:
        raise EvidenceError(f"{label} must use a portable relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or PureWindowsPath(value).is_absolute() \
            or ".." in path.parts:
        raise EvidenceError(f"{label} must use a portable relative path")
    return path.as_posix()


def _validate_retail_install(
    retail: dict[str, Any], opennova_mission: dict[str, Any]
) -> dict[str, Any]:
    install = retail.get("retail_install")
    if not isinstance(install, dict):
        raise EvidenceError("registered retail install provenance is absent")
    expansion = install.get("expansion")
    if not isinstance(expansion, str) or not expansion \
            or opennova_mission.get("expansion") != expansion:
        raise EvidenceError("retail expansion does not match OpenNova")
    expected_paths = [
        f"expansion/{expansion}/{expansion}L.pff",
        f"expansion/{expansion}/{expansion}.pff",
        "language.pff",
        "localres.pff",
        "resource.pff",
    ]
    archives = install.get("mounted_archives")
    if not isinstance(archives, list) or len(archives) != len(expected_paths):
        raise EvidenceError("registered retail archive provenance is incomplete")
    actual_paths: list[str] = []
    for index, row in enumerate(archives):
        if not isinstance(row, dict):
            raise EvidenceError("registered retail archive provenance is malformed")
        actual_paths.append(_portable_metadata_path(
            row.get("path"), f"retail archive {index}"
        ))
        _required_sha256(row.get("sha256"), f"retail archive {index} hash")
    if [path.casefold() for path in actual_paths] \
            != [path.casefold() for path in expected_paths]:
        raise EvidenceError("registered retail archive order is mismatched")
    marker = install.get("version_marker")
    if not isinstance(marker, dict) \
            or _portable_metadata_path(
                marker.get("path"), "retail version marker"
            ).casefold() != f"expansion/{expansion}/{expansion}.bin".casefold():
        raise EvidenceError("registered retail version marker is mismatched")
    _required_sha256(marker.get("sha256"), "retail version marker hash")
    return install


def _validate_raw_evidence(retail: dict[str, Any]) -> dict[str, Any]:
    raw = retail.get("raw_evidence")
    if not isinstance(raw, dict):
        raise EvidenceError("registered retail raw evidence is absent")
    for prefix in (
        "state", "onhook_log", "instance_status", "fixture_result",
        "capture_result",
    ):
        name = _portable_metadata_path(
            raw.get(f"{prefix}_name"), f"retail raw evidence {prefix}"
        )
        if len(PurePosixPath(name).parts) != 1:
            raise EvidenceError("retail raw evidence names must be filenames")
        _required_sha256(
            raw.get(f"{prefix}_sha256"), f"retail raw evidence {prefix} hash"
        )
    try:
        source_pid = int(raw["source_pid"])
    except (KeyError, TypeError, ValueError) as exc:
        raise EvidenceError("retail raw evidence source PID is absent") from exc
    instance_id = raw.get("source_instance_id")
    if source_pid <= 0 or not isinstance(instance_id, str) \
            or not instance_id.startswith(f"{source_pid}-"):
        raise EvidenceError("retail raw evidence source identity is mismatched")
    if type(raw.get("bridge_version_major")) is not int \
            or raw["bridge_version_major"] != 1 \
            or type(raw.get("bridge_version_minor")) is not int \
            or raw["bridge_version_minor"] != 4 \
            or type(raw.get("hook_version")) is not str \
            or raw["hook_version"] != "0.5.0" \
            or raw.get("capture_bundle_supported") is not True:
        raise EvidenceError(
            "retail raw evidence producer version must be bridge 1.4 and "
            "hook 0.5.0 with capture-bundle support"
        )
    return raw


def validate_registered_inputs(
    catalog_path: Path,
    fixture_id: str,
    opennova_manifest_path: Path,
    retail_bundle_path: Path,
) -> dict[str, Any]:
    """Validate and resolve one registered OpenNova/retail input pair."""
    catalog = _load_json(catalog_path, "fixture catalog")
    if catalog.get("schema") != "opennova.render-fixtures.v2":
        raise EvidenceError("unsupported fixture catalog schema")
    catalog_sha = _canonical_catalog_hash(catalog)
    comparison_contract = _comparison_contract(catalog)
    profile_contract = _retail_profile_contract(catalog)
    video_profile_contract = _retail_video_profile_contract(catalog)
    fixture = _fixture(catalog, fixture_id)
    mission_file = fixture.get("mission")
    mission_sha = catalog.get("missions", {}).get(mission_file)
    _required_sha256(mission_sha, "catalog mission hash")
    capture_mode = fixture.get("capture_mode")
    if capture_mode != comparison_contract["capture_mode"]:
        raise EvidenceError(
            "fixture capture mode does not match comparison presentation contract"
        )
    if catalog.get("capture", {}).get("resolution") != list(OPENNOVA_SIZE):
        raise EvidenceError("catalog OpenNova raw resolution must be 2000x1200")
    if catalog.get("capture", {}).get("retail_resolution") != list(RETAIL_SIZE):
        raise EvidenceError("catalog retail raw resolution must be 1920x1200")

    opennova = _load_json(opennova_manifest_path, "OpenNova manifest")
    if opennova.get("schema") != "opennova.render-fixture-captures.v1":
        raise EvidenceError("unsupported OpenNova capture manifest schema")
    if opennova.get("catalog_sha256") != catalog_sha:
        raise EvidenceError("OpenNova catalog hash does not match selected catalog")
    if not _same_fixture_payload(opennova.get("fixture"), fixture):
        raise EvidenceError("OpenNova fixture payload does not match selected fixture")
    opennova_mission = opennova.get("mission", {})
    if opennova_mission.get("file") != mission_file \
            or opennova_mission.get("sha256") != mission_sha:
        raise EvidenceError("OpenNova mission identity does not match fixture catalog")
    opennova_capture = opennova.get("capture", {})
    if opennova_capture.get("mode") != capture_mode:
        raise EvidenceError("OpenNova capture mode does not match fixture")
    if opennova_capture.get("world_only") is not False \
            or opennova_capture.get("viewmodel_hidden") is not False:
        raise EvidenceError(
            "OpenNova HUD-hidden visibility must include viewmodel and world"
        )
    if opennova_capture.get("resolution") != list(OPENNOVA_SIZE):
        raise EvidenceError("OpenNova manifest must declare raw 2000x1200 capture")
    if not _same_camera(opennova_capture.get("camera_bms"), fixture["camera_bms"]):
        raise EvidenceError("OpenNova camera pose does not match fixture")
    opennova_witness, opennova_observed = _validate_opennova_presentation(
        opennova_capture, comparison_contract, fixture
    )
    provenance = opennova.get("provenance", {})
    source_commit = provenance.get("source_commit")
    if not isinstance(source_commit, str) or not re.fullmatch(
        r"[0-9a-f]{40}", source_commit
    ):
        raise EvidenceError("OpenNova source_commit must be a full Git SHA")
    _required_sha256(
        provenance.get("godot_executable_sha256"),
        "OpenNova Godot executable hash",
    )
    _required_sha256(
        provenance.get("gdextension_sha256"),
        "OpenNova GDExtension hash",
    )
    beauty = [
        row for row in opennova.get("artifacts", [])
        if isinstance(row, dict) and row.get("variant") == "beauty"
    ]
    if len(beauty) != 1:
        raise EvidenceError("OpenNova manifest must contain exactly one beauty artifact")
    opennova_artifact = beauty[0]
    if [opennova_artifact.get("width"), opennova_artifact.get("height")] \
            != list(OPENNOVA_SIZE):
        raise EvidenceError("OpenNova beauty artifact is not declared raw 2000x1200")
    opennova_image = resolve_artifact(
        opennova_artifact.get("png_path"), opennova_manifest_path, "OpenNova raw"
    )
    if _image_size(opennova_image, "OpenNova raw") != OPENNOVA_SIZE:
        raise EvidenceError("OpenNova raw image must be exactly 2000x1200")
    if _sha256(opennova_image) != opennova_artifact.get("png_sha256"):
        raise EvidenceError("OpenNova raw image hash does not match manifest")
    opennova_state = resolve_artifact(
        opennova_artifact.get("state_path"),
        opennova_manifest_path,
        "OpenNova capture state",
    )
    declared_state_sha = _required_sha256(
        opennova_artifact.get("state_sha256"),
        "OpenNova capture state hash",
    )
    if _sha256(opennova_state) != declared_state_sha:
        raise EvidenceError("OpenNova capture state hash does not match manifest")
    opennova_state_document = _load_json(
        opennova_state, "OpenNova capture state"
    )
    if opennova_state_document.get("comparison_contract_witness") \
            != opennova_witness:
        raise EvidenceError(
            "OpenNova capture-state presentation witness does not match manifest"
        )
    state_capture = opennova_state_document.get("capture")
    if not isinstance(state_capture, dict):
        raise EvidenceError("OpenNova capture state is missing capture metadata")
    if state_capture.get("label") != opennova_artifact.get("label"):
        raise EvidenceError(
            "OpenNova capture-state label does not match manifest artifact"
        )
    if state_capture.get("png_path") != opennova_image.name \
            or state_capture.get("png_sha256") \
            != opennova_artifact.get("png_sha256"):
        raise EvidenceError(
            "OpenNova capture state is not bound to the selected raw image"
        )

    retail = _load_json(retail_bundle_path, "registered retail bundle")
    if retail.get("schema") != "opennova.registered-retail-capture.v5" \
            or not retail_contract._same_typed_value(
                retail.get("tool"),
                {"name": "register_retail_capture", "version": "4.0.0"},
            ):
        raise EvidenceError("retail input is not a registered capture bundle")
    if retail.get("catalog_sha256") != catalog_sha:
        raise EvidenceError("retail catalog hash does not match selected catalog")
    if retail.get("fixture_id") != fixture_id:
        raise EvidenceError("retail fixture does not match selected fixture")
    retail_mission = retail.get("mission", {})
    if retail_mission.get("file") != mission_file \
            or retail_mission.get("sha256") != mission_sha \
            or retail_mission.get("verified") is not True \
            or retail_mission.get("verification") != [
                "engine_vfs_logical_lookup",
                "same_pid_onhook_launch_log",
            ]:
        raise EvidenceError("retail mission is not verified against fixture catalog")
    retail_capture = retail.get("capture", {})
    if retail_capture.get("mode") != capture_mode:
        raise EvidenceError("retail capture mode does not match fixture")
    if retail_capture.get("retail_presentation") \
            != "game_composite_pre_retail_hud" \
            or retail_capture.get("retail_gameplay_hud_visible") is not False \
            or retail_capture.get("onhook_overlay_present") is not False \
            or retail_capture.get("presentation_policy") \
            != "retail_pre_hud_backbuffer_snapshot.v2" \
            or retail_capture.get("video_profile_id") \
            != retail_contract.PROFILE_ID:
        raise EvidenceError("retail presentation contract is absent or mismatched")
    if retail_capture.get("pre_overlay") is not True \
            or retail_capture.get("frame_correlated") is not True:
        raise EvidenceError("retail capture must be pre-overlay and frame-correlated")
    if retail_capture.get("frame_serial") in {None, "", "0", 0} \
            or retail_capture.get("frame_qpc") in {None, "", "0", 0}:
        raise EvidenceError("retail capture has no correlated frame identity")
    if [retail_capture.get("width"), retail_capture.get("height")] \
            != list(RETAIL_SIZE):
        raise EvidenceError("retail bundle is not declared raw 1920x1200")
    if not _same_camera(retail.get("camera_bms"), fixture["camera_bms"]):
        raise EvidenceError("retail camera pose does not match fixture")
    retail_image = resolve_artifact(
        retail_capture.get("image_path"), retail_bundle_path, "retail raw"
    )
    if _image_size(retail_image, "retail raw") != RETAIL_SIZE:
        raise EvidenceError("retail raw image must be exactly 1920x1200")
    if _sha256(retail_image) != retail_capture.get("sha256"):
        raise EvidenceError("retail raw image hash does not match bundle")
    retail_presentation = _validate_retail_presentation(
        retail, comparison_contract, retail_capture["sha256"]
    )
    retail_stage, retail_stage_path = _validate_retail_stage(
        retail, profile_contract, video_profile_contract, retail_bundle_path
    )
    retail_runtime_witness = _validate_retail_runtime_witness(
        retail, retail_stage, retail_capture
    )
    build = retail.get("build", {})
    if build.get("opennova_source_commit") != source_commit:
        raise EvidenceError("OpenNova build does not match retail pairing bundle")
    for key, label in (
        ("retail_executable_sha256", "retail executable hash"),
        ("onhook_mcp_sha256", "onHook MCP hash"),
        ("onhook_proxy_sha256", "onHook proxy hash"),
        ("onhook_forwarder_sha256", "onHook proxy forwarder hash"),
    ):
        _required_sha256(build.get(key), label)
    retail_install = _validate_retail_install(retail, opennova_mission)
    raw_evidence = _validate_raw_evidence(retail)
    if raw_evidence.get("retail_stage_manifest_name") \
            != retail_stage_path.name \
            or raw_evidence.get("retail_stage_manifest_sha256") \
            != retail_stage["manifest_sha256"]:
        raise EvidenceError(
            "retail raw evidence does not bind the presentation stage manifest"
        )

    return {
        "catalog_sha": catalog_sha,
        "fixture": fixture,
        "mission_file": mission_file,
        "mission_sha": mission_sha,
        "capture_mode": capture_mode,
        "comparison_contract": comparison_contract,
        "opennova_witness": opennova_witness,
        "opennova_observed": opennova_observed,
        "retail_presentation": retail_presentation,
        "retail_stage": retail_stage,
        "retail_runtime_witness": retail_runtime_witness,
        "retail_stage_path": retail_stage_path,
        "source_commit": source_commit,
        "opennova": opennova,
        "opennova_artifact": opennova_artifact,
        "opennova_image": opennova_image,
        "opennova_state": opennova_state,
        "retail": retail,
        "retail_image": retail_image,
        "build": build,
        "retail_install": retail_install,
        "raw_evidence": raw_evidence,
    }


def _normalize(source: Path, destination: Path) -> None:
    script = Path(__file__).with_name("normalize_opennova_for_retail.ps1")
    result = subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            "-InputImage",
            str(source),
            "-OutputImage",
            str(destination),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise EvidenceError(f"horizontal normalization failed: {detail}")


def _load_caption_fonts() -> tuple[str, dict[str, str]]:
    families: list[str] = []
    for path in (CAPTION_FONT_REGULAR, CAPTION_FONT_BOLD):
        font_id = QFontDatabase.addApplicationFont(str(path))
        if font_id < 0:
            raise EvidenceError(f"could not load bundled caption font: {path}")
        loaded = QFontDatabase.applicationFontFamilies(font_id)
        if not loaded:
            raise EvidenceError(f"bundled caption font has no family: {path}")
        families.append(loaded[0])
    if families[0] != families[1]:
        raise EvidenceError("bundled caption font faces have mismatched families")
    return families[0], {
        "family": families[0],
        "regular_sha256": _sha256(CAPTION_FONT_REGULAR),
        "bold_sha256": _sha256(CAPTION_FONT_BOLD),
    }


def _draw_sheet(
    opennova_path: Path,
    retail_path: Path,
    destination: Path,
    opennova_caption: str,
    retail_caption: str,
) -> dict[str, str]:
    if destination.exists():
        raise EvidenceError(f"refusing to overwrite existing output: {destination}")
    app = QGuiApplication.instance() or QGuiApplication([TOOL_NAME])
    _ = app
    font_family, font_provenance = _load_caption_fonts()
    opennova = QImage(str(opennova_path))
    retail = QImage(str(retail_path))
    sheet = QImage(
        RETAIL_SIZE[0] * 2,
        RETAIL_SIZE[1] + HEADER_HEIGHT,
        QImage.Format.Format_RGB32,
    )
    sheet.fill(QColor(12, 12, 12))
    painter = QPainter(sheet)
    try:
        painter.drawImage(0, HEADER_HEIGHT, opennova)
        painter.drawImage(RETAIL_SIZE[0], HEADER_HEIGHT, retail)
        painter.setPen(QPen(QColor(238, 238, 238)))
        painter.setFont(QFont(font_family, 14, QFont.Weight.Bold))
        painter.drawText(QPointF(18, 27), "OPENNOVA")
        painter.drawText(QPointF(RETAIL_SIZE[0] + 18, 27), "RETAIL")
        painter.setPen(QPen(QColor(170, 170, 170)))
        painter.setFont(QFont(font_family, 9))
        painter.drawText(
            QRectF(18, 35, RETAIL_SIZE[0] - 36, 32),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            opennova_caption,
        )
        painter.drawText(
            QRectF(RETAIL_SIZE[0] + 18, 35, RETAIL_SIZE[0] - 36, 32),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            retail_caption,
        )
        painter.setPen(QPen(QColor(90, 90, 90)))
        painter.drawLine(RETAIL_SIZE[0] - 1, 0, RETAIL_SIZE[0] - 1,
                         sheet.height())
    finally:
        painter.end()
    if not sheet.save(str(destination), "PNG"):
        raise EvidenceError(f"could not save side-by-side sheet: {destination}")
    return font_provenance


def _draw_registered_views(
    opennova_path: Path,
    retail_path: Path,
    overlay_path: Path,
    difference_path: Path,
) -> QImage:
    for path in (overlay_path, difference_path):
        if path.exists():
            raise EvidenceError(f"refusing to overwrite existing output: {path}")
    opennova = QImage(str(opennova_path)).convertToFormat(
        QImage.Format.Format_ARGB32_Premultiplied
    )
    retail = QImage(str(retail_path)).convertToFormat(
        QImage.Format.Format_ARGB32_Premultiplied
    )

    overlay = opennova.copy()
    painter = QPainter(overlay)
    try:
        painter.setOpacity(0.5)
        painter.drawImage(0, 0, retail)
    finally:
        painter.end()
    if not overlay.save(str(overlay_path), "PNG"):
        raise EvidenceError(f"could not save 50/50 overlay: {overlay_path}")

    difference = retail.copy()
    painter = QPainter(difference)
    try:
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Difference)
        painter.drawImage(0, 0, opennova)
    finally:
        painter.end()
    if not difference.save(str(difference_path), "PNG"):
        raise EvidenceError(f"could not save absolute difference: {difference_path}")
    return difference


def _difference_metrics(
    difference: QImage,
    region: tuple[int, int, int, int] = (0, 0, *RETAIL_SIZE),
) -> dict[str, object]:
    x, y, width, height = region
    if x < 0 or y < 0 or width <= 0 or height <= 0 \
            or x + width > difference.width() \
            or y + height > difference.height():
        raise EvidenceError(f"metric region is outside the normalized frame: {region}")
    rgb = difference.convertToFormat(QImage.Format.Format_RGB888)
    raw = bytes(rgb.constBits())
    row_bytes = rgb.bytesPerLine()
    total = 0
    total_squares = 0
    maximum = 0
    for row_index in range(y, y + height):
        start = row_index * row_bytes + x * 3
        values = raw[start:start + width * 3]
        if values:
            maximum = max(maximum, max(values))
            total += sum(values)
            total_squares += sum(value * value for value in values)
    channel_count = width * height * 3
    return {
        "region": [x, y, width, height],
        "pixel_count": width * height,
        "channel_count": channel_count,
        "max_channel_delta": maximum,
        "mean_abs_channel_delta": round(total / channel_count, 6),
        "root_mean_square_channel_delta": round(
            math.sqrt(total_squares / channel_count), 6
        ),
    }


def _parse_roi(value: str) -> tuple[str, tuple[int, int, int, int]]:
    try:
        name, coordinates = value.split("=", 1)
        values = tuple(int(part) for part in coordinates.split(","))
    except (ValueError, TypeError) as exc:
        raise EvidenceError(
            "--roi must use NAME=X,Y,WIDTH,HEIGHT with integer coordinates"
        ) from exc
    if not re.fullmatch(r"[A-Za-z0-9._-]+", name) or len(values) != 4:
        raise EvidenceError(
            "--roi must use NAME=X,Y,WIDTH,HEIGHT with a portable name"
        )
    x, y, width, height = values
    if x < 0 or y < 0 or width <= 0 or height <= 0 \
            or x + width > RETAIL_SIZE[0] or y + height > RETAIL_SIZE[1]:
        raise EvidenceError(f"ROI is outside the normalized frame: {value}")
    return name, (x, y, width, height)


def _mask_metrics(difference: QImage, mask_path: Path) -> dict[str, object]:
    mask = QImage(str(mask_path))
    if mask.isNull() or (mask.width(), mask.height()) != RETAIL_SIZE:
        raise EvidenceError(f"metric mask must be a 1920x1200 image: {mask_path}")
    mask = mask.convertToFormat(QImage.Format.Format_Grayscale8)
    rgb = difference.convertToFormat(QImage.Format.Format_RGB888)
    mask_raw = bytes(mask.constBits())
    rgb_raw = bytes(rgb.constBits())
    mask_row_bytes = mask.bytesPerLine()
    rgb_row_bytes = rgb.bytesPerLine()
    pixel_count = 0
    total = 0
    total_squares = 0
    maximum = 0
    for y in range(RETAIL_SIZE[1]):
        mask_row = mask_raw[y * mask_row_bytes:y * mask_row_bytes + RETAIL_SIZE[0]]
        rgb_row = rgb_raw[y * rgb_row_bytes:y * rgb_row_bytes + RETAIL_SIZE[0] * 3]
        for x, selected in enumerate(mask_row):
            if selected == 0:
                continue
            values = rgb_row[x * 3:x * 3 + 3]
            pixel_count += 1
            maximum = max(maximum, max(values))
            total += sum(values)
            total_squares += sum(value * value for value in values)
    if pixel_count == 0:
        raise EvidenceError(f"metric mask selects no pixels: {mask_path}")
    channel_count = pixel_count * 3
    return {
        "name": mask_path.stem,
        "type": "mask",
        "mask": {
            "source_name": mask_path.name,
            "sha256": _sha256(mask_path),
            "dimensions": list(RETAIL_SIZE),
        },
        "pixel_count": pixel_count,
        "channel_count": channel_count,
        "max_channel_delta": maximum,
        "mean_abs_channel_delta": round(total / channel_count, 6),
        "root_mean_square_channel_delta": round(
            math.sqrt(total_squares / channel_count), 6
        ),
    }


def build(args: argparse.Namespace) -> None:
    catalog_path = Path(args.catalog).resolve()
    opennova_manifest_path = Path(args.opennova_manifest).resolve()
    retail_bundle_path = Path(args.retail_bundle).resolve()
    inputs = validate_registered_inputs(
        catalog_path,
        args.fixture_id,
        opennova_manifest_path,
        retail_bundle_path,
    )
    roi_specs = [_parse_roi(value) for value in args.roi]
    mask_paths = [Path(value).resolve() for value in args.mask]
    for mask_path in mask_paths:
        if not mask_path.is_file():
            raise EvidenceError(f"metric mask not found: {mask_path}")
        if _image_size(mask_path, "metric mask") != RETAIL_SIZE:
            raise EvidenceError(
                f"metric mask must be a 1920x1200 image: {mask_path}"
            )

    output_dir = Path(args.output_dir).resolve()
    normalized_path = output_dir / (
        f"{args.fixture_id}-opennova-normalized.png"
    )
    sheet_path = output_dir / f"{args.fixture_id}-side-by-side.png"
    overlay_path = output_dir / f"{args.fixture_id}-overlay-50.png"
    difference_path = output_dir / f"{args.fixture_id}-absolute-diff.png"
    manifest_path = output_dir / f"{args.fixture_id}-comparison.json"
    for path in (
        normalized_path, sheet_path, overlay_path, difference_path, manifest_path
    ):
        if path.exists():
            raise EvidenceError(f"refusing to overwrite existing output: {path}")
    output_dir.mkdir(parents=True, exist_ok=True)

    _normalize(inputs["opennova_image"], normalized_path)
    if _image_size(normalized_path, "normalized OpenNova") != RETAIL_SIZE:
        raise EvidenceError("normalizer did not produce exactly 1920x1200")
    caption_font = _draw_sheet(
        normalized_path,
        inputs["retail_image"],
        sheet_path,
        args.opennova_caption,
        args.retail_caption,
    )
    difference = _draw_registered_views(
        normalized_path, inputs["retail_image"], overlay_path, difference_path
    )
    region_metrics: list[dict[str, object]] = []
    for name, region in roi_specs:
        row = _difference_metrics(difference, region)
        region_metrics.append({"name": name, "type": "roi", **row})
    region_metrics.extend(
        _mask_metrics(difference, mask_path) for mask_path in mask_paths
    )

    opennova = inputs["opennova"]
    retail = inputs["retail"]
    manifest = {
        "schema": "opennova.retail-comparison.v6",
        "tool": {
            "name": TOOL_NAME,
            "version": TOOL_VERSION,
            "create_new": True,
        },
        "fixture_id": args.fixture_id,
        "catalog_sha256": inputs["catalog_sha"],
        "mission": {
            "file": inputs["mission_file"],
            "sha256": inputs["mission_sha"],
        },
        "capture_mode": inputs["capture_mode"],
        "presentations": {
            "opennova": inputs["capture_mode"],
            "retail": "game_composite_pre_retail_hud",
            "retail_gameplay_hud_visible": False,
            "onhook_overlay_present": False,
            "retail_policy": "retail_pre_hud_backbuffer_snapshot.v2",
            "retail_video_profile": retail_contract.PROFILE_ID,
        },
        "comparison_presentation": {
            "contract": inputs["comparison_contract"],
            "match_status": "validated",
            "opennova": {
                "observed": inputs["opennova_observed"],
                "verification": {
                    "method": "runtime_capture_witness",
                    "observed_at": inputs["opennova_witness"]["observed_at"],
                    "telemetry_available": True,
                },
                "source_witness": inputs["opennova_witness"],
            },
            "retail": inputs["retail_presentation"],
            "retail_stage": inputs["retail_stage"],
            "retail_runtime_witness": inputs["retail_runtime_witness"],
        },
        "camera_bms": inputs["fixture"]["camera_bms"],
        "captions": {
            "opennova": args.opennova_caption,
            "retail": args.retail_caption,
        },
        "caption_font": caption_font,
        "metrics_policy": {
            "full_frame": "qualitative_only_matched_hud_hidden_cross_engine",
            "roi_and_mask": "descriptive_explicit_presentation_regions",
        },
        "metrics": {
            "full_frame": _difference_metrics(difference),
            "regions": region_metrics,
        },
        "inputs": {
            "opennova": {
                "source_name": inputs["opennova_image"].name,
                "sha256": _sha256(inputs["opennova_image"]),
                "dimensions": list(OPENNOVA_SIZE),
                "state_name": inputs["opennova_state"].name,
                "state_sha256": _sha256(inputs["opennova_state"]),
                "manifest_sha256": _sha256(opennova_manifest_path),
                "source_commit": inputs["source_commit"],
                "godot_executable_sha256": opennova["provenance"][
                    "godot_executable_sha256"
                ],
                "gdextension_sha256": opennova["provenance"][
                    "gdextension_sha256"
                ],
            },
            "retail": {
                "source_name": inputs["retail_image"].name,
                "sha256": _sha256(inputs["retail_image"]),
                "dimensions": list(RETAIL_SIZE),
                "bundle_sha256": _sha256(retail_bundle_path),
                "frame_serial": retail["capture"]["frame_serial"],
                "frame_qpc": retail["capture"]["frame_qpc"],
                "install": inputs["retail_install"],
                "raw_evidence": inputs["raw_evidence"],
                "presentation_stage": inputs["retail_stage"],
                **inputs["build"],
            },
        },
        "transform": {
            "kind": "horizontal_only",
            "implementation": "System.Drawing.Graphics.DrawImage",
            "interpolation": "HighQualityBicubic",
            "source_rectangle": [0, 0, 2000, 1200],
            "destination_rectangle": [0, 0, 1920, 1200],
            "crop": "none",
            "vertical_scale": 1.0,
        },
        "outputs": {
            "opennova_normalized": {
                "path": normalized_path.name,
                "sha256": _sha256(normalized_path),
                "dimensions": list(RETAIL_SIZE),
            },
            "side_by_side": {
                "path": sheet_path.name,
                "sha256": _sha256(sheet_path),
                "dimensions": [RETAIL_SIZE[0] * 2,
                               RETAIL_SIZE[1] + HEADER_HEIGHT],
            },
            "overlay_50": {
                "path": overlay_path.name,
                "sha256": _sha256(overlay_path),
                "dimensions": list(RETAIL_SIZE),
            },
            "absolute_diff": {
                "path": difference_path.name,
                "sha256": _sha256(difference_path),
                "dimensions": list(RETAIL_SIZE),
            },
        },
    }
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--fixture-id", required=True)
    parser.add_argument("--opennova-manifest", required=True)
    parser.add_argument("--retail-bundle", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--opennova-caption", default="")
    parser.add_argument("--retail-caption", default="")
    parser.add_argument(
        "--roi",
        action="append",
        default=[],
        metavar="NAME=X,Y,WIDTH,HEIGHT",
        help="record metrics for one registered normalized-frame rectangle",
    )
    parser.add_argument(
        "--mask",
        action="append",
        default=[],
        metavar="PATH",
        help="record metrics for nonzero pixels in a 1920x1200 mask",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        build(_parse_args(sys.argv[1:] if argv is None else argv))
    except EvidenceError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
