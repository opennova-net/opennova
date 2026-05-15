"""Test ImportOptions schema changes for Phase 1."""
from opennova_jobs import ImportOptions


class TestImportOptionsSchema:
    def test_new_schema_has_write_max_not_glb_fbx(self) -> None:
        options = ImportOptions()
        # New schema should have write_max
        assert hasattr(options, "write_max")
        assert not options.write_max

        # Old schema should NOT have write_glb or write_fbx
        assert not hasattr(options, "write_glb")
        assert not hasattr(options, "write_fbx")

    def test_all_defaults(self) -> None:
        options = ImportOptions()
        assert options.import_animations is True
        assert options.import_collisions is True
        assert options.import_occlusion is True
        assert options.import_lights is True
        assert options.import_arms is True
        assert options.write_blend is True
        assert options.write_3dp is True
        assert options.write_ase is True
        assert options.write_max is False
        assert options.copy_textures is True

    def test_writes_any_output_file_includes_write_max(self) -> None:
        # Default should have some writes
        assert ImportOptions().writes_any_output_file()

        # With all writes disabled except write_max
        options = ImportOptions(
            write_blend=False,
            write_3dp=False,
            write_ase=False,
            write_max=True,
            copy_textures=False,
        )
        assert options.writes_any_output_file()

    def test_writes_any_export_format_includes_write_max(self) -> None:
        # Default should write at least blend/3dp/ase
        assert ImportOptions().writes_any_export_format()

        # With only write_max enabled
        options = ImportOptions(
            write_blend=False,
            write_3dp=False,
            write_ase=False,
            write_max=True,
        )
        assert options.writes_any_export_format()

    def test_dedupe_tuple_has_write_max(self) -> None:
        options = ImportOptions()
        dedupe = options.dedupe_tuple()
        # Should have 10 elements (animations, collisions, occlusion, lights, arms,
        # blend, 3dp, ase, max, textures)
        assert len(dedupe) == 10
        # Last element should be copy_textures
        assert dedupe[-1] is True
        # The write_max should be at index 8
        assert dedupe[8] is False
