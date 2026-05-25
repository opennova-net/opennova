from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_max_export_macro_is_separate_from_importer_ui() -> None:
    source = _read("opennova_max/maxscript/OpenNovaExport.mcr")

    assert 'macroScript OpenNovaExportAse category:"OpenNova"' in source
    assert "ui.export_ase()" in source
    assert "OpenNovaImporter" not in source
    assert "show_importer" not in source


def test_max_file_export_menu_registers_ase_only() -> None:
    from opennova_max import ui

    menu_script = ui.build_menu_script()

    assert "OpenNovaImporter" not in menu_script
    assert 'menuMan.createActionItem "OpenNovaExportAse" "OpenNova"' in menu_script
    assert 'aseItem.setTitle "Novalogic ASE (.ase)"' in menu_script
    assert "exportMenu.addItem aseItem -1" in menu_script
    assert "OpenNovaExportMenu" not in menu_script
    assert 'createSubMenuItem "OpenNova"' not in menu_script
    assert "Novalogic Anims" not in menu_script


def test_max_ase_export_dialog_dispatches_selected_path(monkeypatch) -> None:
    from opennova_max import ase_scene_exporter

    calls = []

    class FakeRuntime:
        def getSaveFileName(self, **_kwargs):
            return r"C:\out\Shed.ase"

    class FakeExporter:
        def __init__(self, rt):
            self.rt = rt

        def export_scene(self, filepath):
            calls.append((self.rt, filepath))
            return True

    rt = FakeRuntime()
    monkeypatch.setattr(ase_scene_exporter, "_rt", lambda: rt)
    monkeypatch.setattr(ase_scene_exporter, "AseSceneExporter", FakeExporter)

    assert ase_scene_exporter.export_scene_with_dialog()
    assert calls == [(rt, r"C:\out\Shed.ase")]


def test_max_ase_export_dialog_cancel_returns_false(monkeypatch) -> None:
    from opennova_max import ase_scene_exporter

    class FakeRuntime:
        def getSaveFileName(self, **_kwargs):
            return None

    monkeypatch.setattr(ase_scene_exporter, "_rt", lambda: FakeRuntime())

    assert not ase_scene_exporter.export_scene_with_dialog()


def test_max_ase_exporter_does_not_auto_emit_lod_or_bullet_files() -> None:
    source = _read("opennova_max/ase_scene_exporter.py")

    forbidden = [
        "_BulletLOD",
        "_bullet",
        "_find_lod_roots",
        "_find_lod0_root",
        "_lod_index",
    ]
    for needle in forbidden:
        assert needle not in source
