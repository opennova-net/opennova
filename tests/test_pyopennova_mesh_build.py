"""Unit tests for pyopennova.mesh_build (LOD -> per-part FlatMesh)."""
from __future__ import annotations

from dataclasses import dataclass, field

import pytest

from pyopennova.mesh_build import flatten_lod


# ---------------------------------------------------------------------------
# Duck-typed IR stand-ins. The flattener reads via [] and . access only, so
# Python lists / dataclasses look the same as ctypes pointers / Structures.
# ---------------------------------------------------------------------------


@dataclass
class FakeVertex:
    position: tuple = (0.0, 0.0, 0.0)
    normal: tuple = (0.0, 0.0, 1.0)
    uv0: tuple = (0.0, 0.0)
    uv1: tuple = (0.0, 0.0)
    bone_indices: tuple = (0, 0, 0, 0)
    bone_weights: tuple = (0.0, 0.0, 0.0, 0.0)


@dataclass
class FakePrim:
    material_index: int = 0
    part_index: int = 0
    index_offset: int = 0
    index_count: int = 0
    vertex_offset: int = 0
    bone_table_length: int = 0
    bone_table: tuple = ()


@dataclass
class FakePart:
    parent_index: int = -1
    abs_position: tuple = (0.0, 0.0, 0.0)
    rel_position: tuple = (0.0, 0.0, 0.0)


@dataclass
class FakeLod:
    parts: list = field(default_factory=list)
    primitives: list = field(default_factory=list)
    vertices: list = field(default_factory=list)
    indices: list = field(default_factory=list)

    @property
    def part_count(self): return len(self.parts)
    @property
    def primitive_count(self): return len(self.primitives)
    @property
    def vertex_count(self): return len(self.vertices)
    @property
    def index_count(self): return len(self.indices)


@dataclass
class FakeIR:
    lods: list = field(default_factory=list)
    mesh_type: int = 0

    @property
    def lod_count(self): return len(self.lods)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def _single_triangle_lod():
    return FakeLod(
        parts=[FakePart(parent_index=-1)],
        primitives=[FakePrim(
            material_index=0, part_index=0,
            index_offset=0, index_count=3, vertex_offset=0,
        )],
        vertices=[
            FakeVertex(position=(0.0, 0.0, 0.0), uv0=(0.0, 0.0), normal=(0.0, 0.0, 1.0)),
            FakeVertex(position=(1.0, 0.0, 0.0), uv0=(1.0, 0.0), normal=(0.0, 0.0, 1.0)),
            FakeVertex(position=(0.0, 1.0, 0.0), uv0=(0.0, 1.0), normal=(0.0, 0.0, 1.0)),
        ],
        indices=[0, 1, 2],
    )


def test_empty_ir_returns_empty_list():
    ir = FakeIR(lods=[FakeLod()])
    assert flatten_lod(ir, 0) == []


def test_include_empty_parts_preserves_subobject_slots():
    lod = _single_triangle_lod()
    lod.parts.append(FakePart(parent_index=0))
    flat = flatten_lod(FakeIR(lods=[lod]), 0, include_empty_parts=True)
    assert [fm.part_index for fm in flat] == [0, 1]
    assert len(flat[1].vertices) == 0
    assert len(flat[1].faces) == 0


def test_single_triangle_yields_one_flatmesh():
    ir = FakeIR(lods=[_single_triangle_lod()])
    flat = flatten_lod(ir, 0)
    assert len(flat) == 1
    fm = flat[0]
    assert fm.part_index == 0
    assert fm.name == "01 Mesh0"
    assert len(fm.faces) == 1
    assert len(fm.vertices) == 3
    assert fm.face_material_ids == [0]
    assert fm.material_id_set == [0]


def test_index_winding_swap_matches_blender_importer():
    # The flattener reads source indices in the order [0, 2, 1] (the
    # winding swap from the Blender importer). Output vertices are added
    # in encounter order, so the geometric winding of the output triangle
    # corresponds to source[0] -> source[2] -> source[1].
    ir = FakeIR(lods=[_single_triangle_lod()])
    fm = flatten_lod(ir, 0)[0]
    f = fm.faces[0]
    actual_winding = [fm.vertices[i] for i in f]
    # render_space((x,y,z)) = (-x, -z, y); part is at origin, so:
    #   source[0].position (0,0,0) -> ( 0.0, 0.0, 0.0)
    #   source[2].position (0,1,0) -> ( 0.0, 0.0, 1.0)
    #   source[1].position (1,0,0) -> (-1.0, 0.0, 0.0)
    expected_winding = [
        (0.0, 0.0, 0.0),
        (0.0, 0.0, 1.0),
        (-1.0, 0.0, 0.0),
    ]
    for actual, expected in zip(actual_winding, expected_winding):
        assert actual == pytest.approx(expected)


def test_uv_v_flips_to_match_target_dcc_convention():
    ir = FakeIR(lods=[_single_triangle_lod()])
    fm = flatten_lod(ir, 0)[0]
    # Vertex 1 source uv0 was (1.0, 0.0); after V-flip: (1.0, 1.0).
    # The face_uvs0 are ordered to match face vertex order (0, 2, 1):
    #   face_uvs0[0] -> source vertex 0 uv0 = (0.0, 0.0) -> (0.0, 1.0)
    #   face_uvs0[1] -> source vertex 2 uv0 = (0.0, 1.0) -> (0.0, 0.0)
    #   face_uvs0[2] -> source vertex 1 uv0 = (1.0, 0.0) -> (1.0, 1.0)
    assert fm.face_uvs0[0] == pytest.approx((0.0, 1.0))
    assert fm.face_uvs0[1] == pytest.approx((0.0, 0.0))
    assert fm.face_uvs0[2] == pytest.approx((1.0, 1.0))


def test_render_space_is_applied_to_positions():
    # render_space maps (x, y, z) -> (-x, -z, y).
    # Vertex 1 source position (1, 0, 0) -> (-1, 0, 0).
    ir = FakeIR(lods=[_single_triangle_lod()])
    fm = flatten_lod(ir, 0)[0]
    # Find the vertex whose source position was (1, 0, 0).
    assert (-1.0, 0.0, 0.0) in fm.vertices


def test_part_origin_subtracted_from_vertices():
    # Move the part to (5, 6, 7); vertex positions should subtract that
    # before render_space is applied.
    lod = _single_triangle_lod()
    lod.parts[0] = FakePart(parent_index=-1, abs_position=(5.0, 6.0, 7.0))
    # Move source vertex 0 to (5, 6, 7) so part-relative = (0, 0, 0).
    lod.vertices[0] = FakeVertex(position=(5.0, 6.0, 7.0))
    fm = flatten_lod(FakeIR(lods=[lod]), 0)[0]
    # The shifted source vertex should appear at the origin.
    assert (0.0, 0.0, 0.0) in fm.vertices


def test_two_parts_yield_two_flatmeshes_with_correct_indices():
    # One triangle in each of two parts.
    lod = FakeLod(
        parts=[
            FakePart(parent_index=-1),
            FakePart(parent_index=0),
        ],
        primitives=[
            FakePrim(material_index=0, part_index=0,
                     index_offset=0, index_count=3, vertex_offset=0),
            FakePrim(material_index=1, part_index=1,
                     index_offset=3, index_count=3, vertex_offset=3),
        ],
        vertices=[
            FakeVertex(position=(0.0, 0.0, 0.0)),
            FakeVertex(position=(1.0, 0.0, 0.0)),
            FakeVertex(position=(0.0, 1.0, 0.0)),
            FakeVertex(position=(2.0, 0.0, 0.0)),
            FakeVertex(position=(3.0, 0.0, 0.0)),
            FakeVertex(position=(2.0, 1.0, 0.0)),
        ],
        indices=[0, 1, 2, 0, 1, 2],
    )
    flat = flatten_lod(FakeIR(lods=[lod]), 0)
    assert [fm.part_index for fm in flat] == [0, 1]
    assert flat[0].face_material_ids == [0]
    assert flat[1].face_material_ids == [1]
    assert flat[0].name == "01 Mesh0"
    assert flat[1].name == "02 Mesh0"


def test_short_index_buffers_skip_primitive():
    # idx_count < 3 means the primitive is dropped silently.
    lod = _single_triangle_lod()
    lod.primitives[0].index_count = 2
    flat = flatten_lod(FakeIR(lods=[lod]), 0)
    assert flat == []


def test_out_of_range_indices_are_skipped():
    lod = _single_triangle_lod()
    lod.indices = [0, 99, 99]  # second/third indices out of range
    flat = flatten_lod(FakeIR(lods=[lod]), 0)
    assert flat == []


def test_smoothing_groups_returned_per_face():
    ir = FakeIR(lods=[_single_triangle_lod()])
    fm = flatten_lod(ir, 0)[0]
    assert len(fm.smoothing_groups) == len(fm.faces)


def test_static_mesh_collapses_overlapping_positions():
    # Static/basic assets keep the legacy position-dedup behavior. UV seams are
    # represented by per-face map vertices in the DCC writer, while skinned
    # meshes preserve source vertices for weight parity.
    lod = FakeLod(
        parts=[FakePart(parent_index=-1)],
        primitives=[FakePrim(
            material_index=0, part_index=0,
            index_offset=0, index_count=6, vertex_offset=0,
        )],
        vertices=[
            FakeVertex(position=(0.0, 0.0, 0.0)),  # 0
            FakeVertex(position=(1.0, 0.0, 0.0), uv0=(0.0, 0.0)),  # 1
            FakeVertex(position=(0.0, 1.0, 0.0), uv0=(0.0, 1.0)),  # 2
            FakeVertex(position=(1.0, 1.0, 0.0)),  # 3
            FakeVertex(position=(1.0, 0.0, 0.0), uv0=(1.0, 0.0)),  # duplicate position of 1
            FakeVertex(position=(0.0, 1.0, 0.0), uv0=(1.0, 1.0)),  # duplicate position of 2
        ],
        indices=[0, 1, 2, 3, 4, 5],
    )
    fm = flatten_lod(FakeIR(lods=[lod]), 0)[0]
    assert len(fm.vertices) == 4
    assert len(fm.faces) == 2


def test_skinned_mesh_preserves_source_vertices_and_weights():
    lod = FakeLod(
        parts=[FakePart(parent_index=-1)],
        primitives=[FakePrim(
            material_index=0,
            part_index=0,
            index_offset=0,
            index_count=6,
            vertex_offset=0,
            bone_table_length=2,
            bone_table=(4, 7),
        )],
        vertices=[
            FakeVertex(position=(0.0, 0.0, 0.0), bone_indices=(0, 0, 0, 0), bone_weights=(1.0, 0.0, 0.0, 0.0)),
            FakeVertex(position=(1.0, 0.0, 0.0), bone_indices=(1, 0, 0, 0), bone_weights=(1.0, 0.0, 0.0, 0.0)),
            FakeVertex(position=(0.0, 1.0, 0.0), bone_indices=(0, 0, 0, 0), bone_weights=(1.0, 0.0, 0.0, 0.0)),
            FakeVertex(position=(0.0, 0.0, 0.0), bone_indices=(1, 0, 0, 0), bone_weights=(1.0, 0.0, 0.0, 0.0)),
            FakeVertex(position=(1.0, 0.0, 0.0), bone_indices=(0, 0, 0, 0), bone_weights=(1.0, 0.0, 0.0, 0.0)),
            FakeVertex(position=(0.0, 1.0, 0.0), bone_indices=(1, 0, 0, 0), bone_weights=(1.0, 0.0, 0.0, 0.0)),
        ],
        indices=[0, 1, 2, 3, 4, 5],
    )
    fm = flatten_lod(
        FakeIR(lods=[lod], mesh_type=3),
        0,
        track_bone_data=True,
    )[0]
    assert len(fm.vertices) == 6
    assert fm.vertex_bone_data[0] == [(4, 1.0)]
    assert fm.vertex_bone_data[1] == [(4, 1.0)]
    assert fm.vertex_bone_data[2] == [(7, 1.0)]


def test_unique_material_ids_collected_in_first_use_order():
    lod = FakeLod(
        parts=[FakePart(parent_index=-1)],
        primitives=[
            FakePrim(material_index=2, part_index=0,
                     index_offset=0, index_count=3, vertex_offset=0),
            FakePrim(material_index=0, part_index=0,
                     index_offset=3, index_count=3, vertex_offset=0),
            FakePrim(material_index=2, part_index=0,
                     index_offset=6, index_count=3, vertex_offset=0),
        ],
        vertices=[
            FakeVertex(position=(0.0, 0.0, 0.0)),
            FakeVertex(position=(1.0, 0.0, 0.0)),
            FakeVertex(position=(0.0, 1.0, 0.0)),
        ],
        indices=[0, 1, 2, 0, 1, 2, 0, 1, 2],
    )
    fm = flatten_lod(FakeIR(lods=[lod]), 0)[0]
    assert fm.material_id_set == [2, 0]
