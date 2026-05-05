"""Parity checks for shared host-neutral DCC outputs."""
from __future__ import annotations

from pathlib import Path

import pytest

from apps.importer.jobs import ImportOptions, ImportRequest


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"
HOST_OUTPUT_SUFFIXES = {".3dp", ".3da", ".ase"}


def test_blender_importer_writes_shared_host_neutral_outputs(tmp_path: Path) -> None:
    """Blender importer game outputs must be the shared IR writer outputs."""
    if not FIXTURE_3DI.is_file():
        pytest.skip("fixture missing (LFS not pulled?)")

    blender_dir = _write_blender_host_outputs(tmp_path / "blender")
    shared_dir = _write_shared_host_outputs(tmp_path / "shared")

    blender_outputs = _host_output_bytes(blender_dir)
    shared_outputs = _host_output_bytes(shared_dir)

    assert {"Shed.3dp", "Shed.3da", "Shed.ase", "Shed_bullet.ase"} <= blender_outputs.keys()
    assert blender_outputs == shared_outputs


def _write_blender_host_outputs(output_root: Path) -> Path:
    from apps.importer.import_runner import execute_import_request

    options = ImportOptions(
        write_blend=False,
        write_3dp=True,
        write_ase=True,
        write_glb=False,
        write_fbx=False,
    )
    request = ImportRequest.for_loose(
        threedi_path=str(FIXTURE_3DI),
        output_root=str(output_root),
        options=options,
    )
    result = execute_import_request(request)
    if not result.ok and "No module named 'bpy'" in result.error:
        pytest.skip("bpy is not installed in this environment")
    if not result.ok and "No module named '_bpy'" in result.error:
        pytest.skip("bpy native module is not importable in this environment")
    assert result.ok, result.error
    return Path(result.output_path)


def _write_shared_host_outputs(output_root: Path) -> Path:
    from pyopennova.host_outputs import write_host_neutral_outputs
    from pyopennova.threedi_ffi import free_model_ir, read_model_ir

    output_dir = output_root / "Shed"
    output_dir.mkdir(parents=True)
    ir = read_model_ir(str(FIXTURE_3DI))
    try:
        write_host_neutral_outputs(ir, str(output_dir), "Shed")
    finally:
        free_model_ir(ir)
    return output_dir


def _host_output_bytes(output_dir: Path) -> dict[str, bytes]:
    return {
        path.name: path.read_bytes()
        for path in sorted(output_dir.iterdir())
        if path.is_file() and path.suffix.lower() in HOST_OUTPUT_SUFFIXES
    }
