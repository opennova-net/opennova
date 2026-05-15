"""Persistent user preferences for the 3ds Max importer UI."""
from __future__ import annotations

import os
from pathlib import Path

from opennova_qt_ui.preferences import (
    PREFERENCE_KEYS,
    default_preferences,
    load_from_path,
    normalize_preferences,
    save_to_path,
)


SETTINGS_ENV_VAR = "OPENNOVA_MAX_SETTINGS_PATH"


def settings_path() -> Path:
    override = os.environ.get(SETTINGS_ENV_VAR)
    if override:
        return Path(override)
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "OpenNova" / "max_importer_settings.json"
    return Path.home() / ".opennova" / "max_importer_settings.json"


def load_preferences() -> dict[str, str]:
    return load_from_path(settings_path())


def save_preferences(prefs: dict[str, str]) -> bool:
    return save_to_path(settings_path(), prefs)


__all__ = [
    "PREFERENCE_KEYS",
    "SETTINGS_ENV_VAR",
    "default_preferences",
    "load_preferences",
    "normalize_preferences",
    "save_preferences",
    "settings_path",
]
