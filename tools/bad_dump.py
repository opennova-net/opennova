#!/usr/bin/env python3
"""Standalone BAD animation file dump tool.

Loads the opennova native library and prints a human-readable dump of BAD
file contents: header, bone table, channels, events, and translations.

Usage: python tools/bad_dump.py <file.bad> [file2.bad ...]
"""

import ctypes
import platform
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Struct definitions (duplicated from blender/opennova/bad_ffi.py to stay
# self-contained — no Blender dependency)
# ---------------------------------------------------------------------------

class BadBone(ctypes.Structure):
    _fields_ = [
        ("name",          ctypes.c_char * 17),
        ("num_children",  ctypes.c_int32),
        ("child_offset",  ctypes.c_int32),
        ("parent_offset", ctypes.c_int32),
        ("parent_index",  ctypes.c_int32),
        ("length",        ctypes.c_float),
        ("position",      ctypes.c_float * 3),
        ("rotation",      ctypes.c_float * 9),
    ]


class BadQuaternion(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("z", ctypes.c_float),
        ("w", ctypes.c_float),
    ]


class BadChannel(ctypes.Structure):
    _fields_ = [
        ("frame_count",          ctypes.c_uint32),
        ("frame_lengths_offset", ctypes.c_uint32),
        ("rotations_offset",     ctypes.c_uint32),
        ("frame_lengths",        ctypes.POINTER(ctypes.c_uint16)),
        ("rotations",            ctypes.POINTER(BadQuaternion)),
    ]


class BadEvent(ctypes.Structure):
    _fields_ = [
        ("velocity", ctypes.c_float * 3),
        ("bottom",   ctypes.c_float),
        ("top",      ctypes.c_float),
        ("trigger",  ctypes.c_int32),
    ]


class BadFile(ctypes.Structure):
    _fields_ = [
        ("version",     ctypes.c_uint32),
        ("header_size", ctypes.c_uint32),
        ("fps",         ctypes.c_uint32),
        ("frame_count", ctypes.c_uint32),
        ("flags",       ctypes.c_uint32),
        ("bone_count",  ctypes.c_uint32),

        ("bones",     ctypes.POINTER(BadBone)),
        ("num_bones", ctypes.c_size_t),

        ("channels",     ctypes.POINTER(BadChannel)),
        ("num_channels", ctypes.c_size_t),

        ("events",     ctypes.POINTER(BadEvent)),
        ("num_events", ctypes.c_size_t),

        ("translations",     ctypes.POINTER(ctypes.c_float * 3)),
        ("num_translations", ctypes.c_size_t),
    ]


# ---------------------------------------------------------------------------
# Library loading
# ---------------------------------------------------------------------------

def _find_lib() -> str:
    """Locate the opennova shared library relative to the repo root."""
    repo_root = Path(__file__).resolve().parent.parent
    system = platform.system()

    if system == "Windows":
        sub, name = "windows-x64", "opennova.dll"
    elif system == "Linux":
        sub, name = "linux-x64", "libopennova.so"
    else:
        raise OSError(f"Unsupported platform: {system}")

    path = repo_root / "blender" / "lib" / sub / name
    if not path.is_file():
        raise FileNotFoundError(
            f"Native library not found at {path}. "
            f"Run scripts/build_blender_libs.sh to build it."
        )
    return str(path)


def _load_lib():
    lib = ctypes.CDLL(_find_lib())
    lib.bad_parse.restype = ctypes.c_int
    lib.bad_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(BadFile)]
    lib.bad_free.restype = None
    lib.bad_free.argtypes = [ctypes.POINTER(BadFile)]
    return lib


# ---------------------------------------------------------------------------
# Flag names
# ---------------------------------------------------------------------------

_FLAG_BITS = {
    0x01: "LOOPING",
    0x02: "TRANSLATED",
    0x04: "COMPRESSED",
}


def _format_flags(flags: int) -> str:
    names = []
    for bit, name in _FLAG_BITS.items():
        if flags & bit:
            names.append(name)
    if names:
        return f"0x{flags:04x} ({' | '.join(names)})"
    return f"0x{flags:04x}"


# ---------------------------------------------------------------------------
# Dump routines
# ---------------------------------------------------------------------------

def dump_bad(lib, path: str):
    bf = BadFile()
    path_bytes = path.encode("utf-8") if isinstance(path, str) else path
    rc = lib.bad_parse(path_bytes, ctypes.byref(bf))
    if rc != 0:
        print(f"ERROR: bad_parse failed for {path}")
        return

    try:
        print(f"=== {path} ===")
        print()

        # Header
        print("--- Header ---")
        print(f"  version:     {bf.version}")
        print(f"  fps:         {bf.fps}")
        print(f"  frame_count: {bf.frame_count}")
        print(f"  flags:       {_format_flags(bf.flags)}")
        print(f"  bone_count:  {bf.num_bones}")
        print()

        # Bone table
        print(f"--- Bone Table ({bf.num_bones} bones) ---")
        for i in range(bf.num_bones):
            bone = bf.bones[i]
            name = bone.name.decode("utf-8", errors="replace").rstrip("\x00")
            pos = [bone.position[j] for j in range(3)]
            rot = [bone.rotation[j] for j in range(9)]
            print(f"  [{i:2d}] {name!r:20s}  parent={bone.parent_index:3d}  "
                  f"length={bone.length:.4f}")
            print(f"       pos: ({pos[0]:10.4f}, {pos[1]:10.4f}, {pos[2]:10.4f})")
            print(f"       rot: [{rot[0]:8.4f} {rot[1]:8.4f} {rot[2]:8.4f}]")
            print(f"            [{rot[3]:8.4f} {rot[4]:8.4f} {rot[5]:8.4f}]")
            print(f"            [{rot[6]:8.4f} {rot[7]:8.4f} {rot[8]:8.4f}]")
        print()

        # Channels
        print(f"--- Channels ({bf.num_channels} channels) ---")
        for i in range(bf.num_channels):
            ch = bf.channels[i]
            first = last = "(none)"
            if ch.frame_count > 0:
                q = ch.rotations[0]
                first = f"({q.w:.4f}, {q.x:.4f}, {q.y:.4f}, {q.z:.4f})"
                q = ch.rotations[ch.frame_count - 1]
                last = f"({q.w:.4f}, {q.x:.4f}, {q.y:.4f}, {q.z:.4f})"
            print(f"  [{i:2d}] frames={ch.frame_count:4d}  "
                  f"first={first}  last={last}")
        print()

        # Events
        print(f"--- Events ({bf.num_events} events) ---")
        for i in range(bf.num_events):
            evt = bf.events[i]
            vel = [evt.velocity[j] for j in range(3)]
            print(f"  [{i:3d}] velocity=({vel[0]:8.4f}, {vel[1]:8.4f}, {vel[2]:8.4f})  "
                  f"bottom={evt.bottom:.4f}  top={evt.top:.4f}  "
                  f"trigger={evt.trigger}")
        print()

        # Translations
        print(f"--- Translations ({bf.num_translations} entries) ---")
        if bf.num_translations > 0:
            # Show first and last entry as a summary
            t = bf.translations[0]
            print(f"  [  0] ({t[0]:10.4f}, {t[1]:10.4f}, {t[2]:10.4f})")
            if bf.num_translations > 1:
                t = bf.translations[bf.num_translations - 1]
                print(f"  [{bf.num_translations - 1:3d}] ({t[0]:10.4f}, {t[1]:10.4f}, {t[2]:10.4f})")
        print()

    finally:
        lib.bad_free(ctypes.byref(bf))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <file.bad> [file2.bad ...]", file=sys.stderr)
        sys.exit(1)

    lib = _load_lib()
    for path in sys.argv[1:]:
        dump_bad(lib, path)


if __name__ == "__main__":
    main()
