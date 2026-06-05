from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_max_export_macros_cover_ase_and_anims() -> None:
    source = _read("opennova_max/maxscript/OpenNovaExport.mcr")

    assert 'macroScript OpenNovaExportAse category:"OpenNova"' in source
    assert "ui.export_ase()" in source
    assert 'macroScript OpenNovaExportAnims category:"OpenNova"' in source
    assert "ui.export_anims()" in source
    assert "OpenNovaImporter" not in source
    assert "show_importer" not in source


def test_max_file_export_menu_registers_ase_and_anims() -> None:
    from opennova_max import ui

    menu_script = ui.build_menu_script()

    assert "maxOps.GetICuiMenuMgr()" in menu_script
    assert "#cuiRegisterMenus" in menu_script
    assert 'menuMgr.GetMenuById "eed3eaef-ea24-4342-aacc-9dfd87f9a4f4"' in menu_script
    assert 'openNovaFindModernMenuByTitle fileMenu #("&Export", "Export", "&Export...", "Export...")' in menu_script
    assert 'OpenNovaExportAse`OpenNova' in menu_script
    assert 'OpenNovaExportAnims`OpenNova' in menu_script
    assert 'menuMan.findMenu "&File"' in menu_script
    assert "OpenNovaImporter" not in menu_script
    assert 'menuMan.createActionItem "OpenNovaExportAse" "OpenNova"' in menu_script
    assert 'menuMan.createActionItem "OpenNovaExportAnims" "OpenNova"' in menu_script
    assert 'aseItem.setTitle "Novalogic ASE (.ase)"' in menu_script
    assert 'animsItem.setTitle "Novalogic Anims (.adm + .bad)"' in menu_script
    assert "exportMenu.addItem aseItem -1" in menu_script
    assert "exportMenu.addItem animsItem -1" in menu_script
    assert "OpenNovaExportMenu" not in menu_script
    assert 'createSubMenuItem "OpenNova"' not in menu_script


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


def test_max_anim_export_dialog_dispatches_selected_path(monkeypatch) -> None:
    from opennova_max import anim_scene_exporter

    calls = []

    class FakeRuntime:
        def getSaveFileName(self, **_kwargs):
            return r"C:\out\Soldier.adm"

    class FakeExporter:
        def __init__(self, rt):
            self.rt = rt

        def export(self, filepath):
            calls.append((self.rt, filepath))
            return True

    rt = FakeRuntime()
    monkeypatch.setattr(anim_scene_exporter, "_rt", lambda: rt)
    monkeypatch.setattr(anim_scene_exporter, "AnimSceneExporter", FakeExporter)

    assert anim_scene_exporter.export_anims_with_dialog()
    assert calls == [(rt, r"C:\out\Soldier.adm")]


def test_max_anim_export_dialog_cancel_returns_false(monkeypatch) -> None:
    from opennova_max import anim_scene_exporter

    class FakeRuntime:
        def getSaveFileName(self, **_kwargs):
            return None

    monkeypatch.setattr(anim_scene_exporter, "_rt", lambda: FakeRuntime())

    assert not anim_scene_exporter.export_anims_with_dialog()


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


def test_max_ase_edit_normals_read_does_not_touch_live_modify_panel() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    unsafe_calls = []
    node = SimpleNamespace()

    class FakeRuntime:
        def select(self, _node):
            unsafe_calls.append("select")

        def execute(self, command):
            unsafe_calls.append(command)

        class modPanel:
            @staticmethod
            def setCurrentObject(_mod):
                unsafe_calls.append("modPanel.setCurrentObject")

        def update(self, _node):
            unsafe_calls.append("update")

    class Edit_Normals:
        def GetNumFaces(self, *, node):
            return 1

        def GetNormalID(self, face, corner, *, node):
            assert face == 1
            assert node is not None
            return corner

        def GetNormal(self, normal_id, *, node):
            assert node is not None
            return SimpleNamespace(x=float(normal_id), y=0.0, z=0.0)

    exporter = AseSceneExporter(FakeRuntime())
    exporter._skin_modifier = lambda _node: None

    normals = exporter._collect_edit_normals(
        SimpleNamespace(modifiers=[Edit_Normals()]),
        face_count=1,
    )

    assert normals == [(1.0, 0.0, 0.0)] * 3
    assert unsafe_calls == []


def test_max_ase_edit_normals_failure_skips_without_touching_live_modify_panel() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    unsafe_calls = []

    class FakeRuntime:
        def select(self, _node):
            unsafe_calls.append("select")

        def execute(self, command):
            unsafe_calls.append(command)

        class modPanel:
            @staticmethod
            def setCurrentObject(_mod):
                unsafe_calls.append("modPanel.setCurrentObject")

        def update(self, _node):
            unsafe_calls.append("update")

    class Edit_Normals:
        def GetNumFaces(self, *, node):
            raise RuntimeError("requires modifier panel")

    exporter = AseSceneExporter(FakeRuntime())

    assert exporter._collect_edit_normals(
        SimpleNamespace(modifiers=[Edit_Normals()]),
        face_count=1,
    ) == []
    assert unsafe_calls == []


def test_max_ase_scene_state_guard_restores_selection_and_command_panel() -> None:
    from opennova_max.ase_scene_exporter import _SceneStateGuard

    original_selection = [SimpleNamespace(name="A"), SimpleNamespace(name="B")]
    replacement_selection = [SimpleNamespace(name="C")]
    calls = []

    class FakeRuntime:
        def __init__(self):
            self.selection = list(original_selection)
            self.mode = "#modify"

        def getCommandPanelTaskMode(self):
            return self.mode

        def setCommandPanelTaskMode(self, mode):
            calls.append(("mode", mode))
            self.mode = mode

        def select(self, selection):
            calls.append(("select", selection))
            self.selection = list(selection)

        def clearSelection(self):
            calls.append(("clearSelection",))
            self.selection = []

    rt = FakeRuntime()

    with _SceneStateGuard(rt):
        rt.selection = list(replacement_selection)
        rt.mode = "#create"

    assert rt.selection == original_selection
    assert rt.mode == "#modify"
    assert ("mode", "#modify") in calls
    assert ("select", original_selection) in calls


def test_max_ase_export_scene_runs_under_scene_state_guard() -> None:
    source = _read("opennova_max/ase_scene_exporter.py")

    assert "with _SceneStateGuard(self.rt):" in source
