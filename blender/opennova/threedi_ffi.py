"""
ctypes bindings for the threedi IR C API (libopennova.so / opennova.dll).

Every ctypes Structure here mirrors the corresponding C struct in
libs/threedi/include/threedi/threedi_ir.h.  Keep them in sync!
"""

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Enum constants
# ---------------------------------------------------------------------------

THREEDI_IR_SOURCE_UNKNOWN = 0
THREEDI_IR_SOURCE_3DI3    = 1
THREEDI_IR_SOURCE_GPM     = 2
THREEDI_IR_SOURCE_GPS     = 3
THREEDI_IR_SOURCE_GPP     = 4

THREEDI_IR_MESH_INVALID  = 0
THREEDI_IR_MESH_BASIC    = 1
THREEDI_IR_MESH_STATIC   = 2
THREEDI_IR_MESH_SKINNED  = 3

THREEDI_IR_TOPOLOGY_TRIANGLES = 0
THREEDI_IR_TOPOLOGY_STRIP     = 1

THREEDI_IR_TEX_SLOT_DIFFUSE = 1
THREEDI_IR_TEX_SLOT_DETAIL  = 2
THREEDI_IR_TEX_SLOT_NORMAL  = 3

THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST   = 0x01
THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT = 0x02
THREEDI_IR_MATERIAL_FLAG_TWO_SIDED    = 0x04
THREEDI_IR_MATERIAL_FLAG_EMISSIVE     = 0x08

# ---------------------------------------------------------------------------
# Struct definitions  (order must match C headers exactly)
# ---------------------------------------------------------------------------

class ThreediIRVertex(ctypes.Structure):
    _fields_ = [
        ("position",     ctypes.c_float * 3),
        ("normal",       ctypes.c_float * 3),
        ("uv0",          ctypes.c_float * 2),
        ("uv1",          ctypes.c_float * 2),
        ("tangent",      ctypes.c_float * 3),
        ("bitangent",    ctypes.c_float * 3),
        ("bone_weights", ctypes.c_float * 4),
        ("bone_indices", ctypes.c_uint8 * 4),
        ("flags",        ctypes.c_uint32),
    ]


class ThreediIRPrimitive(ctypes.Structure):
    _fields_ = [
        ("material_index",    ctypes.c_int32),
        ("part_index",        ctypes.c_int32),
        ("index_offset",      ctypes.c_uint32),
        ("index_count",       ctypes.c_uint32),
        ("vertex_offset",     ctypes.c_uint32),
        ("vertex_count",      ctypes.c_uint32),
        ("topology",          ctypes.c_int),       # ThreediIRTopology enum
        ("min",               ctypes.c_float * 3),
        ("max",               ctypes.c_float * 3),
        ("bone_table",        ctypes.c_uint8 * 16),
        ("bone_table_length", ctypes.c_uint8),
    ]


class ThreediIRPart(ctypes.Structure):
    _fields_ = [
        ("parent_index",     ctypes.c_int32),
        ("rel_position",     ctypes.c_float * 3),
        ("abs_position",     ctypes.c_float * 3),
        ("bounding_center",  ctypes.c_float * 3),
        ("bounding_radius",  ctypes.c_float),
        ("primitive_start",  ctypes.c_int32),
        ("primitive_count",  ctypes.c_int32),
        ("opaque_count",     ctypes.c_int32),
        ("alpha_count",      ctypes.c_int32),
    ]


class ThreediIRMaterialTexture(ctypes.Structure):
    _fields_ = [
        ("name",  ctypes.c_char * 17),
        ("slot",  ctypes.c_uint8),
        ("type",  ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("frame", ctypes.c_uint8),
    ]


class ThreediIRUvParams(ctypes.Structure):
    _fields_ = [
        ("style",    ctypes.c_uint8),
        ("phase",    ctypes.c_float),
        ("reg",      ctypes.c_int32),
        ("gen_rate", ctypes.c_float),
        ("start",    ctypes.c_float),
        ("end",      ctypes.c_float),
    ]


class ThreediIRAlphaGen(ctypes.Structure):
    _fields_ = [
        ("style", ctypes.c_uint8),
        ("phase", ctypes.c_float),
        ("reg",   ctypes.c_int32),
        ("rate",  ctypes.c_float),
        ("start", ctypes.c_int16),
        ("end",   ctypes.c_int16),
    ]


class ThreediIRRgbGen(ctypes.Structure):
    _fields_ = [
        ("style",       ctypes.c_uint8),
        ("phase",       ctypes.c_float),
        ("reg",         ctypes.c_int32),
        ("rate",        ctypes.c_float),
        ("start_color", ctypes.c_float * 4),
        ("end_color",   ctypes.c_float * 4),
    ]


class ThreediIRTexAnim(ctypes.Structure):
    _fields_ = [
        ("num_frames",       ctypes.c_uint8),
        ("animation_type",   ctypes.c_uint8),
        ("cycle_frame_time", ctypes.c_int16),
    ]


class ThreediIRMaterial(ctypes.Structure):
    _fields_ = [
        ("index",         ctypes.c_int32),
        ("shader_name",   ctypes.c_char * 33),
        ("texture_count", ctypes.c_uint32),
        ("textures",      ThreediIRMaterialTexture * 8),
        ("flags",         ctypes.c_uint32),
        ("alpha_threshold", ctypes.c_float),
        ("blend_mode",    ctypes.c_int),       # ThreediIRBlendMode enum

        # Shader animation parameters
        ("u_params",  ThreediIRUvParams),
        ("v_params",  ThreediIRUvParams),
        ("alpha_gen", ThreediIRAlphaGen),
        ("rgb_gen",   ThreediIRRgbGen),
        ("animation", ThreediIRTexAnim),

        # Reflection / glass
        ("reflect_color", ctypes.c_float * 4),
        ("is_glass",      ctypes.c_int),

        # Material properties
        ("specular_intensity", ctypes.c_uint32),
        ("luminosity",         ctypes.c_uint32),
        ("emissive_color",     ctypes.c_uint32),
        ("emissive_type",      ctypes.c_uint8),

        # Tiling
        ("u_tiling", ctypes.c_float),
        ("v_tiling", ctypes.c_float),

        # Collision surface type
        ("surface_type", ctypes.c_uint8),

        # Collision polygon attributes
        ("pattrib", ctypes.c_uint32),
    ]


class ThreediIRLight(ctypes.Structure):
    _fields_ = [
        ("offset",            ctypes.c_float * 3),
        ("attenuation_start", ctypes.c_float),
        ("attenuation_end",   ctypes.c_float),
        ("color_start",       ctypes.c_float * 3),
        ("color_end",         ctypes.c_float * 3),
        ("style",             ctypes.c_uint8),
        ("phase",             ctypes.c_uint8),
        ("rate",              ctypes.c_uint16),
        ("part_index",        ctypes.c_int32),
        ("flags",             ctypes.c_uint8),
        ("falloff",           ctypes.c_float),
        ("rotation",          ctypes.c_float * 3),
        ("light_type",        ctypes.c_uint8),
    ]


class ThreediIRUserPoint(ctypes.Structure):
    _fields_ = [
        ("name",       ctypes.c_char * 17),
        ("position",   ctypes.c_float * 3),
        ("direction",  ctypes.c_float * 3),
        ("part_index", ctypes.c_int32),
        ("type_code",  ctypes.c_int32),
    ]


# --- Collision ---

class ThreediIRCollisionVertex(ctypes.Structure):
    _fields_ = [("position", ctypes.c_float * 3)]


class ThreediIRCollisionPlane(ctypes.Structure):
    _fields_ = [
        ("normal",   ctypes.c_float * 3),
        ("distance", ctypes.c_float),
        ("flags",    ctypes.c_uint16),
    ]


class ThreediIRCollisionVolume(ctypes.Structure):
    _fields_ = [
        ("type",         ctypes.c_int32),
        ("flags",        ctypes.c_int32),
        ("min",          ctypes.c_float * 3),
        ("max",          ctypes.c_float * 3),
        ("plane_start",  ctypes.c_int32),
        ("plane_count",  ctypes.c_int32),
        ("part_index",   ctypes.c_int32),
        ("object_index", ctypes.c_int32),
    ]


class ThreediIRCollisionNormal(ctypes.Structure):
    _fields_ = [
        ('normal_q14',    ctypes.c_int16 * 3),
        ('dominant_axis', ctypes.c_int16),
    ]


class ThreediIRCollisionFace(ctypes.Structure):
    _fields_ = [
        ("vert_index",      ctypes.c_int16 * 3),
        ('normal_index',    ctypes.c_int16),
        ("material_flags",  ctypes.c_uint32),
        ("poly_type",       ctypes.c_uint8),
        ("normal",          ctypes.c_int16 * 3),
        ("dominate_axis",   ctypes.c_int16),
        ("plane_dist_fp16", ctypes.c_int32),
        ("min_fp16",        ctypes.c_int32 * 3),
        ("max_fp16",        ctypes.c_int32 * 3),
    ]


class _ThreediIRCollisionObjectCenter(ctypes.Union):
    _fields_ = [
        ("mid",         ctypes.c_int32 * 3),
        ("center_fp16", ctypes.c_int32 * 3),
    ]


class _ThreediIRCollisionObjectRadius(ctypes.Union):
    _fields_ = [
        ("radius",      ctypes.c_int32),
        ("radius_fp16", ctypes.c_int32),
    ]


class ThreediIRCollisionObject(ctypes.Structure):
    _anonymous_ = ("_center", "_radius")
    _fields_ = [
        ("num_vertices",            ctypes.c_int32),
        ("num_faces",               ctypes.c_int32),
        ("num_planes",              ctypes.c_int32),
        ("num_bounding_volumes",    ctypes.c_int32),
        ("parent_subobject_index",  ctypes.c_int32),
        ("offset",                  ctypes.c_int32 * 3),
        ("min",                     ctypes.c_int32 * 3),
        ("max",                     ctypes.c_int32 * 3),
        ("_center",                 _ThreediIRCollisionObjectCenter),
        ("_radius",                 _ThreediIRCollisionObjectRadius),
    ]


class ThreediIRCollisionTranslation(ctypes.Structure):
    _fields_ = [("translation", ctypes.c_int32 * 3)]


class ThreediIRCollision(ctypes.Structure):
    _fields_ = [
        ("model_min",          ctypes.c_float * 3),
        ("model_max",          ctypes.c_float * 3),
        ("model_center",       ctypes.c_float * 3),
        ("vertices",           ctypes.POINTER(ThreediIRCollisionVertex)),
        ("vertex_count",       ctypes.c_size_t),
        ("normals",            ctypes.POINTER(ThreediIRCollisionNormal)),
        ("normal_count",       ctypes.c_size_t),
        ("planes",             ctypes.POINTER(ThreediIRCollisionPlane)),
        ("plane_count",        ctypes.c_size_t),
        ("volumes",            ctypes.POINTER(ThreediIRCollisionVolume)),
        ("volume_count",       ctypes.c_size_t),
        ("faces",              ctypes.POINTER(ThreediIRCollisionFace)),
        ("face_count",         ctypes.c_size_t),
        ("objects",            ctypes.POINTER(ThreediIRCollisionObject)),
        ("object_count",       ctypes.c_size_t),
        ("translations",       ctypes.POINTER(ThreediIRCollisionTranslation)),
        ("translation_count",  ctypes.c_size_t),
    ]


# --- Occlusion ---

class ThreediIROcclusionVertex(ctypes.Structure):
    _fields_ = [("position", ctypes.c_float * 3)]


class ThreediIROcclusionFace(ctypes.Structure):
    _fields_ = [
        ("raw_indices",    ctypes.c_uint32),
        ("edge_data",      ctypes.c_uint32),
        ("other_edge_data", ctypes.c_uint32),
    ]


class ThreediIROcclusionPlane(ctypes.Structure):
    _fields_ = [
        ("normal", ctypes.c_float * 3),
        ("radius", ctypes.c_float),
    ]


class ThreediIROcclusionObject(ctypes.Structure):
    _fields_ = [
        ("type",                    ctypes.c_int32),
        ("parent_subobject_index",  ctypes.c_int32),
        ("connecting_subobject",    ctypes.c_int32),
        ("position",                ctypes.c_float * 3),
        ("radius",                  ctypes.c_float),
        ("glow_scale",              ctypes.c_float),
        ("num_vertices",            ctypes.c_int32),
        ("num_planes",              ctypes.c_int32),
        ("face_count",              ctypes.c_int32),
        ("vertex_start",            ctypes.c_int32),
        ("plane_start",             ctypes.c_int32),
        ("face_start",              ctypes.c_int32),
    ]


class ThreediIROcclusion(ctypes.Structure):
    _fields_ = [
        ("vertices",     ctypes.POINTER(ThreediIROcclusionVertex)),
        ("vertex_count", ctypes.c_size_t),
        ("faces",        ctypes.POINTER(ThreediIROcclusionFace)),
        ("face_count",   ctypes.c_size_t),
        ("planes",       ctypes.POINTER(ThreediIROcclusionPlane)),
        ("plane_count",  ctypes.c_size_t),
        ("objects",      ctypes.POINTER(ThreediIROcclusionObject)),
        ("object_count", ctypes.c_size_t),
    ]


# --- Animation ---

class ThreediIRTransform(ctypes.Structure):
    _fields_ = [
        ("control",       ctypes.c_uint8),
        ("control_param", ctypes.c_uint8),
        ("rate",          ctypes.c_int16),
        ("start",         ctypes.c_int16),
        ("end",           ctypes.c_int16),
    ]


class ThreediIRPartAnimation(ctypes.Structure):
    _fields_ = [
        ("flags",            ctypes.c_uint32),
        ("parent_part",      ctypes.c_uint8),
        ("part_index",       ctypes.c_uint8),
        ("matrix_index",     ctypes.c_uint8),
        ("matrix_offset",    ctypes.c_uint8),
        ("bind_matrix_index", ctypes.c_int32),
        ("rotation_x",      ThreediIRTransform),
        ("rotation_y",      ThreediIRTransform),
        ("rotation_z",      ThreediIRTransform),
        ("scale_x",         ThreediIRTransform),
        ("scale_y",         ThreediIRTransform),
        ("scale_z",         ThreediIRTransform),
        ("translation",     ThreediIRTransform),
    ]


# --- LOD ---

class ThreediIRLod(ctypes.Structure):
    _fields_ = [
        ("threshold",            ctypes.c_int32),
        ("declared_part_count",  ctypes.c_int32),
        ("vertices",             ctypes.POINTER(ThreediIRVertex)),
        ("vertex_count",    ctypes.c_size_t),
        ("indices",         ctypes.POINTER(ctypes.c_uint16)),
        ("index_count",     ctypes.c_size_t),
        ("primitives",      ctypes.POINTER(ThreediIRPrimitive)),
        ("primitive_count", ctypes.c_size_t),
        ("parts",           ctypes.POINTER(ThreediIRPart)),
        ("part_count",      ctypes.c_size_t),
        ("part_animations",      ctypes.POINTER(ThreediIRPartAnimation)),
        ("part_animation_count", ctypes.c_size_t),
    ]


class ThreediIRControlRegister(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 25)]


class ThreediIRMatrix(ctypes.Structure):
    _fields_ = [("m", ctypes.c_float * 16)]


# --- Top-level model IR ---

class ThreediModelIR(ctypes.Structure):
    _fields_ = [
        ("name",            ctypes.c_char * 32),
        ("render_function", ctypes.c_char * 5),
        ("source_format",   ctypes.c_int),       # ThreediIRSourceFormat enum
        ("mesh_type",     ctypes.c_int),       # ThreediIRMeshType enum

        ("lods",      ctypes.POINTER(ThreediIRLod)),
        ("lod_count", ctypes.c_size_t),

        ("materials",      ctypes.POINTER(ThreediIRMaterial)),
        ("material_count", ctypes.c_size_t),

        ("lights",      ctypes.POINTER(ThreediIRLight)),
        ("light_count", ctypes.c_size_t),

        ("userpoints",      ctypes.POINTER(ThreediIRUserPoint)),
        ("userpoint_count", ctypes.c_size_t),

        ("collision", ctypes.POINTER(ThreediIRCollision)),
        ("collision_lod", ctypes.c_int32),
        ("occlusion", ctypes.POINTER(ThreediIROcclusion)),

        ("control_registers",      ctypes.POINTER(ThreediIRControlRegister)),
        ("control_register_count", ctypes.c_size_t),

        ("matrices",      ctypes.POINTER(ThreediIRMatrix)),
        ("matrix_count",  ctypes.c_size_t),
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
    # threedi_ir_init
    lib.threedi_ir_init.restype = None
    lib.threedi_ir_init.argtypes = [ctypes.POINTER(ThreediModelIR)]
    # threedi_ir_free
    lib.threedi_ir_free.restype = None
    lib.threedi_ir_free.argtypes = [ctypes.POINTER(ThreediModelIR)]
    # threedi_ir_read
    lib.threedi_ir_read.restype = ctypes.c_int
    lib.threedi_ir_read.argtypes = [ctypes.c_char_p, ctypes.POINTER(ThreediModelIR)]
    _bound = True


def read_model_ir(path: str) -> ThreediModelIR:
    """Read a 3DI file (any format) and return the parsed IR structure.

    Raises RuntimeError on parse failure.
    The caller should eventually call free_model_ir() to release memory.
    """
    _bind()
    lib = load_lib()
    ir = ThreediModelIR()
    lib.threedi_ir_init(ctypes.byref(ir))

    if isinstance(path, str):
        path = path.encode("utf-8")

    rc = lib.threedi_ir_read(path, ctypes.byref(ir))
    if rc != 0:
        lib.threedi_ir_free(ctypes.byref(ir))
        raise RuntimeError(f"threedi_ir_read failed for {path!r}")
    return ir


def free_model_ir(ir: ThreediModelIR):
    """Free all C-side allocations inside an IR structure."""
    _bind()
    lib = load_lib()
    lib.threedi_ir_free(ctypes.byref(ir))
