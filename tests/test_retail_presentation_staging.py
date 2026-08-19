import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

import pytest

from scripts.render import stage_retail_presentation as retail_stage


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "render" / "stage_retail_presentation.py"

HEADER_BYTES = 16
RECORD_BYTES = 0x1080C
SIDE_BYTES = 0x8006
KIT_PAGE_BYTES = 2048
APPROVED_WEAPON_SAV_SHA256 = (
    "f4907820a58505a6988f140a27638dae7dbaaea2cc446642f0bc44c868905623"
)
RESTORE_TOKEN_SUFFIX = ".restore-token.local.json"

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
PRESERVED_DISPLAY_VALUES = {
    "hw3d_deviceno": 0,
    "hw3d_name": "AMD Radeon(TM) Graphics",
    "hw3d_guid": "D7B71EE2-55C1-11CF-63740273A5C2ED35",
    "enable_keyboardtips": 1,
    "enable_gameplaytips": 1,
}
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


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _catalog_sha256() -> str:
    return hashlib.sha256(json.dumps({
        "id": "joint-operations-revx02-video-options-v1",
        "policies": VIDEO_OPTION_POLICIES,
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()


def _approved_weapon_sav(path: Path) -> None:
    data = bytearray(HEADER_BYTES + 5 * RECORD_BYTES)
    data[:8] = b"FPBC0211"
    slot0 = HEADER_BYTES
    blue = slot0
    red = slot0 + SIDE_BYTES

    data[blue] = 9
    data[blue + 1] = 2
    data[blue + 2] = 0
    struct.pack_into("<H", data, blue + 4, 0x0402)

    data[red] = 9
    data[red + 1] = 7
    data[red + 2] = 0
    struct.pack_into("<H", data, red + 4, 0x8207)

    class9_page = blue + 6 + 4 * KIT_PAGE_BYTES
    loadout = b"WPN_M16BURST\0-1\0-1\0-1\0\0"
    data[class9_page:class9_page + len(loadout)] = loadout
    path.write_bytes(data)


def _game_config(
    *,
    hud_detail: int = 0,
    overrides: dict[str, int | float | str] | None = None,
    extra: bytes = b"",
) -> bytes:
    values = HIGHEST_VIDEO_PROFILE | (overrides or {})
    visual_keys = {"NoBlood", "NoCasings", "NoSmoke", "no_anim"}
    return (
        b"version = 29\r\n// DISPLAY\r\n"
        + b"hw3d_deviceno = 0\r\n"
        + b'hw3d_name = "AMD Radeon(TM) Graphics"\r\n'
        + b'hw3d_guid = "D7B71EE2-55C1-11CF-63740273A5C2ED35"\r\n'
        + b"".join(
            f"{key} = {value}\r\n".encode("ascii")
            for key, value in values.items()
            if key not in visual_keys
        )
        + b"enable_keyboardtips = 1\r\n"
        + b"enable_gameplaytips = 1\r\n"
        + b"// AUDIO\r\n"
        + b"".join(
            f"{key} = {values[key]}\r\n".encode("ascii")
            for key in ("NoBlood", "NoCasings", "NoSmoke")
        )
        + b"// CONTROLS\r\n"
        + f"hud_detail = {hud_detail}\r\n".encode("ascii")
        + f"no_anim = {values['no_anim']}\r\n".encode("ascii")
        + extra
    )


def test_stage_retail_reference_applies_the_exhaustive_highest_video_profile(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(retail_stage, "_require_retail_stopped", lambda: None)
    game_cfg = tmp_path / "game.cfg"
    original_values = {
        **HIGHEST_VIDEO_PROFILE,
        "terrain_polydetail": 0,
        "terrain_texdetail": 1,
        "object_polydetail": 2,
        "object_texdetail": 1,
        "water_quality": 1,
        "shadow_quality": 0,
        "particle_density": 0,
        "antialias_mode": 0,
        "texfilter_level": 0,
        "fbeffects_level": 0,
        "shader_usage_level": 0,
        "texcompression_level": 0,
        "NoBlood": 1,
        "NoCasings": 1,
        "NoSmoke": 1,
        "no_anim": 1,
    }
    original = _game_config(hud_detail=2, overrides=original_values)
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-reference.json"

    returned = retail_stage.stage_retail_reference(
        game_cfg=game_cfg,
        weapon_sav=weapon_sav,
        backup=backup,
        manifest=manifest,
        expected_weapon_sav_sha256=_sha256(weapon_sav),
    )

    document = json.loads(manifest.read_text(encoding="utf-8"))
    assert returned == document
    assert document["schema"] == "opennova.retail-presentation-stage.v3"
    assert document["profile"] == {
        "id": "retail_reference_highest_retail_selectable_v2",
        "video_option_catalog_id": "joint-operations-revx02-video-options-v1",
        "video_option_catalog_sha256": _catalog_sha256(),
        "required_values": HIGHEST_VIDEO_PROFILE,
    }
    assert document["game_config"]["original_values"] == original_values
    assert document["game_config"]["effective_values"] == HIGHEST_VIDEO_PROFILE
    assert document["game_config"]["changed_keys"] == [
        key for key in HIGHEST_VIDEO_PROFILE
        if original_values[key] != HIGHEST_VIDEO_PROFILE[key]
    ]
    assert b"hud_detail = 2" in game_cfg.read_bytes()
    assert backup.read_bytes() == original


def test_stage_retail_reference_records_and_preserves_display_identity_and_tips(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = _game_config(
        hud_detail=1, overrides={"object_texdetail": 1}
    )
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "game.cfg.original"
    manifest = tmp_path / "retail-reference.json"

    result = subprocess.run(
        [
            sys.executable, str(SCRIPT), "stage",
            "--game-cfg", str(game_cfg),
            "--weapon-sav", str(weapon_sav),
            "--backup", str(backup),
            "--manifest", str(manifest),
            "--expected-weapon-sav-sha256", _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    document = json.loads(manifest.read_text(encoding="utf-8"))
    assert document["game_config"]["preserved_values"] == (
        PRESERVED_DISPLAY_VALUES | {"hud_detail": 1}
    )
    assert document["game_config"]["changed_keys"] == ["object_texdetail"]
    assert document["game_config"]["original_values"]["antialias_mode"] == 2
    assert document["game_config"]["effective_values"]["antialias_mode"] == 2
    assert game_cfg.read_bytes() == original.replace(
        b"object_texdetail = 1", b"object_texdetail = 3"
    )


def test_stage_retail_reference_binds_the_exact_video_option_catalog(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    game_cfg.write_bytes(_game_config())
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    manifest = tmp_path / "retail-reference.json"
    result = subprocess.run(
        [
            sys.executable, str(SCRIPT), "stage",
            "--game-cfg", str(game_cfg),
            "--weapon-sav", str(weapon_sav),
            "--backup", str(tmp_path / "game.cfg.original"),
            "--manifest", str(manifest),
            "--expected-weapon-sav-sha256", _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    profile = json.loads(manifest.read_text(encoding="utf-8"))["profile"]
    catalog_payload = {
        "id": "joint-operations-revx02-video-options-v1",
        "policies": VIDEO_OPTION_POLICIES,
    }
    expected_hash = hashlib.sha256(json.dumps(
        catalog_payload, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()
    assert profile["video_option_catalog_sha256"] == expected_hash


@pytest.mark.parametrize(
    ("mutate", "message"),
    [
        (
            lambda config: config.replace(b"object_texdetail = 3\r\n", b""),
            "must contain object_texdetail exactly once (found 0)",
        ),
        (
            lambda config: config + b"object_texdetail = 1\r\n",
            "must contain object_texdetail exactly once (found 2)",
        ),
        (
            lambda config: config.replace(
                b"terrain_polydetail = 3", b"terrain_polydetail = maximum"
            ),
            "terrain_polydetail is not an integer",
        ),
        (
            lambda config: config.replace(
                b"particle_density = 2", b"particle_density = 3"
            ),
            "particle_density must be in the retail range 0..2",
        ),
    ],
)
def test_stage_retail_reference_rejects_unclassifiable_video_settings(
    tmp_path: Path,
    mutate: object,
    message: str,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = mutate(_game_config())  # type: ignore[operator]
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "game.cfg.original"
    manifest = tmp_path / "retail-reference.json"

    result = subprocess.run(
        [
            sys.executable, str(SCRIPT), "stage",
            "--game-cfg", str(game_cfg),
            "--weapon-sav", str(weapon_sav),
            "--backup", str(backup),
            "--manifest", str(manifest),
            "--expected-weapon-sav-sha256", _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 1
    assert message in result.stderr
    assert game_cfg.read_bytes() == original
    assert not backup.exists()
    assert not manifest.exists()


def test_stage_retail_reference_rejects_a_new_unclassified_display_option(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = _game_config().replace(
        b"// AUDIO\r\n",
        b"future_video_quality = 3\r\n// AUDIO\r\n",
    )
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    result = subprocess.run(
        [
            sys.executable, str(SCRIPT), "stage",
            "--game-cfg", str(game_cfg),
            "--weapon-sav", str(weapon_sav),
            "--backup", str(tmp_path / "game.cfg.original"),
            "--manifest", str(tmp_path / "retail-reference.json"),
            "--expected-weapon-sav-sha256", _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 1
    assert "unclassified DISPLAY option(s): future_video_quality" in result.stderr
    assert game_cfg.read_bytes() == original


def test_stage_preserves_config_bytes_and_emits_sanitized_profile_manifest(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = _game_config(
        overrides={"texfilter_level": 0},
        extra=b"mouse_sensitivity = 0.75\r\n",
    ).replace(
        b"hud_detail = 0\r\n",
        b"  hud_detail = 0 ; ordinary gameplay\r\n",
    )
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-presentation.json"

    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "stage",
            "--game-cfg",
            str(game_cfg),
            "--weapon-sav",
            str(weapon_sav),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
            "--expected-weapon-sav-sha256",
            _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    assert backup.read_bytes() == original
    effective = original.replace(b"texfilter_level = 0", b"texfilter_level = 3")
    assert game_cfg.read_bytes() == effective

    document = json.loads(manifest.read_text(encoding="utf-8"))
    assert document["schema"] == "opennova.retail-presentation-stage.v3"
    assert document["tool_version"] == "3.0.0"
    assert document["game_config"] == {
        "file_name": "game.cfg",
        "original_sha256": hashlib.sha256(original).hexdigest(),
        "effective_sha256": hashlib.sha256(effective).hexdigest(),
        "original_values": HIGHEST_VIDEO_PROFILE | {"texfilter_level": 0},
        "effective_values": HIGHEST_VIDEO_PROFILE,
        "changed_keys": ["texfilter_level"],
        "preserved_values": PRESERVED_DISPLAY_VALUES | {"hud_detail": 0},
        "hud_detail": {"original": 0, "effective": 0, "unchanged": True},
    }
    assert document["weapon_profile"]["sha256"] == _sha256(weapon_sav)
    serialized = manifest.read_text(encoding="utf-8")
    assert str(game_cfg) not in serialized
    assert original.decode("ascii") not in serialized
    restore_token = manifest.with_name(manifest.name + RESTORE_TOKEN_SUFFIX)
    assert json.loads(restore_token.read_text(encoding="utf-8")) == {
        "schema": "opennova.retail-presentation-restore-token.v3",
        "tool_version": "3.0.0",
        "game_config_resolved_target": str(game_cfg.resolve()),
        "backup_sha256": hashlib.sha256(original).hexdigest(),
        "stage_manifest_sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
    }


def test_restore_reinstates_the_original_bytes_and_verifies_the_hash(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = _game_config(hud_detail=1, extra=b"volume = 0.5\r\n")
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-presentation.json"
    stage = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "stage",
            "--game-cfg",
            str(game_cfg),
            "--weapon-sav",
            str(weapon_sav),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
            "--expected-weapon-sav-sha256",
            _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )
    assert stage.returncode == 0, stage.stderr
    manifest_before_restore = manifest.read_bytes()

    # Retail may save config state as it exits. Restore owns returning the file
    # to the exact pre-capture bytes, not preserving capture-session writes.
    game_cfg.write_bytes(game_cfg.read_bytes() + b"capture_runtime_write = 1\r\n")
    receipt = retail_stage.restore_retail_reference(
        game_cfg=game_cfg,
        backup=backup,
        manifest=manifest,
    )

    assert game_cfg.read_bytes() == original
    assert manifest.read_bytes() == manifest_before_restore
    assert receipt == {
        "restored_sha256": hashlib.sha256(original).hexdigest(),
        "status": "restored",
    }


def test_stage_rejects_an_invalid_retail_hud_detail_without_writing(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = _game_config(hud_detail=7)
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-presentation.json"

    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "stage",
            "--game-cfg",
            str(game_cfg),
            "--weapon-sav",
            str(weapon_sav),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
            "--expected-weapon-sav-sha256",
            _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 1
    assert "hud_detail must be in the retail range 0..3" in result.stderr
    assert game_cfg.read_bytes() == original
    assert not backup.exists()
    assert not manifest.exists()


@pytest.mark.skipif(sys.platform != "win32", reason="retail process guard is Windows-only")
def test_stage_refuses_to_write_while_a_jointops_process_is_running(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = _game_config()
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-presentation.json"

    system_root = Path(os.environ["SystemRoot"])
    harmless_executable = tmp_path / "Jointops.exe"
    shutil.copy2(system_root / "System32" / "ping.exe", harmless_executable)
    fake_retail = subprocess.Popen(
        [str(harmless_executable), "-n", "30", "127.0.0.1"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        for _ in range(20):
            if fake_retail.poll() is None:
                break
            time.sleep(0.05)
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "stage",
                "--game-cfg",
                str(game_cfg),
                "--weapon-sav",
                str(weapon_sav),
                "--backup",
                str(backup),
                "--manifest",
                str(manifest),
                "--expected-weapon-sav-sha256",
                _sha256(weapon_sav),
            ],
            capture_output=True,
            text=True,
        )
    finally:
        fake_retail.terminate()
        try:
            fake_retail.wait(timeout=5)
        except subprocess.TimeoutExpired:
            fake_retail.kill()
            fake_retail.wait(timeout=5)

    assert result.returncode == 1
    assert "retail process is running (jointops.exe)" in result.stderr
    assert game_cfg.read_bytes() == original
    assert not backup.exists()
    assert not manifest.exists()


def test_stage_defaults_to_the_approved_retail_weapon_profile_hash(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = _game_config()
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-presentation.json"

    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "stage",
            "--game-cfg",
            str(game_cfg),
            "--weapon-sav",
            str(weapon_sav),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 1
    assert f"expected={APPROVED_WEAPON_SAV_SHA256}" in result.stderr
    assert game_cfg.read_bytes() == original
    assert not backup.exists()
    assert not manifest.exists()


def test_stage_rejects_a_wrong_character_profile_before_writing(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    original = _game_config()
    game_cfg.write_bytes(original)
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    wrong_profile = bytearray(weapon_sav.read_bytes())
    struct.pack_into("<H", wrong_profile, HEADER_BYTES + 4, 0x0200)
    weapon_sav.write_bytes(wrong_profile)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-presentation.json"

    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "stage",
            "--game-cfg",
            str(game_cfg),
            "--weapon-sav",
            str(weapon_sav),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
            "--expected-weapon-sav-sha256",
            _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 1
    assert "slot 0 does not match the approved retail character profile" in result.stderr
    assert game_cfg.read_bytes() == original
    assert not backup.exists()
    assert not manifest.exists()


def test_restore_rejects_a_corrupt_backup_without_touching_the_staged_config(
    tmp_path: Path,
) -> None:
    game_cfg = tmp_path / "game.cfg"
    game_cfg.write_bytes(_game_config())
    weapon_sav = tmp_path / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-presentation.json"
    stage = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "stage",
            "--game-cfg",
            str(game_cfg),
            "--weapon-sav",
            str(weapon_sav),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
            "--expected-weapon-sav-sha256",
            _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )
    assert stage.returncode == 0, stage.stderr
    staged = game_cfg.read_bytes()
    backup.write_bytes(b"not the original config")

    restore = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "restore",
            "--game-cfg",
            str(game_cfg),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
        ],
        capture_output=True,
        text=True,
    )

    assert restore.returncode == 1
    assert "backup SHA-256 does not match" in restore.stderr
    assert game_cfg.read_bytes() == staged


def test_restore_refuses_to_apply_one_installs_backup_to_another_install(
    tmp_path: Path,
) -> None:
    install_a = tmp_path / "retail-a"
    install_b = tmp_path / "retail-b"
    install_a.mkdir()
    install_b.mkdir()
    game_cfg_a = install_a / "game.cfg"
    game_cfg_b = install_b / "game.cfg"
    original_a = _game_config(extra=b"install = A\r\n")
    original_b = _game_config(hud_detail=2, extra=b"install = B\r\n")
    game_cfg_a.write_bytes(original_a)
    game_cfg_b.write_bytes(original_b)
    weapon_sav = install_a / "weapon.sav"
    _approved_weapon_sav(weapon_sav)
    backup = tmp_path / "capture-state" / "game.cfg.original"
    manifest = tmp_path / "capture-state" / "retail-presentation.json"
    stage = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "stage",
            "--game-cfg",
            str(game_cfg_a),
            "--weapon-sav",
            str(weapon_sav),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
            "--expected-weapon-sav-sha256",
            _sha256(weapon_sav),
        ],
        capture_output=True,
        text=True,
    )
    assert stage.returncode == 0, stage.stderr
    staged_a = game_cfg_a.read_bytes()

    restore = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "restore",
            "--game-cfg",
            str(game_cfg_b),
            "--backup",
            str(backup),
            "--manifest",
            str(manifest),
        ],
        capture_output=True,
        text=True,
    )

    assert restore.returncode == 1
    assert "restore token target does not match --game-cfg" in restore.stderr
    assert game_cfg_b.read_bytes() == original_b
    assert game_cfg_a.read_bytes() == staged_a


def test_local_restore_tokens_are_ignored_inside_publishable_trees() -> None:
    candidate = (
        ROOT
        / "screenshots"
        / "parity"
        / "accidental.retail-presentation.json.restore-token.local.json"
    )
    result = subprocess.run(
        ["git", "check-ignore", "--quiet", "--no-index", str(candidate)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
