"""Discovery helpers for Autodesk 3ds Max batch execution."""
from __future__ import annotations

import os
from pathlib import Path
from typing import Iterable, Optional


ENV_3DSMAXBATCH = "OPENNOVA_3DSMAXBATCH"


def resolve_3dsmaxbatch(search_roots=None):
    # type: (Optional[Iterable[Path]]) -> Optional[Path]
    """Return a usable ``3dsmaxbatch.exe`` path, or ``None`` when unavailable."""
    env_path = os.environ.get(ENV_3DSMAXBATCH, "").strip().strip('"')
    if env_path:
        candidate = Path(env_path)
        if candidate.is_file():
            return candidate

    roots = tuple(search_roots) if search_roots is not None else _default_search_roots()
    candidates = []
    for root in roots:
        root_path = Path(root)
        if root_path.is_file() and root_path.name.lower() == "3dsmaxbatch.exe":
            candidates.append(root_path)
        elif root_path.is_dir():
            candidates.extend(root_path.glob("3ds Max */3dsmaxbatch.exe"))
    for candidate in sorted(candidates, reverse=True):
        if candidate.is_file():
            return candidate
    return None


def _default_search_roots():
    # type: () -> tuple[Path, ...]
    return (
        Path(r"C:\Program Files\Autodesk"),
        Path(r"C:\Program Files (x86)\Autodesk"),
    )
