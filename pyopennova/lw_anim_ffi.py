"""ctypes bindings for the Land Warrior animation C API (threedi_lw_anim).

Wraps the SAF1/KSA/ACA/ANM parsers and the pose sampler from
libs/threedi/threedi_lw_anim.{h,cpp}, plus threedi_lw_read (libs/threedi/threedi_lw.h)
which supplies the per-bone skeleton rest data (parent + raw fixed-point position)
the sampler needs. The pose sampler reproduces LWAnim_PoseSkeleton @ 0x4A0C00 with
gameplay modifiers skipped.
"""

from __future__ import annotations

import ctypes

from ._native import load_lib

MAX_PARTS = 15
ROOT_I16 = 9

# ---------------------------------------------------------------------------
# Skeleton sub-object (matches ThreediLwSubObject in threedi_lw.h); the sampler
# input. Filled by the exported threedi_lw_anim_read_skeleton helper.
# ---------------------------------------------------------------------------


class LwSubObject(ctypes.Structure):
    _fields_ = [
        ("vertex_count", ctypes.c_uint32),
        ("face_count", ctypes.c_uint32),
        ("normal_count", ctypes.c_uint32),
        ("parent", ctypes.c_int32),
        ("pos", ctypes.c_int32 * 3),
    ]


# ---------------------------------------------------------------------------
# Animation structs (threedi_lw_anim.h)
# ---------------------------------------------------------------------------


class _PartRec(ctypes.Structure):
    _fields_ = [("b0", ctypes.c_uint8), ("b1", ctypes.c_uint8),
                ("b2", ctypes.c_uint8), ("pad", ctypes.c_uint8)]


class LwAnimFrame(ctypes.Structure):
    _fields_ = [
        ("parts", _PartRec * MAX_PARTS),
        ("root", ctypes.c_int16 * ROOT_I16),
        ("zero_pad", ctypes.c_uint8 * 10),
    ]


class LwSaf(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("frames", ctypes.POINTER(LwAnimFrame)),
    ]


class LwKsaSlot(ctypes.Structure):
    _fields_ = [
        ("frame_count", ctypes.c_uint32),
        ("stored_slot", ctypes.c_uint32),
        ("loop_frame", ctypes.c_uint32),
        ("frame_base", ctypes.c_uint32),
        ("frames", ctypes.POINTER(LwAnimFrame)),
    ]


class LwKsa(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("blob_size", ctypes.c_uint32),
        ("slot_count", ctypes.c_uint32),
        ("total_frames", ctypes.c_uint32),
        ("slots", ctypes.POINTER(LwKsaSlot)),
        ("frames", ctypes.POINTER(LwAnimFrame)),
    ]


class LwAcaEntry(ctypes.Structure):
    _fields_ = [
        ("slot_id", ctypes.c_uint32),
        ("saf_name", ctypes.c_char * 64),
        ("loop_frame", ctypes.c_int32),
        ("has_loop", ctypes.c_int),
    ]


class LwAca(ctypes.Structure):
    _fields_ = [("entry_count", ctypes.c_uint32), ("entries", ctypes.POINTER(LwAcaEntry))]


class LwAnmEntry(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char * 64),
        ("slot", ctypes.c_uint32),
        ("velocity", ctypes.c_float),
        ("override_val", ctypes.c_int32),
        ("has_velocity", ctypes.c_int),
        ("has_override", ctypes.c_int),
    ]


class LwAnm(ctypes.Structure):
    _fields_ = [("entry_count", ctypes.c_uint32), ("entries", ctypes.POINTER(LwAnmEntry))]


class LwBonePose(ctypes.Structure):
    _fields_ = [("local", (ctypes.c_float * 4) * 3)]  # row-major 3x4 [R|t]


class LwAnimPose(ctypes.Structure):
    _fields_ = [("bone_count", ctypes.c_uint32), ("bones", ctypes.POINTER(LwBonePose))]


_bound = False


def _bind():
    global _bound
    if _bound:
        return
    lib = load_lib()
    # skeleton rest data (exported helper that wraps threedi_lw_read internally)
    lib.threedi_lw_anim_read_skeleton.restype = ctypes.c_int
    lib.threedi_lw_anim_read_skeleton.argtypes = [
        ctypes.c_char_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.POINTER(LwSubObject)),
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint32),
    ]
    lib.threedi_lw_anim_free_skeleton.restype = None
    lib.threedi_lw_anim_free_skeleton.argtypes = [ctypes.POINTER(LwSubObject)]
    # anim readers/free
    for name, typ in (("saf", LwSaf), ("ksa", LwKsa), ("aca", LwAca), ("anm", LwAnm)):
        rd = getattr(lib, f"threedi_lw_{name}_read")
        rd.restype = ctypes.c_int
        rd.argtypes = [ctypes.c_char_p, ctypes.POINTER(typ)]
        fr = getattr(lib, f"threedi_lw_{name}_free")
        fr.restype = None
        fr.argtypes = [ctypes.POINTER(typ)]
    # sampler
    lib.threedi_lw_anim_sample.restype = ctypes.c_int
    lib.threedi_lw_anim_sample.argtypes = [
        ctypes.POINTER(LwAnimFrame),
        ctypes.POINTER(LwSubObject),
        ctypes.c_uint32,
        ctypes.c_float,
        ctypes.POINTER(LwAnimPose),
    ]
    lib.threedi_lw_anim_pose_free.restype = None
    lib.threedi_lw_anim_pose_free.argtypes = [ctypes.POINTER(LwAnimPose)]
    _bound = True


def read_ksa(path: str) -> LwKsa:
    _bind()
    lib = load_lib()
    ksa = LwKsa()
    if lib.threedi_lw_ksa_read(path.encode("utf-8"), ctypes.byref(ksa)) != 0:
        raise RuntimeError(f"threedi_lw_ksa_read failed for {path}")
    return ksa


def free_ksa(ksa: LwKsa) -> None:
    load_lib().threedi_lw_ksa_free(ctypes.byref(ksa))


def read_anm(path: str) -> LwAnm:
    _bind()
    lib = load_lib()
    anm = LwAnm()
    if lib.threedi_lw_anm_read(path.encode("utf-8"), ctypes.byref(anm)) != 0:
        raise RuntimeError(f"threedi_lw_anm_read failed for {path}")
    return anm


def free_anm(anm: LwAnm) -> None:
    load_lib().threedi_lw_anm_free(ctypes.byref(anm))


def read_aca(path: str) -> LwAca:
    _bind()
    lib = load_lib()
    aca = LwAca()
    if lib.threedi_lw_aca_read(path.encode("utf-8"), ctypes.byref(aca)) != 0:
        raise RuntimeError(f"threedi_lw_aca_read failed for {path}")
    return aca


def free_aca(aca: LwAca) -> None:
    load_lib().threedi_lw_aca_free(ctypes.byref(aca))


def read_lw_subobjects(model_path: str, lod: int = 0):
    """Return (list[LwSubObject-copies], lod_flags) for the given LOD's skeleton."""
    _bind()
    lib = load_lib()
    subs_ptr = ctypes.POINTER(LwSubObject)()
    count = ctypes.c_uint32(0)
    flags = ctypes.c_uint32(0)
    if lib.threedi_lw_anim_read_skeleton(
        model_path.encode("utf-8"), ctypes.c_uint32(lod),
        ctypes.byref(subs_ptr), ctypes.byref(count), ctypes.byref(flags)
    ) != 0:
        raise RuntimeError(f"threedi_lw_anim_read_skeleton failed for {model_path}")
    try:
        n = int(count.value)
        out = [LwSubObject(
            vertex_count=subs_ptr[i].vertex_count,
            face_count=subs_ptr[i].face_count,
            normal_count=subs_ptr[i].normal_count,
            parent=subs_ptr[i].parent,
            pos=(ctypes.c_int32 * 3)(*subs_ptr[i].pos),
        ) for i in range(n)]
        return out, int(flags.value)
    finally:
        if subs_ptr:
            lib.threedi_lw_anim_free_skeleton(subs_ptr)


def sample_frame(frame: LwAnimFrame, subobjects, pos_scale: float = 1.0):
    """Sample one runtime frame -> list of per-bone local 3x4 matrices (list of 3 rows of 4)."""
    _bind()
    lib = load_lib()
    n = len(subobjects)
    arr = (LwSubObject * n)(*subobjects)
    pose = LwAnimPose()
    if lib.threedi_lw_anim_sample(ctypes.byref(frame), arr, n,
                                  ctypes.c_float(pos_scale), ctypes.byref(pose)) != 0:
        raise RuntimeError("threedi_lw_anim_sample failed")
    try:
        out = []
        for b in range(pose.bone_count):
            m = pose.bones[b].local
            out.append([[m[r][c] for c in range(4)] for r in range(3)])
        return out
    finally:
        lib.threedi_lw_anim_pose_free(ctypes.byref(pose))
