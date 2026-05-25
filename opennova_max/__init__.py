"""External 3ds Max backend helpers for OpenNova."""
from __future__ import annotations

__all__ = ["MaxBatchRunner", "get_version", "register_menu", "resolve_3dsmaxbatch"]

from .discovery import resolve_3dsmaxbatch
from .runner import MaxBatchRunner


def register_menu():
    # type: () -> None
    """Install the OpenNova menu hooks inside a running 3ds Max session."""
    from .ui import install_menu

    install_menu()


def get_version():
    # type: () -> str
    """Return the packaged plugin version when installed from an MZP."""
    try:
        from ._packaged_version import VERSION
    except Exception:
        return "0.0.0"
    return VERSION
