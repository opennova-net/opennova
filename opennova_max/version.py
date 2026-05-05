"""Version helpers for the 3ds Max plugin."""
from __future__ import annotations

from pathlib import Path


PACKAGE_NAME = "opennova-tools"
UNKNOWN_VERSION = "unknown"


def get_version() -> str:
    """Return the plugin version for source and packaged installs."""
    for resolver in (_packaged_version, _source_version, _metadata_version):
        version = resolver()
        if version:
            return version
    return UNKNOWN_VERSION


def _packaged_version() -> str:
    try:
        from ._packaged_version import VERSION
    except Exception:
        return ""
    return str(VERSION).strip()


def _metadata_version() -> str:
    try:
        from importlib import metadata
    except ImportError:
        try:
            import importlib_metadata as metadata  # type: ignore
        except Exception:
            return ""

    try:
        return str(metadata.version(PACKAGE_NAME)).strip()
    except Exception:
        return ""


def _source_version() -> str:
    try:
        parents = Path(__file__).resolve().parents
    except Exception:
        return ""

    for parent in parents:
        pyproject = parent / "pyproject.toml"
        if not pyproject.is_file():
            continue
        try:
            for line in pyproject.read_text(encoding="utf-8").splitlines():
                stripped = line.strip()
                if stripped.startswith("version") and "=" in stripped:
                    return stripped.split("=", 1)[1].strip().strip('"').strip("'")
        except Exception:
            return ""
    return ""


__version__ = get_version()
