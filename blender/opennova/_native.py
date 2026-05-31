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
    addon_dir = Path(__file__).resolve().parent.parent
    system = platform.system()

    if system == "Windows":
        sub, name = "windows-x64", "opennova.dll"
    elif system == "Linux":
        sub, name = "linux-x64", "libopennova.so"
    elif system == "Darwin":
        sub = "macos-arm64" if platform.machine() == "arm64" else "macos-x64"
        name = "libopennova.dylib"
    else:
        raise OSError(
            f"Unsupported platform: {system} (only Windows, Linux, and macOS are supported)"
        )

    path = addon_dir / "lib" / sub / name
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
