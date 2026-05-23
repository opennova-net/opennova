"""ctypes bindings for the OpenNova 3DI3 C API."""
from __future__ import annotations

import ctypes
from dataclasses import dataclass
from pathlib import Path

from ._native import load_lib


class _PackedStructure(ctypes.Structure):
    _pack_ = 1


class ThreediRawChunk(ctypes.Structure):
    pass


ThreediRawChunk._fields_ = [
    ("id", ctypes.c_char * 5),
    ("is_parent", ctypes.c_int),
    ("data", ctypes.POINTER(ctypes.c_uint8)),
    ("data_len", ctypes.c_size_t),
    ("content_len", ctypes.c_size_t),
    ("offset", ctypes.c_size_t),
    ("children", ctypes.POINTER(ThreediRawChunk)),
    ("child_count", ctypes.c_size_t),
]


class ThreediRawFile(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("root", ctypes.POINTER(ThreediRawChunk)),
        ("buffer", ctypes.POINTER(ctypes.c_uint8)),
        ("buffer_len", ctypes.c_size_t),
    ]


@dataclass(frozen=True)
class ThreediChunkSnapshot:
    id: str
    offset: int
    content_len: int
    is_parent: bool
    data: bytes = b""
    children: tuple["ThreediChunkSnapshot", ...] = ()


# ---------------------------------------------------------------------------
# Direct 3DI3 C API bindings
# ---------------------------------------------------------------------------

THREEDI_MESH_INVALID = 0
THREEDI_MESH_BASIC = 1
THREEDI_MESH_SKINNED = 2

THREEDI_MODEL_MESH_INVALID = 0
THREEDI_MODEL_MESH_BASIC = 1
THREEDI_MODEL_MESH_STATIC = 2
THREEDI_MODEL_MESH_SKINNED = 3

THREEDI_TEX_SLOT_DIFFUSE = 1
THREEDI_TEX_SLOT_DETAIL = 2
THREEDI_TEX_SLOT_NORMAL = 3
THREEDI_TEX_SLOT_NORMAL_B = 4
THREEDI_MAX_MATERIAL_TEXTURES = 24

THREEDI_MATERIAL_FLAG_ALPHA_TEST = 0x01
THREEDI_MATERIAL_FLAG_ALPHA_INVERT = 0x02
THREEDI_MATERIAL_FLAG_TWO_SIDED = 0x04
THREEDI_MATERIAL_FLAG_EMISSIVE = 0x08
THREEDI_EMISSIVE_FULL = 2


class ThreediHeader(_PackedStructure):
    _fields_ = [
        ("has_header", ctypes.c_int),
        ("name", ctypes.c_char * 17),
        ("mesh_type", ctypes.c_int),
        ("lod_count_decl", ctypes.c_int32),
        ("lod_distance", ctypes.c_int32),
    ]


class ThreediInfo(_PackedStructure):
    _fields_ = [
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("data_len", ctypes.c_size_t),
    ]


class ThreediVertex(_PackedStructure):
    _fields_ = [
        ("position", ctypes.c_float * 3),
        ("bone_weights", ctypes.c_float * 3),
        ("bone_indices", ctypes.c_uint8 * 4),
        ("normal", ctypes.c_float * 3),
        ("uv0", ctypes.c_float * 2),
        ("uv1", ctypes.c_float * 2),
        ("tangent", ctypes.c_float * 3),
        ("bitangent", ctypes.c_float * 3),
        ("flags", ctypes.c_uint32),
        ("has_tangents", ctypes.c_int),
        ("is_skinned", ctypes.c_int),
    ]


class ThreediVertexBuffer(_PackedStructure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("stride", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("items", ctypes.POINTER(ThreediVertex)),
    ]

    def __getitem__(self, index):
        return self.items[index]


class ThreediIndexBuffer(_PackedStructure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("indices", ctypes.POINTER(ctypes.c_uint16)),
    ]

    def __getitem__(self, index):
        return self.indices[index]


class ThreediTriangleStrip(_PackedStructure):
    _fields_ = [
        ("material_index", ctypes.c_int32),
        ("index_offset", ctypes.c_int32),
        ("num_indices", ctypes.c_uint16),
        ("num_triangles", ctypes.c_uint16),
        ("is_strip", ctypes.c_int32),
        ("start_vertex", ctypes.c_int32),
        ("num_vertices", ctypes.c_int32),
        ("min", ctypes.c_float * 3),
        ("max", ctypes.c_float * 3),
        ("bone_table", ctypes.c_uint8 * 16),
        ("bone_table_length", ctypes.c_int32),
    ]

    @property
    def index_count(self) -> int:
        return int(self.num_indices)

    @property
    def vertex_offset(self) -> int:
        return int(self.start_vertex)

    @property
    def vertex_count(self) -> int:
        return int(self.num_vertices)


class ThreediRenderObject(_PackedStructure):
    _fields_ = [
        ("num_strips", ctypes.c_int32),
        ("num_alpha_strips", ctypes.c_int32),
        ("parent_index", ctypes.c_int32),
        ("rel", ctypes.c_float * 3),
        ("abs", ctypes.c_float * 3),
        ("bounding_center", ctypes.c_float * 3),
        ("bounding_radius", ctypes.c_float),
    ]

    @property
    def rel_position(self):
        return self.rel

    @property
    def abs_position(self):
        return self.abs

    @property
    def primitive_count(self) -> int:
        return int(self.num_strips) + int(self.num_alpha_strips)


class ThreediMaterialTexture(_PackedStructure):
    _fields_ = [
        ("name", ctypes.c_char * 17),
        ("slot", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("frame", ctypes.c_uint8),
    ]


class ThreediAlphaGen(_PackedStructure):
    _fields_ = [
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_float),
        ("reg", ctypes.c_int32),
        ("rate", ctypes.c_float),
        ("start", ctypes.c_int16),
        ("end", ctypes.c_int16),
    ]


class ThreediRgbGen(_PackedStructure):
    _fields_ = [
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_float),
        ("reg", ctypes.c_int32),
        ("rate", ctypes.c_float),
        ("start_color", ctypes.c_float * 4),
        ("end_color", ctypes.c_float * 4),
    ]


class ThreediUvParams(_PackedStructure):
    _fields_ = [
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_float),
        ("reg", ctypes.c_int32),
        ("gen_rate", ctypes.c_float),
        ("start", ctypes.c_float),
        ("end", ctypes.c_float),
    ]


class ThreediTexAnim(_PackedStructure):
    _fields_ = [
        ("num_frames", ctypes.c_uint8),
        ("animation_type", ctypes.c_uint8),
        ("cycle_frame_time", ctypes.c_int16),
    ]


class ThreediMaterial(_PackedStructure):
    _fields_ = [
        ("index", ctypes.c_int32),
        ("shader_name", ctypes.c_char * 33),
        ("texture_count", ctypes.c_uint32),
        ("textures", ThreediMaterialTexture * THREEDI_MAX_MATERIAL_TEXTURES),
        ("material_flags", ctypes.c_uint8),
        ("alpha_gen", ThreediAlphaGen),
        ("rgb_gen", ThreediRgbGen),
        ("rgb_gen2", ThreediRgbGen),
        ("u_params", ThreediUvParams),
        ("v_params", ThreediUvParams),
        ("reflect_color", ctypes.c_float * 4),
        ("reflect_color2", ctypes.c_float * 4),
        ("emissive_type", ctypes.c_uint8),
        ("emissive_type2", ctypes.c_uint8),
        ("is_glass", ctypes.c_uint8),
        ("glass_type2", ctypes.c_uint8),
        ("alpha_test_value_byte", ctypes.c_uint8),
        ("pad", ctypes.c_uint8 * 3),
        ("animation", ThreediTexAnim),
    ]

    @property
    def flags(self) -> int:
        flags = int(self.material_flags)
        if int(self.emissive_type) == THREEDI_EMISSIVE_FULL or int(self.emissive_type2) == THREEDI_EMISSIVE_FULL:
            flags |= THREEDI_MATERIAL_FLAG_EMISSIVE
        return flags

    @property
    def name(self) -> bytes:
        shader = bytes(self.shader_name).split(b"\0", 1)[0].decode("utf-8", errors="replace")
        return f"Material_{int(self.index)}_{shader}".encode("utf-8")[:79]

    @property
    def alpha_threshold(self) -> float:
        return float(self.alpha_test_value_byte) / 255.0

    @property
    def pattrib(self) -> int:
        return 0

    @property
    def surface_type(self) -> int:
        return 0x01


class ThreediLight(_PackedStructure):
    _fields_ = [
        ("offset", ctypes.c_float * 3),
        ("atten_start", ctypes.c_float),
        ("atten_end", ctypes.c_float),
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_uint8),
        ("rate", ctypes.c_uint16),
        ("color_start", ctypes.c_uint8 * 4),
        ("color_end", ctypes.c_uint8 * 4),
        ("subobj_index", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("unknown1", ctypes.c_uint8),
        ("falloff_byte", ctypes.c_uint8),
        ("rotation", ctypes.c_float * 4),
        ("view_proj", ctypes.c_float * 16),
    ]

    @property
    def attenuation_start(self) -> float:
        return float(self.atten_start)

    @property
    def attenuation_end(self) -> float:
        return float(self.atten_end)

    @property
    def part_index(self) -> int:
        return int(self.subobj_index)

    @property
    def falloff(self) -> float:
        return float(self.falloff_byte)

    @property
    def light_type(self) -> int:
        return (int(self.flags) >> 3) & 1


class ThreediUserPoint(_PackedStructure):
    _fields_ = [
        ("x", ctypes.c_int32),
        ("y", ctypes.c_int32),
        ("z", ctypes.c_int32),
        ("rot_x", ctypes.c_int32),
        ("rot_y", ctypes.c_int32),
        ("rot_z", ctypes.c_int32),
        ("subobject_index", ctypes.c_int32),
        ("userpoint_type", ctypes.c_int32),
        ("name", ctypes.c_char * 17),
    ]

    @property
    def position(self):
        return (self.y / 65536.0, self.z / 65536.0, self.x / 65536.0)

    @property
    def direction(self):
        return (self.rot_y / 65536.0, self.rot_z / 65536.0, self.rot_x / 65536.0)

    @property
    def part_index(self) -> int:
        return int(self.subobject_index)

    @property
    def type_code(self) -> int:
        return int(self.userpoint_type)


class ThreediCollisionModelData(_PackedStructure):
    _fields_ = [
        ("bbox", ctypes.c_float * 6),
        ("radii", ctypes.c_float * 3),
        ("num_vertices", ctypes.c_int32),
        ("num_normals", ctypes.c_int32),
        ("num_faces", ctypes.c_int32),
        ("num_objects", ctypes.c_int32),
        ("num_transforms", ctypes.c_int32),
        ("num_bounding_planes", ctypes.c_int32),
        ("num_bounding_volumes", ctypes.c_int32),
    ]


class ThreediBoundingPlane(_PackedStructure):
    _fields_ = [
        ("flags", ctypes.c_int16),
        ("normal", ctypes.c_float * 3),
        ("radius", ctypes.c_float),
    ]

    @property
    def distance(self) -> float:
        return float(self.radius)


class ThreediBoundingVolume(_PackedStructure):
    _fields_ = [
        ("collidable_type", ctypes.c_int32),
        ("flags", ctypes.c_int32),
        ("min_x_fp16", ctypes.c_int32),
        ("min_y_fp16", ctypes.c_int32),
        ("min_z_fp16", ctypes.c_int32),
        ("max_x_fp16", ctypes.c_int32),
        ("max_y_fp16", ctypes.c_int32),
        ("max_z_fp16", ctypes.c_int32),
        ("plane_count", ctypes.c_int32),
    ]

    @property
    def type(self) -> int:
        return int(self.collidable_type)

    @property
    def min(self):
        return (self.min_x_fp16 / 65536.0, self.min_y_fp16 / 65536.0, self.min_z_fp16 / 65536.0)

    @property
    def max(self):
        return (self.max_x_fp16 / 65536.0, self.max_y_fp16 / 65536.0, self.max_z_fp16 / 65536.0)


class ThreediCollisionVertex(_PackedStructure):
    _fields_ = [("position", ctypes.c_float * 3)]


class ThreediCollisionNormal(_PackedStructure):
    _fields_ = [
        ("normal", ctypes.c_float * 3),
        ("dominate_axis", ctypes.c_int16),
    ]


class ThreediCollisionFace(_PackedStructure):
    _fields_ = [
        ("vert_index", ctypes.c_int16 * 3),
        ("normal_index", ctypes.c_int16),
        ("plane_dist_fp16", ctypes.c_int32),
        ("min_x_fp16", ctypes.c_int32),
        ("min_y_fp16", ctypes.c_int32),
        ("min_z_fp16", ctypes.c_int32),
        ("max_x_fp16", ctypes.c_int32),
        ("max_y_fp16", ctypes.c_int32),
        ("max_z_fp16", ctypes.c_int32),
        ("material_flags", ctypes.c_uint32),
        ("poly_type", ctypes.c_uint8),
        ("pad", ctypes.c_uint8 * 3),
    ]


class ThreediCollisionObject(_PackedStructure):
    _fields_ = [
        ("unk0", ctypes.c_int32),
        ("num_vertices", ctypes.c_int32),
        ("num_faces", ctypes.c_int32),
        ("num_planes", ctypes.c_int32),
        ("num_bounding_volumes", ctypes.c_int32),
        ("parent_subobject_index", ctypes.c_int32),
        ("unk3", ctypes.c_int32),
        ("unk4", ctypes.c_int32),
        ("unk5", ctypes.c_int32),
        ("offset", ctypes.c_float * 3),
        ("min", ctypes.c_float * 3),
        ("max", ctypes.c_float * 3),
        ("med", ctypes.c_float * 3),
        ("radius", ctypes.c_float),
    ]


class ThreediCollisionTranslation(_PackedStructure):
    _fields_ = [("translation", ctypes.c_float * 3)]


class ThreediCollisionModel(_PackedStructure):
    _fields_ = [
        ("model_data", ThreediCollisionModelData),
        ("planes", ctypes.POINTER(ThreediBoundingPlane)),
        ("plane_count", ctypes.c_size_t),
        ("volumes", ctypes.POINTER(ThreediBoundingVolume)),
        ("volume_count", ctypes.c_size_t),
        ("vertices", ctypes.POINTER(ThreediCollisionVertex)),
        ("vertex_count", ctypes.c_size_t),
        ("normals", ctypes.POINTER(ThreediCollisionNormal)),
        ("normal_count", ctypes.c_size_t),
        ("faces", ctypes.POINTER(ThreediCollisionFace)),
        ("face_count", ctypes.c_size_t),
        ("objects", ctypes.POINTER(ThreediCollisionObject)),
        ("object_count", ctypes.c_size_t),
        ("translations", ctypes.POINTER(ThreediCollisionTranslation)),
        ("translation_count", ctypes.c_size_t),
    ]

    @property
    def model_min(self):
        return (self.model_data.bbox[0], self.model_data.bbox[1], self.model_data.bbox[2])

    @property
    def model_max(self):
        return (self.model_data.bbox[3], self.model_data.bbox[4], self.model_data.bbox[5])

    @property
    def model_center(self):
        return (
            (self.model_data.bbox[0] + self.model_data.bbox[3]) * 0.5,
            (self.model_data.bbox[1] + self.model_data.bbox[4]) * 0.5,
            (self.model_data.bbox[2] + self.model_data.bbox[5]) * 0.5,
        )


class ThreediOcclusionVertex(_PackedStructure):
    _fields_ = [("position", ctypes.c_float * 3)]


class ThreediOcclusionFace(_PackedStructure):
    _fields_ = [
        ("raw_indices", ctypes.c_uint32),
        ("edge_data", ctypes.c_uint32),
        ("other_edge_data", ctypes.c_uint32),
    ]


class ThreediOcclusionObject(_PackedStructure):
    _fields_ = [
        ("type", ctypes.c_uint8),
        ("parent_subobject_index", ctypes.c_uint8),
        ("connecting_subobject", ctypes.c_uint8),
        ("unused0", ctypes.c_uint8),
        ("position", ctypes.c_float * 3),
        ("radius", ctypes.c_float),
        ("unk1", ctypes.c_int32),
        ("num_vertices", ctypes.c_int32),
        ("num_planes", ctypes.c_int32),
        ("face_count", ctypes.c_int32),
    ]


class ThreediOcclusionPlane(_PackedStructure):
    _fields_ = [
        ("normal", ctypes.c_float * 3),
        ("radius", ctypes.c_float),
    ]


class ThreediTransform(_PackedStructure):
    _fields_ = [
        ("control", ctypes.c_uint8),
        ("control_param", ctypes.c_uint8),
        ("rate", ctypes.c_int16),
        ("start", ctypes.c_int16),
        ("end", ctypes.c_int16),
    ]


class ThreediPartAnimation(_PackedStructure):
    _fields_ = [
        ("flags", ctypes.c_uint32),
        ("parent_subobject", ctypes.c_uint8),
        ("subobject_index", ctypes.c_uint8),
        ("matrix_index", ctypes.c_uint8),
        ("matrix_offset", ctypes.c_uint8),
        ("bind_matrix_index", ctypes.c_int32),
        ("rotation_x", ThreediTransform),
        ("rotation_y", ThreediTransform),
        ("rotation_z", ThreediTransform),
        ("scale_x", ThreediTransform),
        ("scale_y", ThreediTransform),
        ("scale_z", ThreediTransform),
        ("translation", ThreediTransform),
    ]

    @property
    def parent_part(self) -> int:
        return int(self.parent_subobject)

    @property
    def part_index(self) -> int:
        return int(self.subobject_index)


class ThreediControlRegister(_PackedStructure):
    _fields_ = [("name", ctypes.c_char * 25)]


class ThreediCtrl(_PackedStructure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("record_size", ctypes.c_uint32),
        ("registers", ctypes.POINTER(ThreediControlRegister)),
    ]


class ThreediRawTable(_PackedStructure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("record_size", ctypes.c_uint32),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("data_len", ctypes.c_size_t),
    ]


class ThreediMatrix4x4(_PackedStructure):
    _fields_ = [("m", ctypes.c_float * 16)]


class ThreediMatrixTable(_PackedStructure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("record_size", ctypes.c_uint32),
        ("matrices", ctypes.POINTER(ThreediMatrix4x4)),
    ]


class ThreediTransformDecoded(ctypes.Structure):
    _fields_ = [
        ("control", ctypes.c_uint8),
        ("control_name", ctypes.c_char_p),
        ("control_param", ctypes.c_uint8),
        ("ctrl_reg_name", ctypes.c_char_p),
        ("phase", ctypes.c_float),
        ("rate", ctypes.c_float),
        ("start", ctypes.c_float),
        ("end", ctypes.c_float),
        ("is_rotation", ctypes.c_int),
    ]


class ThreediLod(_PackedStructure):
    _fields_ = [
        ("model_type", ctypes.c_char * 5),
        ("lod_threshold", ctypes.c_int32),
        ("rmdl_render_object_count", ctypes.c_int32),
        ("vertices", ThreediVertexBuffer),
        ("indices", ThreediIndexBuffer),
        ("strips", ctypes.POINTER(ThreediTriangleStrip)),
        ("strip_count", ctypes.c_size_t),
        ("strip_record_size", ctypes.c_uint32),
        ("render_objects", ctypes.POINTER(ThreediRenderObject)),
        ("render_object_count", ctypes.c_size_t),
        ("part_animations", ctypes.POINTER(ThreediPartAnimation)),
        ("part_animation_count", ctypes.c_size_t),
        ("part_animation_record_size", ctypes.c_uint32),
    ]

    @property
    def threshold(self) -> int:
        return int(self.lod_threshold)

    @property
    def vertex_count(self) -> int:
        return int(self.vertices.count)

    @property
    def index_count(self) -> int:
        return int(self.indices.count)

    @property
    def primitives(self):
        return self.strips

    @property
    def primitive_count(self) -> int:
        return int(self.strip_count)

    @property
    def parts(self):
        return self.render_objects

    @property
    def part_count(self) -> int:
        return int(self.render_object_count)

    @property
    def declared_part_count(self) -> int:
        return int(self.rmdl_render_object_count)


class Threedi3di3(_PackedStructure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("header", ThreediHeader),
        ("info", ThreediInfo),
        ("lods", ctypes.POINTER(ThreediLod)),
        ("lod_count", ctypes.c_size_t),
        ("material_count", ctypes.c_uint32),
        ("material_record_size", ctypes.c_uint32),
        ("materials", ctypes.POINTER(ThreediMaterial)),
        ("lights", ctypes.POINTER(ThreediLight)),
        ("light_count", ctypes.c_size_t),
        ("user_points", ctypes.POINTER(ThreediUserPoint)),
        ("user_point_count", ctypes.c_size_t),
        ("collision", ctypes.POINTER(ThreediCollisionModel)),
        ("ctrl", ThreediCtrl),
        ("ovrt", ThreediRawTable),
        ("mtrx", ThreediMatrixTable),
        ("occlusion_vertices", ctypes.POINTER(ThreediOcclusionVertex)),
        ("occlusion_vertex_count", ctypes.c_size_t),
        ("occlusion_vertex_record_size", ctypes.c_uint32),
        ("occlusion_faces", ctypes.POINTER(ThreediOcclusionFace)),
        ("occlusion_face_count", ctypes.c_size_t),
        ("occlusion_face_record_size", ctypes.c_uint32),
        ("occlusion_objects", ctypes.POINTER(ThreediOcclusionObject)),
        ("occlusion_object_count", ctypes.c_size_t),
        ("occlusion_object_record_size", ctypes.c_uint32),
        ("occlusion_planes", ctypes.POINTER(ThreediOcclusionPlane)),
        ("occlusion_plane_count", ctypes.c_size_t),
        ("occlusion_plane_record_size", ctypes.c_uint32),
        ("part_animations", ctypes.POINTER(ThreediPartAnimation)),
        ("part_animation_count", ctypes.c_size_t),
        ("part_animation_record_size", ctypes.c_uint32),
    ]

    @property
    def name(self):
        return self.header.name

    @property
    def render_function(self):
        if self.lod_count > 0:
            return self.lods[0].model_type
        return b""

    @property
    def source_format(self) -> int:
        return 1

    @property
    def mesh_type(self) -> int:
        if int(self.header.mesh_type) == THREEDI_MESH_SKINNED:
            return THREEDI_MODEL_MESH_SKINNED
        if int(self.header.mesh_type) == THREEDI_MESH_BASIC:
            return THREEDI_MODEL_MESH_BASIC
        return THREEDI_MODEL_MESH_INVALID

    @property
    def userpoints(self):
        return self.user_points

    @property
    def userpoint_count(self) -> int:
        return int(self.user_point_count)

    @property
    def control_registers(self):
        return self.ctrl.registers

    @property
    def control_register_count(self) -> int:
        return int(self.ctrl.count)

    @property
    def matrices(self):
        return self.mtrx.matrices

    @property
    def matrix_count(self) -> int:
        return int(self.mtrx.count)


_bound_3di3 = False
_bound_chunk_file = False
_bound_panm_helpers = False


def _bind_chunk_file():
    global _bound_chunk_file
    if _bound_chunk_file:
        return
    lib = load_lib()
    lib.threedi_read_file.restype = ctypes.c_int
    lib.threedi_read_file.argtypes = [ctypes.c_char_p, ctypes.POINTER(ThreediRawFile)]
    lib.threedi_free_file.restype = None
    lib.threedi_free_file.argtypes = [ctypes.POINTER(ThreediRawFile)]
    _bound_chunk_file = True


def _bind_panm_helpers():
    global _bound_panm_helpers
    if _bound_panm_helpers:
        return
    lib = load_lib()
    lib.threedi_ctrl_reg_name.restype = ctypes.c_char_p
    lib.threedi_ctrl_reg_name.argtypes = [ctypes.POINTER(ThreediCtrl), ctypes.c_uint8]
    lib.threedi_decode_transform.restype = ctypes.c_int
    lib.threedi_decode_transform.argtypes = [
        ctypes.POINTER(ThreediTransform),
        ctypes.c_int,
        ctypes.POINTER(ThreediCtrl),
        ctypes.POINTER(ThreediTransformDecoded),
    ]
    _bound_panm_helpers = True


def _bind_3di3():
    global _bound_3di3
    if _bound_3di3:
        return
    lib = load_lib()
    lib.threedi_3di3_read.restype = ctypes.c_int
    lib.threedi_3di3_read.argtypes = [ctypes.c_char_p, ctypes.POINTER(Threedi3di3)]
    lib.threedi_read_model_auto.restype = ctypes.c_int
    lib.threedi_read_model_auto.argtypes = [ctypes.c_char_p, ctypes.POINTER(Threedi3di3)]
    lib.threedi_3di3_free.restype = None
    lib.threedi_3di3_free.argtypes = [ctypes.POINTER(Threedi3di3)]
    _bound_3di3 = True


def read_model_3di3(path: str | bytes | Path) -> Threedi3di3:
    """Read a 3DI3 .3di file and return its typed 3DI3 structure."""
    try:
        magic = Path(path).read_bytes()[:4] if not isinstance(path, bytes) else b""
    except OSError:
        magic = b""
    if magic in (b"GPM\x02", b"GPS\x02", b"GPP\x02"):
        raise RuntimeError("Only 3DI3 .3di files are supported")

    _bind_3di3()
    raw_path = path if isinstance(path, bytes) else str(path).encode("utf-8")

    lib = load_lib()
    model = Threedi3di3()
    rc = lib.threedi_3di3_read(raw_path, ctypes.byref(model))
    if rc != 0:
        lib.threedi_3di3_free(ctypes.byref(model))
        raise RuntimeError(f"threedi_3di3_read failed for {raw_path!r}")
    return model


def read_model(path: str | bytes | Path) -> Threedi3di3:
    """Read a supported .3di model and normalize it to the 3DI3 structure."""
    _bind_3di3()
    raw_path = path if isinstance(path, bytes) else str(path).encode("utf-8")

    lib = load_lib()
    model = Threedi3di3()
    rc = lib.threedi_read_model_auto(raw_path, ctypes.byref(model))
    if rc != 0:
        lib.threedi_3di3_free(ctypes.byref(model))
        raise RuntimeError(f"threedi_read_model_auto failed for {raw_path!r}")
    return model


read_model_auto = read_model


def free_model_3di3(model: Threedi3di3) -> None:
    """Free all C-side allocations inside a 3DI3 structure."""
    _bind_3di3()
    lib = load_lib()
    lib.threedi_3di3_free(ctypes.byref(model))


def ctrl_reg_name_3di3(ctrl: ThreediCtrl, index: int) -> str:
    """Resolve a 3DI3 control-register name through the native PANM helper."""
    if index < 0 or index > 255:
        return ""
    _bind_panm_helpers()
    lib = load_lib()
    name = lib.threedi_ctrl_reg_name(ctypes.byref(ctrl), ctypes.c_uint8(index))
    if not name:
        return ""
    return name.decode("utf-8", errors="replace").rstrip("\x00")


def decode_transform_3di3(
    transform: ThreediTransform,
    *,
    is_rotation: bool,
    ctrl: ThreediCtrl,
) -> ThreediTransformDecoded:
    """Decode packed 3DI3 PANM transform values through the native helper."""
    _bind_panm_helpers()
    lib = load_lib()
    decoded = ThreediTransformDecoded()
    rc = lib.threedi_decode_transform(
        ctypes.byref(transform),
        1 if is_rotation else 0,
        ctypes.byref(ctrl),
        ctypes.byref(decoded),
    )
    if rc != 0:
        raise RuntimeError("threedi_decode_transform failed")
    return decoded


def read_chunk_tree_3di3(path: str | bytes | Path) -> ThreediChunkSnapshot:
    """Read a 3DI3 chunk tree using the native reader and copy it into Python."""
    _bind_chunk_file()
    raw_path = path if isinstance(path, bytes) else str(path).encode("utf-8")

    lib = load_lib()
    file = ThreediRawFile()
    rc = lib.threedi_read_file(raw_path, ctypes.byref(file))
    if rc != 0 or not file.root:
        lib.threedi_free_file(ctypes.byref(file))
        raise ValueError(f"{path} is not a readable 3DI3 file")
    try:
        return _snapshot_raw_chunk(file.root.contents)
    finally:
        lib.threedi_free_file(ctypes.byref(file))


def _snapshot_raw_chunk(chunk: ThreediRawChunk) -> ThreediChunkSnapshot:
    chunk_id = bytes(chunk.id).split(b"\0", 1)[0].decode("ascii", errors="replace")
    if bool(chunk.is_parent):
        children = tuple(
            _snapshot_raw_chunk(chunk.children[index])
            for index in range(int(chunk.child_count))
        )
        data = b""
    else:
        children = ()
        data = ctypes.string_at(chunk.data, int(chunk.data_len)) if chunk.data_len else b""
    return ThreediChunkSnapshot(
        id=chunk_id,
        offset=int(chunk.offset),
        content_len=int(chunk.content_len),
        is_parent=bool(chunk.is_parent),
        data=data,
        children=children,
    )
