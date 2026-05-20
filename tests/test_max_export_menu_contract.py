"""Contracts for 3ds Max current-scene export menu entries."""
from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest


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
    keeper.add_material(SimpleNamespace(name="Material_3_FF_ST_OP"))

    assert keeper.get_global_index(SimpleNamespace(name="Material_3_FF_ST_OP")) == 0


def test_max_material_keeper_ignores_placeholder_slots_and_sorts_by_source_index() -> None:
    from opennova_max.ase_scene_exporter import MaterialKeeper

    keeper = MaterialKeeper()
    keeper.add_material(SimpleNamespace(name="Material_12_FF_ST_OP"))
    keeper.add_material(SimpleNamespace(name="Material #3"))
    keeper.add_material(SimpleNamespace(name="Material_2_FF_MT_OP"))
    keeper.sort_by_source_index()

    assert keeper.count() == 2
    assert keeper.get_material(0).name == "Material_2_FF_MT_OP"
    assert keeper.get_material(1).name == "Material_12_FF_ST_OP"


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


def test_max_ase_material_ref_omits_helper_viewport_materials() -> None:
    from opennova_max import ase_scene_exporter

    class MissingMaterialKeeper:
        def count(self):
            return 1

        def get_global_index(self, _material):
            raise KeyError("helper material")

    exporter = ase_scene_exporter.AseSceneExporter(SimpleNamespace())
    exporter.material_keeper = MissingMaterialKeeper()
    exporter._is_helper_mesh = lambda _node: True
    node = SimpleNamespace(material=SimpleNamespace(name="center_magenta"), name="_01 center")

    assert exporter._material_ref(node) == -1


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


def test_max_ase_omni_light_uses_identity_tm_row2() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    class FakeRuntime:
        def classOf(self, _obj):
            return "OmniLight"

    exporter = AseSceneExporter(FakeRuntime())
    ase_light = SimpleNamespace(
        name=b"",
        type=-1,
        pos=[0.0, 0.0, 0.0],
        color=[0.0, 0.0, 0.0],
        tm_row2=[0.0, 0.0, 0.0],
    )
    node = SimpleNamespace(
        name="LP02",
        transform=SimpleNamespace(
            row1=(1.0, 0.0, 0.0),
            row2=(0.0, 1.0, 0.0),
            row3=(0.0, 0.0, 1.0),
            position=(-0.1757, -3.903, 1.2802),
        ),
        rgb=SimpleNamespace(r=255, g=255, b=255),
        useFarAtten=False,
    )

    exporter._populate_light(ase_light, node)

    assert ase_light.type == 0
    assert ase_light.tm_row2 == [0.0, 0.0, 1.0]


def test_max_ase_y_keeps_render_space_y() -> None:
    from opennova_max.ase_scene_exporter import _ase_y

    assert _ase_y(-0.325) == -0.325
    assert _ase_y(0.325) == 0.325
    assert str(_ase_y(0.0)) == "-0.0"


def test_max_ase_normals_defer_to_shared_writer_without_explicit_modifier() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    exporter = AseSceneExporter(SimpleNamespace())
    ase_obj = SimpleNamespace(
        face_normal_count=0,
        face_normals=None,
        verts=[
            0.0,
            0.0,
            0.0,
            0.0,
            1.5,
            0.0,
            0.0,
            1.55,
            0.0,
        ],
    )

    exporter._populate_face_normals(
        ase_obj,
        tri=SimpleNamespace(),
        face_count=1,
        node=SimpleNamespace(modifiers=[]),
    )

    assert ase_obj.face_normal_count == 0
    assert ase_obj.face_normals is None


def test_max_ase_degenerate_faces_zero_explicit_normals() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    class FakeMeshOps:
        def getFace(self, _tri, _face_index):
            return SimpleNamespace(x=1, y=2, z=3)

        def getVert(self, _tri, index):
            return {
                1: SimpleNamespace(x=0.0, y=0.0, z=0.0),
                2: SimpleNamespace(x=0.0, y=1.5, z=0.0),
                3: SimpleNamespace(x=0.0, y=1.55, z=0.00000001),
            }[int(index)]

        def getFaceRNormals(self, _tri, _face_index):
            return []

    exporter = AseSceneExporter(SimpleNamespace(meshop=FakeMeshOps()))
    exporter._collect_edit_normals = lambda _node, _face_count: [
        (1.0, 0.00001, 0.0),
        (1.0, 0.00001, 0.0),
        (1.0, 0.00001, 0.0),
    ]
    ase_obj = SimpleNamespace(
        face_normal_count=0,
        face_normals=None,
        verts=[
            0.0,
            0.0,
            0.0,
            0.0,
            1.5,
            0.0,
            0.0,
            1.55,
            0.0,
        ],
    )

    exporter._populate_face_normals(
        ase_obj,
        tri=SimpleNamespace(),
        face_count=1,
        node=SimpleNamespace(modifiers=[]),
    )

    assert ase_obj.face_normal_count == 1
    assert [ase_obj.face_normals[i] for i in range(9)] == [0.0] * 9


def test_max_ase_tiny_valid_faces_keep_fallback_explicit_normals() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    class FakeMeshOps:
        def getFace(self, _tri, _face_index):
            return SimpleNamespace(x=1, y=2, z=3)

        def getVert(self, _tri, index):
            return {
                1: SimpleNamespace(x=0.82690001, y=0.13900000, z=-0.72210002),
                2: SimpleNamespace(x=0.82690001, y=0.14010000, z=-0.72420001),
                3: SimpleNamespace(x=0.82690001, y=0.13720000, z=-0.71860003),
            }[int(index)]

        def getFaceRNormals(self, _tri, _face_index):
            return []

    exporter = AseSceneExporter(SimpleNamespace(meshop=FakeMeshOps()))
    exporter._collect_edit_normals = lambda _node, _face_count: [
        (1.0, 0.0, 0.0),
        (1.0, 0.0, 0.0),
        (1.0, 0.0, 0.0),
    ]
    ase_obj = SimpleNamespace(
        face_normal_count=0,
        face_normals=None,
        verts=[
            0.82690001,
            0.13900000,
            -0.72210002,
            0.82690001,
            0.14010000,
            -0.72420001,
            0.82690001,
            0.13720000,
            -0.71860003,
        ],
    )

    exporter._populate_face_normals(
        ase_obj,
        tri=SimpleNamespace(),
        face_count=1,
        node=SimpleNamespace(modifiers=[]),
    )

    assert ase_obj.face_normal_count == 1
    assert [ase_obj.face_normals[i] for i in range(9)] == [
        1.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
    ]


def test_max_ase_degenerate_faces_zero_runtime_fallback_normals() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    class FakeMeshOps:
        def getFace(self, _tri, _face_index):
            return SimpleNamespace(x=1, y=2, z=3)

        def getVert(self, _tri, index):
            return {
                1: SimpleNamespace(x=0.0, y=0.0, z=0.0),
                2: SimpleNamespace(x=0.0, y=1.5, z=0.0),
                3: SimpleNamespace(x=0.0, y=1.55, z=0.0),
            }[int(index)]

        def getFaceRNormals(self, _tri, _face_index):
            return []

    class FakeRuntime:
        meshop = FakeMeshOps()

        def getNormal(self, _tri, _vert_idx):
            return SimpleNamespace(x=1.0, y=0.0, z=0.0)

    exporter = AseSceneExporter(FakeRuntime())
    exporter._collect_edit_normals = lambda _node, _face_count: [
        None,
        None,
        None,
        (0.0, 0.0, 1.0),
    ]
    ase_obj = SimpleNamespace(
        face_normal_count=0,
        face_normals=None,
        verts=[
            0.0,
            0.0,
            0.0,
            0.0,
            1.5,
            0.0,
            0.0,
            1.55,
            0.0,
        ],
    )

    exporter._populate_face_normals(
        ase_obj,
        tri=SimpleNamespace(),
        face_count=1,
        node=SimpleNamespace(modifiers=[]),
    )

    assert ase_obj.face_normal_count == 1
    assert [ase_obj.face_normals[i] for i in range(9)] == [0.0] * 9


def test_max_ase_degenerate_faces_zero_only_runtime_fallback_corner() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    class FakeMeshOps:
        def getFace(self, _tri, _face_index):
            return SimpleNamespace(x=1, y=2, z=3)

        def getVert(self, _tri, index):
            return {
                1: SimpleNamespace(x=0.0, y=0.0, z=0.0),
                2: SimpleNamespace(x=0.0, y=1.5, z=0.0),
                3: SimpleNamespace(x=0.0, y=1.55, z=0.0),
            }[int(index)]

        def getFaceRNormals(self, _tri, _face_index):
            return []

    class FakeRuntime:
        meshop = FakeMeshOps()

        def getNormal(self, _tri, _vert_idx):
            return SimpleNamespace(x=1.0, y=0.0, z=0.0)

    exporter = AseSceneExporter(FakeRuntime())
    exporter._collect_edit_normals = lambda _node, _face_count: [
        (0.237144053, -0.720543921, -0.65159744),
        (0.0840115473, -0.969404936, -0.230643168),
        None,
    ]
    ase_obj = SimpleNamespace(
        face_normal_count=0,
        face_normals=None,
        verts=[
            0.0,
            0.0,
            0.0,
            0.0,
            1.5,
            0.0,
            0.0,
            1.55,
            0.0,
        ],
    )

    exporter._populate_face_normals(
        ase_obj,
        tri=SimpleNamespace(),
        face_count=1,
        node=SimpleNamespace(modifiers=[]),
    )

    assert ase_obj.face_normal_count == 1
    assert [ase_obj.face_normals[i] for i in range(9)] == [
        pytest.approx(0.237144053),
        pytest.approx(-0.720543921),
        pytest.approx(-0.65159744),
        pytest.approx(0.0840115473),
        pytest.approx(-0.969404936),
        pytest.approx(-0.230643168),
        0.0,
        0.0,
        0.0,
    ]


def test_max_ase_degenerate_faces_keep_authored_explicit_normals() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    class FakeMeshOps:
        def getFace(self, _tri, _face_index):
            return SimpleNamespace(x=1, y=2, z=3)

        def getVert(self, _tri, index):
            return {
                1: SimpleNamespace(x=0.0, y=0.0, z=0.0),
                2: SimpleNamespace(x=0.0, y=0.0, z=0.0),
                3: SimpleNamespace(x=1.0, y=0.0, z=0.0),
            }[int(index)]

        def getFaceRNormals(self, _tri, _face_index):
            return []

    exporter = AseSceneExporter(SimpleNamespace(meshop=FakeMeshOps()))
    exporter._collect_edit_normals = lambda _node, _face_count: [
        (-1.0, 0.0, 0.0),
        (-1.0, 0.0, 0.0),
        (-1.0, 0.0, 0.0),
    ]
    ase_obj = SimpleNamespace(
        face_normal_count=0,
        face_normals=None,
        verts=[
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
        ],
    )

    exporter._populate_face_normals(
        ase_obj,
        tri=SimpleNamespace(),
        face_count=1,
        node=SimpleNamespace(modifiers=[]),
    )

    assert ase_obj.face_normal_count == 1
    assert [ase_obj.face_normals[i] for i in range(9)] == [
        -1.0,
        0.0,
        0.0,
        -1.0,
        0.0,
        0.0,
        -1.0,
        0.0,
        0.0,
    ]


def test_max_ase_weight_slots_default_to_minus_one() -> None:
    from opennova_max.ase_scene_exporter import _write_weight_slots

    out = SimpleNamespace(bone_index=[0, 0, 0, 0], weight=[9.0, 9.0, 9.0, 9.0])

    _write_weight_slots(out, [(2, 0.75), (4, 0.25)])

    assert out.bone_index == [2, 4, -1, -1]
    assert out.weight == [0.75, 0.25, 0.0, 0.0]


def test_max_ase_collects_tiny_skin_weights() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    class FakeSkinOps:
        def GetNumberBones(self, _skin):
            return 2

        def GetBoneName(self, _skin, slot, _name_flag):
            return {1: "BN12", 2: "BN18"}[int(slot)]

        def GetVertexWeightCount(self, _skin, _vertex_index):
            return 2

        def GetVertexWeightBoneID(self, _skin, _vertex_index, weight_index):
            return int(weight_index)

        def GetVertexWeight(self, _skin, _vertex_index, weight_index):
            return {1: 0.0007, 2: 0.9993}[int(weight_index)]

    skin = object()
    exporter = AseSceneExporter(SimpleNamespace(skinOps=FakeSkinOps()))
    exporter._skin_modifier = lambda _node: skin

    assert exporter._collect_weight_data(SimpleNamespace(), 1) == [[(11, 0.0007), (17, 0.9993)]]


def test_max_ase_material_descriptor_uses_native_two_sided_flag() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    exporter = AseSceneExporter(SimpleNamespace())

    one_sided = exporter._collect_material_descriptor(
        SimpleNamespace(name="Material_0_FF_ST_OP", opacity=100.0, twoSided=False),
        0,
    )
    two_sided = exporter._collect_material_descriptor(
        SimpleNamespace(name="Material_1_FF_ST_OP", opacity=100.0, twoSided=True),
        1,
    )

    assert not one_sided.two_sided
    assert two_sided.two_sided


def test_max_ase_glass_descriptor_uses_native_ambient_reflect_color() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    exporter = AseSceneExporter(SimpleNamespace())

    desc = exporter._collect_material_descriptor(
        SimpleNamespace(
            name="Material_12_FFP_GLASS",
            opacity=100.0,
            twoSided=False,
            ambient=SimpleNamespace(r=128, g=128, b=128),
        ),
        12,
    )

    assert desc.glass
    assert desc.reflect_color[:3] == (128.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0)


def test_max_center_marker_transform_is_transposed_for_ase_rows() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    exporter = AseSceneExporter(SimpleNamespace())
    node = SimpleNamespace(
        name="_02 center",
        transform=SimpleNamespace(
            row1=(1.0, 2.0, 3.0),
            row2=(4.0, 5.0, 6.0),
            row3=(7.0, 8.0, 9.0),
            position=(10.0, 11.0, 12.0),
        ),
    )

    matrix = exporter._node_tm_for_export(node)

    assert matrix.row1 == (1.0, 4.0, 7.0)
    assert matrix.row2 == (2.0, 5.0, 8.0)
    assert matrix.row3 == (3.0, 6.0, 9.0)
    assert matrix.position == (10.0, 11.0, 12.0)


def test_max_marker_vertices_ignore_marker_rotation() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    class MeshOps:
        def getVert(self, mesh, _index):
            return mesh.vert

    exporter = AseSceneExporter(SimpleNamespace(meshop=MeshOps()))
    node = SimpleNamespace(
        name="_02 center",
        vert=(1.0, 2.0, 3.0),
        transform=SimpleNamespace(position=(10.0, 20.0, 30.0)),
    )
    rotated_snapshot = SimpleNamespace(vert=(99.0, 99.0, 99.0))

    assert exporter._vertex_world_for_export(node, rotated_snapshot, 0) == (
        9.9925,
        19.9925,
        29.9925,
    )


def test_max_bone_marker_mesh_matches_direct_writer_shape() -> None:
    from opennova_max.ase_scene_exporter import _bone_marker_mesh

    verts, faces = _bone_marker_mesh((1.0, 2.0, 3.0))

    assert len(verts) == 9
    assert len(faces) == 14
    assert verts[0] == (1.01, 2.01, 3.01)
    assert verts[8] == (1.0, 2.0, 3.0)
    assert faces[0] == (8, 0, 1)
    assert faces[-1] == (4, 6, 7)


def test_max_helper_meshes_exclude_bone_nodes() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    exporter = AseSceneExporter(SimpleNamespace())
    exporter._is_geometry = lambda _node: True
    exporter._is_visible = lambda _node: True
    exporter._is_render_mesh = lambda _node: False
    helper = SimpleNamespace(name="CB01-colonly", handle=2)
    bone = SimpleNamespace(name="BN01", handle=1)

    assert exporter._ordered_helper_meshes([bone, helper]) == [helper]


def test_max_helper_mesh_order_uses_scene_handle_not_runtime_listing_order() -> None:
    from opennova_max.ase_scene_exporter import AseSceneExporter

    bbl = SimpleNamespace(name="BBL02-colonly", handle=30)
    cb = SimpleNamespace(name="CB01-colonly", handle=20)

    ordered = [
        node.name
        for _index, node in sorted(
            [(0, bbl), (1, cb)],
            key=lambda item: AseSceneExporter._mesh_sort_key(item[1], item[0]),
        )
    ]

    assert ordered == ["CB01-colonly", "BBL02-colonly"]


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
