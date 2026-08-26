"""
ctypes bindings for the 3DI3 model C API (libopennova.so / opennova.dll).

Every ctypes Structure here mirrors the corresponding C struct in
engine/formats/threedi/threedi_3di3.h.  The C structs are
`#pragma pack(push, 1)`, so every mirror carries `_pack_ = 1`.  Keep them in
sync — layout drift is caught by tests/test_threedi_ffi_mirror.py.
"""

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Enum constants
# ---------------------------------------------------------------------------

THREEDI_MESH_INVALID = 0
THREEDI_MESH_BASIC = 1
THREEDI_MESH_SKINNED = 2

THREEDI_TEX_SLOT_DIFFUSE = 1
THREEDI_TEX_SLOT_DETAIL = 2
THREEDI_TEX_SLOT_NORMAL = 3
THREEDI_TEX_SLOT_NORMAL_B = 4

# The MTRL texture-slot array capacity (ThreediMaterial.textures).
THREEDI_MAX_MATERIAL_TEXTURES = 24

THREEDI_MATERIAL_FLAG_ALPHA_TEST = 0x01
THREEDI_MATERIAL_FLAG_ALPHA_INVERT = 0x02
THREEDI_MATERIAL_FLAG_TWO_SIDED = 0x04

THREEDI_TEX_FLAG_ANIMATED = 0x01
THREEDI_TEX_FLAG_CLAMPED = 0x02

THREEDI_EMISSIVE_NONE = 0
THREEDI_EMISSIVE_FULL = 2

THREEDI_LIGHT_FLAG_DISABLE_CORONA = 0x01
THREEDI_LIGHT_FLAG_DISABLE_TERRAIN = 0x02
THREEDI_LIGHT_FLAG_DISABLE_OBJECTS = 0x04
THREEDI_LIGHT_FLAG_TYPE_TARGET = 0x08

THREEDI_VERTEX_FLAG_TANGENTS = 0x14
THREEDI_VERTEX_FLAG_SKINNED = 0x40

# ---------------------------------------------------------------------------
# Struct definitions  (order and packing must match threedi_3di3.h exactly)
# ---------------------------------------------------------------------------


class ThreediHeader(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("has_header",     ctypes.c_int),
        ("name",           ctypes.c_char * 17),
        ("mesh_type",      ctypes.c_int),  # ThreediMeshType enum
        ("lod_count_decl", ctypes.c_int32),
        ("max_radius_fp16", ctypes.c_int32),
    ]


class ThreediInfo(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("data",     ctypes.POINTER(ctypes.c_uint8)),
        ("data_len", ctypes.c_size_t),
    ]


class ThreediVertex(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("position",     ctypes.c_float * 3),
        ("bone_weights", ctypes.c_float * 3),  # only valid if is_skinned
        ("bone_indices", ctypes.c_uint8 * 4),  # only valid if is_skinned
        ("normal",       ctypes.c_float * 3),
        ("uv0",          ctypes.c_float * 2),
        ("uv1",          ctypes.c_float * 2),
        ("tangent",      ctypes.c_float * 3),  # only valid if has_tangents
        ("bitangent",    ctypes.c_float * 3),  # only valid if has_tangents
        ("flags",        ctypes.c_uint32),
        ("has_tangents", ctypes.c_int),
        ("is_skinned",   ctypes.c_int),
    ]


class ThreediVertexBuffer(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("count",  ctypes.c_uint32),
        ("stride", ctypes.c_uint32),
        ("flags",  ctypes.c_uint32),
        ("items",  ctypes.POINTER(ThreediVertex)),
    ]


class ThreediIndexBuffer(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("count",   ctypes.c_uint32),
        ("indices", ctypes.POINTER(ctypes.c_uint16)),
    ]


class ThreediTriangleStrip(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("material_index",    ctypes.c_int32),
        ("index_offset",      ctypes.c_int32),
        ("num_indices",       ctypes.c_uint16),
        ("num_triangles",     ctypes.c_uint16),
        ("is_strip",          ctypes.c_int32),
        ("start_vertex",      ctypes.c_int32),
        ("num_vertices",      ctypes.c_int32),
        ("min",               ctypes.c_float * 3),
        ("max",               ctypes.c_float * 3),
        ("bone_table",        ctypes.c_uint8 * 16),
        ("bone_table_length", ctypes.c_int32),  # 0 if absent
    ]


class ThreediRenderObject(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("num_strips",       ctypes.c_int32),
        ("num_alpha_strips", ctypes.c_int32),
        ("parent_index",     ctypes.c_int32),
        ("rel",              ctypes.c_float * 3),
        ("abs",              ctypes.c_float * 3),
        ("bounding_center",  ctypes.c_float * 3),
        ("bounding_radius",  ctypes.c_float),
    ]


class ThreediMaterialTexture(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("name",  ctypes.c_char * 17),
        ("slot",  ctypes.c_uint8),
        ("type",  ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("frame", ctypes.c_uint8),
    ]


class ThreediAlphaGen(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_float),
        ("reg",   ctypes.c_int32),  # -1 if unused
        ("rate",  ctypes.c_float),
        ("start", ctypes.c_int16),
        ("end",   ctypes.c_int16),
    ]


class ThreediRgbGen(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("style",       ctypes.c_uint8),
        ("phase",       ctypes.c_float),
        ("reg",         ctypes.c_int32),  # -1 if unused
        ("rate",        ctypes.c_float),
        ("start_color", ctypes.c_float * 4),  # RGBA 0..1
        ("end_color",   ctypes.c_float * 4),  # RGBA 0..1
    ]


class ThreediUvParams(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("style",    ctypes.c_uint8),
        ("phase",    ctypes.c_float),
        ("reg",      ctypes.c_int32),  # -1 if unused
        ("gen_rate", ctypes.c_float),
        ("start",    ctypes.c_float),
        ("end",      ctypes.c_float),
    ]


class ThreediTexAnim(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("num_frames",       ctypes.c_uint8),
        ("animation_type",   ctypes.c_uint8),  # 0=time, 1=ctrl reg driven
        ("cycle_frame_time", ctypes.c_int16),
    ]


class ThreediMaterial(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("index",                ctypes.c_int32),
        ("shader_name",          ctypes.c_char * 33),
        ("texture_count",        ctypes.c_uint32),
        ("textures",             ThreediMaterialTexture * 24),
        ("material_flags",       ctypes.c_uint8),
        ("alpha_gen",            ThreediAlphaGen),
        ("rgb_gen",              ThreediRgbGen),
        ("rgb_gen2",             ThreediRgbGen),
        ("u_params",             ThreediUvParams),
        ("v_params",             ThreediUvParams),
        ("reflect_color",        ctypes.c_float * 4),
        ("reflect_color2",       ctypes.c_float * 4),
        ("emissive_type",        ctypes.c_uint8),
        ("emissive_type2",       ctypes.c_uint8),
        ("is_glass",             ctypes.c_uint8),
        ("glass_type2",          ctypes.c_uint8),
        ("alpha_test_value_byte", ctypes.c_uint8),
        ("pad",                  ctypes.c_uint8 * 3),
        ("animation",            ThreediTexAnim),
    ]


class ThreediLight(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("offset",       ctypes.c_float * 3),
        ("atten_start",  ctypes.c_float),
        ("atten_end",    ctypes.c_float),
        ("style",        ctypes.c_uint8),
        ("phase",        ctypes.c_uint8),
        ("rate",         ctypes.c_uint16),
        ("color_start",  ctypes.c_uint8 * 4),  # packed B,G,R,unused
        ("color_end",    ctypes.c_uint8 * 4),  # packed B,G,R,unused
        ("subobj_index", ctypes.c_uint8),
        ("flags",        ctypes.c_uint8),
        ("unknown1",     ctypes.c_uint8),
        ("falloff_byte", ctypes.c_uint8),
        ("rotation",     ctypes.c_float * 4),   # { -rotY, rotZ, rotX, cos(falloff) }
        ("view_proj",    ctypes.c_float * 16),
    ]


class ThreediUserPoint(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("x",               ctypes.c_int32),
        ("y",               ctypes.c_int32),
        ("z",               ctypes.c_int32),
        ("rot_x",           ctypes.c_int32),
        ("rot_y",           ctypes.c_int32),
        ("rot_z",           ctypes.c_int32),
        ("subobject_index", ctypes.c_int32),
        ("userpoint_type",  ctypes.c_int32),
        ("name",            ctypes.c_char * 17),
    ]


# --- Collision ---


class ThreediCollisionModelData(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("bbox",                 ctypes.c_float * 6),
        ("radii",                ctypes.c_float * 3),
        ("num_vertices",         ctypes.c_int32),
        ("num_normals",          ctypes.c_int32),
        ("num_faces",            ctypes.c_int32),
        ("num_objects",          ctypes.c_int32),
        ("num_transforms",       ctypes.c_int32),
        ("num_bounding_planes",  ctypes.c_int32),
        ("num_bounding_volumes", ctypes.c_int32),
    ]


class ThreediBoundingPlane(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("flags",  ctypes.c_int16),
        ("normal", ctypes.c_float * 3),
        ("radius", ctypes.c_float),
    ]


class ThreediBoundingVolume(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("collidable_type", ctypes.c_int32),
        ("flags",           ctypes.c_int32),
        ("min_x_fp16",      ctypes.c_int32),
        ("min_y_fp16",      ctypes.c_int32),
        ("min_z_fp16",      ctypes.c_int32),
        ("max_x_fp16",      ctypes.c_int32),
        ("max_y_fp16",      ctypes.c_int32),
        ("max_z_fp16",      ctypes.c_int32),
        ("plane_count",     ctypes.c_int32),
    ]


class ThreediCollisionVertex(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("position", ctypes.c_float * 3)]


class ThreediCollisionNormal(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("normal",        ctypes.c_float * 3),
        ("dominate_axis", ctypes.c_int16),
    ]


class ThreediCollisionFace(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("vert_index",      ctypes.c_int16 * 3),
        ("normal_index",    ctypes.c_int16),
        ("plane_dist_fp16", ctypes.c_int32),
        ("min_x_fp16",      ctypes.c_int32),
        ("min_y_fp16",      ctypes.c_int32),
        ("min_z_fp16",      ctypes.c_int32),
        ("max_x_fp16",      ctypes.c_int32),
        ("max_y_fp16",      ctypes.c_int32),
        ("max_z_fp16",      ctypes.c_int32),
        ("material_flags",  ctypes.c_uint32),
        ("poly_type",       ctypes.c_uint8),
        ("pad",             ctypes.c_uint8 * 3),
    ]


class ThreediCollisionObject(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("unk0",                    ctypes.c_int32),
        ("num_vertices",            ctypes.c_int32),
        ("num_faces",               ctypes.c_int32),
        ("num_normals",             ctypes.c_int32),
        ("num_bounding_volumes",    ctypes.c_int32),
        ("parent_subobject_index",  ctypes.c_int32),
        ("unk3",                    ctypes.c_int32),
        ("unk4",                    ctypes.c_int32),
        ("unk5",                    ctypes.c_int32),
        ("offset",                  ctypes.c_int32 * 3),  # exact authored 16.16
        ("min",                     ctypes.c_int32 * 3),
        ("max",                     ctypes.c_int32 * 3),
        ("med",                     ctypes.c_int32 * 3),
        ("radius",                  ctypes.c_int32),
    ]


class ThreediCollisionTranslation(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("translation", ctypes.c_int32 * 3)]


class ThreediCollisionModel(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("model_data",        ThreediCollisionModelData),
        ("planes",            ctypes.POINTER(ThreediBoundingPlane)),
        ("plane_count",       ctypes.c_size_t),
        ("volumes",           ctypes.POINTER(ThreediBoundingVolume)),
        ("volume_count",      ctypes.c_size_t),
        ("vertices",          ctypes.POINTER(ThreediCollisionVertex)),
        ("vertex_count",      ctypes.c_size_t),
        ("normals",           ctypes.POINTER(ThreediCollisionNormal)),
        ("normal_count",      ctypes.c_size_t),
        ("faces",             ctypes.POINTER(ThreediCollisionFace)),
        ("face_count",        ctypes.c_size_t),
        ("objects",           ctypes.POINTER(ThreediCollisionObject)),
        ("object_count",      ctypes.c_size_t),
        ("translations",      ctypes.POINTER(ThreediCollisionTranslation)),
        ("translation_count", ctypes.c_size_t),
    ]


# --- Occlusion ---


class ThreediOcclusionVertex(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("position", ctypes.c_float * 3)]


class ThreediOcclusionFace(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("raw_indices",     ctypes.c_uint32),
        ("edge_data",       ctypes.c_uint32),
        ("other_edge_data", ctypes.c_uint32),
    ]


class ThreediOcclusionObject(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("type",                    ctypes.c_uint8),
        ("parent_subobject_index",  ctypes.c_uint8),
        ("connecting_subobject",    ctypes.c_uint8),
        ("unused0",                 ctypes.c_uint8),
        ("position",                ctypes.c_float * 3),
        ("radius",                  ctypes.c_float),
        ("glow_scale",              ctypes.c_float),
        ("num_vertices",            ctypes.c_int32),
        ("num_planes",              ctypes.c_int32),
        ("face_count",              ctypes.c_int32),
    ]


class ThreediOcclusionPlane(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("normal", ctypes.c_float * 3),
        ("radius", ctypes.c_float),
    ]


# --- Part animation (PANM) ---


class ThreediTransform(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("control",       ctypes.c_uint8),
        ("control_param", ctypes.c_uint8),
        ("rate",          ctypes.c_int16),
        ("start",         ctypes.c_int16),
        ("end",           ctypes.c_int16),
    ]


class ThreediPartAnimation(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("flags",             ctypes.c_uint32),
        ("parent_subobject",  ctypes.c_uint8),
        ("subobject_index",   ctypes.c_uint8),  # transform_as
        ("matrix_index",      ctypes.c_uint8),
        ("matrix_offset",     ctypes.c_uint8),
        ("bind_matrix_index", ctypes.c_int32),
        ("rotation_x",        ThreediTransform),
        ("rotation_y",        ThreediTransform),
        ("rotation_z",        ThreediTransform),
        ("scale_x",           ThreediTransform),
        ("scale_y",           ThreediTransform),
        ("scale_z",           ThreediTransform),
        ("translation",       ThreediTransform),
    ]


# --- Misc tables ---


class ThreediControlRegister(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("name", ctypes.c_char * 25)]


class ThreediCtrl(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("count",       ctypes.c_uint32),
        ("record_size", ctypes.c_uint32),
        ("registers",   ctypes.POINTER(ThreediControlRegister)),
    ]


class ThreediRawTable(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("count",       ctypes.c_uint32),
        ("record_size", ctypes.c_uint32),
        ("data",        ctypes.POINTER(ctypes.c_uint8)),
        ("data_len",    ctypes.c_size_t),
    ]


class ThreediVec3(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("z", ctypes.c_float),
    ]


class ThreediMatrix4x4(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("m", ctypes.c_float * 16)]


class ThreediMatrixTable(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("count",       ctypes.c_uint32),
        ("record_size", ctypes.c_uint32),
        ("matrices",    ctypes.POINTER(ThreediMatrix4x4)),
    ]


# --- LOD + model ---


class ThreediLod(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("model_type",                 ctypes.c_char * 5),  # from RMDL
        ("lod_threshold",              ctypes.c_int32),
        ("rmdl_render_object_count",   ctypes.c_int32),
        ("vertices",                   ThreediVertexBuffer),
        ("indices",                    ThreediIndexBuffer),
        ("strips",                     ctypes.POINTER(ThreediTriangleStrip)),
        ("strip_count",                ctypes.c_size_t),
        ("strip_record_size",          ctypes.c_uint32),
        ("render_objects",             ctypes.POINTER(ThreediRenderObject)),
        ("render_object_count",        ctypes.c_size_t),
        ("part_animations",            ctypes.POINTER(ThreediPartAnimation)),
        ("part_animation_count",       ctypes.c_size_t),
        ("part_animation_record_size", ctypes.c_uint32),
    ]


class Threedi3di3(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("version",                      ctypes.c_uint32),
        ("header",                       ThreediHeader),
        ("info",                         ThreediInfo),
        ("lods",                         ctypes.POINTER(ThreediLod)),
        ("lod_count",                    ctypes.c_size_t),

        # Materials (MTRL)
        ("material_count",               ctypes.c_uint32),
        ("material_record_size",         ctypes.c_uint32),
        ("materials",                    ctypes.POINTER(ThreediMaterial)),

        # Lights (LGHT)
        ("lights",                       ctypes.POINTER(ThreediLight)),
        ("light_count",                  ctypes.c_size_t),

        # User points (USRP)
        ("user_points",                  ctypes.POINTER(ThreediUserPoint)),
        ("user_point_count",             ctypes.c_size_t),

        # Collision data
        ("collision",                    ctypes.POINTER(ThreediCollisionModel)),

        # Misc chunks
        ("ctrl",                         ThreediCtrl),
        ("ovrt",                         ThreediRawTable),
        ("mtrx",                         ThreediMatrixTable),

        # Occlusion (OCCL children)
        ("occlusion_vertices",           ctypes.POINTER(ThreediOcclusionVertex)),
        ("occlusion_vertex_count",       ctypes.c_size_t),
        ("occlusion_vertex_record_size", ctypes.c_uint32),
        ("occlusion_faces",              ctypes.POINTER(ThreediOcclusionFace)),
        ("occlusion_face_count",         ctypes.c_size_t),
        ("occlusion_face_record_size",   ctypes.c_uint32),
        ("occlusion_objects",            ctypes.POINTER(ThreediOcclusionObject)),
        ("occlusion_object_count",       ctypes.c_size_t),
        ("occlusion_object_record_size", ctypes.c_uint32),
        ("occlusion_planes",             ctypes.POINTER(ThreediOcclusionPlane)),
        ("occlusion_plane_count",        ctypes.c_size_t),
        ("occlusion_plane_record_size",  ctypes.c_uint32),

        # Part animations (PANM, model-level fallback)
        ("part_animations",              ctypes.POINTER(ThreediPartAnimation)),
        ("part_animation_count",         ctypes.c_size_t),
        ("part_animation_record_size",   ctypes.c_uint32),
    ]


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

_bound = False


def _bind():
    global _bound
    if _bound:
        return
    lib = load_lib()
    # threedi_3di3_read
    lib.threedi_3di3_read.restype = ctypes.c_int
    lib.threedi_3di3_read.argtypes = [ctypes.c_char_p, ctypes.POINTER(Threedi3di3)]
    # threedi_3di3_free
    lib.threedi_3di3_free.restype = None
    lib.threedi_3di3_free.argtypes = [ctypes.POINTER(Threedi3di3)]
    _bound = True


def read_model_3di3(path) -> Threedi3di3:
    """Read a .3di (3DI3) file and return the parsed model structure.

    Raises RuntimeError on parse failure.
    The caller should eventually call free_model_3di3() to release memory.
    """
    _bind()
    lib = load_lib()
    model = Threedi3di3()

    path = str(path)
    if isinstance(path, str):
        path = path.encode("utf-8")

    rc = lib.threedi_3di3_read(path, ctypes.byref(model))
    if rc != 0:
        lib.threedi_3di3_free(ctypes.byref(model))
        raise RuntimeError(f"threedi_3di3_read failed for {path!r}")
    return model


def free_model_3di3(model: Threedi3di3) -> None:
    """Free all C-side allocations inside a parsed model."""
    _bind()
    lib = load_lib()
    lib.threedi_3di3_free(ctypes.byref(model))


def read_model(path) -> Threedi3di3:
    """Read a .3di into the parsed 3DI3 model structure."""
    return read_model_3di3(path)


def ctrl_reg_name_3di3(ctrl, index: int) -> str:
    if index < 0:
        return ""
    try:
        value = ctrl.registers[index].name
    except Exception:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace").rstrip("\x00")
    return str(value).rstrip("\x00")


def decode_transform_3di3(transform, *, is_rotation: bool, ctrl):
    del is_rotation, ctrl

    class Decoded:
        control = int(getattr(transform, "control", 0))
        control_name = None
        control_param = int(getattr(transform, "control_param", 0))
        ctrl_reg_name = None
        phase = float(control_param)
        rate = float(getattr(transform, "rate", 0))
        start = float(getattr(transform, "start", 0))
        end = float(getattr(transform, "end", 0))

    return Decoded()


def user_point_position(up) -> tuple:
    """Decode a userpoint's 16.16 position into model space (mirrors
    threedi_user_point_position: x->z, y->-x, z->y)."""
    return (-up.y / 65536.0, up.z / 65536.0, up.x / 65536.0)


def user_point_direction(up) -> tuple:
    """Decode a userpoint's 16.16 direction vector into model space."""
    return (-up.rot_y / 65536.0, up.rot_z / 65536.0, up.rot_x / 65536.0)
