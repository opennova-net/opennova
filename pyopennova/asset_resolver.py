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
from pathlib import Path

from .bfc1_ffi import is_bfc1, decompress as bfc1_decompress
from .pff_ffi import PffArchive
from .scr_ffi import is_scr, get_version, decrypt, SCR_KEY_DEFAULT, SCR_KEY_JO_DFX2, SCR_KEY_SHADERS

_SCR_VERSION_KEYS = {
    0: SCR_KEY_DEFAULT,
    1: SCR_KEY_JO_DFX2,
    2: SCR_KEY_SHADERS,
}

TEXTURE_STRATEGY_GENERIC = "generic"
TEXTURE_STRATEGY_3DI3_DF4OED = "3di3_df4oed"

THREEDI_IR_SOURCE_3DI3 = 1

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
        remains owned by the material IR.
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
    if source_format == THREEDI_IR_SOURCE_3DI3:
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
