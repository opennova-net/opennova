"""Tests for pyopennova.scan.scan_definitions."""
from __future__ import annotations

from pathlib import Path

import pytest

from opennova_jobs import ScanResult


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DEF_DIR = ROOT / "fixtures" / "def"


def test_scan_definitions_empty_path() -> None:
    from pyopennova.scan import scan_definitions

    result = scan_definitions("")
    assert isinstance(result, ScanResult)
    assert not result.ok
    assert "Game directory is required" in result.error


def test_scan_definitions_missing_path(tmp_path: Path) -> None:
    from pyopennova.scan import scan_definitions

    target = tmp_path / "does-not-exist"
    result = scan_definitions(str(target))
    assert not result.ok
    assert "does not exist" in result.error


@pytest.mark.skipif(
    not FIXTURE_DEF_DIR.exists(),
    reason="fixtures/def not present in this checkout",
)
def test_scan_definitions_fixture_directory() -> None:
    from pyopennova.scan import scan_definitions

    result = scan_definitions(str(FIXTURE_DEF_DIR))
    assert result.ok
    assert result.items, "fixture should produce at least one ScanItem"
    types = {item.type for item in result.items}
    assert types <= {"weapon", "item"}
    for item in result.items:
        assert item.name
        assert item.source_model.endswith(".3di") or not item.source_model
        assert item.output_stem
