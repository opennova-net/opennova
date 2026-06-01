"""
Unified loose-file + PFF asset resolver.

Allows pointing at a game install directory containing .pff archives
instead of requiring pre-extracted assets. Loose files still work too.

Since all C FFI parsers take file paths (not buffers), PFF-extracted
assets are written to a temporary directory.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

from .bfc1_ffi import is_bfc1, decompress as bfc1_decompress
from .pff_ffi import PffArchive
from .scr_ffi import (
    is_scr,
    get_version,
    decrypt,
    SCR_KEY_DEFAULT,
    SCR_KEY_JO_DFX2,
    SCR_KEY_SHADERS,
    SCR_KEY_DFLW,
)

_SCR_VERSION_KEYS = {
    0: SCR_KEY_DEFAULT,
    1: SCR_KEY_JO_DFX2,
    2: SCR_KEY_SHADERS,
}

_LW_TEXT_EXTS = {".def", ".anm", ".aca"}


class AssetResolver:
    """Context manager that resolves asset filenames to filesystem paths.

    Search order for resolve():
      1. Loose file in base_dir (case-insensitive)
      2. Already-extracted temp file (avoids re-extraction)
      3. PFF archives (sorted alphabetically, first match wins)
         - SCR-encrypted files are auto-decrypted on extraction

    Usage::

        with AssetResolver(base_dir) as resolver:
            path = resolver.resolve("weapon.def")
    """

    def __init__(self, base_dir: str):
        self.base_dir = Path(base_dir)
        self._tmp_path = Path(tempfile.mkdtemp(prefix="opennova_"))

        # Case-insensitive lookup for loose files: lowercase name -> actual Path
        self._loose: dict[str, Path] = {}
        try:
            for entry in os.scandir(str(self.base_dir)):
                if entry.is_file():
                    self._loose[entry.name.lower()] = Path(entry.path)
        except OSError:
            pass

        # Open all PFF archives found in the directory, sorted alphabetically
        self._archives: list[PffArchive] = []
        pff_paths = sorted(
            p for p in self.base_dir.iterdir()
            if p.is_file() and p.suffix.lower() == ".pff"
        ) if self.base_dir.is_dir() else []
        for pff_path in pff_paths:
            try:
                self._archives.append(PffArchive(str(pff_path)))
            except RuntimeError:
                pass  # skip archives that fail to open

        # Track already-extracted temp files: lowercase name -> temp Path
        self._extracted: dict[str, Path] = {}

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def close(self):
        """Close all PFF archive handles.

        Extracted temp files are intentionally left on disk so that
        Blender can continue to reference textures by path.  The OS
        cleans the system temp directory on reboot.
        """
        for arc in self._archives:
            try:
                arc.close()
            except Exception:
                pass
        self._archives.clear()

    def _ensure_decoded(self, key: str, path: Path) -> str:
        """If a loose file is encoded/compressed, decode it to temp."""
        try:
            with open(path, "rb") as f:
                header = f.read(8)
        except OSError:
            return str(path)

        needs_decode = _is_lw_text_asset_name(key)
        if len(header) >= 4 and is_scr(header[:4]):
            needs_decode = True
        elif len(header) >= 8 and is_bfc1(header[:8]):
            needs_decode = True

        if not needs_decode:
            return str(path)

        data = path.read_bytes()
        decoded = _decode_asset_payload(key, data)
        if decoded == data:
            return str(path)

        dest = self._tmp_path / key
        dest.write_bytes(decoded)
        self._extracted[key] = dest
        return str(dest)

    def resolve(self, filename: str) -> str | None:
        """Resolve a filename to a filesystem path, or None if not found.

        Returns a string path suitable for passing to C FFI functions.
        """
        if not filename:
            return None
        key = filename.lower()

        # 1. Loose file
        if key in self._loose:
            return self._ensure_decoded(key, self._loose[key])

        # 2. Already extracted
        if key in self._extracted:
            return str(self._extracted[key])

        # 3. PFF archives
        for arc in self._archives:
            entry = arc.find(filename)
            if entry is not None:
                data = _decode_asset_payload(filename, arc.extract(entry))
                dest = self._tmp_path / filename
                dest.write_bytes(data)
                self._extracted[key] = dest
                return str(dest)

        return None

    def resolve_texture(self, texture_name: str) -> str | None:
        """Resolve a texture name with extension fallback.

        Strips known extensions, then tries candidates in priority order:
        original name, .dds, .tga, .png, .mdt.
        """
        if not texture_name:
            return None

        _EXTS = (".dds", ".tga", ".png", ".mdt")

        # Strip all known extensions to get base name
        base = texture_name
        stripped = True
        while stripped:
            stripped = False
            for ext in _EXTS:
                if base.lower().endswith(ext):
                    base = base[: len(base) - len(ext)]
                    stripped = True
                    break

        # Build ordered candidate list
        candidates = [texture_name]
        for ext in _EXTS:
            c = base + ext
            if c.lower() != texture_name.lower():
                candidates.append(c)

        for candidate in candidates:
            result = self.resolve(candidate)
            if result is not None:
                return result

        return None


def _decode_asset_payload(filename: str, data: bytes) -> bytes:
    if len(data) >= 4 and is_scr(data[:4]):
        data = _decode_scr_blob(filename, data)
    elif _should_try_lw_loose_text_decode(filename, data):
        data = _decode_lw_loose_text_asset(filename, data)
    if len(data) >= 8 and is_bfc1(data[:8]):
        data = bfc1_decompress(data)
    return data


def _decode_scr_blob(filename: str, data: bytes) -> bytes:
    ver = get_version(data[:4])
    fallback_key = _SCR_VERSION_KEYS.get(ver, SCR_KEY_DEFAULT)
    keys = [fallback_key]
    if ver == 1 and _is_lw_text_asset_name(filename):
        keys.insert(0, SCR_KEY_DFLW)

    seen: set[int] = set()
    fallback_decoded: bytes | None = None
    for key in keys:
        if key in seen:
            continue
        seen.add(key)
        decoded = decrypt(data, key)
        if fallback_decoded is None:
            fallback_decoded = decoded
        if key == SCR_KEY_DFLW and _looks_like_lw_text_asset(filename, decoded):
            return decoded
        if key != SCR_KEY_DFLW:
            fallback_decoded = decoded
    return fallback_decoded if fallback_decoded is not None else data


def _should_try_lw_loose_text_decode(filename: str, data: bytes) -> bool:
    if not _is_lw_text_asset_name(filename):
        return False
    if data.startswith(b"CBIN"):
        return False
    if _looks_like_lw_text_asset(filename, data):
        return False
    return True


def _decode_lw_loose_text_asset(filename: str, data: bytes) -> bytes:
    payload = _scr_encrypt_payload(data, SCR_KEY_DEFAULT)
    decoded = _scr_decrypt_payload(payload, SCR_KEY_DFLW)
    return decoded if _looks_like_lw_text_asset(filename, decoded) else data


def _is_lw_text_asset_name(filename: str) -> bool:
    return Path(filename).suffix.lower() in _LW_TEXT_EXTS


def _looks_like_lw_text_asset(filename: str, data: bytes) -> bool:
    if not _looks_like_text(data):
        return False
    lower = data[:8192].lower()
    suffix = Path(filename).suffix.lower()
    if suffix == ".def":
        return any(token in lower for token in (b"begin", b"graphic", b"gfx", b"type", b"weapon", b"ammoclass"))
    if suffix == ".aca":
        return b"slot" in lower and b".saf" in lower
    if suffix == ".anm":
        return b"override" in lower or b"walking" in lower or b"standing" in lower
    return False


def _looks_like_text(data: bytes) -> bool:
    if not data:
        return False
    sample = data[:8192]
    printable = 0
    for byte in sample:
        if byte in (9, 10, 13) or 32 <= byte <= 126:
            printable += 1
    return printable / len(sample) >= 0.90


def _scr_encrypt_payload(plaintext: bytes, key: int) -> bytes:
    return _xor_with_scr_keystream(plaintext, key)[::-1]


def _scr_decrypt_payload(payload: bytes, key: int) -> bytes:
    return _xor_with_scr_keystream(payload[::-1], key)


def _xor_with_scr_keystream(data: bytes, key: int) -> bytes:
    out = bytearray(data)
    key &= 0xFFFFFFFF
    for i in range(len(out)):
        key = (_rol32((key + _rol32(key, 11)) & 0xFFFFFFFF, 4) ^ 1) & 0xFFFFFFFF
        out[i] ^= key & 0xFF
    return bytes(out)


def _rol32(value: int, shift: int) -> int:
    value &= 0xFFFFFFFF
    return ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF
