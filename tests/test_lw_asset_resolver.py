from __future__ import annotations

from pathlib import Path
import struct

import pytest


LW_SCR_KEY = 0x01234567
SCR_KEY_DEFAULT = 0xABEEFACE

ITEMS_DEF = (
    b'\r\nbegin "LW Test Crate"\r\n'
    b"  id 104001\r\n"
    b"  type powerup\r\n"
    b"  graphic LwCrate\r\n"
    b"  sid lwcrate\r\n"
    b"  hp 50\r\n"
    b"end\r\n"
)


def _rol32(value: int, shift: int) -> int:
    value &= 0xFFFFFFFF
    return ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF


def _xor_with_scr_keystream(data: bytes, key: int) -> bytes:
    out = bytearray(data)
    key &= 0xFFFFFFFF
    for i in range(len(out)):
        key = (_rol32((key + _rol32(key, 11)) & 0xFFFFFFFF, 4) ^ 1) & 0xFFFFFFFF
        out[i] ^= key & 0xFF
    return bytes(out)


def _scr_encrypt_payload(plaintext: bytes, key: int) -> bytes:
    return _xor_with_scr_keystream(plaintext, key)[::-1]


def _scr_decrypt_payload(payload: bytes, key: int) -> bytes:
    return _xor_with_scr_keystream(payload[::-1], key)


def _scr_blob(plaintext: bytes, key: int, version: int = 1) -> bytes:
    return b"SCR" + bytes([version]) + _scr_encrypt_payload(plaintext, key)


def _asset_resolver_cls():
    try:
        from pyopennova.asset_resolver import AssetResolver
    except (FileNotFoundError, OSError, RuntimeError) as exc:
        pytest.skip(f"native library unavailable: {exc}")
    return AssetResolver


def test_lw_scr_v1_def_files_decode_with_land_warrior_key(tmp_path: Path) -> None:
    (tmp_path / "items.def").write_bytes(_scr_blob(ITEMS_DEF, LW_SCR_KEY, version=1))

    AssetResolver = _asset_resolver_cls()
    with AssetResolver(str(tmp_path)) as resolver:
        resolved = resolver.resolve("ITEMS.DEF")

    assert resolved is not None
    assert Path(resolved).read_bytes() == ITEMS_DEF


def test_lw_loose_def_files_predecoded_with_default_key_are_normalized(tmp_path: Path) -> None:
    lw_payload = _scr_encrypt_payload(ITEMS_DEF, LW_SCR_KEY)
    loose_wrong_key_plaintext = _scr_decrypt_payload(lw_payload, SCR_KEY_DEFAULT)
    (tmp_path / "items.def").write_bytes(loose_wrong_key_plaintext)

    AssetResolver = _asset_resolver_cls()
    with AssetResolver(str(tmp_path)) as resolver:
        resolved = resolver.resolve("items.def")

    assert resolved is not None
    assert Path(resolved).read_bytes() == ITEMS_DEF


def test_onimport_scan_reads_lw_scr_encoded_items_def(tmp_path: Path) -> None:
    (tmp_path / "items.def").write_bytes(_scr_blob(ITEMS_DEF, LW_SCR_KEY, version=1))

    try:
        from apps.importer.import_runner import scan_directory_result
    except (FileNotFoundError, OSError, RuntimeError) as exc:
        pytest.skip(f"native library unavailable: {exc}")

    result = scan_directory_result(str(tmp_path))

    assert result.ok, result.error
    assert [item.name for item in result.items] == ["LW Test Crate"]
    assert result.items[0].type == "item"
    assert result.items[0].source_model == "LwCrate.3di"


def test_pcx_textures_are_materialized_as_png_for_host_loaders(tmp_path: Path) -> None:
    (tmp_path / "AMIDAL.PCX").write_bytes(_pcx_1x1_indexed((10, 20, 30)))

    AssetResolver = _asset_resolver_cls()
    with AssetResolver(str(tmp_path)) as resolver:
        resolved = resolver.resolve_texture("AMIDAL.PCX")

    assert resolved is not None
    resolved_path = Path(resolved)
    assert resolved_path.suffix.lower() == ".png"
    assert resolved_path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")


def _pcx_1x1_indexed(rgb: tuple[int, int, int]) -> bytes:
    header = bytearray(128)
    header[0] = 0x0A  # manufacturer
    header[1] = 5     # version
    header[2] = 1     # RLE encoding
    header[3] = 8     # bits per pixel per plane
    struct.pack_into("<HHHH", header, 4, 0, 0, 0, 0)
    header[65] = 1    # color planes
    struct.pack_into("<H", header, 66, 1)  # bytes per line
    palette = bytearray(768)
    palette[3:6] = bytes(rgb)
    return bytes(header) + b"\x01" + b"\x0c" + bytes(palette)
