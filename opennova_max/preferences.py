"""Persistent user preferences for the 3ds Max importer UI."""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any


SETTINGS_ENV_VAR = "OPENNOVA_MAX_SETTINGS_PATH"
PREFERENCE_KEYS = (
    "last_resource_dir",
    "last_output_dir",
    "last_loose_file_dir",
)


def settings_path() -> Path:
    override = os.environ.get(SETTINGS_ENV_VAR)
    if override:
        return Path(override)

    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "OpenNova" / "max_importer_settings.json"

    return Path.home() / ".opennova" / "max_importer_settings.json"


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


def load_preferences() -> dict[str, str]:
    path = settings_path()
    try:
        return normalize_preferences(json.loads(path.read_text(encoding="utf-8")))
    except Exception:
        return default_preferences()


def save_preferences(prefs: dict[str, str]) -> bool:
    path = settings_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(normalize_preferences(prefs), indent=2),
            encoding="utf-8",
        )
        return True
    except Exception:
        return False
