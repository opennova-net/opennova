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
from pathlib import Path

from .bfc1_ffi import is_bfc1, decompress as bfc1_decompress
from .pff_ffi import PffArchive
from .scr_ffi import is_scr, get_version, decrypt, SCR_KEY_DEFAULT, SCR_KEY_JO_DFX2, SCR_KEY_SHADERS

_SCR_VERSION_KEYS = {
    0: SCR_KEY_DEFAULT,
    1: SCR_KEY_JO_DFX2,
    2: SCR_KEY_SHADERS,
}


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
        """If a loose file is SCR-encrypted or BFC1-compressed, decode it to temp."""
        try:
            with open(path, "rb") as f:
                header = f.read(8)
        except OSError:
            return str(path)

        needs_decode = False
        if len(header) >= 4 and is_scr(header[:4]):
            needs_decode = True
        elif len(header) >= 8 and is_bfc1(header[:8]):
            needs_decode = True

        if not needs_decode:
            return str(path)

        data = path.read_bytes()
        if len(data) >= 4 and is_scr(data[:4]):
            ver = get_version(data[:4])
            scr_key = _SCR_VERSION_KEYS.get(ver, SCR_KEY_DEFAULT)
            data = decrypt(data, scr_key)
        if len(data) >= 8 and is_bfc1(data[:8]):
            data = bfc1_decompress(data)

        dest = self._tmp_path / key
        dest.write_bytes(data)
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
                data = arc.extract(entry)
                # Auto-decrypt SCR if needed
                if len(data) >= 4 and is_scr(data[:4]):
                    ver = get_version(data[:4])
                    scr_key = _SCR_VERSION_KEYS.get(ver, SCR_KEY_DEFAULT)
                    data = decrypt(data, scr_key)
                # Auto-decompress BFC1 if needed
                if len(data) >= 8 and is_bfc1(data[:8]):
                    data = bfc1_decompress(data)
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
                return self._ensure_texture_extension(result)

        return None

    def _ensure_texture_extension(self, path: str) -> str:
        """Return a path whose extension matches the bitmap payload.

        Some game assets are DDS files with a .TGA name. DCC bitmap loaders
        often pick the decoder from the extension, so materialize a temp copy
        with the detected extension while preserving the original source file.
        """
        source = Path(path)
        detected_ext = _detect_bitmap_extension(source)
        if detected_ext is None or source.suffix.lower() == detected_ext:
            return str(source)

        key = f"{str(source).lower()}|{detected_ext}"
        cached = self._texture_paths.get(key)
        if cached is not None and cached.is_file():
            return str(cached)

        dest = self._tmp_path / f"{source.stem}{detected_ext}"
        if dest.exists() and dest.resolve() != source.resolve():
            dest = self._tmp_path / f"{source.stem}_{abs(hash(key)) & 0xFFFFFFFF:08x}{detected_ext}"
        try:
            shutil.copyfile(source, dest)
        except OSError:
            return str(source)
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
