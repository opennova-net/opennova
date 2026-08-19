import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

import pytest

try:
    from PySide6.QtGui import QColor, QImage
except ImportError as exc:  # headless box without libGL/Qt
    pytest.skip(f"PySide6 QtGui is unavailable: {exc}", allow_module_level=True)

from scripts.render import build_retail_side_by_side as evidence_builder


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "render" / "register_retail_capture.py"

HIGHEST_VIDEO_PROFILE = {
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
RUNTIME_VIDEO_PROFILE = {
    key: value
    for key, value in HIGHEST_VIDEO_PROFILE.items()
    if key not in {"windowed", "video_res"}
}
LIVE_FLOAT32_GAMMA = 0.800000011920929
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
PRESERVED_CONFIG_VALUES = {
    "hw3d_deviceno": 0,
    "hw3d_name": "AMD Radeon(TM) Graphics",
    "hw3d_guid": "D7B71EE2-55C1-11CF-63740273A5C2ED35",
    "enable_keyboardtips": 1,
    "enable_gameplaytips": 1,
    "hud_detail": 0,
}


def _video_catalog_sha256() -> str:
    payload = {
        "id": "joint-operations-revx02-video-options-v1",
        "policies": VIDEO_OPTION_POLICIES,
    }
    return hashlib.sha256(json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()


def _game_config(values: dict[str, int | float | str]) -> bytes:
    return b"".join(
        f"{key} = {value}\r\n".encode("ascii")
        for key, value in (PRESERVED_CONFIG_VALUES | values).items()
    )


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write_pff(path: Path, entries: list[tuple[str, bytes]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = b"".join(data for _, data in entries)
    table_offset = 20 + len(payload)
    result = bytearray(struct.pack("<5I", 20, 0x33464650, len(entries), 36, table_offset))
    result += payload
    offset = 20
    for name, data in entries:
        record = bytearray(36)
        struct.pack_into("<4I", record, 0, 0, offset, len(data), 0)
        encoded = name.encode("ascii")[:16]
        record[16:16 + len(encoded)] = encoded
        result += record
        offset += len(data)
    path.write_bytes(result)


def _weapon_profile_bytes() -> bytes:
    data = bytearray(16 + 5 * 0x1080C)
    struct.pack_into("<4s4sII", data, 0, b"FPBC", b"0211", 0, 0)
    blue = 16
    red = blue + 0x8006
    struct.pack_into("<BBBxH", data, blue, 9, 2, 0, 0x0402)
    struct.pack_into("<BBBxH", data, red, 9, 7, 0, 0x8207)
    engineer_page = blue + 6 + 4 * 2048
    tokens = (
        "WPN_KNIFE", "-1", "-1", "-1",
        "WPN_M16BURST", "-1", "-1", "-1",
    )
    page = "\0".join(tokens).encode("ascii") + b"\0\0"
    data[engineer_page:engineer_page + len(page)] = page
    return bytes(data)


def _catalog(path: Path, mission_bytes: bytes, profile_bytes: bytes) -> dict:
    fixture = {
        "id": "fixture-retail",
        "mission": "TEST.bms",
        "capture_mode": "hud_hidden",
        "retail_player_bms": {
            "applied": [10.0, 20.0, 30.0],
            "provenance_status": "required_at_capture",
        },
        "camera_bms": {
            "position": [0.0, 0.0, 0.0],
            "yaw_deg": 0.0,
            "pitch_deg": -5.0,
            "vertical_fov_deg": 53.4468,
        },
        "minutes_of_day": [900],
    }
    catalog = {
        "schema": "opennova.render-fixtures.v2",
        "comparison_contract": {
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
        },
        "retail_profile_contract": {
            "path": "expansion/revx02/weapon.sav",
            "sha256": hashlib.sha256(profile_bytes).hexdigest(),
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
        },
        "retail_video_profile_contract": {
            "id": "retail_reference_highest_retail_selectable_v2",
            "video_option_catalog_id": "joint-operations-revx02-video-options-v1",
            "video_option_catalog_sha256": _video_catalog_sha256(),
            "required_values": HIGHEST_VIDEO_PROFILE,
        },
        "retail_eye_model": {"eye_up": 0.91, "eye_right": 0.24},
        "capture": {
            "resolution": [2000, 1200],
            "retail_resolution": [1920, 1200],
        },
        "missions": {
            "TEST.bms": hashlib.sha256(mission_bytes).hexdigest(),
        },
        "fixtures": [fixture],
    }
    canonical = json.dumps(catalog, sort_keys=True, separators=(",", ":")).encode()
    catalog["catalog_sha256"] = hashlib.sha256(canonical).hexdigest()
    path.write_text(json.dumps(catalog), encoding="utf-8")
    return catalog


def _inputs(tmp_path: Path) -> dict[str, Path | str]:
    mission_bytes = b"registered retail mission\n"
    profile_bytes = _weapon_profile_bytes()
    catalog_path = tmp_path / "catalog.json"
    catalog = _catalog(catalog_path, mission_bytes, profile_bytes)

    game_dir = tmp_path / "retail-game"
    expansion_dir = game_dir / "expansion" / "revx02"
    _write_pff(game_dir / "language.pff", [])
    _write_pff(game_dir / "localres.pff", [])
    _write_pff(game_dir / "resource.pff", [("TEST.bms", mission_bytes)])
    _write_pff(expansion_dir / "revx02.pff", [])
    _write_pff(expansion_dir / "revx02L.pff", [])
    (expansion_dir / "revx02.bin").write_bytes(b"revision marker")
    original_values = HIGHEST_VIDEO_PROFILE | {"object_texdetail": 1}
    original_config = _game_config(original_values)
    effective_config = _game_config(HIGHEST_VIDEO_PROFILE)
    (game_dir / "game.cfg").write_bytes(effective_config)
    (expansion_dir / "weapon.sav").write_bytes(profile_bytes)
    retail_stage_manifest = tmp_path / "retail-stage.json"
    retail_stage_manifest.write_text(json.dumps({
        "schema": "opennova.retail-presentation-stage.v3",
        "tool_version": "3.0.0",
        "profile": catalog["retail_video_profile_contract"],
        "game_config": {
            "file_name": "game.cfg",
            "original_sha256": hashlib.sha256(original_config).hexdigest(),
            "effective_sha256": hashlib.sha256(effective_config).hexdigest(),
            "original_values": original_values,
            "effective_values": HIGHEST_VIDEO_PROFILE,
            "changed_keys": ["object_texdetail"],
            "preserved_values": PRESERVED_CONFIG_VALUES,
            "hud_detail": {"original": 0, "effective": 0, "unchanged": True},
        },
        "weapon_profile": {
            "file_name": "weapon.sav",
            "sha256": hashlib.sha256(profile_bytes).hexdigest(),
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
    }), encoding="utf-8")
    for path, contents in (
        (game_dir / "Jointops.exe", b"retail executable"),
        (game_dir / "onhook-mcp.exe", b"onhook mcp"),
        (game_dir / "binkw32.dll", b"onhook proxy"),
        (game_dir / "binkw32_.dll", b"retail bink forwarder"),
    ):
        path.write_bytes(contents)

    image_path = tmp_path / "retail.png"
    image = QImage(1920, 1200, QImage.Format.Format_RGB32)
    image.fill(QColor(20, 30, 40))
    assert image.save(str(image_path), "PNG")
    source_commit = "a" * 40
    state = {
        "schema": "opennova.render_capture_bundle.v4",
        "source": {
            "instance_id": "1234-ABC",
            "pid": 1234,
            "process_start_time": str(int("ABC", 16)),
            "executable": "Jointops.exe",
            "hook_version": "0.5.0",
            "bridge_version_major": 1,
            "bridge_version_minor": 4,
            "pre_overlay": True,
            "frame_correlated": True,
        },
        "fixture_context": {
            "fixture_id": "fixture-retail",
            "verified": False,
            "reason": "caller-supplied context",
        },
        "frame_serial": "42",
        "frame_qpc": "9001",
        "telemetry": {
            "available": ["view_matrix", "projection_matrix", "viewport"],
            "unavailable": [],
            "unsupported": [],
        },
        "observed_state": {
            "player_present": True,
            "capture_ready": True,
            "camera_available": True,
            "camera_mode": "first_person",
            "position_bms": {"x": 10.0, "y": 20.0, "z": 30.0},
            "heading_bam": 1073741824,
            "pitch_bam": -59652323,
        },
        "render_state": {
            "view_matrix_row_major": [
                1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0,
                0.0, 0.0, 0.0, 1.0,
            ],
            "projection_matrix_row_major": [
                1.1917536, 0.0, 0.0, 0.0,
                0.0, 1.9862564, 0.0, 0.0,
                0.0, 0.0, 1.0, 1.0,
                0.0, 0.0, -1.0, 0.0,
            ],
            "viewport": {
                "x": 0,
                "y": 0,
                "width": 1920,
                "height": 1200,
                "min_z": 0.0,
                "max_z": 1.0,
            },
        },
        "image": {
            "width": 1920,
            "height": 1200,
            "sha256": _sha256(image_path),
        },
        "video_settings": {
            "profile_id": "retail_reference_highest_retail_selectable_v2",
            "source": "engine_runtime_global",
            "available": list(RUNTIME_VIDEO_PROFILE),
            "unavailable": [],
            "values": RUNTIME_VIDEO_PROFILE | {"gamma": LIVE_FLOAT32_GAMMA},
        },
        "d3d_device": {
            "frame_serial": "42",
            "frame_qpc": "9001",
            "adapter": {
                "ordinal": 0,
                "name": "AMD Radeon(TM) Graphics",
                "guid": "D7B71EE2-55C1-11CF-6374-0273A5C2ED35",
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
    }
    state_path = tmp_path / "retail.state.json"
    state_path.write_text(json.dumps(state), encoding="utf-8")
    instance_status_path = tmp_path / "onhook-instances.json"
    instance_status_path.write_text(json.dumps({
        "instances": [{
            "instance_id": "1234-ABC",
            "pid": 1234,
            "executable": "Jointops.exe",
            "build_flavor": "proxy",
            "capture_bundle_supported": True,
            "render_fixture_supported": True,
            "render_state_available": True,
            "capture_ready": True,
            "frame_width": 1920,
            "frame_height": 1200,
        }],
        "count": 1,
    }), encoding="utf-8")
    fixture_result_path = tmp_path / "fixture-result.json"
    fixture_result_path.write_text(json.dumps({
        "schema": "opennova.render_fixture_result.v1",
        "instance_id": "1234-ABC",
        "fixture_id": "fixture-retail",
        "exact": True,
        "frame_serial": "40",
        "frame_qpc": "8990",
        "fixture": {
            "position_bms": {"x": 10.0, "y": 20.0, "z": 30.0},
            "yaw_degrees": 0.0,
            "pitch_degrees": -5.0,
            "camera_mode": "first_person",
            "vertical_fov_degrees": 53.4468,
            "time_of_day_seconds": 54000,
            "mission_file": "TEST.bms",
            "mission_sha256": catalog["missions"]["TEST.bms"],
        },
    }), encoding="utf-8")
    capture_result_path = tmp_path / "capture-result.json"
    capture_result_path.write_text(json.dumps({
        "schema": "opennova.render_capture_bundle.v4",
        "profile_id": "retail_reference_highest_retail_selectable_v2",
        "instance_id": "1234-ABC",
        "fixture_id": "fixture-retail",
        "path": str(image_path.resolve()),
        "state_path": str(state_path.resolve()),
        "mime_type": "image/png",
        "file_bytes": image_path.stat().st_size,
        "state_bytes": state_path.stat().st_size,
        "frame_serial": "42",
        "frame_qpc": "9001",
        "source": {"width": 1920, "height": 1200},
    }), encoding="utf-8")
    log_path = tmp_path / "onhook.log"
    log_path.write_text(
        "Debug LAN host: armed mission=TEST.bms, explicit port\n"
        "Debug bridge: listening on \\\\.\\pipe\\opennova.onhook.v1.1234.ABC\n"
        "Debug LAN host: starting mission 'TEST.bms'\n",
        encoding="utf-8",
    )
    return {
        "catalog": catalog_path,
        "game_dir": game_dir,
        "state": state_path,
        "instance_status": instance_status_path,
        "fixture_result": fixture_result_path,
        "capture_result": capture_result_path,
        "image": image_path,
        "log": log_path,
        "retail_stage_manifest": retail_stage_manifest,
        "source_commit": source_commit,
        "output": tmp_path / "registered.json",
    }


def _run(
    inputs: dict[str, Path | str],
    confirm_presentation: bool = True,
) -> subprocess.CompletedProcess[str]:
    game_dir = Path(inputs["game_dir"])
    retail_executable = Path(
        inputs.get("retail_executable", game_dir / "Jointops.exe")
    )
    onhook_mcp = Path(inputs.get("onhook_mcp", game_dir / "onhook-mcp.exe"))
    onhook_proxy = Path(inputs.get("onhook_proxy", game_dir / "binkw32.dll"))
    onhook_forwarder = Path(
        inputs.get("onhook_forwarder", game_dir / "binkw32_.dll")
    )
    confirmation = ["--confirm-retail-presentation-contract"] \
        if confirm_presentation else []
    return subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--catalog", str(inputs["catalog"]),
            "--fixture-id", "fixture-retail",
            "--raw-state", str(inputs["state"]),
            "--instance-status", str(inputs["instance_status"]),
            "--fixture-result", str(inputs["fixture_result"]),
            "--capture-result", str(inputs["capture_result"]),
            "--raw-image", str(inputs["image"]),
            "--onhook-log", str(inputs["log"]),
            "--game-dir", str(game_dir),
            "--expansion", "revx02",
            "--retail-executable", str(retail_executable),
            "--onhook-mcp", str(onhook_mcp),
            "--onhook-proxy", str(onhook_proxy),
            "--onhook-forwarder", str(onhook_forwarder),
            "--retail-stage-manifest", str(inputs["retail_stage_manifest"]),
            "--opennova-source-commit", str(inputs["source_commit"]),
            "--output", str(inputs["output"]),
            *confirmation,
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def _write_state_and_rebind_capture(
    inputs: dict[str, Path | str], state: dict[str, object]
) -> None:
    state_path = Path(inputs["state"])
    state_path.write_text(json.dumps(state), encoding="utf-8")
    capture_path = Path(inputs["capture_result"])
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    capture["state_bytes"] = state_path.stat().st_size
    capture_path.write_text(json.dumps(capture), encoding="utf-8")


def test_preflight_registers_only_vfs_and_frame_correlated_evidence(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    result = _run(inputs)
    assert result.returncode == 0, result.stdout + result.stderr
    registration = json.loads(Path(inputs["output"]).read_text(encoding="utf-8"))
    assert registration["schema"] == "opennova.registered-retail-capture.v5"
    assert registration["tool"]["version"] == "4.0.0"
    assert registration["fixture_id"] == "fixture-retail"
    assert registration["mission"]["verified"] is True
    assert registration["mission"]["verification"] == [
        "engine_vfs_logical_lookup",
        "same_pid_onhook_launch_log",
    ]
    assert registration["capture"].items() >= {
        "mode": "hud_hidden",
        "retail_presentation": "game_composite_pre_retail_hud",
        "retail_gameplay_hud_visible": False,
        "onhook_overlay_present": False,
        "width": 1920,
        "height": 1200,
        "pre_overlay": True,
        "frame_correlated": True,
        "frame_serial": "42",
        "frame_qpc": "9001",
        "sha256": _sha256(Path(inputs["image"])),
    }.items()
    assert registration["capture"]["presentation_policy"] \
        == "retail_pre_hud_backbuffer_snapshot.v2"
    assert registration["comparison_presentation"] == {
        "contract": {
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
        },
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
                "bound_image_sha256": _sha256(Path(inputs["image"])),
                "telemetry_available": True,
            },
        }
    assert registration["retail_runtime_witness"] == {
        key: json.loads(Path(inputs["state"]).read_text(encoding="utf-8"))[key]
        for key in ("video_settings", "d3d_device", "presentation")
    }
    runtime_video = registration["retail_runtime_witness"]["video_settings"]
    assert len(runtime_video["available"]) == 21
    assert set(runtime_video["available"]) == set(RUNTIME_VIDEO_PROFILE)
    assert runtime_video["values"] \
        == RUNTIME_VIDEO_PROFILE | {"gamma": LIVE_FLOAT32_GAMMA}
    assert "windowed" not in runtime_video["values"]
    assert "video_res" not in runtime_video["values"]
    stage = json.loads(
        Path(inputs["retail_stage_manifest"]).read_text(encoding="utf-8")
    )
    assert registration["retail_presentation_stage"] == {
        "manifest_path": "retail-stage.json",
        "manifest_sha256": _sha256(Path(inputs["retail_stage_manifest"])),
        "schema": stage["schema"],
        "tool_version": stage["tool_version"],
        "profile": stage["profile"],
        "game_config": stage["game_config"],
        "weapon_profile": stage["weapon_profile"],
    }
    assert registration["build"]["opennova_source_commit"] == "a" * 40
    assert registration["build"]["retail_executable_sha256"] \
        == _sha256(Path(inputs["game_dir"]) / "Jointops.exe")
    assert registration["build"]["onhook_mcp_sha256"] \
        == _sha256(Path(inputs["game_dir"]) / "onhook-mcp.exe")
    assert registration["build"]["onhook_proxy_sha256"] \
        == _sha256(Path(inputs["game_dir"]) / "binkw32.dll")
    assert registration["build"]["onhook_forwarder_sha256"] \
        == _sha256(Path(inputs["game_dir"]) / "binkw32_.dll")
    archives = registration["retail_install"]["mounted_archives"]
    assert [row["path"] for row in archives] == [
        "expansion/revx02/revx02L.pff",
        "expansion/revx02/revx02.pff",
        "language.pff",
        "localres.pff",
        "resource.pff",
    ]
    assert all(len(row["sha256"]) == 64 for row in archives)
    assert registration["retail_install"]["version_marker"] == {
        "path": "expansion/revx02/revx02.bin",
        "sha256": _sha256(
            Path(inputs["game_dir"]) / "expansion" / "revx02" / "revx02.bin"
        ),
    }
    assert registration["raw_evidence"]["state_sha256"] \
        == _sha256(Path(inputs["state"]))
    assert registration["raw_evidence"]["onhook_log_sha256"] \
        == _sha256(Path(inputs["log"]))
    assert registration["raw_evidence"]["instance_status_sha256"] \
        == _sha256(Path(inputs["instance_status"]))
    assert registration["raw_evidence"]["fixture_result_sha256"] \
        == _sha256(Path(inputs["fixture_result"]))
    assert registration["raw_evidence"]["capture_result_sha256"] \
        == _sha256(Path(inputs["capture_result"]))
    assert registration["raw_evidence"]["bridge_version_major"] == 1
    assert registration["raw_evidence"]["bridge_version_minor"] == 4
    assert registration["raw_evidence"]["hook_version"] == "0.5.0"


def test_preflight_rejects_v3_capture_result_schema(tmp_path: Path) -> None:
    inputs = _inputs(tmp_path)
    capture_path = Path(inputs["capture_result"])
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    capture["schema"] = "opennova.render_capture_bundle.v3"
    capture_path.write_text(json.dumps(capture), encoding="utf-8")

    result = _run(inputs)

    assert result.returncode != 0
    assert "capture result identity is mismatched" in result.stderr
    assert not Path(inputs["output"]).exists()


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("bridge_version_major", 0),
        ("bridge_version_major", 2),
        ("bridge_version_major", "1"),
        ("bridge_version_minor", 3),
        ("bridge_version_minor", 5),
        ("bridge_version_minor", "4"),
        ("hook_version", "0.4.0"),
        ("hook_version", "0.5.1"),
        ("hook_version", ["0.5.0"]),
    ],
)
def test_preflight_requires_the_v4_capture_producer_version(
    tmp_path: Path, field: str, value: object,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["source"][field] = value
    _write_state_and_rebind_capture(inputs, state)

    result = _run(inputs)

    assert result.returncode != 0
    assert "capture producer version" in result.stderr
    assert not Path(inputs["output"]).exists()


@pytest.mark.parametrize(
    "field", ["bridge_version_major", "bridge_version_minor", "hook_version"]
)
def test_preflight_requires_every_v4_capture_producer_version_field(
    tmp_path: Path, field: str,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    del state["source"][field]
    _write_state_and_rebind_capture(inputs, state)

    result = _run(inputs)

    assert result.returncode != 0
    assert "capture producer version" in result.stderr
    assert not Path(inputs["output"]).exists()


@pytest.mark.parametrize(
    "boundary", ["world_labels", "pre_feed"]
)
def test_preflight_accepts_each_proven_pre_hud_boundary(
    tmp_path: Path, boundary: str,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["presentation"]["boundary"] = boundary
    _write_state_and_rebind_capture(inputs, state)

    result = _run(inputs)

    assert result.returncode == 0, result.stdout + result.stderr


def test_preflight_accepts_snapshot_cutpoint_at_target_qpc(tmp_path: Path) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["presentation"]["target_frame_qpc"] = state["frame_qpc"]
    _write_state_and_rebind_capture(inputs, state)

    result = _run(inputs)

    assert result.returncode == 0, result.stdout + result.stderr


def test_preflight_rejects_nonobject_raw_source_cleanly(tmp_path: Path) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["source"] = ["bridge", "1.4"]
    _write_state_and_rebind_capture(inputs, state)

    result = _run(inputs)

    assert result.returncode != 0
    assert "capture producer version" in result.stderr
    assert "Traceback" not in result.stderr


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("legacy_policy", "does not prove one pre-HUD snapshot"),
        ("legacy_fields", "pre-HUD snapshot presentation witness is malformed"),
        ("extra_field", "pre-HUD snapshot presentation witness is malformed"),
        ("snapshot_zero", "does not prove one pre-HUD snapshot"),
        ("snapshot_two", "does not prove one pre-HUD snapshot"),
        ("main_hud_fallback", "does not prove one pre-HUD snapshot"),
        ("unknown_boundary", "does not prove one pre-HUD snapshot"),
        ("non_string_boundary", "does not prove one pre-HUD snapshot"),
        ("after_hud", "does not prove one pre-HUD snapshot"),
        ("scene_end_zero", "D3D scene and ordinary retail UI restored"),
        ("scene_begin_two", "D3D scene and ordinary retail UI restored"),
        ("scene_not_restored", "D3D scene and ordinary retail UI restored"),
        ("ui_call_zero", "D3D scene and ordinary retail UI restored"),
        ("ui_call_two", "D3D scene and ordinary retail UI restored"),
        ("ui_modified", "D3D scene and ordinary retail UI restored"),
        ("armed_serial_gap", "exact next target frame"),
        ("target_serial_mismatch", "exact next target frame"),
        ("armed_qpc_at_cutpoint", "exact next target frame"),
        ("target_qpc_before_cutpoint", "exact next target frame"),
        ("numeric_armed_qpc", "positive decimal string"),
        ("numeric_root_qpc", "positive decimal string"),
        ("d3d_serial_mismatch", "pre-HUD cutpoint"),
        ("d3d_qpc_mismatch", "pre-HUD cutpoint"),
        ("numeric_d3d_qpc", "positive decimal string"),
    ],
)
def test_preflight_requires_exact_pre_hud_snapshot_proof(
    tmp_path: Path, mutation: str, message: str,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    presentation = state["presentation"]
    if mutation == "legacy_policy":
        presentation["policy"] = "retail_hud_suppressed_one_frame.v1"
    elif mutation == "legacy_fields":
        state["presentation"] = {
            "policy": "retail_hud_suppressed_one_frame.v1",
            "generation": 7,
            "target_frame_serial": "42",
            "target_frame_qpc": "9001",
            "hud_passes_suppressed": 1,
            "hud_restores_succeeded": 1,
            "before": {"hud_detail": 0},
            "effective": {"hud_detail": 3},
            "after": {"hud_detail": 0},
            "restored": True,
        }
    elif mutation == "extra_field":
        presentation["hud_passes_suppressed"] = 1
    elif mutation == "snapshot_zero":
        presentation["snapshot_count"] = 0
    elif mutation == "snapshot_two":
        presentation["snapshot_count"] = 2
    elif mutation == "main_hud_fallback":
        presentation["boundary"] = "main_hud_fallback"
    elif mutation == "unknown_boundary":
        presentation["boundary"] = "after_hud"
    elif mutation == "non_string_boundary":
        presentation["boundary"] = ["pre_feed"]
    elif mutation == "after_hud":
        presentation["before_retail_hud"] = False
    elif mutation == "scene_end_zero":
        presentation["d3d_scene_split"]["end_count"] = 0
    elif mutation == "scene_begin_two":
        presentation["d3d_scene_split"]["begin_count"] = 2
    elif mutation == "scene_not_restored":
        presentation["d3d_scene_split"]["restored"] = False
    elif mutation == "ui_call_zero":
        presentation["retail_ui"]["original_call_count"] = 0
    elif mutation == "ui_call_two":
        presentation["retail_ui"]["original_call_count"] = 2
    elif mutation == "ui_modified":
        presentation["retail_ui"]["executed_unmodified"] = False
    elif mutation == "armed_serial_gap":
        presentation["armed_frame_serial"] = "40"
    elif mutation == "target_serial_mismatch":
        presentation["target_frame_serial"] = "43"
    elif mutation == "armed_qpc_at_cutpoint":
        presentation["armed_frame_qpc"] = state["frame_qpc"]
    elif mutation == "target_qpc_before_cutpoint":
        presentation["target_frame_qpc"] = "9000"
    elif mutation == "numeric_armed_qpc":
        presentation["armed_frame_qpc"] = 8995
    elif mutation == "numeric_root_qpc":
        state["frame_qpc"] = 9001
    elif mutation == "d3d_serial_mismatch":
        state["d3d_device"]["frame_serial"] = "43"
    elif mutation == "d3d_qpc_mismatch":
        state["d3d_device"]["frame_qpc"] = "9002"
    elif mutation == "numeric_d3d_qpc":
        state["d3d_device"]["frame_qpc"] = 9001
    _write_state_and_rebind_capture(inputs, state)

    result = _run(inputs)

    assert result.returncode != 0
    assert message in result.stderr
    assert not Path(inputs["output"]).exists()


def test_registered_output_is_accepted_by_the_comparison_builder(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    result = _run(inputs)
    assert result.returncode == 0, result.stdout + result.stderr

    catalog = json.loads(Path(inputs["catalog"]).read_text(encoding="utf-8"))
    fixture = catalog["fixtures"][0]
    contract = catalog["comparison_contract"]
    opennova_image = tmp_path / "opennova.png"
    image = QImage(2000, 1200, QImage.Format.Format_RGB32)
    image.fill(QColor(20, 30, 40))
    assert image.save(str(opennova_image), "PNG")
    player_position = fixture["retail_player_bms"]["applied"]
    camera = fixture["camera_bms"]
    witness = {
        **contract,
        "observed_at": "after_pose_settle_before_fixture_freeze",
        "player_class": 9,
        "player_position_bms": player_position,
        "requested_player_pose_bms": {
            "position": player_position,
            "yaw_deg": camera["yaw_deg"],
            "pitch_deg": camera["pitch_deg"],
        },
        "hud_canvas_layer_visible": True,
        "viewmodel_canvas_layer_visible": True,
        "terrain_data_available": True,
        "terrain_node_visible": True,
    }
    state_path = tmp_path / "opennova.state.json"
    state_path.write_text(json.dumps({
        "capture": {
            "png_path": opennova_image.name,
            "png_sha256": _sha256(opennova_image),
        },
        "comparison_contract_witness": witness,
    }), encoding="utf-8")
    manifest_path = tmp_path / "opennova-manifest.json"
    manifest_path.write_text(json.dumps({
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
            "mode": "hud_hidden",
            "world_only": False,
            "viewmodel_hidden": False,
            "camera_bms": camera,
            "comparison_contract": contract,
            "comparison_contract_witness": witness,
        },
        "provenance": {
            "source_commit": inputs["source_commit"],
            "godot_executable_sha256": "b" * 64,
            "gdextension_sha256": "c" * 64,
        },
        "artifacts": [{
            "variant": "beauty",
            "png_path": opennova_image.name,
            "png_sha256": _sha256(opennova_image),
            "state_path": state_path.name,
            "state_sha256": _sha256(state_path),
            "width": 2000,
            "height": 1200,
        }],
    }), encoding="utf-8")

    validated = evidence_builder.validate_registered_inputs(
        Path(inputs["catalog"]),
        "fixture-retail",
        manifest_path,
        Path(inputs["output"]),
    )
    assert validated["capture_mode"] == "hud_hidden"
    assert validated["retail_stage_path"] \
        == Path(inputs["retail_stage_manifest"]).resolve()


def test_preflight_requires_explicit_visual_presentation_confirmation(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    result = _run(inputs, confirm_presentation=False)
    assert result.returncode != 0
    assert "visual confirmation" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_fixture_outside_hud_hidden_contract(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    catalog_path = Path(inputs["catalog"])
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    catalog["fixtures"][0]["capture_mode"] = "world_only"
    catalog.pop("catalog_sha256")
    canonical = json.dumps(
        catalog, sort_keys=True, separators=(",", ":")
    ).encode()
    catalog["catalog_sha256"] = hashlib.sha256(canonical).hexdigest()
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")

    result = _run(inputs)
    assert result.returncode != 0
    assert "hud_hidden" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_changed_presentation_contract(tmp_path: Path) -> None:
    inputs = _inputs(tmp_path)
    catalog_path = Path(inputs["catalog"])
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    catalog["comparison_contract"]["equipped_weapon"] = "WPN_M4AUTO"
    catalog.pop("catalog_sha256")
    canonical = json.dumps(
        catalog, sort_keys=True, separators=(",", ":")
    ).encode()
    catalog["catalog_sha256"] = hashlib.sha256(canonical).hexdigest()
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")

    result = _run(inputs)
    assert result.returncode != 0
    assert "comparison_contract" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_type_coerced_presentation_contract(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    catalog_path = Path(inputs["catalog"])
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    catalog["comparison_contract"]["gameplay_hud_visible"] = 0
    catalog.pop("catalog_sha256")
    canonical = json.dumps(
        catalog, sort_keys=True, separators=(",", ":")
    ).encode()
    catalog["catalog_sha256"] = hashlib.sha256(canonical).hexdigest()
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")

    result = _run(inputs)
    assert result.returncode != 0
    assert "comparison_contract" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_a_legacy_retail_video_profile(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    catalog_path = Path(inputs["catalog"])
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    catalog["retail_video_profile_contract"]["id"] = \
        "retail_reference_highest_supported_v1"
    catalog.pop("catalog_sha256")
    canonical = json.dumps(
        catalog, sort_keys=True, separators=(",", ":")
    ).encode()
    catalog["catalog_sha256"] = hashlib.sha256(canonical).hexdigest()
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")

    result = _run(inputs)

    assert result.returncode != 0
    assert "exhaustive highest-quality profile" in result.stderr
    assert not Path(inputs["output"]).exists()


@pytest.mark.parametrize(
    ("path", "value", "message"),
    [
        (("schema",), "opennova.retail-presentation-stage.v2", "schema/version"),
        (("tool_version",), "2.0.0", "schema/version"),
        (("game_config", "hud_detail", "effective"), 3, "hud_detail was unchanged"),
        (
            ("game_config", "preserved_values", "hw3d_guid"),
            "not-a-guid",
            "hw3d_guid is malformed",
        ),
        (("weapon_profile", "sha256"), "0" * 64, "weapon.sav identity"),
        (("weapon_profile", "slot"), False, "weapon.sav identity"),
        (("weapon_profile", "blue", "avatar_packed"), 0x0200, "slot-0 facts"),
        (
            ("weapon_profile", "blue_selected_kit_contains_wpn_m16burst"),
            False,
            "slot-0 facts",
        ),
    ],
)
def test_preflight_rejects_mismatched_retail_presentation_stage(
    tmp_path: Path,
    path: tuple[str, ...],
    value: object,
    message: str,
) -> None:
    inputs = _inputs(tmp_path)
    stage_path = Path(inputs["retail_stage_manifest"])
    stage = json.loads(stage_path.read_text(encoding="utf-8"))
    target = stage
    for key in path[:-1]:
        target = target[key]
    target[path[-1]] = value
    stage_path.write_text(json.dumps(stage), encoding="utf-8")

    result = _run(inputs)
    assert result.returncode != 0
    assert message in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_profile_contract_not_bound_to_catalog(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    catalog_path = Path(inputs["catalog"])
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    catalog["retail_profile_contract"]["blue"]["character_id"] = 0x0200
    catalog.pop("catalog_sha256")
    canonical = json.dumps(
        catalog, sort_keys=True, separators=(",", ":")
    ).encode()
    catalog["catalog_sha256"] = hashlib.sha256(canonical).hexdigest()
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")

    result = _run(inputs)
    assert result.returncode != 0
    assert "witnessed slot-0 profile" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_live_game_config_that_differs_from_stage(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    game_dir = Path(inputs["game_dir"])
    (game_dir / "game.cfg").write_text(
        "version = 29\nhud_detail = 0\n", encoding="ascii"
    )

    result = _run(inputs)
    assert result.returncode != 0
    assert "live game.cfg hash" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_live_weapon_profile_that_differs_from_stage(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    profile = Path(inputs["game_dir"]) / "expansion" / "revx02" / "weapon.sav"
    data = bytearray(profile.read_bytes())
    data[16] = 8
    profile.write_bytes(data)

    result = _run(inputs)
    assert result.returncode != 0
    assert "live weapon.sav hash" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_requires_stage_manifest_as_output_sibling(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    nested = tmp_path / "staging"
    nested.mkdir()
    stage_path = Path(inputs["retail_stage_manifest"])
    nested_stage = nested / stage_path.name
    stage_path.replace(nested_stage)
    inputs["retail_stage_manifest"] = nested_stage

    result = _run(inputs)
    assert result.returncode != 0
    assert "colocated sibling" in result.stderr
    assert not Path(inputs["output"]).exists()


@pytest.mark.parametrize("field,replacement", [
    ("path", "wrong.png"),
    ("state_path", "wrong.state.json"),
    ("frame_serial", "41"),
    ("frame_qpc", "9000"),
    ("file_bytes", 1),
    ("state_bytes", 1),
    ("mime_type", "image/jpeg"),
])
def test_preflight_rejects_capture_result_that_does_not_bind_the_raw_pair(
    tmp_path: Path,
    field: str,
    replacement: object,
) -> None:
    inputs = _inputs(tmp_path)
    path = Path(inputs["capture_result"])
    document = json.loads(path.read_text(encoding="utf-8"))
    document[field] = replacement
    path.write_text(json.dumps(document), encoding="utf-8")
    result = _run(inputs)
    assert result.returncode != 0


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("omit_profile_with_alias", "profile identity"),
        ("wrong_profile", "profile identity"),
        ("non_string_profile", "profile identity"),
        ("omit_fixture_with_alias", "fixture identity"),
        ("cross_fixture", "fixture identity"),
        ("non_string_fixture", "fixture identity"),
    ],
)
def test_preflight_requires_exact_capture_result_profile_and_fixture_bindings(
    tmp_path: Path,
    mutation: str,
    message: str,
) -> None:
    inputs = _inputs(tmp_path)
    path = Path(inputs["capture_result"])
    document = json.loads(path.read_text(encoding="utf-8"))
    if mutation == "omit_profile_with_alias":
        del document["profile_id"]
        document["profile"] = "retail_reference_highest_retail_selectable_v2"
    elif mutation == "wrong_profile":
        document["profile_id"] = "retail_reference_highest_supported_v1"
    elif mutation == "non_string_profile":
        document["profile_id"] = ["retail_reference_highest_retail_selectable_v2"]
    elif mutation == "omit_fixture_with_alias":
        del document["fixture_id"]
        document["fixture_name"] = "fixture-retail"
    elif mutation == "cross_fixture":
        document["fixture_id"] = "another-catalog-fixture"
    elif mutation == "non_string_fixture":
        document["fixture_id"] = 123
    path.write_text(json.dumps(document), encoding="utf-8")

    result = _run(inputs)

    assert result.returncode != 0
    assert message in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_capture_that_precedes_the_fixture_application(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    path = Path(inputs["fixture_result"])
    document = json.loads(path.read_text(encoding="utf-8"))
    document["frame_serial"] = "43"
    document["frame_qpc"] = "9002"
    path.write_text(json.dumps(document), encoding="utf-8")
    result = _run(inputs)
    assert result.returncode != 0


def test_preflight_rejects_capture_on_the_fixture_application_frame(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    path = Path(inputs["fixture_result"])
    document = json.loads(path.read_text(encoding="utf-8"))
    document["frame_serial"] = "42"
    document["frame_qpc"] = "9001"
    path.write_text(json.dumps(document), encoding="utf-8")

    result = _run(inputs)

    assert result.returncode != 0
    assert "must follow the exact fixture application" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_capture_too_many_frames_after_application(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["frame_serial"] = "200"
    state["frame_qpc"] = "9999"
    state["d3d_device"]["frame_serial"] = "200"
    state["d3d_device"]["frame_qpc"] = "9999"
    state["presentation"]["armed_frame_serial"] = "199"
    state["presentation"]["armed_frame_qpc"] = "9990"
    state["presentation"]["target_frame_serial"] = "200"
    state["presentation"]["target_frame_qpc"] = "10005"
    state_path.write_text(json.dumps(state), encoding="utf-8")
    capture_path = Path(inputs["capture_result"])
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    capture["frame_serial"] = "200"
    capture["frame_qpc"] = "9999"
    capture["state_bytes"] = state_path.stat().st_size
    capture_path.write_text(json.dumps(capture), encoding="utf-8")
    result = _run(inputs)
    assert result.returncode != 0
    assert "more than 120 frames" in result.stderr


def test_preflight_rejects_process_start_that_disagrees_with_instance_token(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["source"]["process_start_time"] = "1"
    state_path.write_text(json.dumps(state), encoding="utf-8")
    capture_path = Path(inputs["capture_result"])
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    capture["state_bytes"] = state_path.stat().st_size
    capture_path.write_text(json.dumps(capture), encoding="utf-8")
    result = _run(inputs)
    assert result.returncode != 0
    assert "process start" in result.stderr


def test_preflight_rejects_legacy_uncorrelated_sidecars(tmp_path: Path) -> None:
    inputs = _inputs(tmp_path)
    instance_status_path = Path(inputs["instance_status"])
    status = json.loads(instance_status_path.read_text(encoding="utf-8"))
    status["instances"][0]["capture_bundle_supported"] = False
    instance_status_path.write_text(json.dumps(status), encoding="utf-8")
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["source"].update({
        "pre_overlay": False,
        "frame_correlated": False,
    })
    state["fixture_context"]["verified"] = False
    state["frame_serial"] = "0"
    state["frame_qpc"] = "0"
    state_path.write_text(json.dumps(state), encoding="utf-8")

    result = _run(inputs)
    assert result.returncode != 0
    assert "CAP_CAPTURE_BUNDLE" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_launch_log_not_bound_to_exact_capture_instance(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    Path(inputs["log"]).write_text(
        "Debug bridge: listening on \\\\.\\pipe\\opennova.onhook.v1.9999.ABC\n"
        "Debug LAN host: starting mission 'TEST.bms'\n",
        encoding="utf-8",
    )
    result = _run(inputs)
    assert result.returncode != 0
    assert "exact capture instance" in result.stderr


def test_preflight_rejects_nonexact_fixture_application(tmp_path: Path) -> None:
    inputs = _inputs(tmp_path)
    result_path = Path(inputs["fixture_result"])
    fixture_result = json.loads(result_path.read_text(encoding="utf-8"))
    fixture_result["exact"] = False
    result_path.write_text(json.dumps(fixture_result), encoding="utf-8")
    result = _run(inputs)
    assert result.returncode != 0
    assert "fixture application was not exact" in result.stderr


@pytest.mark.parametrize(
    "field",
    [
        "vertical_fov_degrees",
        "time_of_day_seconds",
        "mission_file",
        "mission_sha256",
    ],
)
def test_preflight_requires_every_catalog_fixture_application_binding(
    tmp_path: Path,
    field: str,
) -> None:
    inputs = _inputs(tmp_path)
    result_path = Path(inputs["fixture_result"])
    fixture_result = json.loads(result_path.read_text(encoding="utf-8"))
    del fixture_result["fixture"][field]
    result_path.write_text(json.dumps(fixture_result), encoding="utf-8")

    result = _run(inputs)

    assert result.returncode != 0
    assert "required catalog bindings" in result.stderr
    assert not Path(inputs["output"]).exists()


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("vertical_fov_degrees", 53.44680000001, "vertical FOV"),
        ("vertical_fov_degrees", "53.4468", "vertical FOV"),
        ("time_of_day_seconds", 54001, "time of day"),
        ("time_of_day_seconds", 54000.0, "time of day"),
        ("mission_file", "OTHER.bms", "mission identity"),
        ("mission_file", 123, "mission identity"),
        ("mission_sha256", "0" * 64, "mission identity"),
        ("mission_sha256", 123, "mission identity"),
    ],
)
def test_preflight_rejects_fixture_application_binding_type_or_drift(
    tmp_path: Path,
    field: str,
    value: object,
    message: str,
) -> None:
    inputs = _inputs(tmp_path)
    result_path = Path(inputs["fixture_result"])
    fixture_result = json.loads(result_path.read_text(encoding="utf-8"))
    fixture_result["fixture"][field] = value
    result_path.write_text(json.dumps(fixture_result), encoding="utf-8")

    result = _run(inputs)

    assert result.returncode != 0
    assert message in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_mission_bytes_that_do_not_match_catalog(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    _write_pff(
        Path(inputs["game_dir"]) / "resource.pff",
        [("TEST.bms", b"different mission bytes")],
    )
    result = _run(inputs)
    assert result.returncode != 0
    assert "engine VFS mission hash mismatch" in result.stderr


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("projection", "projection matrix"),
        ("position", "view matrix camera"),
        ("yaw", "observed yaw"),
        ("executable", "executable identity"),
    ],
)
def test_preflight_rejects_mismatched_observed_pose_projection_and_build(
    tmp_path: Path,
    mutation: str,
    message: str,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    if mutation == "projection":
        state["render_state"]["projection_matrix_row_major"][0] = 0.5
    elif mutation == "position":
        state["render_state"]["view_matrix_row_major"][12] = 1.0
    elif mutation == "yaw":
        state["observed_state"]["heading_bam"] = 0
    elif mutation == "executable":
        state["source"]["executable"] = "other.exe"
    state_path.write_text(json.dumps(state), encoding="utf-8")
    capture_result_path = Path(inputs["capture_result"])
    capture_result = json.loads(capture_result_path.read_text(encoding="utf-8"))
    capture_result["state_bytes"] = state_path.stat().st_size
    capture_result_path.write_text(json.dumps(capture_result), encoding="utf-8")

    result = _run(inputs)
    assert result.returncode != 0
    assert message in result.stderr


@pytest.mark.parametrize(
    ("input_key", "file_name", "message"),
    [
        ("retail_executable", "Jointops.exe", "retail executable must be"),
        ("onhook_proxy", "binkw32.dll", "onHook proxy binary must be"),
        ("onhook_forwarder", "binkw32_.dll", "onHook proxy forwarder must be"),
    ],
)
def test_preflight_rejects_same_named_retail_binaries_outside_game_dir(
    tmp_path: Path,
    input_key: str,
    file_name: str,
    message: str,
) -> None:
    inputs = _inputs(tmp_path)
    outsider = tmp_path / "unrelated-build" / file_name
    outsider.parent.mkdir()
    outsider.write_bytes(b"unrelated same-named binary")
    inputs[input_key] = outsider

    result = _run(inputs)

    assert result.returncode != 0
    assert message in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_rejects_a_game_binary_symlink_that_escapes_game_dir(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    game_executable = Path(inputs["game_dir"]) / "Jointops.exe"
    outsider = tmp_path / "unrelated-build" / "Jointops.exe"
    outsider.parent.mkdir()
    outsider.write_bytes(b"unrelated symlink target")
    game_executable.unlink()
    try:
        game_executable.symlink_to(outsider)
    except OSError as exc:
        pytest.skip(f"file symlinks are unavailable: {exc}")

    result = _run(inputs)

    assert result.returncode != 0
    assert "retail executable must be" in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_accepts_an_explicit_external_onhook_mcp_build(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    external_mcp = tmp_path / "retained-build" / "onhook-mcp.exe"
    external_mcp.parent.mkdir()
    external_mcp.write_bytes(b"explicit retained onhook mcp")
    inputs["onhook_mcp"] = external_mcp

    result = _run(inputs)

    assert result.returncode == 0, result.stdout + result.stderr
    registration = json.loads(
        Path(inputs["output"]).read_text(encoding="utf-8")
    )
    assert registration["build"]["onhook_mcp_sha256"] \
        == _sha256(external_mcp)


def test_preflight_rejects_a_runtime_witness_bound_to_another_image(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["image"]["sha256"] = "0" * 64
    state_path.write_text(json.dumps(state), encoding="utf-8")
    capture_path = Path(inputs["capture_result"])
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    capture["state_bytes"] = state_path.stat().st_size
    capture_path.write_text(json.dumps(capture), encoding="utf-8")

    result = _run(inputs)

    assert result.returncode != 0
    assert "sidecar image hash does not bind the raw PNG" in result.stderr
    assert not Path(inputs["output"]).exists()


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("lower_quality", "below the highest-quality profile"),
        ("missing_quality", "exact required key set"),
        ("derived_runtime_global", "exact required key set"),
        ("type_coerced_quality", "type-coerced"),
        ("nearby_gamma", "below the highest-quality profile"),
        ("nonfinite_gamma", "below the highest-quality profile"),
        ("wrong_gamma", "below the highest-quality profile"),
        ("wrong_adapter", "does not match staged device identity"),
        ("wrong_adapter_guid", "does not match staged device identity"),
        ("token_3_none", "below the highest-quality profile"),
        ("forged_8x", "highest-retail-selectable 4x MSAA"),
        ("lower_token_1", "below the highest-quality profile"),
        ("retail_selectable_subset", "highest-retail-selectable 4x MSAA"),
        ("hardware_usable_mask", "highest-retail-selectable 4x MSAA"),
        ("retail_highest_mismatch", "highest-retail-selectable 4x MSAA"),
        ("hardware_highest_mismatch", "highest-retail-selectable 4x MSAA"),
        ("retail_source", "highest-retail-selectable 4x MSAA"),
        ("hardware_source", "highest-retail-selectable 4x MSAA"),
        ("wrong_token_semantics", "highest-retail-selectable 4x MSAA"),
        ("nonzero_4x_quality", "highest-retail-selectable 4x MSAA"),
        ("selected_not_highest", "highest-retail-selectable 4x MSAA"),
        ("legacy_raw_schema", "unsupported raw retail capture bundle schema"),
        ("legacy_profile", "wrong profile or provenance"),
        ("failed_scene_restore", "D3D scene and ordinary retail UI restored"),
    ],
)
def test_preflight_rejects_incomplete_or_lower_runtime_video_witnesses(
    tmp_path: Path,
    mutation: str,
    message: str,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    if mutation == "lower_quality":
        state["video_settings"]["values"]["object_texdetail"] = 1
    elif mutation == "missing_quality":
        state["video_settings"]["available"].remove("object_texdetail")
        del state["video_settings"]["values"]["object_texdetail"]
    elif mutation == "derived_runtime_global":
        state["video_settings"]["available"].append("windowed")
        state["video_settings"]["values"]["windowed"] = 0
    elif mutation == "type_coerced_quality":
        state["video_settings"]["values"]["antialias_mode"] = 2.0
    elif mutation == "nearby_gamma":
        state["video_settings"]["values"]["gamma"] = 0.8000000715255737
    elif mutation == "nonfinite_gamma":
        state["video_settings"]["values"]["gamma"] = float("nan")
    elif mutation == "wrong_gamma":
        state["video_settings"]["values"]["gamma"] = 0.7
    elif mutation == "wrong_adapter":
        state["d3d_device"]["adapter"]["name"] = "Other adapter"
    elif mutation == "wrong_adapter_guid":
        state["d3d_device"]["adapter"]["guid"] = \
            "D7B71EE2-55C1-11CF-6374-0273A5C2ED34"
    elif mutation == "token_3_none":
        state["video_settings"]["values"]["antialias_mode"] = 3
        state["d3d_device"]["multisample"].update({
            "mode_token": 3,
            "type": "D3DMULTISAMPLE_NONE",
            "samples": 1,
        })
    elif mutation == "forged_8x":
        state["d3d_device"]["multisample"].update({
            "type": "D3DMULTISAMPLE_8_SAMPLES",
            "samples": 8,
        })
    elif mutation == "lower_token_1":
        state["video_settings"]["values"]["antialias_mode"] = 1
        state["d3d_device"]["multisample"].update({
            "mode_token": 1,
            "type": "D3DMULTISAMPLE_2_SAMPLES",
            "samples": 2,
        })
    elif mutation == "retail_selectable_subset":
        state["d3d_device"]["multisample"]["retail_selectable"][
            "maskable_samples"
        ] = [4]
    elif mutation == "hardware_usable_mask":
        state["d3d_device"]["multisample"]["hardware_usable"][
            "maskable_samples"
        ] = [2, 4]
    elif mutation == "retail_highest_mismatch":
        state["d3d_device"]["multisample"]["retail_selectable"][
            "highest_samples"
        ] = 8
    elif mutation == "hardware_highest_mismatch":
        state["d3d_device"]["multisample"]["hardware_usable"][
            "highest_samples"
        ] = 4
    elif mutation == "retail_source":
        state["d3d_device"]["multisample"]["retail_selectable"][
            "source"
        ] = "game_cfg_token"
    elif mutation == "hardware_source":
        state["d3d_device"]["multisample"]["hardware_usable"][
            "source"
        ] = "color_format_only"
    elif mutation == "wrong_token_semantics":
        state["d3d_device"]["multisample"]["token_semantics"] = "sample_count"
    elif mutation == "nonzero_4x_quality":
        state["d3d_device"]["multisample"]["quality"] = 1
    elif mutation == "selected_not_highest":
        state["d3d_device"]["multisample"]["retail_selectable"][
            "is_selected_highest"
        ] = False
    elif mutation == "legacy_raw_schema":
        state["schema"] = "opennova.render_capture_bundle.v3"
    elif mutation == "legacy_profile":
        state["video_settings"]["profile_id"] = \
            "retail_reference_highest_supported_v1"
    elif mutation == "failed_scene_restore":
        state["presentation"]["d3d_scene_split"]["restored"] = False
    state_path.write_text(json.dumps(state), encoding="utf-8")
    capture_path = Path(inputs["capture_result"])
    capture = json.loads(capture_path.read_text(encoding="utf-8"))
    capture["state_bytes"] = state_path.stat().st_size
    capture_path.write_text(json.dumps(capture), encoding="utf-8")

    result = _run(inputs)

    assert result.returncode != 0
    assert message in result.stderr
    assert not Path(inputs["output"]).exists()


def test_preflight_retains_settled_player_pose_while_camera_stays_authoritative(
    tmp_path: Path,
) -> None:
    inputs = _inputs(tmp_path)
    state_path = Path(inputs["state"])
    state = json.loads(state_path.read_text(encoding="utf-8"))
    state["observed_state"]["position_bms"]["z"] = 30.75
    state_path.write_text(json.dumps(state), encoding="utf-8")
    capture_result_path = Path(inputs["capture_result"])
    capture_result = json.loads(capture_result_path.read_text(encoding="utf-8"))
    capture_result["state_bytes"] = state_path.stat().st_size
    capture_result_path.write_text(json.dumps(capture_result), encoding="utf-8")

    result = _run(inputs)
    assert result.returncode == 0, result.stdout + result.stderr
    registration = json.loads(Path(inputs["output"]).read_text(encoding="utf-8"))
    assert registration["render_state"]["observed_player_bms"] == [
        10.0, 20.0, 30.75,
    ]
    assert registration["camera_bms"]["position"] == pytest.approx(
        [0.0, 0.0, 0.0], abs=1.0e-6
    )


def test_preflight_output_is_create_new(tmp_path: Path) -> None:
    inputs = _inputs(tmp_path)
    first = _run(inputs)
    assert first.returncode == 0, first.stdout + first.stderr
    digest = _sha256(Path(inputs["output"]))
    second = _run(inputs)
    assert second.returncode != 0
    assert "refusing to overwrite" in second.stderr
    assert _sha256(Path(inputs["output"])) == digest
