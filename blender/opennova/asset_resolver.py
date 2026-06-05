"""
Unified loose-file + PFF asset resolver.

Allows pointing at a game install directory containing .pff archives instead of
requiring pre-extracted assets. Loose files still work too.

Thin wrapper over the engine-faithful native VFS (libs/vfs): mount precedence and
SCR/BFC1 decoding happen in C, so Blender and the importer resolve byte-identical
assets. Since the C FFI parsers take file paths (not buffers), resolved files are
materialized to a temporary directory.
"""

from __future__ import annotations

import tempfile
from pathlib import Path

from . import gameprofile_ffi
from .vfs_ffi import Vfs


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
        # Game-aware SCR keying: resolve the policy through the single C mapping so Blender
        # decodes exactly like the engine. Unknown/None code -> JO default.
        self._vfs.set_scr_policy(gameprofile_ffi.scr_policy_for_code(game))

        # Materialized temp files: lowercase logical name -> temp Path
        self._extracted: dict[str, Path] = {}

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
