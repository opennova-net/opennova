"""
Unified loose-file + PFF asset resolver.

Allows pointing at a game install directory containing .pff archives instead of
requiring pre-extracted assets. Loose files still work too.

Thin wrapper over the engine-faithful native VFS (libs/vfs): mount precedence and
SCR/BFC1 decoding happen in C, so the importer and the Godot runtime resolve
byte-identical assets. Since the C FFI parsers take file paths (not buffers),
resolved files are materialized to a temporary directory.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import tempfile
from pathlib import Path

from . import gameprofile_ffi
from .vfs_ffi import Vfs

TEXTURE_STRATEGY_GENERIC = "generic"
TEXTURE_STRATEGY_3DI3_DF4OED = "3di3_df4oed"

THREEDI_SOURCE_3DI3 = 1

_TEXTURE_EXTS = (".dds", ".tga", ".png", ".mdt", ".pcx")


class AssetResolver:
    """Context manager that resolves asset filenames to filesystem paths.

    Backed by the native VFS, which mounts base_dir the way the game engine does:
    loose files in base_dir shadow archived files, and every .pff in base_dir is
    mounted (sorted) as a secondary archive. SCR-encrypted / BFC1-compressed payloads
    are decoded natively. Resolved bytes are materialized to a temp file so the C/DCC
    parsers (which take paths) can read them.

    `game` is the source game's code (e.g. "jo", "jodemo"); it selects the SCR decode key,
    since the JO Demo keys version-1 payloads differently from retail JO/DFX2. Defaults to "jo".

    Usage::

        with AssetResolver(base_dir, game="jodemo") as resolver:
            path = resolver.resolve("weapon.def")
    """

    def __init__(self, base_dir: str, game: str = "jo"):
        self.base_dir = Path(base_dir)
        self.game = game
        self._tmp_path = Path(tempfile.mkdtemp(prefix="opennova_"))

        self._vfs = Vfs()
        # No expansion -> base-game mounting (loose shadows archives; *.pff mounted
        # sorted). A non-directory base_dir leaves the VFS empty, so resolve() -> None.
        self._vfs.mount_game(str(self.base_dir))
        # Game-aware SCR keying: resolve the policy through the single C mapping so the
        # importer decodes exactly like the engine. Unknown/None code -> JO default.
        self._vfs.set_scr_policy(gameprofile_ffi.scr_policy_for_code(game))

        # Materialized temp files: lowercase logical name -> temp Path
        self._extracted: dict[str, Path] = {}

        # Track texture files copied to temp with a DCC-loadable extension.
        self._texture_paths: dict[str, Path] = {}

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def close(self):
        """Release the native VFS (and its open archive handles).

        Materialized temp files are intentionally left on disk so that Blender can
        keep referencing textures by path. The OS cleans the system temp dir on reboot.
        """
        if getattr(self, "_vfs", None) is not None:
            self._vfs.close()
            self._vfs = None

    def resolve(self, filename: str) -> str | None:
        """Resolve a filename to a filesystem path, or None if not found.

        Returns a string path suitable for passing to C FFI functions. The bytes are
        already SCR-decrypted / BFC1-decompressed by the native VFS.
        """
        if not filename:
            return None
        key = filename.lower()

        cached = self._extracted.get(key)
        if cached is not None and cached.is_file():
            return str(cached)

        if getattr(self, "_vfs", None) is None:
            return None
        data = self._vfs.read_file(filename)
        if data is None:
            return None

        dest = self._tmp_path / Path(filename).name
        dest.write_bytes(data)
        self._extracted[key] = dest
        return str(dest)

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

        The returned path is for DCC-side loading. The authored texture name
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
