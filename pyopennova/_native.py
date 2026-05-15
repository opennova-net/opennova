"""
Shared native library loader for all FFI modules.

Loads the opennova shared library (opennova.dll / libopennova.so) once
and caches the handle as a singleton.
"""

from __future__ import annotations

import ctypes
import platform
from pathlib import Path


def _lib_path() -> str:
    pkg_dir = Path(__file__).resolve().parent
    system = platform.system()

    if system == "Windows":
        sub, name = "windows-x64", "opennova.dll"
        # Prefer the build output (fresh build) if available and newer.
        build_path = pkg_dir.parent / "build" / "Debug" / name
        if build_path.is_file():
            pkg_path = pkg_dir / "lib" / sub / name
            if not pkg_path.is_file() or build_path.stat().st_mtime > pkg_path.stat().st_mtime:
                return str(build_path)
    elif system == "Linux":
        sub, name = "linux-x64", "libopennova.so"
    else:
        raise OSError(f"Unsupported platform: {system} (only Windows and Linux are supported)")

    path = pkg_dir / "lib" / sub / name
    if not path.is_file():
        raise FileNotFoundError(
            f"Native library not found at {path}. "
            f"Run scripts/package_addon.sh to build it."
        )
    return str(path)


_lib = None


def load_lib():
    """Return the cached ctypes.CDLL handle for the opennova native library."""
    global _lib
    if _lib is None:
        _lib = ctypes.CDLL(_lib_path())
    return _lib
