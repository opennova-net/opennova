from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

from apps.importer.jobs import ImportOptions, ImportRequest
from tests.dcc_ase_assertions import assert_oed_render_mesh_names


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"


def test_generate_source_from_3di_writes_oed_source_without_blender(tmp_path: Path) -> None:
    from apps.importer.source_generator import generate_source_from_3di

    generated = generate_source_from_3di(
        FIXTURE_3DI,
        tmp_path / "source",
        output_stem="Shed",
        write_3dp=True,
        write_ase=True,
    )

    assert generated.project_path == tmp_path / "source" / "Shed.3dp"
    assert generated.project_path.is_file()
    assert (tmp_path / "source" / "Shed.3da").is_file()
    assert generated.ase_paths
    assert generated.ase_paths[0] == tmp_path / "source" / "Shed.ase"
    assert_oed_render_mesh_names(generated.ase_paths)


def test_loose_source_only_worker_does_not_initialize_bpy(tmp_path: Path) -> None:
    from apps.importer.worker import run_one

    request = ImportRequest.for_loose(
        threedi_path=str(FIXTURE_3DI),
        output_root=str(tmp_path),
        output_stem="Shed",
        options=ImportOptions(
            write_blend=False,
            write_3dp=True,
            write_ase=True,
            write_glb=False,
            write_fbx=False,
        ),
    )

    with patch(
        "apps.importer.bpy_session.init_headless",
        side_effect=AssertionError("bpy should not initialize for source-only exports"),
    ):
        result = run_one(request)

    assert result.ok, result.error
    assert result.output_path == str(tmp_path / "Shed")
    assert Path(result.output_path, "Shed.3dp").is_file()
    assert Path(result.output_path, "Shed.ase").is_file()
