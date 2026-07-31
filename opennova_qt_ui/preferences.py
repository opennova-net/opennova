"""Persistent preferences shared by embedders of the Qt importer dialog."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any


PREFERENCE_KEYS = (
    "window_geometry",
    "last_game_dir",
    "last_output_dir",
    "last_loose_file_dir",
)


def default_preferences() -> dict[str, str]:
    return {key: "" for key in PREFERENCE_KEYS}


def normalize_preferences(raw: Any) -> dict[str, str]:
    prefs = default_preferences()
    if not isinstance(raw, dict):
        return prefs
    for key in PREFERENCE_KEYS:
        value = raw.get(key)
        if value is None:
            continue
        prefs[key] = str(value).strip()
    return prefs


def load_from_path(path: Path) -> dict[str, str]:
    try:
        return normalize_preferences(json.loads(path.read_text(encoding="utf-8")))
    except Exception:
        return default_preferences()


def save_to_path(path: Path, prefs: dict[str, str]) -> bool:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(normalize_preferences(prefs), indent=2), encoding="utf-8")
        return True
    except Exception:
        return False
