"""Safely stage and restore retail presentation settings for parity captures.

The staging operation is intentionally separate from process launch. It refuses
to touch ``game.cfg`` while Joint Operations is running, writes a caller-owned
byte backup, applies the one exhaustive highest-quality retail reference
profile, and emits a sanitized manifest suitable for binding into registered
capture evidence. HUD state is deliberately not staged; the bridge suppresses
it only for the correlated capture frame.

Stage also writes ``<manifest>.restore-token.local.json``. That companion is
machine-local safety state: it contains the resolved ``game.cfg`` target and
must never be published or passed to the capture registrar. Restore derives its
path from ``--manifest`` and requires it, so the command line remains unchanged.

Production staging::

    uv run python scripts/render/stage_retail_presentation.py stage \
      --game-cfg C:/GAMES/JOTAC/Game/JO/game.cfg \
      --weapon-sav C:/GAMES/JOTAC/Game/JO/expansion/revx02/weapon.sav \
      --backup .scratch/retail-stage/game.cfg.original \
      --manifest .scratch/retail-stage/retail-presentation.json

Restore immediately after the retail process exits::

    uv run python scripts/render/stage_retail_presentation.py restore \
      --game-cfg C:/GAMES/JOTAC/Game/JO/game.cfg \
      --backup .scratch/retail-stage/game.cfg.original \
      --manifest .scratch/retail-stage/retail-presentation.json
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
from io import StringIO
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from typing import Any


SCHEMA = "opennova.retail-presentation-stage.v3"
RESTORE_TOKEN_SCHEMA = "opennova.retail-presentation-restore-token.v3"
RESTORE_TOKEN_SUFFIX = ".restore-token.local.json"
TOOL_VERSION = "3.0.0"
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
PRESERVED_CONFIG_TYPES: dict[str, type[int] | type[str]] = {
    "hw3d_deviceno": int,
    "hw3d_name": str,
    "hw3d_guid": str,
    "enable_keyboardtips": int,
    "enable_gameplaytips": int,
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
VIDEO_OPTION_RANGES: dict[str, tuple[int | float, int | float]] = {
    "windowed": (0, 1),
    "gamma": (0.0, 1.0),
    "terrain_polydetail": (0, 3),
    "terrain_texdetail": (0, 3),
    "object_polydetail": (0, 3),
    "object_texdetail": (0, 3),
    "display_16x9": (0, 1),
    "water_quality": (0, 3),
    "shadow_quality": (0, 3),
    "particle_density": (0, 2),
    "antialias_mode": (0, 16),
    "texfilter_level": (0, 3),
    "fbeffects_level": (0, 3),
    "shader_usage_level": (0, 2),
    "texcompression_level": (0, 2),
    "lock_framerate": (0, 1),
    "force_vsync": (0, 1),
    "reduce_mouselag": (0, 1),
    "NoBlood": (0, 1),
    "NoCasings": (0, 1),
    "NoSmoke": (0, 1),
    "no_anim": (0, 1),
}
TARGET_PROCESS_NAMES = frozenset({"jointops.exe", "jointops"})
APPROVED_WEAPON_SAV_SHA256 = (
    "f4907820a58505a6988f140a27638dae7dbaaea2cc446642f0bc44c868905623"
)

# engine/formats/playersav/weapon_sav.h is the authoritative codec. It has no
# Python/flat-C entry point, so this capture-side reader mirrors only the
# witnessed slot-0 fields needed for validation and never writes weapon.sav.
WEAPON_HEADER_BYTES = 16
WEAPON_RECORD_BYTES = 0x1080C
WEAPON_SIDE_BYTES = 0x8006
WEAPON_KIT_PAGE_BYTES = 2048
WEAPON_PROFILE_SLOTS = 5
WEAPON_FIRST_KIT_PAGE_OFFSET = 6
WEAPON_MIN_CLASS = 5

EXPECTED_BLUE = (9, 2, 0, 0x0402)
EXPECTED_RED = (9, 7, 0, 0x8207)
REQUIRED_BLUE_WEAPON = "WPN_M16BURST"

SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
INTEGER_PATTERN = re.compile(r"^[+-]?\d+$")
FLOAT_PATTERN = re.compile(r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$")
RESOLUTION_PATTERN = re.compile(r"^[1-9]\d*x[1-9]\d*$")
DISPLAY_SECTION_PATTERN = re.compile(
    rb"(?ims)^[ \t]*//[ \t]*DISPLAY[ \t]*(?:\r?\n|\r)"
    rb"(.*?)"
    rb"(?=^[ \t]*//[ \t]*AUDIO[ \t]*(?:\r?\n|\r))"
)
ASSIGNMENT_PATTERN = re.compile(rb"^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=")


class PresentationStageError(ValueError):
    pass


@dataclass(frozen=True)
class SideProfile:
    player_class: int
    avatar_a: int
    avatar_b: int
    avatar_packed: int

    def to_json(self) -> dict[str, int]:
        return {
            "player_class": self.player_class,
            "avatar_a": self.avatar_a,
            "avatar_b": self.avatar_b,
            "avatar_packed": self.avatar_packed,
        }


@dataclass(frozen=True)
class WeaponProfile:
    sha256: str
    blue: SideProfile
    red: SideProfile
    blue_selected_kit_contains_wpn_m16burst: bool

    def to_json(self, file_name: str) -> dict[str, Any]:
        return {
            "file_name": file_name,
            "sha256": self.sha256,
            "slot": 0,
            "blue": self.blue.to_json(),
            "red": self.red.to_json(),
            "blue_selected_kit_contains_wpn_m16burst": (
                self.blue_selected_kit_contains_wpn_m16burst
            ),
        }


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _video_option_catalog_sha256() -> str:
    payload = {
        "id": VIDEO_OPTION_CATALOG_ID,
        "policies": VIDEO_OPTION_POLICIES,
    }
    return _sha256_bytes(json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode("utf-8"))


def _restore_token_path(manifest: Path) -> Path:
    return manifest.with_name(manifest.name + RESTORE_TOKEN_SUFFIX)


def _resolved_target(path: Path) -> str:
    return str(path.resolve())


def _same_resolved_target(left: str, right: Path) -> bool:
    return os.path.normcase(left) == os.path.normcase(_resolved_target(right))


def _read_required(path: Path, label: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise PresentationStageError(f"could not read {label} {path}: {exc}") from exc


def _running_process_image_names() -> set[str]:
    try:
        if os.name == "nt":
            result = subprocess.run(
                ["tasklist", "/FO", "CSV", "/NH"],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
            rows = csv.reader(StringIO(result.stdout))
            return {
                Path(row[0]).name.casefold()
                for row in rows
                if row and row[0] and not row[0].startswith("INFO:")
            }
        result = subprocess.run(
            ["ps", "-A", "-o", "comm="],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
        return {
            Path(line.strip()).name.casefold()
            for line in result.stdout.splitlines()
            if line.strip()
        }
    except (OSError, UnicodeError, subprocess.SubprocessError) as exc:
        raise PresentationStageError(
            f"could not verify that the retail process is stopped: {exc}"
        ) from exc


def _require_retail_stopped() -> None:
    running = TARGET_PROCESS_NAMES.intersection(_running_process_image_names())
    if running:
        names = ", ".join(sorted(running))
        raise PresentationStageError(
            f"retail process is running ({names}); stage/restore only before launch "
            "or after process exit"
        )


def _decode_side(data: bytes, offset: int) -> SideProfile:
    return SideProfile(
        player_class=data[offset],
        avatar_a=data[offset + 1],
        avatar_b=data[offset + 2],
        avatar_packed=struct.unpack_from("<H", data, offset + 4)[0],
    )


def _kit_names(page: bytes) -> list[str]:
    tokens: list[str] = []
    position = 0
    while position < len(page):
        end = page.find(b"\0", position)
        if end < 0:
            end = len(page)
        token = page[position:end]
        if not token:
            break
        try:
            tokens.append(token.decode("ascii"))
        except UnicodeDecodeError as exc:
            raise PresentationStageError(
                "weapon.sav slot-0 selected kit contains non-ASCII data"
            ) from exc
        position = end + 1
    return tokens[::4]


def _read_weapon_profile(path: Path, expected_sha256: str) -> WeaponProfile:
    data = _read_required(path, "weapon.sav")
    actual_sha256 = _sha256_bytes(data)
    if actual_sha256 != expected_sha256:
        raise PresentationStageError(
            "weapon.sav SHA-256 does not match the approved capture profile: "
            f"expected={expected_sha256} actual={actual_sha256}"
        )
    required_size = WEAPON_HEADER_BYTES + WEAPON_PROFILE_SLOTS * WEAPON_RECORD_BYTES
    if len(data) < required_size or data[:8] != b"FPBC0211":
        raise PresentationStageError("weapon.sav has an invalid header or is truncated")

    blue_offset = WEAPON_HEADER_BYTES
    red_offset = blue_offset + WEAPON_SIDE_BYTES
    blue = _decode_side(data, blue_offset)
    red = _decode_side(data, red_offset)
    blue_tuple = (
        blue.player_class,
        blue.avatar_a,
        blue.avatar_b,
        blue.avatar_packed,
    )
    red_tuple = (
        red.player_class,
        red.avatar_a,
        red.avatar_b,
        red.avatar_packed,
    )
    if blue_tuple != EXPECTED_BLUE or red_tuple != EXPECTED_RED:
        raise PresentationStageError(
            "weapon.sav slot 0 does not match the approved retail character profile: "
            f"blue={blue_tuple!r} red={red_tuple!r}"
        )

    page_offset = (
        blue_offset
        + WEAPON_FIRST_KIT_PAGE_OFFSET
        + (blue.player_class - WEAPON_MIN_CLASS) * WEAPON_KIT_PAGE_BYTES
    )
    page = data[page_offset:page_offset + WEAPON_KIT_PAGE_BYTES]
    has_m16 = REQUIRED_BLUE_WEAPON in _kit_names(page)
    if not has_m16:
        raise PresentationStageError(
            "weapon.sav slot-0 blue selected kit does not contain WPN_M16BURST"
        )
    return WeaponProfile(actual_sha256, blue, red, has_m16)


def _value_pattern(key: str) -> re.Pattern[bytes]:
    return re.compile(
        rb"(?im)^([ \t]*" + re.escape(key.encode("ascii"))
        + rb"[ \t]*=[ \t]*)(\"[^\"\r\n]*\"|[^ \t;#\r\n]+)"
        + rb"(?=[ \t]*(?:[;#][^\r\n]*)?(?:\r?\n|\r|$))"
    )


def _typed_value(key: str, token: bytes, expected: int | float | str) -> int | float | str:
    try:
        text = token.decode("ascii")
    except UnicodeDecodeError as exc:
        raise PresentationStageError(f"game.cfg {key} is not ASCII") from exc
    if isinstance(expected, str):
        if key == "video_res" and not RESOLUTION_PATTERN.fullmatch(text):
            raise PresentationStageError(f"game.cfg {key} is not a WIDTHxHEIGHT value")
        if key != "video_res":
            if len(text) < 2 or not text.startswith('"') or not text.endswith('"'):
                raise PresentationStageError(f"game.cfg {key} is not a quoted string")
            return text[1:-1]
        return text
    if isinstance(expected, float):
        if not FLOAT_PATTERN.fullmatch(text):
            raise PresentationStageError(f"game.cfg {key} is not a decimal number")
        return float(text)
    if not INTEGER_PATTERN.fullmatch(text):
        raise PresentationStageError(f"game.cfg {key} is not an integer")
    return int(text)


def _encoded_value(value: int | float | str) -> bytes:
    return str(value).encode("ascii")


def _require_valid_video_value(
    key: str,
    value: int | float | str,
) -> None:
    limits = VIDEO_OPTION_RANGES.get(key)
    if limits is None:
        return
    minimum, maximum = limits
    if not isinstance(value, (int, float)) or not minimum <= value <= maximum:
        raise PresentationStageError(
            f"game.cfg {key} must be in the retail range {minimum}..{maximum}"
        )


def _read_hud_detail(original: bytes) -> int:
    pattern = _value_pattern("hud_detail")
    matches = list(pattern.finditer(original))
    if len(matches) != 1:
        raise PresentationStageError(
            f"game.cfg must contain hud_detail exactly once (found {len(matches)})"
        )
    value = _typed_value("hud_detail", matches[0].group(2), 0)
    assert isinstance(value, int)
    if value < 0 or value > 3:
        raise PresentationStageError("game.cfg hud_detail must be in the retail range 0..3")
    return value


def _require_classified_display_options(original: bytes) -> None:
    section = DISPLAY_SECTION_PATTERN.search(original)
    if section is None:
        raise PresentationStageError(
            "game.cfg must contain bounded // DISPLAY and // AUDIO sections"
        )
    found: set[str] = set()
    for line in section.group(1).splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(b"//"):
            continue
        match = ASSIGNMENT_PATTERN.match(line)
        if match is None:
            raise PresentationStageError("game.cfg DISPLAY section has a malformed row")
        try:
            found.add(match.group(1).decode("ascii"))
        except UnicodeDecodeError as exc:
            raise PresentationStageError(
                "game.cfg DISPLAY option name is not ASCII"
            ) from exc
    classified = {
        key for key, policy in VIDEO_OPTION_POLICIES.items()
        if policy != "visual_effect"
    }
    unknown = sorted(found - classified, key=str.casefold)
    if unknown:
        raise PresentationStageError(
            "game.cfg has unclassified DISPLAY option(s): " + ", ".join(unknown)
        )


def _rewrite_video_profile(
    original: bytes,
) -> tuple[
    bytes,
    dict[str, int | float | str],
    list[str],
    int,
    dict[str, int | str],
]:
    original_values: dict[str, int | float | str] = {}
    _require_classified_display_options(original)
    replacements: list[tuple[int, int, bytes]] = []
    changed_keys: list[str] = []
    for key, expected in HIGHEST_VIDEO_PROFILE.items():
        matches = list(_value_pattern(key).finditer(original))
        if len(matches) != 1:
            raise PresentationStageError(
                f"game.cfg must contain {key} exactly once (found {len(matches)})"
            )
        match = matches[0]
        actual = _typed_value(key, match.group(2), expected)
        _require_valid_video_value(key, actual)
        original_values[key] = actual
        if actual != expected:
            start, end = match.span(2)
            replacements.append((start, end, _encoded_value(expected)))
            changed_keys.append(key)

    effective = original
    for start, end, replacement in reversed(replacements):
        effective = effective[:start] + replacement + effective[end:]
    preserved_values: dict[str, int | str] = {}
    for key, value_type in PRESERVED_CONFIG_TYPES.items():
        matches = list(_value_pattern(key).finditer(original))
        if len(matches) != 1:
            raise PresentationStageError(
                f"game.cfg must contain {key} exactly once (found {len(matches)})"
            )
        prototype: int | str = 0 if value_type is int else ""
        value = _typed_value(key, matches[0].group(2), prototype)
        if key in {"enable_keyboardtips", "enable_gameplaytips"} \
                and value not in {0, 1}:
            raise PresentationStageError(f"game.cfg {key} must be 0 or 1")
        if key == "hw3d_deviceno" and (not isinstance(value, int) or value < 0):
            raise PresentationStageError("game.cfg hw3d_deviceno must be non-negative")
        assert isinstance(value, (int, str)) and not isinstance(value, float)
        preserved_values[key] = value
    hud_detail = _read_hud_detail(original)
    preserved_values["hud_detail"] = hud_detail
    return effective, original_values, changed_keys, hud_detail, preserved_values


def _validate_output_paths(
    game_cfg: Path,
    weapon_sav: Path,
    backup: Path,
    manifest: Path,
) -> None:
    if game_cfg.name.casefold() != "game.cfg":
        raise PresentationStageError("--game-cfg must name game.cfg")
    if weapon_sav.name.casefold() != "weapon.sav":
        raise PresentationStageError("--weapon-sav must name weapon.sav")
    restore_token = _restore_token_path(manifest)
    resolved = [
        path.resolve()
        for path in (game_cfg, weapon_sav, backup, manifest, restore_token)
    ]
    if len(set(resolved)) != len(resolved):
        raise PresentationStageError("input, backup, and manifest paths must be distinct")
    if backup.exists():
        raise PresentationStageError(f"backup already exists: {backup}")
    if manifest.exists():
        raise PresentationStageError(f"manifest already exists: {manifest}")
    if restore_token.exists():
        raise PresentationStageError(
            f"local restore token already exists: {restore_token}"
        )


def _write_exclusive(path: Path, data: bytes, label: str) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    except OSError as exc:
        raise PresentationStageError(f"could not write {label} {path}: {exc}") from exc


def _replace_bytes(path: Path, data: bytes) -> None:
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary = Path(output.name)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except OSError as exc:
        raise PresentationStageError(f"could not replace {path}: {exc}") from exc
    finally:
        if temporary is not None and temporary.exists():
            try:
                temporary.unlink()
            except OSError:
                pass


def stage_retail_reference(
    *,
    game_cfg: Path,
    weapon_sav: Path,
    backup: Path,
    manifest: Path,
    expected_weapon_sav_sha256: str = APPROVED_WEAPON_SAV_SHA256,
) -> dict[str, Any]:
    """Apply and attest the sole publishable retail reference profile."""
    game_cfg = Path(game_cfg)
    weapon_sav = Path(weapon_sav)
    backup = Path(backup)
    manifest = Path(manifest)
    restore_token = _restore_token_path(manifest)
    _validate_output_paths(game_cfg, weapon_sav, backup, manifest)
    _require_retail_stopped()

    original = _read_required(game_cfg, "game.cfg")
    (
        effective,
        original_values,
        changed_keys,
        hud_detail,
        preserved_values,
    ) = _rewrite_video_profile(original)
    profile = _read_weapon_profile(weapon_sav, expected_weapon_sav_sha256)
    original_sha256 = _sha256_bytes(original)
    effective_sha256 = _sha256_bytes(effective)

    _write_exclusive(backup, original, "game.cfg backup")
    if _sha256_bytes(_read_required(backup, "game.cfg backup")) != original_sha256:
        raise PresentationStageError("game.cfg backup verification failed")

    document = {
        "schema": SCHEMA,
        "tool_version": TOOL_VERSION,
        "profile": {
            "id": PROFILE_ID,
            "video_option_catalog_id": VIDEO_OPTION_CATALOG_ID,
            "video_option_catalog_sha256": _video_option_catalog_sha256(),
            "required_values": HIGHEST_VIDEO_PROFILE,
        },
        "game_config": {
            "file_name": game_cfg.name,
            "original_sha256": original_sha256,
            "effective_sha256": effective_sha256,
            "original_values": original_values,
            "effective_values": HIGHEST_VIDEO_PROFILE,
            "changed_keys": changed_keys,
            "preserved_values": preserved_values,
            "hud_detail": {
                "original": hud_detail,
                "effective": hud_detail,
                "unchanged": True,
            },
        },
        "weapon_profile": profile.to_json(weapon_sav.name),
    }
    encoded_manifest = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode()
    restore_document = {
        "schema": RESTORE_TOKEN_SCHEMA,
        "tool_version": TOOL_VERSION,
        "game_config_resolved_target": _resolved_target(game_cfg),
        "backup_sha256": original_sha256,
        "stage_manifest_sha256": _sha256_bytes(encoded_manifest),
    }
    encoded_restore_token = (
        json.dumps(restore_document, indent=2, sort_keys=True) + "\n"
    ).encode()

    try:
        _require_retail_stopped()
        _replace_bytes(game_cfg, effective)
        if _sha256_bytes(_read_required(game_cfg, "staged game.cfg")) != effective_sha256:
            raise PresentationStageError("staged game.cfg verification failed")
        _write_exclusive(manifest, encoded_manifest, "presentation manifest")
        _write_exclusive(
            restore_token,
            encoded_restore_token,
            "local restore token",
        )
    except Exception:
        _replace_bytes(game_cfg, original)
        if _sha256_bytes(_read_required(game_cfg, "restored game.cfg")) != original_sha256:
            raise PresentationStageError(
                "staging failed and byte-exact game.cfg rollback also failed"
            )
        raise
    return document


def _load_stage_manifest(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PresentationStageError(
            f"could not read presentation manifest {path}: {exc}"
        ) from exc
    if not isinstance(document, dict) or document.get("schema") != SCHEMA:
        raise PresentationStageError("presentation manifest schema is unsupported")
    config = document.get("game_config")
    if not isinstance(config, dict):
        raise PresentationStageError("presentation manifest has no game_config record")
    if config.get("file_name") != "game.cfg":
        raise PresentationStageError("presentation manifest does not describe game.cfg")
    if config.get("effective_values") != HIGHEST_VIDEO_PROFILE:
        raise PresentationStageError(
            "presentation manifest was not staged for the highest video profile"
        )
    for field in ("original_sha256", "effective_sha256"):
        value = config.get(field)
        if not isinstance(value, str) or not SHA256_PATTERN.fullmatch(value):
            raise PresentationStageError(
                f"presentation manifest {field} is absent or malformed"
            )
    return document


def _load_restore_token(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PresentationStageError(
            f"could not read local restore token {path}: {exc}"
        ) from exc
    if not isinstance(document, dict) or document.get("schema") != RESTORE_TOKEN_SCHEMA:
        raise PresentationStageError("local restore token schema is unsupported")
    target = document.get("game_config_resolved_target")
    if not isinstance(target, str) or not target:
        raise PresentationStageError("local restore token target is absent")
    for field in ("backup_sha256", "stage_manifest_sha256"):
        value = document.get(field)
        if not isinstance(value, str) or not SHA256_PATTERN.fullmatch(value):
            raise PresentationStageError(
                f"local restore token {field} is absent or malformed"
            )
    return document


def restore_retail_reference(
    *,
    game_cfg: Path,
    backup: Path,
    manifest: Path,
) -> dict[str, str]:
    """Restore the exact bytes captured by :func:`stage_retail_reference`."""
    game_cfg = Path(game_cfg)
    backup = Path(backup)
    manifest = Path(manifest)
    restore_token_path = _restore_token_path(manifest)
    if game_cfg.name.casefold() != "game.cfg":
        raise PresentationStageError("--game-cfg must name game.cfg")
    resolved = [
        path.resolve()
        for path in (game_cfg, backup, manifest, restore_token_path)
    ]
    if len(set(resolved)) != len(resolved):
        raise PresentationStageError("game.cfg, backup, and manifest paths must be distinct")

    _require_retail_stopped()
    restore_token = _load_restore_token(restore_token_path)
    if not _same_resolved_target(
        restore_token["game_config_resolved_target"], game_cfg
    ):
        raise PresentationStageError(
            "restore token target does not match --game-cfg"
        )
    manifest_bytes = _read_required(manifest, "presentation manifest")
    if _sha256_bytes(manifest_bytes) != restore_token["stage_manifest_sha256"]:
        raise PresentationStageError(
            "presentation manifest SHA-256 does not match the local restore token"
        )
    document = _load_stage_manifest(manifest)
    original_sha256 = document["game_config"]["original_sha256"]
    original = _read_required(backup, "game.cfg backup")
    backup_sha256 = _sha256_bytes(original)
    if backup_sha256 != restore_token["backup_sha256"]:
        raise PresentationStageError(
            "game.cfg backup SHA-256 does not match the local restore token"
        )
    if backup_sha256 != original_sha256:
        raise PresentationStageError(
            "game.cfg backup SHA-256 does not match the staging manifest"
        )
    _require_retail_stopped()
    _replace_bytes(game_cfg, original)
    restored_sha256 = _sha256_bytes(_read_required(game_cfg, "restored game.cfg"))
    if restored_sha256 != original_sha256:
        raise PresentationStageError("restored game.cfg SHA-256 verification failed")
    return {"status": "restored", "restored_sha256": restored_sha256}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    stage = subparsers.add_parser(
        "stage", help="stage the exhaustive highest-quality retail profile"
    )
    stage.add_argument("--game-cfg", type=Path, required=True)
    stage.add_argument("--weapon-sav", type=Path, required=True)
    stage.add_argument("--backup", type=Path, required=True)
    stage.add_argument("--manifest", type=Path, required=True)
    stage.add_argument(
        "--expected-weapon-sav-sha256",
        default=APPROVED_WEAPON_SAV_SHA256,
        help="approved active profile hash (defaults to the PR #503 retail profile)",
    )
    restore = subparsers.add_parser(
        "restore", help="restore the exact pre-capture game.cfg bytes"
    )
    restore.add_argument("--game-cfg", type=Path, required=True)
    restore.add_argument("--backup", type=Path, required=True)
    restore.add_argument("--manifest", type=Path, required=True)
    return parser


def main() -> int:
    parser = _parser()
    args = parser.parse_args()
    expected_hash = getattr(args, "expected_weapon_sav_sha256", "").casefold()
    if expected_hash and not SHA256_PATTERN.fullmatch(expected_hash):
        parser.error("--expected-weapon-sav-sha256 must be 64 lowercase hex digits")
    if expected_hash:
        args.expected_weapon_sav_sha256 = expected_hash
    try:
        if args.operation == "stage":
            document = stage_retail_reference(
                game_cfg=args.game_cfg,
                weapon_sav=args.weapon_sav,
                backup=args.backup,
                manifest=args.manifest,
                expected_weapon_sav_sha256=args.expected_weapon_sav_sha256,
            )
        elif args.operation == "restore":
            document = restore_retail_reference(
                game_cfg=args.game_cfg,
                backup=args.backup,
                manifest=args.manifest,
            )
        else:
            parser.error(f"unsupported operation: {args.operation}")
            return 2
    except PresentationStageError as exc:
        print(f"retail presentation staging failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(document, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
