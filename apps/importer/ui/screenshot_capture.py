"""Deterministic screenshot capture for the standalone importer UI."""
from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

from opennova_jobs import ImportJob, ImportRequest, ImportResult, ScanItem, ScanResult
from opennova_qt_ui.backend import BackendCapabilities


DEFAULT_WINDOW_SIZE = (1220, 780)
DEFAULT_OUTPUT_NAME = "importer.png"
SETTINGS_ENV = "OPENNOVA_IMPORTER_SETTINGS_PATH"


class ScreenshotBackend:
    """Static importer backend used only for documentation screenshots."""

    max_workers = 2

    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            name="Screenshot",
            supports_blend=True,
            supports_max=True,
            supports_parallel=True,
        )

    def scan(self, _base_dir: str) -> ScanResult:
        return ScanResult(ok=True, items=demo_scan_items())

    def execute(self, request: ImportRequest) -> ImportResult:
        return ImportResult.success(
            request,
            output_path=request.likely_output_dir,
            written_files=[f"{request.output_display_name}.blend", f"{request.output_display_name}.ase"],
            elapsed_seconds=3.8,
        )

    def shutdown(self) -> None:
        return None


def demo_scan_items() -> list[ScanItem]:
    return [
        ScanItem("M16A2", "weapon", "m16_1st.3di", "m16_1st"),
        ScanItem("MP5", "weapon", "mp5_1st.3di", "mp5_1st"),
        ScanItem("CAR15", "weapon", "car15_1st.3di", "car15_1st"),
        ScanItem("Armry01", "item", "Armry01.3di", "Armry01"),
        ScanItem("MH53", "item", "MH53.3di", "MH53"),
        ScanItem("Medic Pack", "item", "medpack.3di", "medpack"),
        ScanItem("Radio Tower", "item", "RdioTwr.3di", "RdioTwr"),
    ]


def build_importer_screenshot_dialog(*, version: str = ""):
    """Build a populated importer dialog without touching real game assets."""

    _ensure_isolated_settings()
    app = _ensure_qapplication()
    del app

    from opennova_qt_ui import OpenNovaImporterDialog

    backend = ScreenshotBackend()
    dialog = OpenNovaImporterDialog(backend=backend, version=version)
    dialog.resize(*DEFAULT_WINDOW_SIZE)
    dialog.game_dir_edit.setText(r"C:\Games\Joint Operations")
    dialog.output_root_edit.setText(r"C:\OpenNova\exports")
    dialog.asset_dir_edit.setText(r"C:\Games\Joint Operations")
    dialog.loose_output_edit.setText(r"C:\OpenNova\exports")
    dialog._set_loose_paths([r"C:\Games\Joint Operations\objects\MH53.3di"])

    if dialog.max_check is not None:
        dialog.max_check.setChecked(True)
    if dialog.glb_check is not None:
        dialog.glb_check.setChecked(True)

    dialog._all_items = demo_scan_items()
    dialog._apply_filters("Scanned")
    if dialog.resource_table.rowCount() > 0:
        dialog.resource_table.selectRow(0)

    options = dialog._build_options()
    done_request = ImportRequest.for_definition(
        base_dir=dialog.game_dir_edit.text(),
        item_name="M16A2",
        item_type="weapon",
        output_root=dialog.output_root_edit.text(),
        output_stem="m16_1st",
        options=options,
    )
    done_job = ImportJob(request=done_request)
    done_job.finish(
        ImportResult.success(
            done_request,
            output_path=done_request.likely_output_dir,
            written_files=["m16_1st.blend", "m16_1st.ase", "m16_1st.3dp"],
            elapsed_seconds=4.2,
        )
    )

    running_request = ImportRequest.for_definition(
        base_dir=dialog.game_dir_edit.text(),
        item_name="Armry01",
        item_type="item",
        output_root=dialog.output_root_edit.text(),
        output_stem="Armry01",
        options=options,
    )
    running_job = ImportJob(request=running_request)
    running_job.mark_running()

    failed_request = ImportRequest.for_loose(
        threedi_path=r"C:\Games\Joint Operations\objects\broken.3di",
        output_root=dialog.output_root_edit.text(),
        base_dir=dialog.asset_dir_edit.text(),
        options=options,
    )
    failed_job = ImportJob(request=failed_request)
    failed_job.finish(
        ImportResult.failure(
            failed_request,
            error="Missing diffuse texture: broken_d.tga",
            output_path=failed_request.likely_output_dir,
            elapsed_seconds=1.1,
        )
    )

    dialog._jobs = [done_job, running_job, failed_job]
    dialog._log_append("Scanned 7 resource(s).")
    dialog._log_append("Queued 3 job(s).")
    dialog._log_append("Done: weapon M16A2", job_id=done_job.id)
    dialog._log_append(
        "FAILED: loose broken: Missing diffuse texture: broken_d.tga",
        level="error",
        job_id=failed_job.id,
    )
    dialog._refresh_queue()
    dialog._update_action_states()
    return dialog


def capture_importer_screenshot(
    output_path: str | Path | None = None,
    *,
    version: str = "",
) -> Path:
    app = _ensure_qapplication()
    out_path = Path(output_path) if output_path is not None else default_output_path()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    dialog = build_importer_screenshot_dialog(version=version)
    try:
        dialog.show()
        for _index in range(8):
            app.processEvents()
        pixmap = dialog.grab()
        if pixmap.isNull():
            raise RuntimeError("importer screenshot grab returned a null pixmap")
        if not pixmap.save(str(out_path), "PNG"):
            raise RuntimeError(f"could not save importer screenshot to {out_path}")
        return out_path
    finally:
        dialog.close()
        app.processEvents()


def default_output_path() -> Path:
    return Path(__file__).resolve().parents[3] / "screenshots" / DEFAULT_OUTPUT_NAME


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Capture the OpenNova importer UI screenshot.")
    parser.add_argument("--output", type=Path, default=default_output_path())
    parser.add_argument("--version", default=_version_string())
    args = parser.parse_args(argv)

    path = capture_importer_screenshot(args.output, version=args.version)
    print(f"Importer screenshot written to {path}")
    return 0


def _ensure_qapplication():
    if _should_force_offscreen_platform():
        os.environ["QT_QPA_PLATFORM"] = "offscreen"
    from PySide6 import QtWidgets

    return QtWidgets.QApplication.instance() or QtWidgets.QApplication(sys.argv[:1])


def _should_force_offscreen_platform() -> bool:
    if os.environ.get("QT_QPA_PLATFORM"):
        return False
    if not sys.platform.startswith("linux"):
        return False
    return not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


def _ensure_isolated_settings() -> None:
    if SETTINGS_ENV not in os.environ:
        os.environ[SETTINGS_ENV] = str(
            Path(tempfile.gettempdir()) / "opennova-importer-screenshot-settings.json"
        )


def _version_string() -> str:
    try:
        from importlib.metadata import version

        return version("opennova-tools")
    except Exception:
        return ""


if __name__ == "__main__":
    raise SystemExit(main())
