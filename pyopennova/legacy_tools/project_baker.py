"""OpenNova `.3dp` + `.ase` to JO `.3di` bake entrypoint."""

from __future__ import annotations

from pathlib import Path

from pyopennova import object_bake_ffi, threedi_ffi


def bake_jo_project(
    project_path: str | Path,
    output_path: str | Path,
    *,
    model_name: str | None = None,
    validate: bool = True,
) -> Path:
    """Bake a JO project through OpenNova's object exporter.

    This is the OpenNova side of the original-tool acceptance harness. It
    parses the `.3dp`, resolves every referenced LOD ASE, bakes a `.3di`, and
    optionally validates that the result is readable by the native 3DI parser.
    """
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    object_bake_ffi.export_project(
        project_path,
        output_path,
        model_name=model_name,
        update_mask=object_bake_ffi.BAKE_UPDATE_ALL,
    )

    if validate:
        ir = threedi_ffi.read_model_3di3(str(output_path))
        threedi_ffi.free_model_3di3(ir)
    return output_path
