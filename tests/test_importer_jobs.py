from __future__ import annotations

import sys
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from apps.importer.cli import build_parser, options_from_args
from apps.importer.import_runner import (
    execute_import_request,
    scan_directory,
    scan_directory_result,
)
from apps.importer.jobs import (
    JOB_ERROR,
    JOB_PENDING,
    ImportJob,
    ImportOptions,
    ImportRequest,
    has_active_duplicate,
    validate_import_request,
)
from apps.importer.ui.app import (
    CUSTOM_PRESET_LABEL,
    OPTION_PRESETS,
    ImporterApp,
    preset_name_for_options,
)


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_DEF_DIR = ROOT / "fixtures" / "def"
FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"


class ImportOptionsTests(unittest.TestCase):
    def test_defaults_match_gui_baseline(self) -> None:
        options = ImportOptions()
        self.assertTrue(options.import_animations)
        self.assertTrue(options.import_collisions)
        self.assertTrue(options.import_occlusion)
        self.assertTrue(options.import_lights)
        self.assertTrue(options.import_arms)
        self.assertTrue(options.write_blend)
        self.assertTrue(options.write_3dp)
        self.assertTrue(options.write_ase)
        self.assertFalse(options.write_glb)
        self.assertFalse(options.write_fbx)

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
            "--no-blend",
            "--no-3dp",
            "--glb",
        ])
        options = options_from_args(args)
        self.assertFalse(options.import_occlusion)
        self.assertFalse(options.import_arms)
        self.assertFalse(options.write_blend)
        self.assertFalse(options.write_3dp)
        self.assertTrue(options.write_ase)
        self.assertTrue(options.write_glb)

    def test_blender_only_preset_writes_only_blend(self) -> None:
        options = OPTION_PRESETS["Blender only"]
        self.assertTrue(options.write_blend)
        self.assertTrue(options.writes_any_output_file())
        self.assertFalse(options.writes_any_export_format())
        self.assertTrue(options.import_collisions)

    def test_preset_name_matches_known_options(self) -> None:
        self.assertEqual("Round-trip", preset_name_for_options(ImportOptions()))
        self.assertEqual(
            "Godot/runtime export",
            preset_name_for_options(OPTION_PRESETS["Godot/runtime export"]),
        )

    def test_preset_name_reports_custom_options(self) -> None:
        options = ImportOptions(write_fbx=True)
        self.assertEqual(CUSTOM_PRESET_LABEL, preset_name_for_options(options))

    def test_legacy_preferences_default_missing_new_options(self) -> None:
        app = object.__new__(ImporterApp)
        app._prefs = {
            "last_options": {
                "import_animations": True,
                "import_collisions": True,
                "import_occlusion": True,
                "import_lights": True,
                "import_arms": True,
                "write_3dp": True,
                "write_ase": True,
                "write_glb": False,
                "write_fbx": False,
            }
        }
        options = app._options_from_preferences()
        self.assertTrue(options.write_blend)
        self.assertEqual("Round-trip", preset_name_for_options(options))


class ImportRequestValidationTests(unittest.TestCase):
    def test_definition_request_allows_blender_only_output(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(write_3dp=False, write_ase=False),
        )
        errors = validate_import_request(request)
        self.assertNotIn("Select at least one file to write.", errors)

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
                write_glb=False,
                write_fbx=False,
            ),
        )
        errors = validate_import_request(request)
        self.assertIn("Select at least one file to write.", errors)

    def test_loose_request_requires_3di_file(self) -> None:
        request = ImportRequest.for_loose(
            threedi_path=str(ROOT / "README.md"),
            output_root=str(ROOT),
        )
        errors = validate_import_request(request)
        self.assertIn("Loose imports require a .3di file.", errors)

    def test_active_duplicate_only_blocks_pending_or_running_jobs(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
        )
        job = ImportJob(request=request)
        self.assertTrue(has_active_duplicate([job], request))
        job.status = JOB_ERROR
        self.assertFalse(has_active_duplicate([job], request))
        job.retry()
        self.assertEqual(JOB_PENDING, job.status)
        self.assertTrue(has_active_duplicate([job], request))

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
        self.assertEqual(str(ROOT / "M16"), definition.likely_output_dir)
        self.assertEqual(str(ROOT / "Shed"), loose.likely_output_dir)


class ImportRunnerTests(unittest.TestCase):
    def test_execute_definition_request_dispatches_shared_options(self) -> None:
        request = ImportRequest.for_definition(
            base_dir=str(FIXTURE_DEF_DIR),
            item_name="M16",
            item_type="weapon",
            output_root=str(ROOT),
            options=ImportOptions(
                import_occlusion=False,
                import_arms=False,
                write_blend=False,
                write_glb=True,
            ),
        )
        with patch("apps.importer.import_runner.run_import", return_value=True) as run_import:
            result = execute_import_request(request, init_blender=False, reset_scene=False)

        self.assertTrue(result.ok)
        run_import.assert_called_once()
        kwargs = run_import.call_args.kwargs
        self.assertEqual("M16", kwargs["item_name"])
        self.assertFalse(kwargs["import_occlusion"])
        self.assertFalse(kwargs["import_arms"])
        self.assertFalse(kwargs["write_blend"])
        self.assertTrue(kwargs["write_glb"])

    def test_execute_loose_request_uses_single_scene_reset_boundary(self) -> None:
        output_root = ROOT
        request = ImportRequest.for_loose(
            threedi_path=str(FIXTURE_3DI),
            output_root=str(output_root),
        )
        with patch("apps.importer.import_runner.run_loose_import", return_value=True) as run_loose:
            result = execute_import_request(request, init_blender=False, reset_scene=False)

        self.assertTrue(result.ok)
        self.assertEqual(str(output_root / "Shed"), result.output_path)
        run_loose.assert_called_once()
        kwargs = run_loose.call_args.kwargs
        self.assertEqual(str(output_root / "Shed"), kwargs["output_dir"])
        self.assertFalse(kwargs["reset_scene"])
        self.assertTrue(kwargs["write_blend"])

    def test_run_import_passes_blend_choice_to_basic_model(self) -> None:
        from apps.importer.import_runner import run_import

        with patch("apps.importer.import_runner._setup_blender_package"):
            with patch("apps.importer.import_runner._import_basic_model", return_value=True) as basic:
                ok = run_import(
                    base_dir=str(FIXTURE_DEF_DIR),
                    item_name="M16",
                    item_type="weapon",
                    output_dir=str(ROOT),
                    write_blend=False,
                )

        self.assertTrue(ok)
        basic.assert_called_once()
        self.assertFalse(basic.call_args.kwargs["write_blend"])

    def test_run_loose_import_skips_blend_save_when_disabled(self) -> None:
        from apps.importer.import_runner import run_loose_import

        fake_ir = object()
        threedi_module = types.ModuleType("blender.opennova.threedi_ffi")
        threedi_module.read_model_ir = Mock(return_value=fake_ir)
        threedi_module.free_model_ir = Mock()

        asset_module = types.ModuleType("blender.opennova.asset_resolver")

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
        builder.bullet_lod_index = -1
        scene_module.BlenderSceneBuilder = Mock(return_value=builder)

        modules = {
            "blender.opennova.threedi_ffi": threedi_module,
            "blender.opennova.asset_resolver": asset_module,
            "apps.importer.scene_builder": scene_module,
        }
        with patch.dict(sys.modules, modules):
            with patch("apps.importer.import_runner._setup_blender_package"):
                with patch("apps.importer.import_runner._write_3dp_from_ir"):
                    with patch("apps.importer.import_runner._export_ase"):
                        with patch("apps.importer.import_runner._save_blend_scene") as save_blend:
                            ok = run_loose_import(
                                threedi_path=str(FIXTURE_3DI),
                                output_dir=str(ROOT),
                                write_blend=False,
                                reset_scene=False,
                            )

        self.assertTrue(ok)
        save_blend.assert_not_called()

    def test_scan_failure_is_not_empty_success(self) -> None:
        missing = str(ROOT / ".scratch" / "__missing_scan_dir__")
        result = scan_directory_result(missing)
        items = scan_directory(missing)
        self.assertFalse(result.ok)
        self.assertEqual("Game directory does not exist.", result.error)
        self.assertEqual([], items)


if __name__ == "__main__":
    unittest.main()
