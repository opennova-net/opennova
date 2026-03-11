"""
ctypes bindings for the ASE C API (libopennova.so / opennova.dll).

Every ctypes Structure here mirrors the corresponding C struct in
libs/ase/include/ase/types.h.  Keep them in sync!
"""

import ctypes

from ._native import load_lib


# ---------------------------------------------------------------------------
# Struct definitions (order must match C headers exactly)
# ---------------------------------------------------------------------------

class AseUV(ctypes.Structure):
    _fields_ = [
        ("u", ctypes.c_float),
        ("v", ctypes.c_float),
        ("w", ctypes.c_float),
    ]


class AseWeight(ctypes.Structure):
    _fields_ = [
        ("bone_index", ctypes.c_int32 * 4),
        ("weight", ctypes.c_float * 4),
    ]


class AseFace(ctypes.Structure):
    _fields_ = [
        ("material_id", ctypes.c_int32),
        ("material_index", ctypes.c_int32),
        ("smoothing_mask", ctypes.c_uint32),
        ("vert", ctypes.c_int32 * 4),
        ("edge_visibility", ctypes.c_uint8 * 4),
        ("uv", ctypes.c_int32 * 4),
        ("color", ctypes.c_int32 * 3),
        ("reserved1", ctypes.c_int32),
        ("reserved2", ctypes.c_int32),
    ]


class AseMappingChannel(ctypes.Structure):
    _fields_ = [
        ("channel_id", ctypes.c_int32),
        ("tv_count", ctypes.c_int32),
        ("tverts", ctypes.POINTER(AseUV)),
        ("face_count", ctypes.c_int32),
        ("faces", ctypes.c_void_p),  # int32_t (*)[3]
    ]


class AseObject(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 64),
        ("node_id", ctypes.c_int32),
        ("material_ref", ctypes.c_int32),
        ("mapping_channel_count", ctypes.c_int32),
        ("mapping_channels", ctypes.POINTER(AseMappingChannel)),
        ("parent_name", ctypes.c_char * 64),
        ("vert_count", ctypes.c_int32),
        ("verts", ctypes.POINTER(ctypes.c_float)),
        ("weight_count", ctypes.c_int32),
        ("weights", ctypes.POINTER(AseWeight)),
        ("uv_count", ctypes.c_int32),
        ("uvs", ctypes.POINTER(AseUV)),
        ("face_count", ctypes.c_int32),
        ("faces", ctypes.POINTER(AseFace)),
        ("color_count", ctypes.c_int32),
        ("colors", ctypes.POINTER(ctypes.c_uint32)),
        ("color_face_count", ctypes.c_int32),
        ("tm_row", (ctypes.c_float * 3) * 4),
        ("skinned", ctypes.c_int32),
        ("face_normal_count", ctypes.c_int32),
        ("face_normals", ctypes.POINTER(ctypes.c_float)),
    ]


class AseLight(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 64),
        ("type", ctypes.c_int32),
        ("pos", ctypes.c_float * 3),
        ("color", ctypes.c_float * 3),
        ("intensity", ctypes.c_float),
        ("atten_start", ctypes.c_float),
        ("atten_end", ctypes.c_float),
        ("near_atten_start", ctypes.c_float),
        ("near_atten_end", ctypes.c_float),
        ("hotspot", ctypes.c_float),
        ("falloff", ctypes.c_float),
        ("tm_row2", ctypes.c_float * 3),
    ]


class AseMaterial(ctypes.Structure):
    pass

# Forward-declare _fields_ so the self-referential pointer works.
AseMaterial._fields_ = [
    ("flags", ctypes.c_uint32),
    ("name", ctypes.c_char * 32),
    ("maps", (ctypes.c_char * 32) * 4),
    ("uv_u_offset", ctypes.c_float * 2),
    ("uv_v_offset", ctypes.c_float * 2),
    ("uv_u_tiling", ctypes.c_float * 2),
    ("uv_v_tiling", ctypes.c_float * 2),
    ("extra_flags", ctypes.c_uint32),
    ("has_submaterials", ctypes.c_uint32),
    ("ambient", ctypes.c_float * 3),
    ("diffuse", ctypes.c_float * 3),
    ("specular", ctypes.c_float * 3),
    ("shine", ctypes.c_float),
    ("shine_strength", ctypes.c_float),
    ("transparency", ctypes.c_float),
    ("wiresize", ctypes.c_float),
    ("shading", ctypes.c_int32),
    ("submaterial_count", ctypes.c_int32),
    ("submaterials", ctypes.POINTER(AseMaterial)),
]


class AseDocument(ctypes.Structure):
    _fields_ = [
        ("_reserved", ctypes.c_uint8 * 64),
        ("object_count", ctypes.c_int32),
        ("light_count", ctypes.c_int32),
        ("material_count", ctypes.c_int32),
        ("flags", ctypes.c_uint32),
        ("objects", ctypes.POINTER(AseObject)),
        ("lights", ctypes.POINTER(AseLight)),
        ("materials", ctypes.POINTER(AseMaterial)),
        ("skinned_flags", ctypes.c_int32),
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

    lib.ase_parse.restype = ctypes.c_int
    lib.ase_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(AseDocument)]

    lib.ase_write.restype = ctypes.c_int
    lib.ase_write.argtypes = [ctypes.c_char_p, ctypes.POINTER(AseDocument)]

    lib.ase_free.restype = None
    lib.ase_free.argtypes = [ctypes.POINTER(AseDocument)]

    lib.ase_alloc.restype = None
    lib.ase_alloc.argtypes = [
        ctypes.POINTER(AseDocument), ctypes.c_int, ctypes.c_int, ctypes.c_int
    ]

    lib.ase_alloc_object.restype = None
    lib.ase_alloc_object.argtypes = [
        ctypes.POINTER(AseObject),
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int
    ]

    lib.ase_alloc_submaterials.restype = None
    lib.ase_alloc_submaterials.argtypes = [ctypes.POINTER(AseMaterial), ctypes.c_int]

    _bound = True


def create_document(objects: int, materials: int, lights: int) -> AseDocument:
    """Allocate a new AseDocument with the given counts."""
    _bind()
    lib = load_lib()
    doc = AseDocument()
    lib.ase_alloc(ctypes.byref(doc), objects, materials, lights)
    return doc


def alloc_object(obj, verts: int, uvs: int, faces: int,
                 colors: int = 0, weights: int = 0):
    """Allocate arrays within an AseObject."""
    _bind()
    lib = load_lib()
    lib.ase_alloc_object(ctypes.byref(obj), verts, uvs, faces, colors, weights)


def alloc_submaterials(mat, count: int):
    """Allocate sub-materials within an AseMaterial (for Multi/Sub-Object)."""
    _bind()
    lib = load_lib()
    lib.ase_alloc_submaterials(ctypes.byref(mat), count)


def write_file(filepath: str, doc: AseDocument):
    """Write Document to ASE file. Raises RuntimeError on failure."""
    _bind()
    lib = load_lib()
    if isinstance(filepath, str):
        filepath = filepath.encode("utf-8")
    rc = lib.ase_write(filepath, ctypes.byref(doc))
    if rc != 0:
        raise RuntimeError(f"ase_write failed for {filepath!r}")


def free_document(doc: AseDocument):
    """Free all C-side allocations inside a Document."""
    _bind()
    lib = load_lib()
    lib.ase_free(ctypes.byref(doc))


def copy_str(dst, value: str, max_len: int = None):
    """Copy a Python string into a ctypes c_char array."""
    if isinstance(value, str):
        value = value.encode("utf-8")
    if max_len is None:
        max_len = len(dst)
    dst.value = value[:max_len - 1]
