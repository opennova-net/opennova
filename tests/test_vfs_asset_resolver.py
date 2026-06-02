"""Tests for the FFI-backed VFS (pyopennova.vfs_ffi) and the AssetResolver wrapper.

Exercises the engine-faithful precedence (loose > primary > secondary, expansion override)
and the frozen AssetResolver public API. Fixtures are synthesized in tmp dirs, so no game data
is required. Skipped if the native opennova library is not built.
"""

from __future__ import annotations

import struct
from pathlib import Path

import pytest

try:
    from pyopennova.vfs_ffi import Vfs

    _probe = Vfs()
    _probe.close()
    _HAVE_NATIVE = True
except Exception:  # native lib not built / not loadable on this platform
    _HAVE_NATIVE = False

pytestmark = pytest.mark.skipif(not _HAVE_NATIVE, reason="native opennova library not available")


def write_pff(path: Path, entries: list[tuple[str, bytes]]) -> None:
    """Write a modern PFF3 archive: header(20) | payloads | entry table(36 each)."""
    payload = b"".join(data for _, data in entries)
    table_offset = 20 + len(payload)
    out = bytearray()
    out += struct.pack("<5I", 20, 0x33464650, len(entries), 36, table_offset)  # header
    out += payload
    offset = 20
    for name, data in entries:
        rec = bytearray(36)
        struct.pack_into("<4I", rec, 0, 0, offset, len(data), 0)  # flags, offset, size, ts
        nb = name.encode("ascii")[:16]
        rec[16 : 16 + len(nb)] = nb
        out += rec
        offset += len(data)
    path.write_bytes(bytes(out))


def write_loose(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


# ----------------------------- AssetResolver (frozen public API) -----------------------------

def test_resolver_loose_shadows_archive(tmp_path: Path) -> None:
    from pyopennova.asset_resolver import AssetResolver

    write_loose(tmp_path / "shared.txt", b"LOOSE")
    write_pff(tmp_path / "data.pff", [("shared.txt", b"ARCHIVE"), ("archonly.txt", b"ARCH")])

    with AssetResolver(str(tmp_path)) as r:
        shared = r.resolve("shared.txt")
        assert shared is not None
        assert Path(shared).read_bytes() == b"LOOSE"  # loose shadows the archived copy

        archonly = r.resolve("archonly.txt")
        assert archonly is not None
        assert Path(archonly).read_bytes() == b"ARCH"

        assert r.resolve("missing.xyz") is None
        assert r.resolve("") is None

        # caching: same logical name -> same materialized path
        assert r.resolve("shared.txt") == shared


def test_resolver_is_context_manager_and_idempotent_close(tmp_path: Path) -> None:
    from pyopennova.asset_resolver import AssetResolver

    write_pff(tmp_path / "data.pff", [("a.txt", b"A")])
    r = AssetResolver(str(tmp_path))
    p = r.resolve("a.txt")
    assert p is not None and Path(p).is_file()
    r.close()
    r.close()  # idempotent
    # temp files persist after close (DCC loaders keep referencing them)
    assert Path(p).is_file()


def test_resolve_texture_extension_fallback(tmp_path: Path) -> None:
    from pyopennova.asset_resolver import AssetResolver

    # Only a .dds exists; requesting the .tga name should fall back to it.
    write_loose(tmp_path / "tex.dds", b"DDS \x00\x00\x00\x00payload")
    with AssetResolver(str(tmp_path)) as r:
        resolved = r.resolve_texture("tex.tga")
        assert resolved is not None
        assert Path(resolved).name.lower() == "tex.dds"


# ----------------------------- low-level Vfs precedence -----------------------------

def test_vfs_loose_primary_secondary_precedence(tmp_path: Path) -> None:
    write_loose(tmp_path / "loose" / "x.txt", b"LOOSE")
    write_pff(tmp_path / "primary.pff", [("x.txt", b"PRIMARY"), ("ponly.txt", b"PONLY")])
    write_pff(tmp_path / "secondary.pff", [("x.txt", b"SECONDARY"), ("sonly.txt", b"SONLY")])

    with Vfs() as v:
        assert v.add_search_path(str(tmp_path / "loose"))
        assert v.set_primary_archive(str(tmp_path / "primary.pff"))
        assert v.add_secondary_archive(str(tmp_path / "secondary.pff"))

        assert v.read_file("x.txt") == b"LOOSE"       # loose beats all archives
        assert v.read_file("ponly.txt") == b"PONLY"   # primary
        assert v.read_file("sonly.txt") == b"SONLY"   # secondary
        assert v.has_file("x.txt")
        assert not v.has_file("nope.txt")
        assert v.read_file("nope.txt") is None


def test_vfs_mount_game_expansion_override(tmp_path: Path) -> None:
    root = tmp_path
    exp = root / "expansion" / "jox01"
    exp.mkdir(parents=True)

    write_pff(root / "resource.pff", [("shared.txt", b"BASE"), ("base_only.txt", b"BASE_ONLY")])
    write_pff(exp / "jox01.pff", [("shared.txt", b"MAIN"), ("expmain.txt", b"EXP_MAIN")])
    write_pff(exp / "jox01L.pff", [("shared.txt", b"LOCAL")])
    write_loose(exp / "shared.txt", b"LOOSE")

    with Vfs() as v:
        assert v.mount_game(str(root), "jox01")
        assert v.read_file("shared.txt") == b"LOOSE"       # loose expansion file wins
        assert v.read_file("expmain.txt") == b"EXP_MAIN"   # expansion main beats base
        assert v.read_file("base_only.txt") == b"BASE_ONLY"  # base reachable

    # A bad/missing expansion falls back to base-game mounting (no crash).
    with Vfs() as v:
        assert v.mount_game(str(root), "doesnotexist")
        assert v.read_file("base_only.txt") == b"BASE_ONLY"
