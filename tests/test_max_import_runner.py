from __future__ import annotations

from pathlib import Path

from opennova_jobs import ImportOptions, ImportRequest


def test_max_scene_import_support_is_enabled() -> None:
    import opennova_max.import_runner as import_runner

    assert import_runner.SUPPORTS_SCENE_IMPORT is True


def test_execute_loose_request_dispatches_real_runner(monkeypatch, tmp_path: Path) -> None:
    import opennova_max.import_runner as import_runner

    request = ImportRequest.for_loose(
        threedi_path=str(tmp_path / "Shed.3di"),
        output_root=str(tmp_path / "out"),
        output_stem="Shed",
        base_dir=str(tmp_path),
        options=ImportOptions(
            write_blend=False,
            write_max=True,
            write_ase=True,
            write_3dp=True,
        ),
    )
    calls = []

    def fake_run_loose(**kwargs):
        calls.append(kwargs)
        return [str(tmp_path / "out" / "Shed" / "Shed.max")]

    monkeypatch.setattr(import_runner, "_run_loose_import_impl", fake_run_loose)

    result = import_runner.execute_import_request(request)

    assert result.ok
    assert result.output_path == str(tmp_path / "out" / "Shed")
    assert result.written_files == [str(tmp_path / "out" / "Shed" / "Shed.max")]
    assert calls == [
        {
            "threedi_path": str(tmp_path / "Shed.3di"),
            "output_dir": str(tmp_path / "out" / "Shed"),
            "output_stem": "Shed",
            "asset_base_dir": str(tmp_path),
            "import_collisions": True,
            "import_occlusion": True,
            "import_lights": True,
            "write_max": True,
            "write_ase": True,
            "write_3dp": True,
            "reset_scene": True,
            "game": "jo",
        }
    ]


def test_execute_definition_request_dispatches_real_runner(monkeypatch, tmp_path: Path) -> None:
    import opennova_max.import_runner as import_runner

    request = ImportRequest.for_definition(
        base_dir=str(tmp_path / "game"),
        item_name="M16A2",
        item_type="weapon",
        output_root=str(tmp_path / "out"),
        output_stem="M16",
        options=ImportOptions(
            import_arms=True,
            import_animations=False,
            write_blend=False,
            write_max=True,
            write_ase=False,
            write_3dp=False,
        ),
    )
    calls = []

    def fake_run_definition(**kwargs):
        calls.append(kwargs)
        return [str(tmp_path / "out" / "M16" / "M16.max")]

    monkeypatch.setattr(import_runner, "_run_definition_import_impl", fake_run_definition)

    result = import_runner.execute_import_request(request)

    assert result.ok
    assert result.output_path == str(tmp_path / "out" / "M16")
    assert result.written_files == [str(tmp_path / "out" / "M16" / "M16.max")]
    assert calls == [
        {
            "base_dir": str(tmp_path / "game"),
            "item_name": "M16A2",
            "item_type": "weapon",
            "output_root": str(tmp_path / "out"),
            "output_stem": "M16",
            "import_arms": True,
            "import_animations": False,
            "import_collisions": True,
            "import_occlusion": True,
            "import_lights": True,
            "write_max": True,
            "write_ase": False,
            "write_3dp": False,
            "game": "jo",
        }
    ]


def test_max_write_outputs_uses_scene_exporter_for_ase(monkeypatch, tmp_path: Path) -> None:
    import opennova_max.output_writers as output_writers

    calls = []

    monkeypatch.setattr(output_writers, "save_max_scene", lambda *_args: (_ for _ in ()).throw(AssertionError))
    monkeypatch.setattr(
        output_writers,
        "export_ase_scene",
        lambda output_dir, name: calls.append(("ase", output_dir, name))
        or str(Path(output_dir) / f"{name}.ase"),
    )
    monkeypatch.setattr(
        output_writers,
        "_write_project_outputs",
        lambda *_args, **_kwargs: (_ for _ in ()).throw(AssertionError),
    )

    written = output_writers.write_outputs(
        model=None,
        resolver=None,
        output_dir=str(tmp_path / "out"),
        output_name="Shed",
        write_max=False,
        write_ase=True,
        write_3dp=False,
    )

    assert calls == [("ase", str(tmp_path / "out"), "Shed")]
    assert written == [str(tmp_path / "out" / "Shed.ase")]


def test_max_project_outputs_write_object_workspace_without_legacy_3da(tmp_path: Path) -> None:
    from opennova_max.output_writers import _write_project_outputs
    from pyopennova.threedi_ffi import free_model_3di3, read_model

    model = read_model("fixtures/threedi/3di3/Shed.3di")
    try:
        written = _write_project_outputs(model, str(tmp_path), "Shed")
    finally:
        free_model_3di3(model)

    assert written == [str(tmp_path / "Shed.3dp")]
    assert (tmp_path / "Shed.3dp").is_file()
    assert not (tmp_path / "Shed.3da").exists()
