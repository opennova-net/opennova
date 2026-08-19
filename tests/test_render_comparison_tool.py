import json
import subprocess
from pathlib import Path

import pytest

try:
    from PySide6.QtGui import QColor, QImage
except ImportError as exc:  # headless box without libGL/Qt
    pytest.skip(f"PySide6 QtGui is unavailable: {exc}", allow_module_level=True)


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "render" / "build_render_comparison.ps1"


def _solid(
    path: Path,
    color: tuple[int, int, int],
    size: tuple[int, int] = (160, 90),
) -> None:
    image = QImage(size[0], size[1], QImage.Format.Format_RGB32)
    image.fill(QColor(*color))
    assert image.save(str(path), "PNG")


def test_comparison_tool_builds_four_panel_sheet_heatmap_and_metrics(tmp_path: Path) -> None:
    retail = tmp_path / "retail.png"
    before = tmp_path / "before.png"
    after = tmp_path / "after.png"
    sheet = tmp_path / "sheet.png"
    heatmap = tmp_path / "heatmap.png"
    metrics = tmp_path / "metrics.json"
    _solid(retail, (10, 20, 30))
    _solid(before, (50, 60, 70))
    _solid(after, (11, 22, 33))

    result = subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(SCRIPT),
            "-FixtureId",
            "fixture-one",
            "-RetailImage",
            str(retail),
            "-BeforeImage",
            str(before),
            "-AfterImage",
            str(after),
            "-OutputImage",
            str(sheet),
            "-HeatmapImage",
            str(heatmap),
            "-MetricsJson",
            str(metrics),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr

    sheet_image = QImage(str(sheet))
    heatmap_image = QImage(str(heatmap))
    assert (sheet_image.width(), sheet_image.height()) == (1600, 984)
    assert (heatmap_image.width(), heatmap_image.height()) == (160, 90)

    report = json.loads(metrics.read_text(encoding="utf-8-sig"))
    assert report["schema"] == "opennova.render-comparison.v1"
    assert report["fixture_id"] == "fixture-one"
    assert report["comparison_mode"] == "retail-parity"
    assert report["labels"] == {
        "reference": "RETAIL",
        "before": "OPENNOVA - PRE-FIX",
        "after": "OPENNOVA - FINAL",
        "heatmap": "OPENNOVA - FINAL vs RETAIL heatmap",
    }
    assert report["comparison_size"] == {"width": 160, "height": 90}
    assert report["resampled"] is False
    assert report["source_sizes"] == {
        "retail": {"width": 160, "height": 90},
        "before": {"width": 160, "height": 90},
        "after": {"width": 160, "height": 90},
    }
    assert report["retail_vs_before"]["max_channel_delta"] == 40
    assert report["retail_vs_after"]["max_channel_delta"] == 3
    assert report["retail_vs_before"]["mean_abs_channel_delta"] == 40.0
    assert report["retail_vs_after"]["mean_abs_channel_delta"] == 2.0
    for image in ("retail", "before", "after", "sheet", "heatmap"):
        assert len(report["sha256"][image]) == 64


def test_comparison_tool_records_subsystem_ab_roles(tmp_path: Path) -> None:
    reference = tmp_path / "off.png"
    lighting = tmp_path / "lighting.png"
    beauty = tmp_path / "beauty.png"
    metrics = tmp_path / "metrics.json"
    _solid(reference, (10, 20, 30))
    _solid(lighting, (50, 60, 70))
    _solid(beauty, (11, 22, 33))

    result = subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(SCRIPT),
            "-FixtureId",
            "shadow-ab",
            "-RetailImage",
            str(reference),
            "-BeforeImage",
            str(lighting),
            "-AfterImage",
            str(beauty),
            "-OutputImage",
            str(tmp_path / "sheet.png"),
            "-HeatmapImage",
            str(tmp_path / "heatmap.png"),
            "-MetricsJson",
            str(metrics),
            "-ComparisonMode",
            "subsystem-ab",
            "-RetailLabel",
            "SHADOWS OFF",
            "-BeforeLabel",
            "LIGHTING ONLY",
            "-AfterLabel",
            "BEAUTY / SHADOWS ON",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    report = json.loads(metrics.read_text(encoding="utf-8-sig"))
    assert report["comparison_mode"] == "subsystem-ab"
    assert report["labels"] == {
        "reference": "SHADOWS OFF",
        "before": "LIGHTING ONLY",
        "after": "BEAUTY / SHADOWS ON",
        "heatmap": "BEAUTY / SHADOWS ON vs SHADOWS OFF heatmap",
    }


def test_comparison_tool_rejects_resolution_normalization(tmp_path: Path) -> None:
    retail = tmp_path / "retail.png"
    before = tmp_path / "before.png"
    after = tmp_path / "after.png"
    _solid(retail, (10, 20, 30))
    _solid(before, (50, 60, 70), (320, 180))
    _solid(after, (11, 22, 33))

    result = subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(SCRIPT),
            "-FixtureId",
            "mismatched",
            "-RetailImage",
            str(retail),
            "-BeforeImage",
            str(before),
            "-AfterImage",
            str(after),
            "-OutputImage",
            str(tmp_path / "sheet.png"),
            "-HeatmapImage",
            str(tmp_path / "heatmap.png"),
            "-MetricsJson",
            str(tmp_path / "metrics.json"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "identical dimensions" in result.stderr
    assert not (tmp_path / "metrics.json").exists()
