import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Sequence

import pytest

try:
    from PySide6.QtGui import QColor, QImage
except ImportError as exc:  # headless box without libGL/Qt
    pytest.skip(f"PySide6 QtGui is unavailable: {exc}", allow_module_level=True)

from scripts.render import build_retail_side_by_side as evidence_builder


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "render" / "build_retail_side_by_side.py"
CATALOG_PATH = ROOT / "docs" / "render" / "render-fixtures-retail-v1.json"
CAPTION_FONT_REGULAR = (
    ROOT / "third_party" / "gut" / "addons" / "gut" / "fonts"
    / "CourierPrime-Regular.ttf"
)
CAPTION_FONT_BOLD = (
    ROOT / "third_party" / "gut" / "addons" / "gut" / "fonts"
    / "CourierPrime-Bold.ttf"
)
PUBLISHED_EVIDENCE_ROOT = (
    ROOT / "screenshots" / "parity" / "render-lighting-2026-08"
    / "registered-2026-08-20"
)
LEGACY_CATALOG_SHA256 = (
    "6948682efc5e58d0ea887728e1318dd3c618b286a322b1bce430e9fbfff6ebf2"
)
CURRENT_CATALOG_PATH = (
    ROOT / "docs" / "render" / "render-fixtures-retail-v3.json"
)
CURRENT_EVIDENCE_ROOT = PUBLISHED_EVIDENCE_ROOT
CURRENT_CATALOG_SHA256 = (
    "8b21c2a0feed2e55ac11fd555f9ad96b9c533d49069c6ce0780b2984d62982ef"
)


def _lfs_pointer_stub(path: Path) -> bool:
    """True when the checkout holds a git-lfs pointer instead of content."""
    try:
        with path.open("rb") as fh:
            return fh.read(42).startswith(b"version https://git-lfs.github.com/spec/")
    except OSError:
        return False


# This suite validates the repo's OWN published evidence store, so its gate is
# LFS materialization rather than an env var: a clone that excluded
# screenshots/** from LFS fetch (a reasonable local setting) holds 3-line
# pointer stubs where the PNGs should be, and every hash/decode assertion
# below would hard-fail on the stub text. Skip-as-pass instead, per
# docs/asset-gated-tests.md.
_PROBE_PNG = next(iter(sorted(PUBLISHED_EVIDENCE_ROOT.rglob("*.png"))), None)
if _PROBE_PNG is not None and _lfs_pointer_stub(_PROBE_PNG):
    pytest.skip(
        "render-evidence store holds LFS pointer stubs (run `git lfs pull "
        "--include=screenshots/**`)",
        allow_module_level=True,
    )
CURRENT_SOURCE_COMMIT = "3ada00e96b055075db4442ac7724ee08fee19325"
# The archived v1 catalog predates the tire-marks fixture; its id set is
# pinned separately from the current publication's.
LEGACY_FIXTURE_IDS = frozenset({
    "00tra-armory-glass-retail",
    "00tra-courtyard-retail",
    "00tra-fire-barrel-close-retail",
    "00tra-fire-barrel-east-retail",
    "00tra-fire-barrel-full-composite-retail",
    "cp01-water-oblique-retail",
    "cp01-water-shallow-retail",
    "cp01-water-steep-retail",
    "cp01-water-wide-retail",
    "cp01-waterline-above-retail",
    "cp01-waterline-below-retail",
    "cp04-checkpoint-fires-retail",
    "cp12-truck-material-retail",
    "cp12-yard-road-retail",
    "cp12-yard-tanks-retail",
})
PUBLISHED_FIXTURE_IDS = LEGACY_FIXTURE_IDS | {
    "00tra-tire-marks-retail",
}
PUBLISHED_VARIANTS = frozenset({
    "beauty",
    "directional_shadow_atlas",
    "lighting_only",
    "shadows_off",
    "unshaded",
})
PUBLISHED_OUTPUT_DIMENSIONS = {
    "absolute_diff": [1920, 1200],
    "opennova_normalized": [1920, 1200],
    "overlay_50": [1920, 1200],
    "side_by_side": [3840, 1272],
}
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
HIGHEST_VIDEO_PROFILE = evidence_builder.retail_contract.HIGHEST_VIDEO_PROFILE
RUNTIME_VIDEO_PROFILE = evidence_builder.retail_contract.RUNTIME_VIDEO_PROFILE
LIVE_FLOAT32_GAMMA = 0.800000011920929
VIDEO_PROFILE_CONTRACT = {
    "id": evidence_builder.retail_contract.PROFILE_ID,
    "video_option_catalog_id": (
        evidence_builder.retail_contract.VIDEO_OPTION_CATALOG_ID
    ),
    "video_option_catalog_sha256": (
        evidence_builder.retail_contract._video_option_catalog_sha256()
    ),
    "required_values": HIGHEST_VIDEO_PROFILE,
}
PRESERVED_CONFIG_VALUES = {
    "hw3d_deviceno": 0,
    "hw3d_name": "AMD Radeon(TM) Graphics",
    "hw3d_guid": "D7B71EE2-55C1-11CF-63740273A5C2ED35",
    "enable_keyboardtips": 1,
    "enable_gameplaytips": 1,
    "hud_detail": 0,
}


def test_json_value_comparison_accepts_only_exact_numeric_equivalents() -> None:
    assert evidence_builder._same_typed_value(30.0, 30)
    assert evidence_builder._same_typed_value([1.0, 0.0, 0.0], [1, 0, 0])
    assert not evidence_builder._same_typed_value(30.5, 30)
    assert not evidence_builder._same_typed_value(True, 1)
    assert not evidence_builder._same_typed_value(float("inf"), 1)


def test_fixture_payload_comparison_allows_only_json_roundtrip_noise() -> None:
    expected = {"camera_bms": {"position": [-738.8314622233299]}}
    roundtripped = {"camera_bms": {"position": [-738.83146222333]}}
    cp12_expected = {"camera_bms": {"position": [1539.608020813166]}}
    cp12_roundtripped = {"camera_bms": {"position": [1539.60802081317]}}
    drifted = {"camera_bms": {"position": [-738.831461]}}
    assert evidence_builder._same_fixture_payload(roundtripped, expected)
    assert evidence_builder._same_fixture_payload(
        cp12_roundtripped, cp12_expected
    )
    assert not evidence_builder._same_fixture_payload(drifted, expected)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _solid(path: Path, color: tuple[int, int, int], size: tuple[int, int]) -> None:
    image = QImage(size[0], size[1], QImage.Format.Format_RGB32)
    image.fill(QColor(*color))
    assert image.save(str(path), "PNG")


def _inputs(
    tmp_path: Path,
    fixture_id: str = "00tra-courtyard-retail",
) -> tuple[Path, Path, dict]:
    catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    catalog["schema"] = "opennova.render-fixtures.v2"
    catalog["retail_video_profile_contract"] = VIDEO_PROFILE_CONTRACT
    catalog_payload = dict(catalog)
    catalog_payload.pop("catalog_sha256")
    catalog["catalog_sha256"] = hashlib.sha256(json.dumps(
        catalog_payload, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()
    (tmp_path / "catalog.json").write_text(
        json.dumps(catalog), encoding="utf-8"
    )
    fixture = next(
        row for row in catalog["fixtures"]
        if row["id"] == fixture_id
    )
    opennova_image = tmp_path / "opennova-raw.png"
    retail_image = tmp_path / "retail-raw.png"
    _solid(opennova_image, (20, 40, 60), (2000, 1200))
    _solid(retail_image, (21, 42, 63), (1920, 1200))

    source_commit = "a" * 40
    player_position_bms = fixture["retail_player_bms"]["applied"]
    camera_bms = fixture["camera_bms"]
    opennova_witness = {
        **MATCHED_PRESENTATION_CONTRACT,
        "observed_at": "after_pose_settle_before_fixture_freeze",
        "player_class": 9,
        "player_position_bms": player_position_bms,
        "requested_player_pose_bms": {
            "position": player_position_bms,
            "yaw_deg": camera_bms["yaw_deg"],
            "pitch_deg": camera_bms["pitch_deg"],
        },
        "hud_canvas_layer_visible": True,
        "viewmodel_canvas_layer_visible": True,
        "terrain_data_available": True,
        "terrain_node_visible": True,
    }
    opennova_state = tmp_path / "opennova-raw.state.json"
    opennova_state.write_text(json.dumps({
        "capture": {
            "label": "00tra-courtyard-retail-m0900-beauty",
            "png_path": opennova_image.name,
            "png_sha256": _sha256(opennova_image),
        },
        "comparison_contract_witness": opennova_witness,
    }), encoding="utf-8")
    opennova_manifest = {
        "schema": "opennova.render-fixture-captures.v1",
        "catalog_schema": catalog["schema"],
        "catalog_sha256": catalog["catalog_sha256"],
        "fixture": fixture,
        "mission": {
            "file": fixture["mission"],
            "sha256": catalog["missions"][fixture["mission"]],
            "expansion": "revx02",
        },
        "capture": {
            "resolution": [2000, 1200],
            "mode": fixture["capture_mode"],
            "world_only": False,
            "viewmodel_hidden": False,
            "camera_bms": fixture["camera_bms"],
            "comparison_contract": MATCHED_PRESENTATION_CONTRACT,
            "comparison_contract_witness": opennova_witness,
        },
        "provenance": {
            "source_commit": source_commit,
            "godot_executable_sha256": "b" * 64,
            "gdextension_sha256": "c" * 64,
        },
        "artifacts": [
            {
                "label": "00tra-courtyard-retail-m0900-beauty",
                "variant": "beauty",
                "png_path": opennova_image.name,
                "png_sha256": _sha256(opennova_image),
                "state_path": opennova_state.name,
                "state_sha256": _sha256(opennova_state),
                "width": 2000,
                "height": 1200,
            }
        ],
    }
    opennova_manifest_path = tmp_path / "opennova-manifest.json"
    opennova_manifest_path.write_text(
        json.dumps(opennova_manifest), encoding="utf-8"
    )

    retail_bundle = {
        "schema": "opennova.registered-retail-capture.v5",
        "tool": {"name": "register_retail_capture", "version": "4.0.0"},
        "catalog_sha256": catalog["catalog_sha256"],
        "fixture_id": fixture["id"],
        "mission": {
            "file": fixture["mission"],
            "sha256": catalog["missions"][fixture["mission"]],
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
            "video_profile_id": evidence_builder.retail_contract.PROFILE_ID,
            "image_path": retail_image.name,
            "sha256": _sha256(retail_image),
            "width": 1920,
            "height": 1200,
            "pre_overlay": True,
            "frame_correlated": True,
            "frame_serial": "42",
            "frame_qpc": "9001",
        },
        "comparison_presentation": {
            "contract": MATCHED_PRESENTATION_CONTRACT,
            "observed": {
                "equipped_weapon": "WPN_M16BURST",
                "arms_appearance": "bare",
                "gameplay_hud_visible": False,
                "terrain_enabled": True,
                "viewmodel_enabled": True,
                "ads_active": False,
                "big_map_active": False,
            },
            "verification": {
                "method": "frame_correlated_pre_hud_snapshot",
                "bound_image_sha256": _sha256(retail_image),
                "telemetry_available": True,
            },
        },
        "retail_presentation_stage": {
            "manifest_path": "retail-stage.json",
            "manifest_sha256": "c" * 64,
            "schema": "opennova.retail-presentation-stage.v3",
            "tool_version": "3.0.0",
            "profile": VIDEO_PROFILE_CONTRACT,
            "game_config": {
                "file_name": "game.cfg",
                "original_sha256": "d" * 64,
                "effective_sha256": "e" * 64,
                "original_values": HIGHEST_VIDEO_PROFILE | {
                    "object_texdetail": 1,
                },
                "effective_values": HIGHEST_VIDEO_PROFILE,
                "changed_keys": ["object_texdetail"],
                "preserved_values": PRESERVED_CONFIG_VALUES,
                "hud_detail": {
                    "original": 0,
                    "effective": 0,
                    "unchanged": True,
                },
            },
            "weapon_profile": {
                "file_name": "weapon.sav",
                "sha256": catalog["retail_profile_contract"]["sha256"],
                "slot": 0,
                "blue": {
                    "player_class": 9,
                    "avatar_a": 2,
                    "avatar_b": 0,
                    "avatar_packed": 0x0402,
                },
                "red": {
                    "player_class": 9,
                    "avatar_a": 7,
                    "avatar_b": 0,
                    "avatar_packed": 0x8207,
                },
                "blue_selected_kit_contains_wpn_m16burst": True,
            },
        },
        "retail_runtime_witness": {
            "video_settings": {
                "profile_id": evidence_builder.retail_contract.PROFILE_ID,
                "source": "engine_runtime_global",
                "available": list(RUNTIME_VIDEO_PROFILE),
                "unavailable": [],
                "values": RUNTIME_VIDEO_PROFILE | {
                    "gamma": LIVE_FLOAT32_GAMMA,
                },
            },
            "d3d_device": {
                "frame_serial": "42",
                "frame_qpc": "9001",
                "adapter": {
                    "ordinal": 0,
                    "name": "AMD Radeon(TM) Graphics",
                    "guid": "D7B71EE2-55C1-11CF-63740273A5C2ED35",
                },
                "device_generation": 1,
                "reset_generation": 0,
                "windowed": True,
                "backbuffer": {
                    "width": 1920,
                    "height": 1200,
                    "format": "D3DFMT_X8R8G8B8",
                },
                "depth_format": "D3DFMT_D24S8",
                "multisample": {
                    "mode_token": 2,
                    "token_semantics": (
                        "one_based_ordinal_into_retail_selectable"
                    ),
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
                },
            },
            "presentation": {
                "policy": "retail_pre_hud_backbuffer_snapshot.v2",
                "generation": 7,
                "armed_frame_serial": "41",
                "armed_frame_qpc": "8995",
                "target_frame_serial": "42",
                "target_frame_qpc": "9010",
                "snapshot_count": 1,
                "boundary": "pre_feed",
                "before_retail_hud": True,
                "d3d_scene_split": {
                    "end_count": 1,
                    "begin_count": 1,
                    "restored": True,
                },
                "retail_ui": {
                    "original_call_count": 1,
                    "executed_unmodified": True,
                },
            },
        },
        "camera_bms": fixture["camera_bms"],
        "build": {
            "opennova_source_commit": source_commit,
            "retail_executable_sha256": "d" * 64,
            "onhook_mcp_sha256": "e" * 64,
            "onhook_proxy_sha256": "f" * 64,
            "onhook_forwarder_sha256": "1" * 64,
        },
        "retail_install": {
            "expansion": "revx02",
            "mounted_archives": [
                {"path": "expansion/revx02/RevX02L.pff", "sha256": "2" * 64},
                {"path": "expansion/revx02/RevX02.pff", "sha256": "3" * 64},
                {"path": "language.pff", "sha256": "4" * 64},
                {"path": "localres.pff", "sha256": "5" * 64},
                {"path": "resource.pff", "sha256": "6" * 64},
            ],
            "version_marker": {
                "path": "expansion/revx02/revx02.bin",
                "sha256": "7" * 64,
            },
        },
        "raw_evidence": {
            "state_name": "retail.state.json",
            "state_sha256": "8" * 64,
            "onhook_log_name": "onhook.log",
            "onhook_log_sha256": "9" * 64,
            "instance_status_name": "instance-status.json",
            "instance_status_sha256": "a" * 64,
            "fixture_result_name": "fixture-result.json",
            "fixture_result_sha256": "b" * 64,
            "capture_result_name": "capture-result.json",
            "capture_result_sha256": "c" * 64,
            "retail_stage_manifest_name": "retail-stage.json",
            "retail_stage_manifest_sha256": "c" * 64,
            "source_pid": 1234,
            "source_instance_id": "1234-ABC",
            "bridge_version_major": 1,
            "bridge_version_minor": 4,
            "hook_version": "0.5.0",
            "capture_bundle_supported": True,
        },
    }
    retail_stage_path = tmp_path / "retail-stage.json"
    retail_stage = retail_bundle["retail_presentation_stage"]
    retail_stage_path.write_text(json.dumps({
        "schema": retail_stage["schema"],
        "tool_version": retail_stage["tool_version"],
        "profile": retail_stage["profile"],
        "game_config": retail_stage["game_config"],
        "weapon_profile": retail_stage["weapon_profile"],
    }), encoding="utf-8")
    retail_stage["manifest_sha256"] = _sha256(retail_stage_path)
    retail_bundle["raw_evidence"]["retail_stage_manifest_sha256"] \
        = _sha256(retail_stage_path)
    retail_bundle_path = tmp_path / "retail-bundle.json"
    retail_bundle_path.write_text(json.dumps(retail_bundle), encoding="utf-8")
    return opennova_manifest_path, retail_bundle_path, fixture


def _run(
    opennova_manifest: Path,
    retail_bundle: Path,
    output_dir: Path,
    extra_args: Sequence[str] = (),
    fixture_id: str = "00tra-courtyard-retail",
    catalog_path: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    selected_catalog = catalog_path or opennova_manifest.parent / "catalog.json"
    return subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--catalog",
            str(selected_catalog),
            "--fixture-id",
            fixture_id,
            "--opennova-manifest",
            str(opennova_manifest),
            "--retail-bundle",
            str(retail_bundle),
            "--output-dir",
            str(output_dir),
            "--opennova-caption",
            "full-frame M16, authored start minute",
            "--retail-caption",
            "pre-overlay, frame-correlated",
            *extra_args,
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def test_registered_pair_is_normalized_once_and_fully_manifested(
    tmp_path: Path,
) -> None:
    opennova_manifest, retail_bundle, fixture = _inputs(tmp_path)
    output_dir = tmp_path / "published"

    result = _run(opennova_manifest, retail_bundle, output_dir)
    assert result.returncode == 0, result.stdout + result.stderr

    normalized = output_dir / "00tra-courtyard-retail-opennova-normalized.png"
    sheet = output_dir / "00tra-courtyard-retail-side-by-side.png"
    overlay = output_dir / "00tra-courtyard-retail-overlay-50.png"
    difference = output_dir / "00tra-courtyard-retail-absolute-diff.png"
    manifest_path = output_dir / "00tra-courtyard-retail-comparison.json"
    assert (QImage(str(normalized)).width(), QImage(str(normalized)).height()) \
        == (1920, 1200)
    assert (QImage(str(sheet)).width(), QImage(str(sheet)).height()) \
        == (3840, 1272)
    assert (QImage(str(overlay)).width(), QImage(str(overlay)).height()) \
        == (1920, 1200)
    assert (QImage(str(difference)).width(), QImage(str(difference)).height()) \
        == (1920, 1200)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    opennova_manifest_data = json.loads(
        opennova_manifest.read_text(encoding="utf-8")
    )
    retail_bundle_data = json.loads(retail_bundle.read_text(encoding="utf-8"))
    assert manifest["schema"] == "opennova.retail-comparison.v6"
    assert manifest["tool"] == {
        "name": "build_retail_side_by_side",
        "version": "4.0.0",
        "create_new": True,
    }
    assert manifest["caption_font"] == {
        "family": "Courier Prime",
        "regular_sha256": _sha256(CAPTION_FONT_REGULAR),
        "bold_sha256": _sha256(CAPTION_FONT_BOLD),
    }
    assert manifest["fixture_id"] == fixture["id"]
    assert manifest["catalog_sha256"] == json.loads(
        (tmp_path / "catalog.json").read_text(encoding="utf-8")
    )["catalog_sha256"]
    assert manifest["capture_mode"] == "hud_hidden"
    assert manifest["presentations"] == {
        "opennova": "hud_hidden",
        "retail": "game_composite_pre_retail_hud",
        "retail_gameplay_hud_visible": False,
        "onhook_overlay_present": False,
        "retail_policy": "retail_pre_hud_backbuffer_snapshot.v2",
        "retail_video_profile": evidence_builder.retail_contract.PROFILE_ID,
    }
    assert manifest["comparison_presentation"] == {
        "contract": MATCHED_PRESENTATION_CONTRACT,
        "match_status": "validated",
        "opennova": {
            "observed": {
                "ads_active": False,
                "arms_camo": [1, 0, 0],
                "arms_graphic": "IndoArms.3di",
                "big_map_active": False,
                "character_id": 0x0402,
                "equipped_weapon": "WPN_M16BURST",
                "weapon_clip": 30,
                "weapon_reserve": 270,
                "gameplay_hud_visible": False,
                "hud_canvas_layer_active": True,
                "hud_detail_level": 3,
                "player_view_effects_active": True,
                "terrain_enabled": True,
                "viewmodel_enabled": True,
            },
            "verification": {
                "method": "runtime_capture_witness",
                "observed_at": "after_pose_settle_before_fixture_freeze",
                "telemetry_available": True,
            },
            "source_witness": opennova_manifest_data["capture"][
                "comparison_contract_witness"
            ],
        },
        "retail": retail_bundle_data["comparison_presentation"],
        "retail_stage": retail_bundle_data["retail_presentation_stage"],
        "retail_runtime_witness": retail_bundle_data["retail_runtime_witness"],
    }
    assert manifest["captions"] == {
        "opennova": "full-frame M16, authored start minute",
        "retail": "pre-overlay, frame-correlated",
    }
    assert manifest["transform"] == {
        "kind": "horizontal_only",
        "implementation": "System.Drawing.Graphics.DrawImage",
        "interpolation": "HighQualityBicubic",
        "source_rectangle": [0, 0, 2000, 1200],
        "destination_rectangle": [0, 0, 1920, 1200],
        "crop": "none",
        "vertical_scale": 1.0,
    }
    assert manifest["inputs"]["opennova"]["dimensions"] == [2000, 1200]
    assert manifest["inputs"]["opennova"]["state_name"] \
        == "opennova-raw.state.json"
    assert manifest["inputs"]["opennova"]["state_sha256"] \
        == _sha256(tmp_path / "opennova-raw.state.json")
    assert manifest["inputs"]["retail"]["dimensions"] == [1920, 1200]
    assert manifest["inputs"]["retail"]["install"] == retail_bundle_data[
        "retail_install"
    ]
    assert manifest["inputs"]["retail"]["raw_evidence"] == retail_bundle_data[
        "raw_evidence"
    ]
    assert manifest["inputs"]["retail"]["presentation_stage"] \
        == retail_bundle_data["retail_presentation_stage"]
    assert manifest["outputs"]["opennova_normalized"] == {
        "path": normalized.name,
        "sha256": _sha256(normalized),
        "dimensions": [1920, 1200],
    }
    assert manifest["outputs"]["side_by_side"] == {
        "path": sheet.name,
        "sha256": _sha256(sheet),
        "dimensions": [3840, 1272],
    }
    assert manifest["outputs"]["overlay_50"] == {
        "path": overlay.name,
        "sha256": _sha256(overlay),
        "dimensions": [1920, 1200],
    }
    assert manifest["outputs"]["absolute_diff"] == {
        "path": difference.name,
        "sha256": _sha256(difference),
        "dimensions": [1920, 1200],
    }
    assert manifest["metrics_policy"] == {
        "full_frame": "qualitative_only_matched_hud_hidden_cross_engine",
        "roi_and_mask": "descriptive_explicit_presentation_regions",
    }
    assert manifest["metrics"]["full_frame"] == {
        "region": [0, 0, 1920, 1200],
        "pixel_count": 1920 * 1200,
        "channel_count": 1920 * 1200 * 3,
        "max_channel_delta": 3,
        "mean_abs_channel_delta": 2.0,
        "root_mean_square_channel_delta": 2.160247,
    }
    assert manifest["metrics"]["regions"] == []


def test_registered_pair_records_optional_roi_and_mask_metrics(
    tmp_path: Path,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    mask_path = tmp_path / "world-mask.png"
    mask = QImage(1920, 1200, QImage.Format.Format_Grayscale8)
    mask.fill(0)
    for y in range(5, 15):
        for x in range(10, 30):
            mask.setPixel(x, y, 255)
    assert mask.save(str(mask_path), "PNG")

    output_dir = tmp_path / "published"
    result = _run(
        opennova_manifest,
        retail_bundle,
        output_dir,
        ("--roi", "barrel=10,5,20,10", "--mask", str(mask_path)),
    )
    assert result.returncode == 0, result.stdout + result.stderr
    manifest = json.loads(
        (output_dir / "00tra-courtyard-retail-comparison.json").read_text(
            encoding="utf-8"
        )
    )
    assert manifest["metrics"]["regions"] == [
        {
            "name": "barrel",
            "type": "roi",
            "region": [10, 5, 20, 10],
            "pixel_count": 200,
            "channel_count": 600,
            "max_channel_delta": 3,
            "mean_abs_channel_delta": 2.0,
            "root_mean_square_channel_delta": 2.160247,
        },
        {
            "name": "world-mask",
            "type": "mask",
            "mask": {
                "source_name": mask_path.name,
                "sha256": _sha256(mask_path),
                "dimensions": [1920, 1200],
            },
            "pixel_count": 200,
            "channel_count": 600,
            "max_channel_delta": 3,
            "mean_abs_channel_delta": 2.0,
            "root_mean_square_channel_delta": 2.160247,
        },
    ]


def _set_nested(document: dict, path: tuple[object, ...], value: object) -> None:
    target: object = document
    for key in path[:-1]:
        target = target[key]  # type: ignore[index]
    target[path[-1]] = value  # type: ignore[index]


@pytest.mark.parametrize(
    ("path", "value", "message"),
    [
        (("fixture_id",), "cp01-water-wide-retail", "retail fixture"),
        (("catalog_sha256",), "0" * 64, "retail catalog hash"),
        (("mission", "file"), "CP01.bms", "retail mission"),
        (("capture", "mode"), "world_only", "retail capture mode"),
        (("camera_bms", "position", 0), 999.0, "retail camera pose"),
        (("build", "opennova_source_commit"), "1" * 40, "OpenNova build"),
        (("retail_install", "expansion"), "other", "retail expansion"),
        (("raw_evidence", "capture_result_sha256"), "bad", "raw evidence"),
    ],
)
def test_comparison_rejects_mismatched_registered_identity(
    tmp_path: Path,
    path: tuple[object, ...],
    value: object,
    message: str,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    bundle = json.loads(retail_bundle.read_text(encoding="utf-8"))
    _set_nested(bundle, path, value)
    retail_bundle.write_text(json.dumps(bundle), encoding="utf-8")

    output_dir = tmp_path / "published"
    result = _run(opennova_manifest, retail_bundle, output_dir)
    assert result.returncode != 0
    assert message in result.stderr
    assert not output_dir.exists()


def test_comparison_requires_the_colocated_retail_stage_artifact(
    tmp_path: Path,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    (tmp_path / "retail-stage.json").unlink()

    result = _run(opennova_manifest, retail_bundle, tmp_path / "published")
    assert result.returncode != 0
    assert "retail stage manifest file not found" in result.stderr


@pytest.mark.parametrize(
    ("path", "value"),
    [
        (("schema",), "opennova.registered-retail-capture.v4"),
        (("tool", "version"), "3.0.0"),
    ],
)
def test_comparison_rejects_a_legacy_registered_capture_contract(
    tmp_path: Path, path: tuple[str, ...], value: str,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    bundle = json.loads(retail_bundle.read_text(encoding="utf-8"))
    target = bundle
    for key in path[:-1]:
        target = target[key]
    target[path[-1]] = value
    retail_bundle.write_text(json.dumps(bundle), encoding="utf-8")

    result = _run(opennova_manifest, retail_bundle, tmp_path / "published")

    assert result.returncode != 0
    assert "not a registered capture bundle" in result.stderr


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("bridge_version_major", 0),
        ("bridge_version_minor", 3),
        ("hook_version", "0.4.0"),
        ("capture_bundle_supported", False),
    ],
)
def test_comparison_rejects_legacy_registered_capture_producer_evidence(
    tmp_path: Path, field: str, value: object,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    bundle = json.loads(retail_bundle.read_text(encoding="utf-8"))
    bundle["raw_evidence"][field] = value
    retail_bundle.write_text(json.dumps(bundle), encoding="utf-8")

    result = _run(opennova_manifest, retail_bundle, tmp_path / "published")

    assert result.returncode != 0
    assert "raw evidence producer version" in result.stderr


def test_comparison_rejects_tampered_retail_stage_artifact(
    tmp_path: Path,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    stage_path = tmp_path / "retail-stage.json"
    stage = json.loads(stage_path.read_text(encoding="utf-8"))
    stage["game_config"]["hud_detail"]["effective"] = 3
    stage_path.write_text(json.dumps(stage), encoding="utf-8")

    result = _run(opennova_manifest, retail_bundle, tmp_path / "published")
    assert result.returncode != 0
    assert "retail stage manifest hash" in result.stderr


@pytest.mark.parametrize("bad_path", ["C:/capture/retail-stage.json", "../stage.json"])
def test_comparison_rejects_nonportable_retail_stage_artifact_path(
    tmp_path: Path,
    bad_path: str,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    bundle = json.loads(retail_bundle.read_text(encoding="utf-8"))
    bundle["retail_presentation_stage"]["manifest_path"] = bad_path
    retail_bundle.write_text(json.dumps(bundle), encoding="utf-8")

    result = _run(opennova_manifest, retail_bundle, tmp_path / "published")
    assert result.returncode != 0
    assert "portable relative path" in result.stderr


@pytest.mark.parametrize(
    ("document_name", "path", "value", "message"),
    [
        (
            "opennova",
            ("capture", "world_only"),
            True,
            "OpenNova HUD-hidden visibility",
        ),
        (
            "opennova",
            ("capture", "viewmodel_hidden"),
            True,
            "OpenNova HUD-hidden visibility",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "equipped_weapon"),
            "WPN_M4AUTO",
            "OpenNova equipped weapon",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "weapon_clip"),
            30.5,
            "OpenNova weapon clip",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "weapon_reserve"),
            300,
            "OpenNova weapon reserve",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "character_id"),
            0x0200,
            "OpenNova character id",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "arms_graphic"),
            "ArmsG.3di",
            "OpenNova arms graphic",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "arms_camo"),
            [0, 0, 0],
            "OpenNova arms camo",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "hud_detail_level"),
            0,
            "OpenNova hud detail level",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "gameplay_hud_visible"),
            True,
            "OpenNova gameplay hud visible",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness",
             "player_view_effects_active"),
            False,
            "OpenNova player view effects active",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "ads_active"),
            True,
            "OpenNova ads active",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "big_map_active"),
            True,
            "OpenNova big map active",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness",
             "hud_canvas_layer_visible"),
            False,
            "OpenNova HUD CanvasLayer witness",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness",
             "viewmodel_canvas_layer_visible"),
            False,
            "OpenNova viewmodel witness",
        ),
        (
            "opennova",
            ("capture", "comparison_contract_witness", "terrain_node_visible"),
            False,
            "OpenNova terrain witness",
        ),
        (
            "retail",
            ("comparison_presentation", "observed", "equipped_weapon"),
            "WPN_M4AUTO",
            "retail equipped weapon witness",
        ),
        (
            "retail",
            ("comparison_presentation", "observed", "arms_appearance"),
            "camo_sleeve_black_gloves",
            "retail arms appearance witness",
        ),
        (
            "retail",
            ("comparison_presentation", "observed", "gameplay_hud_visible"),
            True,
            "retail gameplay hud visible witness",
        ),
        (
            "retail",
            ("comparison_presentation", "observed", "viewmodel_enabled"),
            False,
            "retail viewmodel enabled witness",
        ),
        (
            "retail",
            ("comparison_presentation", "observed", "terrain_enabled"),
            False,
            "retail terrain enabled witness",
        ),
        (
            "retail",
            ("comparison_presentation", "verification", "bound_image_sha256"),
            "0" * 64,
            "hashed frame",
        ),
    ],
)
def test_comparison_rejects_unmatched_presentation_witnesses(
    tmp_path: Path,
    document_name: str,
    path: tuple[object, ...],
    value: object,
    message: str,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    document_path = (
        opennova_manifest if document_name == "opennova" else retail_bundle
    )
    document = json.loads(document_path.read_text(encoding="utf-8"))
    _set_nested(document, path, value)
    document_path.write_text(json.dumps(document), encoding="utf-8")

    output_dir = tmp_path / "published"
    result = _run(opennova_manifest, retail_bundle, output_dir)
    assert result.returncode != 0
    assert message in result.stderr
    assert not output_dir.exists()


def test_comparison_requires_the_canonical_hud_hidden_m16_contract(
    tmp_path: Path,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    catalog = json.loads((tmp_path / "catalog.json").read_text(encoding="utf-8"))
    catalog["comparison_contract"]["equipped_weapon"] = "WPN_M4AUTO"
    payload = dict(catalog)
    payload.pop("catalog_sha256")
    catalog["catalog_sha256"] = hashlib.sha256(json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode()).hexdigest()
    catalog_path = tmp_path / "mismatched-catalog.json"
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
    for document_path in (opennova_manifest, retail_bundle):
        document = json.loads(document_path.read_text(encoding="utf-8"))
        document["catalog_sha256"] = catalog["catalog_sha256"]
        document_path.write_text(json.dumps(document), encoding="utf-8")

    output_dir = tmp_path / "published"
    result = _run(
        opennova_manifest,
        retail_bundle,
        output_dir,
        catalog_path=catalog_path,
    )
    assert result.returncode != 0
    assert "matched HUD-hidden WPN_M16BURST" in result.stderr
    assert not output_dir.exists()


def test_comparison_rejects_rehashed_capture_state_witness_divergence(
    tmp_path: Path,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    manifest = json.loads(opennova_manifest.read_text(encoding="utf-8"))
    artifact = manifest["artifacts"][0]
    state_path = opennova_manifest.parent / artifact["state_path"]
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["comparison_contract_witness"]["equipped_weapon"] = "WPN_M4AUTO"
    state_path.write_text(json.dumps(state), encoding="utf-8")
    artifact["state_sha256"] = _sha256(state_path)
    opennova_manifest.write_text(json.dumps(manifest), encoding="utf-8")

    output_dir = tmp_path / "published"
    result = _run(opennova_manifest, retail_bundle, output_dir)
    assert result.returncode != 0
    assert "capture-state presentation witness" in result.stderr
    assert not output_dir.exists()


def test_comparison_rejects_rehashed_capture_state_label_divergence(
    tmp_path: Path,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    manifest = json.loads(opennova_manifest.read_text(encoding="utf-8"))
    artifact = manifest["artifacts"][0]
    state_path = opennova_manifest.parent / artifact["state_path"]
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["capture"]["label"] = "transport-truncated"
    state_path.write_text(json.dumps(state), encoding="utf-8")
    artifact["state_sha256"] = _sha256(state_path)
    opennova_manifest.write_text(json.dumps(manifest), encoding="utf-8")

    output_dir = tmp_path / "published"
    result = _run(opennova_manifest, retail_bundle, output_dir)
    assert result.returncode != 0
    assert "capture-state label does not match manifest artifact" in result.stderr
    assert not output_dir.exists()


def test_comparison_rejects_pre_normalized_opennova_input(tmp_path: Path) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    manifest = json.loads(opennova_manifest.read_text(encoding="utf-8"))
    image_path = opennova_manifest.parent / manifest["artifacts"][0]["png_path"]
    _solid(image_path, (20, 40, 60), (1920, 1200))
    manifest["capture"]["resolution"] = [1920, 1200]
    manifest["artifacts"][0].update({
        "width": 1920,
        "height": 1200,
        "png_sha256": _sha256(image_path),
    })
    opennova_manifest.write_text(json.dumps(manifest), encoding="utf-8")

    result = _run(opennova_manifest, retail_bundle, tmp_path / "published")
    assert result.returncode != 0
    assert "raw 2000x1200" in result.stderr


@pytest.mark.parametrize(
    ("document_name", "field_path", "bad_path"),
    [
        ("opennova", ("artifacts", 0, "png_path"), "C:/capture/raw.png"),
        ("opennova", ("artifacts", 0, "png_path"), "../raw.png"),
        ("retail", ("capture", "image_path"), "C:/capture/raw.png"),
        ("retail", ("capture", "image_path"), "../raw.png"),
    ],
)
def test_comparison_rejects_nonportable_or_escaping_input_paths(
    tmp_path: Path,
    document_name: str,
    field_path: tuple[object, ...],
    bad_path: str,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    path = opennova_manifest if document_name == "opennova" else retail_bundle
    document = json.loads(path.read_text(encoding="utf-8"))
    _set_nested(document, field_path, bad_path)
    path.write_text(json.dumps(document), encoding="utf-8")

    result = _run(opennova_manifest, retail_bundle, tmp_path / "published")
    assert result.returncode != 0
    assert "portable relative path" in result.stderr


def test_comparison_rejects_undeclared_artifact_and_hash_divergence(
    tmp_path: Path,
) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    manifest = json.loads(opennova_manifest.read_text(encoding="utf-8"))
    manifest["artifacts"][0]["variant"] = "lighting_only"
    opennova_manifest.write_text(json.dumps(manifest), encoding="utf-8")
    result = _run(opennova_manifest, retail_bundle, tmp_path / "undeclared")
    assert result.returncode != 0
    assert "exactly one beauty artifact" in result.stderr

    manifest["artifacts"][0]["variant"] = "beauty"
    manifest["artifacts"][0]["png_sha256"] = "0" * 64
    opennova_manifest.write_text(json.dumps(manifest), encoding="utf-8")
    result = _run(opennova_manifest, retail_bundle, tmp_path / "bad-hash")
    assert result.returncode != 0
    assert "image hash" in result.stderr


def test_comparison_outputs_are_create_new(tmp_path: Path) -> None:
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path)
    output_dir = tmp_path / "published"
    first = _run(opennova_manifest, retail_bundle, output_dir)
    assert first.returncode == 0, first.stdout + first.stderr
    hashes_before = {
        path.name: _sha256(path) for path in output_dir.iterdir() if path.is_file()
    }

    second = _run(opennova_manifest, retail_bundle, output_dir)
    assert second.returncode != 0
    assert "refusing to overwrite" in second.stderr
    assert {
        path.name: _sha256(path) for path in output_dir.iterdir() if path.is_file()
    } == hashes_before


def test_hud_hidden_fixture_keeps_cross_engine_metrics_qualitative(
    tmp_path: Path,
) -> None:
    fixture_id = "00tra-fire-barrel-full-composite-retail"
    opennova_manifest, retail_bundle, _fixture = _inputs(tmp_path, fixture_id)
    output_dir = tmp_path / "published"
    result = _run(
        opennova_manifest,
        retail_bundle,
        output_dir,
        fixture_id=fixture_id,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    manifest = json.loads(
        (output_dir / f"{fixture_id}-comparison.json").read_text(
            encoding="utf-8"
        )
    )
    assert manifest["metrics_policy"]["full_frame"] \
        == "qualitative_only_matched_hud_hidden_cross_engine"


def _assert_published_artifact(
    manifest_path: Path,
    path_value: object,
    expected_sha256: str,
    expected_dimensions: Sequence[int] | None = None,
) -> Path:
    artifact_path = evidence_builder.resolve_artifact(
        path_value, manifest_path, "published artifact"
    )
    assert _sha256(artifact_path) == expected_sha256
    if expected_dimensions is not None:
        image = QImage(str(artifact_path))
        assert not image.isNull()
        assert [image.width(), image.height()] == list(expected_dimensions)
    return artifact_path


def test_previous_registered_evidence_catalog_is_rejected() -> None:
    catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
    assert catalog["catalog_sha256"] == LEGACY_CATALOG_SHA256
    assert catalog["comparison_contract"] == MATCHED_PRESENTATION_CONTRACT
    assert catalog["retail_profile_contract"] \
        == evidence_builder.CANONICAL_RETAIL_PROFILE_FACTS | {
            "sha256": (
                "f4907820a58505a6988f140a27638dae7dbaaea2cc446642f0bc44c868905623"
            ),
        }
    assert {fixture["id"] for fixture in catalog["fixtures"]} \
        == LEGACY_FIXTURE_IDS

    fixture_id = sorted(LEGACY_FIXTURE_IDS)[0]
    with pytest.raises(
        evidence_builder.EvidenceError,
        match="unsupported fixture catalog schema",
    ):
        evidence_builder.validate_registered_inputs(
            CATALOG_PATH,
            fixture_id,
            Path("unused-opennova-manifest.json"),
            Path("unused-retail-bundle.json"),
        )


def test_current_max_quality_publication_has_the_complete_portable_inventory(
) -> None:
    assert CURRENT_EVIDENCE_ROOT.is_dir()
    fixture_ids = {
        path.name for path in CURRENT_EVIDENCE_ROOT.iterdir()
        if path.is_dir()
    }
    assert fixture_ids == PUBLISHED_FIXTURE_IDS
    assert {
        path.name for path in CURRENT_EVIDENCE_ROOT.iterdir()
        if path.is_file()
    } == {"README.md"}

    published_files = [
        path for path in CURRENT_EVIDENCE_ROOT.rglob("*") if path.is_file()
    ]
    assert len(published_files) == 305
    assert sum(path.suffix == ".png" for path in published_files) == 160
    assert sum(path.suffix == ".json" for path in published_files) == 144
    assert sum(path.suffix == ".md" for path in published_files) == 1


@pytest.mark.parametrize("fixture_id", sorted(PUBLISHED_FIXTURE_IDS))
def test_current_max_quality_publication_is_registered_and_hash_complete(
    fixture_id: str,
) -> None:
    fixture_root = CURRENT_EVIDENCE_ROOT / fixture_id
    retail_root = fixture_root / "retail"
    opennova_root = fixture_root / "opennova"
    comparison_root = fixture_root / "comparison"
    opennova_manifest_path = (
        opennova_root / f"{fixture_id}-manifest.json"
    )
    retail_bundle_path = retail_root / "registered.json"
    comparison_manifest_path = (
        comparison_root / f"{fixture_id}-comparison.json"
    )

    validated = evidence_builder.validate_registered_inputs(
        CURRENT_CATALOG_PATH,
        fixture_id,
        opennova_manifest_path,
        retail_bundle_path,
    )
    assert validated["catalog_sha"] == CURRENT_CATALOG_SHA256
    assert validated["source_commit"] == CURRENT_SOURCE_COMMIT

    assert {
        path.name for path in retail_root.iterdir() if path.is_file()
    } == {"retail.png", "retail-stage.json", "registered.json"}

    opennova_manifest = validated["opennova"]
    assert opennova_manifest["schema"] \
        == "opennova.render-fixture-captures.v1"
    assert opennova_manifest["catalog_sha256"] == CURRENT_CATALOG_SHA256
    assert opennova_manifest["provenance"]["source_commit"] \
        == CURRENT_SOURCE_COMMIT
    artifacts = opennova_manifest["artifacts"]
    assert {artifact["variant"] for artifact in artifacts} \
        == PUBLISHED_VARIANTS
    assert len(artifacts) == 5

    expected_opennova_files = {opennova_manifest_path.name}
    for artifact in artifacts:
        _assert_published_artifact(
            opennova_manifest_path,
            artifact["png_path"],
            artifact["png_sha256"],
            [2000, 1200],
        )
        _assert_published_artifact(
            opennova_manifest_path,
            artifact["state_path"],
            artifact["state_sha256"],
        )
        expected_opennova_files.update({
            artifact["png_path"],
            artifact["state_path"],
        })
    assert {
        path.name for path in opennova_root.iterdir() if path.is_file()
    } == expected_opennova_files

    comparison = json.loads(
        comparison_manifest_path.read_text(encoding="utf-8")
    )
    assert comparison["schema"] == "opennova.retail-comparison.v6"
    assert comparison["tool"] == {
        "name": "build_retail_side_by_side",
        "version": "4.0.0",
        "create_new": True,
    }
    assert comparison["catalog_sha256"] == CURRENT_CATALOG_SHA256
    assert comparison["fixture_id"] == fixture_id
    assert comparison["inputs"]["opennova"]["source_commit"] \
        == CURRENT_SOURCE_COMMIT
    assert comparison["inputs"]["opennova"]["manifest_sha256"] \
        == _sha256(opennova_manifest_path)
    assert comparison["inputs"]["retail"]["bundle_sha256"] \
        == _sha256(retail_bundle_path)
    assert comparison["metrics_policy"] == {
        "full_frame": "qualitative_only_matched_hud_hidden_cross_engine",
        "roi_and_mask": "descriptive_explicit_presentation_regions",
    }
    assert {
        (region["name"], region["type"], tuple(region["region"]))
        for region in comparison["metrics"]["regions"]
    } == {
        ("world_center", "roi", (240, 180, 1320, 420)),
        ("viewmodel_arms", "roi", (850, 700, 900, 500)),
    }
    captions = comparison["captions"]
    assert set(captions) == {"opennova", "retail"}
    assert all(isinstance(value, str) and value for value in captions.values())

    expected_comparison_files = {comparison_manifest_path.name}
    for output_name, dimensions in PUBLISHED_OUTPUT_DIMENSIONS.items():
        output = comparison["outputs"][output_name]
        _assert_published_artifact(
            comparison_manifest_path,
            output["path"],
            output["sha256"],
            dimensions,
        )
        expected_comparison_files.add(output["path"])
    assert {
        path.name for path in comparison_root.iterdir() if path.is_file()
    } == expected_comparison_files
