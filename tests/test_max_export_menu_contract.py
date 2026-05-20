"""Contracts for 3ds Max current-scene export menu entries."""
from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_max_export_macros_are_separate_from_importer() -> None:
    source = _read("opennova_max/maxscript/OpenNovaImporter.mcr")

    assert 'macroScript OpenNovaImporter category:"OpenNova"' in source
    assert 'macroScript OpenNovaExportAse category:"OpenNova"' in source
    assert 'macroScript OpenNovaExportAnims category:"OpenNova"' in source
    assert "ui.show_importer()" in source
    assert "ui.export_ase()" in source
    assert "ui.export_anims()" in source


def test_max_file_export_menu_has_separate_ase_and_animation_entries() -> None:
    from opennova_max import ui

    menu_script = ui.build_menu_script()

    assert "macroScript OpenNovaImporter" not in menu_script
    assert "macroScript OpenNovaExportAse" not in menu_script
    assert "macroScript OpenNovaExportAnims" not in menu_script
    assert 'menuMan.createActionItem "OpenNovaImporter" "OpenNova"' in menu_script
    assert 'menuMan.createActionItem "OpenNovaExportAse" "OpenNova"' in menu_script
    assert 'menuMan.createActionItem "OpenNovaExportAnims" "OpenNova"' in menu_script
    assert 'aseItem.setTitle "Novalogic ASE (.ase)"' in menu_script
    assert 'animItem.setTitle "Novalogic Anims (.adm + .bad)"' in menu_script
    assert 'aseOpenNovaItem.setTitle "Novalogic ASE (.ase)"' in menu_script
    assert 'animOpenNovaItem.setTitle "Novalogic Anims (.adm + .bad)"' in menu_script
    assert "openNovaExportMenu" in menu_script
    assert "maxOps.GetICuiMenuMgr" in menu_script
    assert "#cuiRegisterMenus" in menu_script
    assert 'OpenNovaExportAse`OpenNova' in menu_script
    assert 'OpenNovaExportAnims`OpenNova' in menu_script
    assert "openNovaCreateModernAction openNovaMenu OPENNOVA_ASE_ACTION_GUID" in menu_script
    assert "openNovaCreateModernAction openNovaMenu OPENNOVA_ANIM_ACTION_GUID" in menu_script
    assert "openNovaFindModernMenuByTitle" in menu_script
    assert "OpenNova modern menu registration failed:" in menu_script
    assert "OpenNova modern menu refresh failed:" in menu_script
    assert "OpenNova menu action missing:" in menu_script
    assert "openNovaRemoveLegacyMenu mainMenuBar \"OpenNova\"" in menu_script
    assert "openNovaBuildLegacyOpenNovaMenu mainMenuBar" in menu_script
    assert "OpenNova legacy menu registration context already exists; rebuilding menu." in menu_script
    assert "if menuMan.registerMenuContext 0x5cb72810 then" not in menu_script
    assert "OpenNova > Importer" not in menu_script


def test_max_exporter_source_does_not_read_importer_metadata() -> None:
    source = _read("opennova_max/ase_scene_exporter.py")

    forbidden = [
        "descriptor_from_user_props",
        "getUserProp",
        '"nl_',
        "'nl_",
        '"opennova_',
        "'opennova_",
    ]

    for needle in forbidden:
        assert needle not in source


def test_max_exporter_source_does_not_auto_emit_lod_files_or_bullet_ase() -> None:
    source = _read("opennova_max/ase_scene_exporter.py")

    assert "_BulletLOD" not in source
    assert "_bullet" not in source
    assert "_find_lod_roots" not in source
    assert "_find_lod0_root" not in source


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


def test_max_ase_material_population_passes_texture_cache_by_keyword(monkeypatch) -> None:
    from opennova_max import ase_scene_exporter

    calls = []
    parent = SimpleNamespace(
        name=b"",
        has_submaterials=0,
        submaterial_count=0,
        submaterials=None,
    )
    doc = SimpleNamespace(material_count=1, materials=[parent])
    exporter = ase_scene_exporter.AseSceneExporter(SimpleNamespace())
    exporter.material_keeper = SimpleNamespace(
        count=lambda: 1,
        get_material=lambda _index: SimpleNamespace(name="Test", opacity=100.0),
    )

    def fake_alloc_submaterials(mat, count):
        mat.submaterials = [SimpleNamespace() for _ in range(count)]

    def fake_populate(sub, desc, *, used_tex_names):
        calls.append((sub, desc, used_tex_names))

    monkeypatch.setattr(ase_scene_exporter.ase_ffi, "alloc_submaterials", fake_alloc_submaterials)
    monkeypatch.setattr(ase_scene_exporter, "populate_ase_submaterial", fake_populate)

    exporter._populate_materials(doc)

    assert calls
    assert calls[0][2] is exporter._used_tex_names


def test_max_material_keeper_matches_rewrapped_material_proxy_by_name() -> None:
    from opennova_max.ase_scene_exporter import MaterialKeeper

    keeper = MaterialKeeper()
    keeper.add_material(SimpleNamespace(name="Concrete"))

    assert keeper.get_global_index(SimpleNamespace(name="Concrete")) == 0


def test_max_ase_material_ref_falls_back_when_material_proxy_is_not_indexed() -> None:
    from opennova_max import ase_scene_exporter

    class MissingMaterialKeeper:
        def count(self):
            return 1

        def get_global_index(self, _material):
            raise KeyError("rewrapped material")

    exporter = ase_scene_exporter.AseSceneExporter(SimpleNamespace())
    exporter.material_keeper = MissingMaterialKeeper()
    node = SimpleNamespace(material=SimpleNamespace(name="Concrete"), name="visible_mesh")

    assert exporter._material_ref(node) == 0


def test_max_light_color_tuple_accepts_rgb_properties_without_indexing() -> None:
    from opennova_max.ase_scene_exporter import _color_tuple

    class FakeMaxColor:
        r = 128
        g = 64
        b = 255

        def __getitem__(self, _index):
            raise IndexError("Error getting index")

    assert _color_tuple(FakeMaxColor()) == (
        128.0 / 255.0,
        64.0 / 255.0,
        1.0,
    )


def test_max_anim_export_plan_uses_current_timeline_range() -> None:
    from opennova_max.anim_exporter import build_timeline_export_plan

    rt = SimpleNamespace(
        animationRange=SimpleNamespace(start=5, end=17),
    )

    plan = build_timeline_export_plan(rt, r"C:\out\walk.adm")

    assert plan.adm_path == r"C:\out\walk.adm"
    assert plan.reset_bad_path == r"C:\out\walk_reset.bad"
    assert plan.clip_bad_path == r"C:\out\walk.bad"
    assert plan.reset_clip.action_name == "anim_reset"
    assert plan.reset_clip.bad_name == "walk_reset"
    assert plan.reset_clip.start_frame == 5
    assert plan.reset_clip.end_frame == 5
    assert plan.clip.action_name == "anim_walk"
    assert plan.clip.bad_name == "walk"
    assert plan.clip.start_frame == 5
    assert plan.clip.end_frame == 17


def test_max_anim_export_dialog_dispatches_selected_path(monkeypatch) -> None:
    from opennova_max import anim_exporter

    calls = []

    class FakeRuntime:
        animationRange = SimpleNamespace(start=1, end=2)
        objects = []

        def getSaveFileName(self, **_kwargs):
            return r"C:\out\walk.adm"

    class FakeExporter:
        def __init__(self, rt):
            self.rt = rt

        def export_plan(self, plan):
            calls.append((self.rt, plan.adm_path, plan.clip.bad_name))
            return True

    rt = FakeRuntime()
    monkeypatch.setattr(anim_exporter, "_rt", lambda: rt)
    monkeypatch.setattr(anim_exporter, "MaxTimelineAnimExporter", FakeExporter)

    assert anim_exporter.export_anims_with_dialog()
    assert calls == [(rt, r"C:\out\walk.adm", "walk")]
