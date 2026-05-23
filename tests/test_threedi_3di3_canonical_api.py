from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "fixtures" / "threedi"


def test_read_model_auto_returns_3di3_for_modern_and_gp() -> None:
    from blender.opennova.threedi_ffi import free_model_3di3, read_model_auto

    paths = [
        FIXTURES / "3di3" / "Shed.3di",
        FIXTURES / "gp" / "wcrate5.3di",
    ]

    for path in paths:
        model = read_model_auto(str(path))
        try:
            assert model.header.has_header == 1
            assert model.header.name.rstrip(b"\x00")
            assert model.lod_count > 0
            assert model.material_count > 0
        finally:
            free_model_3di3(model)


def test_tdp_from_3di3_writes_project_files(tmp_path: Path) -> None:
    from blender.opennova.project_writer import write_3dp_from_3di3
    from blender.opennova.tdp_ffi import free_tdp, parse_tdp
    from blender.opennova.threedi_ffi import free_model_3di3, read_model_auto

    model = read_model_auto(str(FIXTURES / "3di3" / "Shed.3di"))
    try:
        tdp_path, tda_path = write_3dp_from_3di3(model, str(tmp_path / "Shed.3dp"))
    finally:
        free_model_3di3(model)

    assert Path(tdp_path).is_file()
    assert Path(tda_path).is_file()

    project = parse_tdp(str(tdp_path))
    try:
        assert project.material_count > 0
        assert project.lods[0].scene_file.rstrip(b"\x00") == b"Shed.ase"
    finally:
        free_tdp(project)


def test_tdp_from_3di3_can_reference_generated_bullet_lod(tmp_path: Path) -> None:
    from blender.opennova.project_writer import write_3dp_from_3di3
    from blender.opennova.tdp_ffi import TDP_MAX_LODS, free_tdp, parse_tdp
    from blender.opennova.threedi_ffi import free_model_3di3, read_model_auto

    model = read_model_auto(str(FIXTURES / "3di3" / "Shed.3di"))
    try:
        bullet_lod_index = int(model.lod_count)
        assert bullet_lod_index < TDP_MAX_LODS
        tdp_path, _ = write_3dp_from_3di3(
            model,
            str(tmp_path / "Shed.3dp"),
            collision_lod_index=bullet_lod_index,
        )
    finally:
        free_model_3di3(model)

    project = parse_tdp(str(tdp_path))
    try:
        assert project.poly_collision_lod == bullet_lod_index
        assert project.lods[bullet_lod_index].scene_file.rstrip(b"\x00") == b"Shed_bullet.ase"
        assert project.lods[bullet_lod_index].attributes == project.lods[0].attributes
        assert project.lods[bullet_lod_index].render_function == project.lods[0].render_function
    finally:
        free_tdp(project)
