from __future__ import annotations

import sys
import types
from pathlib import Path
from unittest.mock import Mock, patch

from apps.importer.cli import build_parser, options_from_args
from apps.importer.import_runner import (
    scan_directory,
    scan_directory_result,
)
from opennova_jobs import (
    JOB_ERROR,
    JOB_PENDING,
    JOB_RUNNING,
    ImportJob,
    ImportOptions,
    ImportRequest,
    ScanItem,
    has_active_duplicate,
    validate_import_request,
)


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DEF_DIR = ROOT / "fixtures" / "def"
FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"


class TestImportOptions:
    def test_defaults_match_gui_baseline(self) -> None:
        options = ImportOptions()
        assert options.import_animations
        assert options.import_collisions
        assert options.import_occlusion
        assert options.import_lights
        assert options.import_arms
        assert options.write_blend
        assert options.write_3dp
        assert options.write_ase
        assert not options.write_max
        assert not options.write_glb
        assert not options.write_fbx

    def test_cli_options_override_defaults(self) -> None:
        parser = build_parser()
        args = parser.parse_args([
            "import",
            "--dir",
            "game",
            "--item",
            "M16",
            "--type",
            "weapon",
            "--output",
            "out",
            "--no-occlusion",
            "--no-arms",
            "--no-3dp",
            "--max",
            "--glb",
            "--fbx",
        ])
        options = options_from_args(args)
        assert not options.import_occlusion
        assert not options.import_arms
        assert options.write_blend
        assert not options.write_3dp
        assert options.write_ase
        assert options.write_max
        assert options.write_glb
        assert options.write_fbx

    def test_blender_only_options_write_only_blend(self) -> None:
        options = ImportOptions(write_3dp=False, write_ase=False)
        assert options.write_blend
        assert options.writes_any_output_file()
        assert options.writes_any_export_format()
        assert options.import_collisions

    def test_max_only_options_are_a_native_scene_export(self) -> None:
        options = ImportOptions(write_blend=False, write_3dp=False, write_ase=False, write_max=True)
        assert not options.write_blend
        assert not options.write_3dp
        assert not options.write_ase
        assert options.write_max
        assert options.writes_any_export_format()
        assert options.ase_export_owner() == ""

    def test_ase_export_owner_prefers_blender_over_max(self) -> None:
        assert ImportOptions(write_ase=True, write_blend=True, write_max=True).ase_export_owner() == "blender"
        assert ImportOptions(write_ase=True, write_blend=False, write_max=True).ase_export_owner() == "max"
        assert ImportOptions(write_ase=False, write_blend=True, write_max=True).ase_export_owner() == ""

    def test_glb_and_fbx_require_blender_output(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(
                write_blend=False,
                write_3dp=False,
                write_ase=False,
                write_max=False,
                write_glb=True,
                write_fbx=True,
            ),
        )

        errors = validate_import_request(request)

        assert "GLB/FBX export requires .blend output." in errors


class TestImportRequestValidation:
    def test_definition_request_allows_blender_only_output(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(write_3dp=False, write_ase=False),
        )
        errors = validate_import_request(request)
        assert "Select at least one file to write." not in errors

    def test_request_requires_at_least_one_output_file(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(
                write_blend=False,
                write_3dp=False,
                write_ase=False,
                write_max=False,
            ),
        )
        errors = validate_import_request(request)
        assert "Select at least one file to write." in errors

    def test_ase_requires_dcc_scene_output(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(write_blend=False, write_max=False, write_3dp=True, write_ase=True),
        )
        errors = validate_import_request(request)
        assert "ASE export requires .blend or .max output." in errors

    def test_ase_with_max_output_is_valid(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(write_blend=False, write_max=True, write_3dp=False, write_ase=True),
        )
        errors = validate_import_request(request)
        assert "ASE export requires .blend or .max output." not in errors

    def test_loose_request_requires_3di_file(self) -> None:
        request = ImportRequest.for_loose(
            threedi_path=str(ROOT / "README.md"),
            output_root=str(ROOT),
        )
        errors = validate_import_request(request)
        assert "Loose imports require a .3di file." in errors

    def test_active_duplicate_only_blocks_pending_or_running_jobs(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
        )
        job = ImportJob(request=request)
        assert has_active_duplicate([job], request)
        job.status = JOB_ERROR
        assert not has_active_duplicate([job], request)
        job.retry()
        assert job.status == JOB_PENDING
        assert has_active_duplicate([job], request)

    def test_likely_output_dir_matches_mode(self) -> None:
        definition = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
        )
        loose = ImportRequest.for_loose(
            threedi_path=str(FIXTURE_3DI),
            output_root=str(ROOT),
        )
        assert definition.likely_output_dir == str(ROOT / "M16")
        assert loose.likely_output_dir == str(ROOT / "Shed")

    def test_definition_request_uses_scanned_output_stem_when_available(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="WPN_M16",
            item_type="weapon",
            output_root=str(ROOT),
            output_stem="m16_1st",
        )
        assert request.likely_output_dir == str(ROOT / "m16_1st")

    def test_scan_item_is_typed_and_mapping_compatible(self) -> None:
        item = ScanItem.from_mapping({
            "name": "WPN_M16",
            "type": "weapon",
            "source_model": "m16_1st.3di",
            "output_stem": "m16_1st",
        })
        assert item.name == "WPN_M16"
        assert item["type"] == "weapon"
        assert item.to_dict()["output_stem"] == "m16_1st"


class TestImportRunner:
    def test_execute_definition_request_dispatches_shared_options(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(
                import_occlusion=False,
                import_arms=False,
                write_blend=True,
                write_max=False,
                write_ase=False,
                write_glb=True,
                write_fbx=True,
            ),
        )
        # Call run_one directly (in-process) so the mock applies. The
        # production execute_import_request dispatches via subprocess where
        # parent-process mocks don't reach.
        from apps.importer.worker import run_one
        with patch("apps.importer.import_runner.resolve_definition_output_stem", return_value="m16_1st"), \
             patch("apps.importer.import_runner.run_import", return_value=True) as run_import, \
             patch("apps.importer.bpy_session.init_headless"):
            result = run_one(request)

        assert result.ok
        assert result.output_path == str(ROOT / "m16_1st")
        assert result.elapsed_seconds >= 0
        run_import.assert_called_once()
        kwargs = run_import.call_args.kwargs
        assert kwargs["item_name"] == "M16"
        assert not kwargs["import_occlusion"]
        assert not kwargs["import_arms"]
        assert kwargs["write_blend"]
        assert kwargs["write_glb"]
        assert kwargs["write_fbx"]
        assert "write_max" not in kwargs

    def test_blender_worker_rejects_raw_max_output(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(write_blend=False, write_3dp=False, write_ase=False, write_max=True),
        )
        from apps.importer.worker import run_one

        result = run_one(request)

        assert not result.ok
        assert ".max output must be routed through the standalone backend" in result.error

    def test_execute_loose_request_uses_single_scene_reset_boundary(self) -> None:
        output_root = ROOT
        request = ImportRequest.for_loose(
            threedi_path=str(FIXTURE_3DI),
            output_root=str(output_root),
        )
        from apps.importer.worker import run_one
        with patch("apps.importer.import_runner.run_loose_import", return_value=True) as run_loose, \
             patch("apps.importer.bpy_session.init_headless"):
            result = run_one(request)

        assert result.ok
        assert result.output_path == str(output_root / "Shed")
        run_loose.assert_called_once()
        kwargs = run_loose.call_args.kwargs
        assert kwargs["output_dir"] == str(output_root / "Shed")
        assert not kwargs["reset_scene"]
        assert kwargs["write_blend"]

    def test_run_import_passes_blend_choice_to_basic_model(self) -> None:
        from apps.importer.import_runner import run_import

        with patch("apps.importer.import_runner._setup_blender_package"):
            with patch("apps.importer.import_runner._import_basic_model", return_value=True) as basic:
                ok = run_import(
                    base_dir=str(FIXTURE_DEF_DIR),
                    item_name="M16",
                    item_type="weapon",
                    output_dir=str(ROOT),
                    write_blend=True,
                    write_glb=True,
                    write_fbx=True,
                )

        assert ok
        basic.assert_called_once()
        assert basic.call_args.kwargs["write_blend"]
        assert basic.call_args.kwargs["write_glb"]
        assert basic.call_args.kwargs["write_fbx"]

    def test_run_loose_import_skips_blend_save_when_disabled(self) -> None:
        from apps.importer.import_runner import run_loose_import

        fake_ir = object()
        threedi_module = types.ModuleType("pyopennova.threedi_ffi")
        threedi_module.read_model_ir = Mock(return_value=fake_ir)
        threedi_module.free_model_ir = Mock()

        asset_module = types.ModuleType("pyopennova.asset_resolver")

        class FakeResolver:
            def __init__(self, _base_dir: str) -> None:
                pass

            def __enter__(self) -> "FakeResolver":
                return self

            def __exit__(self, _exc_type: object, _exc: object, _tb: object) -> bool:
                return False

        asset_module.AssetResolver = FakeResolver

        scene_module = types.ModuleType("apps.importer.scene_builder")
        builder = Mock()
        builder.build_basic_scene.return_value = True
        scene_module.BlenderSceneBuilder = Mock(return_value=builder)

        modules = {
            "pyopennova.threedi_ffi": threedi_module,
            "pyopennova.asset_resolver": asset_module,
            "apps.importer.scene_builder": scene_module,
        }
        with patch.dict(sys.modules, modules):
            with patch("apps.importer.import_runner._setup_blender_package"):
                with patch("apps.importer.import_runner._write_3dp_from_ir") as write_3dp:
                    with patch("apps.importer.import_runner._export_ase"):
                        with patch("apps.importer.import_runner._save_blend_scene") as save_blend:
                            ok = run_loose_import(
                                threedi_path=str(FIXTURE_3DI),
                                output_dir=str(ROOT),
                                write_blend=False,
                                reset_scene=False,
                            )

        assert ok
        write_3dp.assert_called_once_with(fake_ir, str(ROOT / "Shed.3dp"))
        save_blend.assert_not_called()

    def test_run_loose_import_exports_glb_and_fbx_from_blender_scene(self) -> None:
        from apps.importer.import_runner import run_loose_import

        fake_ir = object()
        threedi_module = types.ModuleType("pyopennova.threedi_ffi")
        threedi_module.read_model_ir = Mock(return_value=fake_ir)
        threedi_module.free_model_ir = Mock()

        asset_module = types.ModuleType("pyopennova.asset_resolver")

        class FakeResolver:
            def __init__(self, _base_dir: str) -> None:
                pass

            def __enter__(self) -> "FakeResolver":
                return self

            def __exit__(self, _exc_type: object, _exc: object, _tb: object) -> bool:
                return False

        asset_module.AssetResolver = FakeResolver

        scene_module = types.ModuleType("apps.importer.scene_builder")
        builder = Mock()
        builder.build_basic_scene.return_value = True
        scene_module.BlenderSceneBuilder = Mock(return_value=builder)

        modules = {
            "pyopennova.threedi_ffi": threedi_module,
            "pyopennova.asset_resolver": asset_module,
            "apps.importer.scene_builder": scene_module,
        }
        with patch.dict(sys.modules, modules):
            with patch("apps.importer.import_runner._setup_blender_package"):
                with patch("apps.importer.import_runner._write_3dp_from_ir"):
                    with patch("apps.importer.import_runner._export_ase"):
                        with patch("apps.importer.import_runner._export_glb") as export_glb:
                            with patch("apps.importer.import_runner._export_fbx") as export_fbx:
                                with patch("apps.importer.import_runner._save_blend_scene"):
                                    ok = run_loose_import(
                                        threedi_path=str(FIXTURE_3DI),
                                        output_dir=str(ROOT),
                                        write_glb=True,
                                        write_fbx=True,
                                        reset_scene=False,
                                    )

        assert ok
        export_glb.assert_called_once_with(str(ROOT), "Shed")
        export_fbx.assert_called_once_with(str(ROOT), "Shed")

    def test_export_fbx_uses_nla_without_rebaking_all_actions(self) -> None:
        from apps.importer.import_runner import _export_fbx

        bpy_module = types.ModuleType("bpy")
        bpy_module.data = Mock()
        bpy_module.data.actions = [object()]
        bpy_module.ops = Mock()

        modules = {"bpy": bpy_module}
        with patch.dict(sys.modules, modules):
            _export_fbx(str(ROOT), "Shed")

        bpy_module.ops.export_scene.fbx.assert_called_once()
        kwargs = bpy_module.ops.export_scene.fbx.call_args.kwargs
        assert kwargs["filepath"] == str(ROOT / "Shed.fbx")
        assert kwargs["bake_anim"]
        assert kwargs["bake_anim_use_nla_strips"]
        assert not kwargs["bake_anim_use_all_actions"]

    def test_project_writer_has_no_bullet_lod_override_parameter(self) -> None:
        import inspect
        from apps.importer.import_runner import _write_3dp_from_ir

        assert "bullet_lod_index" not in inspect.signature(_write_3dp_from_ir).parameters

    def test_scan_failure_is_not_empty_success(self) -> None:
        missing = str(ROOT / ".scratch" / "__missing_scan_dir__")
        result = scan_directory_result(missing)
        items = scan_directory(missing)
        assert not result.ok
        assert result.error == "Game directory does not exist."
        assert items == []


class TestImportDispatcher:
    def test_spawn_pool_can_return_validation_failure(self, tmp_path: Path) -> None:
        from apps.importer.dispatcher import ImportDispatcher

        request = ImportRequest.for_loose(
            threedi_path=str(tmp_path / "missing.3di"),
            output_root=str(tmp_path),
        )

        with ImportDispatcher(max_workers=1) as dispatcher:
            result = dispatcher.submit(request).result(timeout=30)

        assert not result.ok
        assert result.error == ".3di file does not exist."

    def test_uses_spawn_context_for_queue_and_pool(self) -> None:
        from apps.importer.dispatcher import ImportDispatcher

        spawn_context = Mock()
        log_queue = Mock()
        spawn_context.Queue.return_value = log_queue

        with patch("apps.importer.dispatcher.mp.get_context", return_value=spawn_context) as get_context, \
             patch("apps.importer.dispatcher.logging.handlers.QueueListener") as listener_cls, \
             patch("apps.importer.dispatcher.concurrent.futures.ProcessPoolExecutor") as executor_cls:
            dispatcher = ImportDispatcher(max_workers=2)
            try:
                assert dispatcher.max_workers == 2
                get_context.assert_called_once_with("spawn")
                spawn_context.Queue.assert_called_once_with()

                executor_cls.assert_called_once()
                kwargs = executor_cls.call_args.kwargs
                assert kwargs["initargs"] == (log_queue,)
                assert kwargs["mp_context"] is spawn_context
                assert kwargs["max_tasks_per_child"] == 1
                listener_cls.return_value.start.assert_called_once_with()
            finally:
                dispatcher.close()


class TestImporterHelpers:
    def test_running_job_elapsed_time_is_available(self) -> None:
        request = ImportRequest.for_loose(
            threedi_path=str(FIXTURE_3DI),
            output_root=str(ROOT),
        )
        job = ImportJob(request=request)
        job.mark_running()
        assert job.status == JOB_RUNNING
        assert job.elapsed_seconds >= 0
