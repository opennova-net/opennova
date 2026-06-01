"""
Unified loose-file + PFF asset resolver.

Allows pointing at a game install directory containing .pff archives
instead of requiring pre-extracted assets. Loose files still work too.

Since all C FFI parsers take file paths (not buffers), PFF-extracted
assets are written to a temporary directory.
"""

from __future__ import annotations

import os
import shutil
import tempfile
import hashlib
import struct
import zlib
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

TEXTURE_STRATEGY_GENERIC = "generic"
TEXTURE_STRATEGY_3DI3_DF4OED = "3di3_df4oed"

THREEDI_SOURCE_3DI3 = 1

_TEXTURE_EXTS = (".dds", ".tga", ".png", ".mdt", ".pcx")


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

        # Track texture files copied to temp with a DCC-loadable extension.
        self._texture_paths: dict[str, Path] = {}

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

    def resolve_texture(
        self,
        texture_name: str,
        *,
        strategy: str | None = None,
        source_format: int | None = None,
        slot: int | None = None,
        tex_type: int | None = None,
        flags: int | None = None,
        role: str | None = None,
    ) -> str | None:
        """Resolve a texture name with extension fallback.

        The returned path is for host/DCC loading. The authored texture name
        remains owned by the material descriptor.
        """
        if not texture_name:
            return None

        _ = (slot, tex_type, flags, role)  # Reserved for slot-specific flavor rules.
        strategy_name = _texture_strategy(strategy, source_format)
        candidates = _texture_candidates(texture_name, strategy_name)

        for candidate in candidates:
            result = self.resolve(candidate)
            if result is not None:
                return self._ensure_texture_extension(result)

        return None

    def _ensure_texture_extension(self, path: str) -> str:
        """Return a path whose extension matches the bitmap payload.

        Some game assets are DDS files with a .TGA name. DCC bitmap loaders
        often pick the decoder from the extension, so materialize a temp copy
        with the detected extension while preserving the original source file.
        """
        source = Path(path)
        if source.suffix.lower() == ".pcx":
            key = f"{str(source).lower()}|.png"
            cached = self._texture_paths.get(key)
            if cached is not None and cached.is_file():
                return str(cached)
            dest = self._tmp_path / f"{source.stem}.png"
            if dest.exists() and not _same_path(dest, source):
                dest = self._tmp_path / f"{source.stem}_{_stable_path_hash(key)}.png"
            converted = _copy_pcx_as_png(source, dest)
            if converted is None:
                for root in _texture_cache_roots(self.base_dir):
                    converted = _copy_pcx_as_png(
                        source,
                        root / f"{source.stem}_{_stable_path_hash(key)}.png",
                    )
                    if converted is not None:
                        break
            if converted is not None:
                self._texture_paths[key] = converted
                return str(converted)

        detected_ext = _detect_bitmap_extension(source)
        if detected_ext is None or source.suffix.lower() == detected_ext:
            return str(source)

        key = f"{str(source).lower()}|{detected_ext}"
        cached = self._texture_paths.get(key)
        if cached is not None and cached.is_file():
            return str(cached)

        dest = self._tmp_path / f"{source.stem}{detected_ext}"
        if dest.exists() and not _same_path(dest, source):
            dest = self._tmp_path / f"{source.stem}_{_stable_path_hash(key)}{detected_ext}"
        copied = _copy_texture_payload(source, dest)
        if copied is None:
            for root in _texture_cache_roots(self.base_dir):
                copied = _copy_texture_payload(
                    source,
                    root / f"{source.stem}_{_stable_path_hash(key)}{detected_ext}",
                )
                if copied is not None:
                    break
        if copied is None:
            return str(source)
        dest = copied
        self._texture_paths[key] = dest
        return str(dest)


def _detect_bitmap_extension(path: Path) -> str | None:
    try:
        with open(path, "rb") as f:
            header = f.read(16)
    except OSError:
        return None

    if header.startswith(b"DDS "):
        return ".dds"
    if header.startswith(b"\x89PNG\r\n\x1a\n"):
        return ".png"
    if header.startswith(b"\xff\xd8\xff"):
        return ".jpg"
    if header.startswith(b"BM"):
        return ".bmp"
    if header.startswith(b"II*\x00") or header.startswith(b"MM\x00*"):
        return ".tif"
    return None


def _copy_pcx_as_png(source: Path, dest: Path) -> Path | None:
    try:
        png = _pcx_to_png(source.read_bytes())
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(png)
        return dest
    except (OSError, ValueError, struct.error, zlib.error):
        return None


def _pcx_to_png(data: bytes) -> bytes:
    if len(data) < 128:
        raise ValueError("PCX header truncated")
    if data[0] != 0x0A or data[2] != 1:
        raise ValueError("Unsupported PCX header")

    bits_per_pixel = data[3]
    xmin, ymin, xmax, ymax = struct.unpack_from("<HHHH", data, 4)
    if xmax < xmin or ymax < ymin:
        raise ValueError("Invalid PCX dimensions")
    width = xmax - xmin + 1
    height = ymax - ymin + 1
    if width <= 0 or height <= 0:
        raise ValueError("Invalid PCX dimensions")

    planes = data[65]
    bytes_per_line = struct.unpack_from("<H", data, 66)[0]
    if bits_per_pixel != 8 or bytes_per_line < width:
        raise ValueError("Unsupported PCX layout")

    indexed = planes == 1
    rgb_planes = planes == 3
    if not indexed and not rgb_planes:
        raise ValueError("Unsupported PCX plane count")

    raster_end = len(data)
    palette = b""
    if indexed:
        if len(data) < 128 + 769 or data[-769] != 0x0C:
            raise ValueError("PCX 256-color palette missing")
        palette = data[-768:]
        raster_end -= 769

    expected = height * bytes_per_line * planes
    decoded = _decode_pcx_rle(data, 128, raster_end, expected)
    rows: list[bytes] = []
    stride = bytes_per_line * planes
    for y in range(height):
        row = decoded[y * stride:(y + 1) * stride]
        if indexed:
            rgb = bytearray(width * 3)
            for x in range(width):
                idx = row[x]
                rgb[x * 3:x * 3 + 3] = palette[idx * 3:idx * 3 + 3]
            rows.append(bytes(rgb))
        else:
            red = row[0:bytes_per_line]
            green = row[bytes_per_line:bytes_per_line * 2]
            blue = row[bytes_per_line * 2:bytes_per_line * 3]
            rgb = bytearray(width * 3)
            for x in range(width):
                rgb[x * 3 + 0] = red[x]
                rgb[x * 3 + 1] = green[x]
                rgb[x * 3 + 2] = blue[x]
            rows.append(bytes(rgb))

    return _png_from_rgb_rows(width, height, rows)


def _decode_pcx_rle(data: bytes, start: int, end: int, expected: int) -> bytes:
    out = bytearray()
    pos = start
    while pos < end and len(out) < expected:
        value = data[pos]
        pos += 1
        if (value & 0xC0) == 0xC0:
            if pos >= end:
                raise ValueError("PCX RLE run truncated")
            run = value & 0x3F
            value = data[pos]
            pos += 1
            out.extend([value] * run)
        else:
            out.append(value)
    if len(out) < expected:
        raise ValueError("PCX scanline truncated")
    return bytes(out[:expected])


def _png_from_rgb_rows(width: int, height: int, rows: list[bytes]) -> bytes:
    raw = b"".join(b"\x00" + row for row in rows)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(raw))
        + _png_chunk(b"IEND", b"")
    )


def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


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


def _copy_texture_payload(source: Path, dest: Path) -> Path | None:
    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
        return dest
    except OSError:
        return None


def _texture_cache_roots(base_dir: Path) -> list[Path]:
    roots: list[Path] = []
    override = os.environ.get("OPENNOVA_TEXTURE_CACHE_DIR", "").strip()
    if override:
        roots.append(Path(override))
    local_appdata = os.environ.get("LOCALAPPDATA", "").strip()
    if local_appdata:
        roots.append(Path(local_appdata) / "OpenNova" / "texture-cache")
    roots.append(base_dir / ".opennova_texture_cache")
    return roots


def _stable_path_hash(value: str) -> str:
    return hashlib.sha1(value.encode("utf-8", errors="replace")).hexdigest()[:8]


def _same_path(a: Path, b: Path) -> bool:
    try:
        return a.resolve() == b.resolve()
    except OSError:
        return False


def _texture_strategy(strategy: str | None, source_format: int | None) -> str:
    if strategy:
        value = strategy.lower()
        if value in {"3di3", "df4oed", TEXTURE_STRATEGY_3DI3_DF4OED}:
            return TEXTURE_STRATEGY_3DI3_DF4OED
        return TEXTURE_STRATEGY_GENERIC
    if source_format == THREEDI_SOURCE_3DI3:
        return TEXTURE_STRATEGY_3DI3_DF4OED
    return TEXTURE_STRATEGY_GENERIC


def _texture_candidates(texture_name: str, strategy: str) -> list[str]:
    base, ext = _strip_known_texture_extensions(texture_name)
    if strategy == TEXTURE_STRATEGY_3DI3_DF4OED:
        return _dedupe_texture_candidates(_df4oed_texture_candidates(texture_name, base, ext))
    return _dedupe_texture_candidates(_generic_texture_candidates(texture_name, base))


def _strip_known_texture_extensions(texture_name: str) -> tuple[str, str]:
    base = texture_name
    ext = ""
    stripped = True
    while stripped:
        stripped = False
        lower = base.lower()
        for candidate_ext in _TEXTURE_EXTS:
            if lower.endswith(candidate_ext):
                base = base[: len(base) - len(candidate_ext)]
                ext = candidate_ext
                stripped = True
                break
    return base, ext


def _generic_texture_candidates(texture_name: str, base: str) -> list[str]:
    candidates = [texture_name]
    for ext in (".dds", ".tga", ".png", ".mdt"):
        candidate = base + ext
        if candidate.lower() != texture_name.lower():
            candidates.append(candidate)
    return candidates


def _df4oed_texture_candidates(texture_name: str, base: str, ext: str) -> list[str]:
    if ext == ".mdt":
        return [texture_name, base + ".mdt"]
    if ext == ".dds":
        return [texture_name, base + ".dds"]

    candidates = [base + ".dds", texture_name]
    if ext == ".pcx":
        candidates.append(base + ".pcx")
    else:
        candidates.extend([base + ".tga", base + ".pcx", base + ".png", base + ".mdt"])
    return candidates


def _dedupe_texture_candidates(candidates: list[str]) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for candidate in candidates:
        key = candidate.lower()
        if candidate and key not in seen:
            seen.add(key)
            out.append(candidate)
    return out
