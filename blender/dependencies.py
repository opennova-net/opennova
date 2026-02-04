"""
Dependency management for Blender addon native library check.
"""

from __future__ import annotations


def check_native_library():
    """Check if the native opennova shared library is available."""
    try:
        from .opennova._native import _lib_path
        path = _lib_path()
        return True, path
    except (FileNotFoundError, OSError) as e:
        return False, str(e)
