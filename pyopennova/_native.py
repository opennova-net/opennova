"""Native library loader shared by the standalone DCC backends."""
from __future__ import annotations

import ctypes
import os
import platform
from pathlib import Path


def _candidate_paths() -> list[Path]:
    root = Path(__file__).resolve().parent.parent
    system = platform.system()
    if system == "Windows":
        subdir, name = "windows-x64", "opennova.dll"
    elif system == "Linux":
        subdir, name = "linux-x64", "libopennova.so"
    elif system == "Darwin":
        subdir = "macos-arm64" if platform.machine() == "arm64" else "macos-x64"
        name = "libopennova.dylib"
    else:
        raise OSError(
            f"Unsupported platform: {system} (only Windows, Linux, and macOS are supported)"
        )

    paths: list[Path] = []
    override = os.environ.get("OPENNOVA_NATIVE_LIBRARY", "").strip()
    if override:
        paths.append(Path(override))
    paths.extend(
        [
            root / "build" / "Debug" / name,
            root / "build" / "Release" / name,
            root / "blender" / "lib" / subdir / name,
            root / "pyopennova" / "lib" / subdir / name,
            root / "lib" / subdir / name,
        ]
    )
    return paths


def _lib_path() -> str:
    for path in _candidate_paths():
        if path.is_file():
            return str(path)
    searched = "\n  ".join(str(path) for path in _candidate_paths())
    raise FileNotFoundError(
        "Native library not found. Searched:\n  %s\nRun scripts/build.sh or package the addon first."
        % searched
    )


_lib = None


def load_lib():
    """Return the cached ctypes.CDLL handle for the OpenNova native library."""
    global _lib
    if _lib is None:
        _lib = ctypes.CDLL(_lib_path())
    return _lib
