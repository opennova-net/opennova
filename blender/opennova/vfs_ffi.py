"""ctypes bindings for the engine-faithful VFS C ABI (libs/vfs, in opennova.dll / libopennova.so).

The native handle is opaque, so nothing here mirrors a C++/struct layout: there is no ABI-drift
risk of the kind the field-by-field PffArchiveC mirror carries. Wraps the mount API and file
resolution from libs/vfs/include/vfs/vfs_capi.h. Resolution precedence matches the original
engine (ordered loose search paths, then primary archive, then ordered secondary archives;
loose shadows archived). read_file() returns SCR/BFC1-decoded bytes.
"""

from __future__ import annotations

import ctypes

from ._native import load_lib

# Mount modes, matching OPENNOVA_VFS_MODE_* in vfs_capi.h / opennova::VfsMountMode.
MOUNT_LOOSE_ONLY = 0
MOUNT_PACKED = 1
MOUNT_PACKED_WITH_LOOSE = 2

# SCR decode policies, matching ScrPolicy in gameprofile.h / VfsScrPolicy in vfs_decode.h.
SCR_POLICY_VERSION_DETECT = 0
SCR_POLICY_FORCE_DEFAULT = 1
SCR_POLICY_FORCE_JO_DFX2 = 2
SCR_POLICY_FORCE_SHADERS = 3

_bound = False


def _bind():
    global _bound
    if _bound:
        return
    lib = load_lib()

    lib.opennova_vfs_create.restype = ctypes.c_void_p
    lib.opennova_vfs_create.argtypes = []
    lib.opennova_vfs_destroy.restype = None
    lib.opennova_vfs_destroy.argtypes = [ctypes.c_void_p]

    for name in (
        "opennova_vfs_add_search_path",
        "opennova_vfs_set_primary_archive",
        "opennova_vfs_add_secondary_archive",
    ):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int
        fn.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    lib.opennova_vfs_mount_game.restype = ctypes.c_int
    lib.opennova_vfs_mount_game.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]

    lib.opennova_vfs_clear.restype = None
    lib.opennova_vfs_clear.argtypes = [ctypes.c_void_p]

    lib.opennova_vfs_set_scr_policy.restype = None
    lib.opennova_vfs_set_scr_policy.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.opennova_vfs_has_file.restype = ctypes.c_int
    lib.opennova_vfs_has_file.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    for name in ("opennova_vfs_read_file", "opennova_vfs_read_file_raw"):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int
        fn.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.POINTER(ctypes.c_size_t),
        ]

    lib.opennova_vfs_free.restype = None
    lib.opennova_vfs_free.argtypes = [ctypes.POINTER(ctypes.c_uint8)]

    lib.opennova_vfs_file_count.restype = ctypes.c_int
    lib.opennova_vfs_file_count.argtypes = [ctypes.c_void_p]
    for name in (
        "opennova_vfs_file_name_at",
        "opennova_vfs_file_source_at",
        "opennova_vfs_file_archive_at",
    ):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_char_p
        fn.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.opennova_vfs_last_error.restype = ctypes.c_char_p
    lib.opennova_vfs_last_error.argtypes = [ctypes.c_void_p]

    _bound = True


def _enc(value: str | bytes | None) -> bytes | None:
    if value is None:
        return None
    return value.encode("utf-8") if isinstance(value, str) else value


class Vfs:
    """Engine-faithful virtual file system over PFF archives + loose files."""

    def __init__(self):
        _bind()
        self._lib = load_lib()
        self._handle = self._lib.opennova_vfs_create()
        if not self._handle:
            raise RuntimeError("opennova_vfs_create failed")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def close(self):
        """Destroy the handle, releasing any open archive file handles. Idempotent."""
        if getattr(self, "_handle", None):
            self._lib.opennova_vfs_destroy(self._handle)
            self._handle = None

    # --- Mount API ---
    def mount_game(
        self, game_root: str, expansion: str | None = None, mode: int = MOUNT_PACKED_WITH_LOOSE
    ) -> bool:
        return (
            self._lib.opennova_vfs_mount_game(self._handle, _enc(game_root), _enc(expansion), mode)
            == 1
        )

    def add_search_path(self, directory: str) -> bool:
        return self._lib.opennova_vfs_add_search_path(self._handle, _enc(directory)) == 1

    def set_primary_archive(self, pff_path: str) -> bool:
        return self._lib.opennova_vfs_set_primary_archive(self._handle, _enc(pff_path)) == 1

    def add_secondary_archive(self, pff_path: str) -> bool:
        return self._lib.opennova_vfs_add_secondary_archive(self._handle, _enc(pff_path)) == 1

    def clear(self):
        self._lib.opennova_vfs_clear(self._handle)

    def set_scr_policy(self, policy: int) -> None:
        """Choose how read_file keys SCR payloads (an SCR_POLICY_* value). Persists across
        mounts. Resolve a game's policy via gameprofile_ffi.scr_policy_for_code()."""
        self._lib.opennova_vfs_set_scr_policy(self._handle, int(policy))

    # --- Resolution ---
    def has_file(self, name: str) -> bool:
        return self._lib.opennova_vfs_has_file(self._handle, _enc(name)) == 1

    def read_file(self, name: str, *, decode: bool = True) -> bytes | None:
        """Return the file's bytes, or None if not found. By default the payload is
        SCR/BFC1-decoded; pass decode=False for the stored bytes."""
        out = ctypes.POINTER(ctypes.c_uint8)()
        size = ctypes.c_size_t(0)
        fn = self._lib.opennova_vfs_read_file if decode else self._lib.opennova_vfs_read_file_raw
        if fn(self._handle, _enc(name), ctypes.byref(out), ctypes.byref(size)) != 1:
            return None
        try:
            return ctypes.string_at(out, size.value)
        finally:
            self._lib.opennova_vfs_free(out)

    def last_error(self) -> str:
        msg = self._lib.opennova_vfs_last_error(self._handle)
        return msg.decode("utf-8", "replace") if msg else ""

    # --- Enumeration ---
    def file_count(self) -> int:
        return self._lib.opennova_vfs_file_count(self._handle)

    def file_at(self, index: int) -> tuple[str, str, str] | None:
        """Return (logical_name, source, archive_path) at index, or None.
        source is "loose" or "pff"; archive_path is "" for loose files."""
        name = self._lib.opennova_vfs_file_name_at(self._handle, index)
        if name is None:
            return None
        source = self._lib.opennova_vfs_file_source_at(self._handle, index)
        archive = self._lib.opennova_vfs_file_archive_at(self._handle, index)
        return (
            name.decode("utf-8", "replace"),
            source.decode("utf-8", "replace") if source else "",
            archive.decode("utf-8", "replace") if archive else "",
        )
