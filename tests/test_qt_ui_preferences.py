"""Tests for opennova_qt_ui.preferences."""
from __future__ import annotations

import json
from pathlib import Path

import pytest


def test_default_preferences_has_required_keys() -> None:
    from opennova_qt_ui.preferences import default_preferences

    prefs = default_preferences()
    assert prefs == {
        "last_resource_dir": "",
        "last_output_dir": "",
        "last_loose_file_dir": "",
    }


def test_normalize_preferences_with_strings() -> None:
    from opennova_qt_ui.preferences import normalize_preferences

    raw = {"last_resource_dir": " /game ", "last_output_dir": "/out"}
    prefs = normalize_preferences(raw)
    assert prefs["last_resource_dir"] == "/game"
    assert prefs["last_output_dir"] == "/out"
    assert prefs["last_loose_file_dir"] == ""


def test_normalize_preferences_ignores_non_dict() -> None:
    from opennova_qt_ui.preferences import normalize_preferences

    assert normalize_preferences(None) == {
        "last_resource_dir": "",
        "last_output_dir": "",
        "last_loose_file_dir": "",
    }


def test_load_save_roundtrip(tmp_path: Path) -> None:
    from opennova_qt_ui.preferences import load_from_path, save_to_path

    target = tmp_path / "prefs.json"
    sample = {"last_resource_dir": "/x", "last_output_dir": "/y", "last_loose_file_dir": "/z"}
    save_to_path(target, sample)

    out = load_from_path(target)
    assert out == sample


def test_load_from_path_missing_returns_defaults(tmp_path: Path) -> None:
    from opennova_qt_ui.preferences import load_from_path

    out = load_from_path(tmp_path / "nope.json")
    assert out == {
        "last_resource_dir": "",
        "last_output_dir": "",
        "last_loose_file_dir": "",
    }
