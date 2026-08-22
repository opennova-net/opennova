#!/usr/bin/env python
"""Retake fixtures for ITERATION: capture OpenNova only, compare to published retail.

The fast loop while working on a render divergence. The registered retail
frames do not change when our source changes, so re-shooting one OpenNova
fixture (~90 s, windowed) and comparing it against the already-published
retail PNG answers "did my change move this fixture toward retail?" without
staging retail, registering anything, or sweeping every fixture.

The output is deliberately NOT evidence: it lives under ``.scratch/retake``,
its caption carries the working commit (plus ``+dirty`` when the tree is not
clean), and nothing under ``screenshots/`` is touched. Replacing the published
sheets still requires the full publication flow in
docs/render/render-parity-runbook.md.

Metrics reuse the registered comparison builder's own normalization
(``normalize_opennova_for_retail.ps1``) and difference math
(``_difference_metrics``), so the printed published-vs-retake MAE deltas are
apples-to-apples with the publication's numbers.

Usage::

    uv run python scripts/render/retake_fixture.py 00tra-tire-marks-retail

Must run in the FOREGROUND of an interactive desktop session (the probe fails
closed from a detached shell). Machine paths come from the same environment
variables the capture sweep uses (``NOVA_MISSION_RESOURCE_DIR``,
``NOVA_RUNTIME_RESOURCE_DIR``, optionally ``GODOT_BIN``).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    from scripts.render import build_retail_side_by_side as builder
except ImportError:  # invoked from scripts/render directly
    import build_retail_side_by_side as builder  # type: ignore[no-redef]

DEFAULT_CATALOG = "docs/render/render-fixtures-retail-v5.json"
DEFAULT_PUBLISHED = (
    "screenshots/parity/render-lighting-2026-08/registered-2026-08-20"
)
RETAIL_SIZE = (1920, 1200)


class RetakeError(RuntimeError):
    """A fail-closed retake error."""


def run(args: list[str], **kwargs) -> str:
    return subprocess.run(
        args, check=True, capture_output=True, text=True, **kwargs
    ).stdout.strip()


def repo_root() -> Path:
    return Path(run(["git", "rev-parse", "--show-toplevel"]))


def godot_bin(repo: Path) -> Path:
    override = os.environ.get("GODOT_BIN")
    if override:
        return Path(override)
    common = Path(run(
        ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
        cwd=repo,
    ))
    return common.parent / ".godot-bin" / "Godot_v4.6.1-stable_win64.exe"


def capture(repo: Path, catalog_rel: str, fixture_id: str, commit: str,
            out_dir: Path) -> None:
    """Run the exact-pose probe for one fixture into ``out_dir``."""
    for var in ("NOVA_MISSION_RESOURCE_DIR", "NOVA_RUNTIME_RESOURCE_DIR"):
        if not os.environ.get(var):
            raise RetakeError(f"{var} must be set (see the runbook)")
    dll = repo / "godot" / "bin" / "libopennova.windows.template_debug.x86_64.dll"
    if not dll.is_file():
        raise RetakeError(f"GDExtension not built: {dll}")
    binary = godot_bin(repo)
    if not binary.is_file():
        raise RetakeError(f"Godot binary not found: {binary}")

    if out_dir.exists():
        shutil.rmtree(out_dir)
    transaction = out_dir.with_name(out_dir.name + ".publication-transaction")
    if transaction.exists():
        shutil.rmtree(transaction)

    env = os.environ.copy()
    env.update({
        "NOVA_EVIDENCE_SOURCE_COMMIT": commit,
        "NOVA_GDEXTENSION_BINARY": str(dll),
        "NOVA_RENDER_FIXTURE_CATALOG":
            "res://../" + catalog_rel.replace("\\", "/"),
        "NOVA_RENDER_CAPTURE_MODE": "hud_hidden",
        "NOVA_EXPANSION": env.get("NOVA_EXPANSION", "revx02"),
        "NOVA_RENDER_FIXTURE_ID": fixture_id,
        "NOVA_RENDER_FIXTURE_OUTPUT": str(out_dir),
    })
    subprocess.run(
        [str(binary), "--path", "godot", "--resolution", "2000x1200",
         "res://tests/render_fixture_capture_probe.tscn"],
        cwd=repo, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    )
    files = list(out_dir.glob("*")) if out_dir.is_dir() else []
    if len(files) != 11:
        raise RetakeError(
            f"{fixture_id}: probe left {len(files)} files, expected 11 -- "
            "run from an interactive desktop session and check the witness"
        )


def beauty_path(out_dir: Path, fixture_id: str) -> Path:
    manifest = json.loads(
        (out_dir / f"{fixture_id}-manifest.json").read_text(encoding="utf-8-sig")
    )
    entry = next(a for a in manifest["artifacts"] if a["variant"] == "beauty")
    return out_dir / entry["png_path"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixtures", nargs="+")
    parser.add_argument("--catalog", default=DEFAULT_CATALOG)
    parser.add_argument("--published-root", default=DEFAULT_PUBLISHED)
    parser.add_argument(
        "--skip-capture", action="store_true",
        help="reuse the existing .scratch/retake capture (compare only)",
    )
    args = parser.parse_args(argv)

    repo = repo_root()
    published_root = repo / args.published_root
    commit = run(["git", "rev-parse", "HEAD"], cwd=repo)
    dirty = bool(run(["git", "status", "--porcelain"], cwd=repo))
    tag = commit[:9] + ("+dirty" if dirty else "")
    retake_root = repo / ".scratch" / "retake"

    from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QPainter
    QGuiApplication([])

    for fixture_id in args.fixtures:
        pub = published_root / fixture_id
        comparison_path = pub / "comparison" / f"{fixture_id}-comparison.json"
        if not comparison_path.is_file():
            raise RetakeError(
                f"{fixture_id} has no published comparison at {comparison_path}"
            )
        comparison = json.loads(comparison_path.read_text(encoding="utf-8-sig"))
        retail_png = pub / "retail" / "retail.png"
        retail = QImage(str(retail_png))
        if (retail.width(), retail.height()) != RETAIL_SIZE:
            raise RetakeError(
                f"published retail frame unreadable (LFS stub?): {retail_png}"
            )

        out_dir = retake_root / "opennova" / fixture_id
        if not args.skip_capture:
            print(f"capturing {fixture_id} at {tag} ...")
            capture(repo, args.catalog, fixture_id, commit, out_dir)

        # The registered builder's own normalization, so metrics line up.
        normalized_path = retake_root / f"{fixture_id}-normalized.png"
        if normalized_path.exists():
            normalized_path.unlink()
        builder._normalize(beauty_path(out_dir, fixture_id), normalized_path)
        nova = QImage(str(normalized_path)).convertToFormat(
            QImage.Format.Format_ARGB32_Premultiplied
        )
        reference = retail.convertToFormat(
            QImage.Format.Format_ARGB32_Premultiplied
        )
        difference = reference.copy()
        painter = QPainter(difference)
        try:
            painter.setCompositionMode(
                QPainter.CompositionMode.CompositionMode_Difference
            )
            painter.drawImage(0, 0, nova)
        finally:
            painter.end()

        regions: dict[str, tuple[int, int, int, int]] = {
            "full_frame": (0, 0, *RETAIL_SIZE),
        }
        published_mae = {
            "full_frame":
                comparison["metrics"]["full_frame"]["mean_abs_channel_delta"],
        }
        for entry in comparison["metrics"].get("regions", []):
            regions[entry["name"]] = tuple(entry["region"])
            published_mae[entry["name"]] = entry["mean_abs_channel_delta"]

        print(f"{fixture_id}  published -> retake MAE (lower = closer to retail)")
        for name, rect in regions.items():
            fresh = builder._difference_metrics(difference, rect)
            new_value = fresh["mean_abs_channel_delta"]
            old_value = published_mae[name]
            delta = new_value - old_value
            # Foliage sway / particle phase moves a same-commit retake by a
            # few tenths; only call movement beyond that band.
            if abs(delta) <= 0.5:
                verdict = "~ within capture noise"
            elif delta < 0:
                verdict = f"improved by {-delta:.4f}"
            else:
                verdict = f"WORSE by {delta:.4f}"
            print(f"  {name:<16} {old_value:10.4f} -> {new_value:10.4f}  "
                  f"{verdict}")

        sheet = QImage(RETAIL_SIZE[0] * 2, RETAIL_SIZE[1] + 72,
                       QImage.Format.Format_RGB32)
        sheet.fill(QColor(12, 12, 12))
        painter = QPainter(sheet)
        painter.drawImage(0, 72, nova)
        painter.drawImage(RETAIL_SIZE[0], 72, reference)
        font = QFont("Consolas")
        font.setPixelSize(28)
        painter.setFont(font)
        painter.setPen(QColor(255, 170, 60))
        painter.drawText(
            24, 46,
            f"OPENNOVA ITERATION {tag} - NOT EVIDENCE  {fixture_id}",
        )
        painter.setPen(QColor(180, 220, 255))
        painter.drawText(RETAIL_SIZE[0] + 24, 46,
                         "RETAIL (published, registered)")
        painter.end()
        sheet_path = retake_root / f"{fixture_id}-side-by-side.png"
        sheet_path.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(str(sheet_path))
        print(f"  sheet: {sheet_path}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RetakeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
