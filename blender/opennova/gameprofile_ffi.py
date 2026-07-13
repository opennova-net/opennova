"""ctypes binding for the asset decode profile table (libs/gameprofile).

The source-profile -> SCR-policy mapping is the single source of truth in C; Blender resolves
through it rather than duplicating the table, so imported assets use the correct title-specific
codec. This tooling table does not select or advertise game-runtime support.
"""

from __future__ import annotations

import ctypes

from ._native import load_lib

# ScrPolicy values, matching gameprofile.h (mirrored in vfs_ffi.SCR_POLICY_*).
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
    lib.gameprofile_scr_policy_for_code.restype = ctypes.c_int
    lib.gameprofile_scr_policy_for_code.argtypes = [ctypes.c_char_p]
    _bound = True


def scr_policy_for_code(code: str | None) -> int:
    """The SCR decode policy for an asset profile code (for example "jo" or "jodemo").
    A None/unknown code resolves to version-detect (the JO default), matching the C helper."""
    _bind()
    enc = code.encode("utf-8") if isinstance(code, str) else None
    return int(load_lib().gameprofile_scr_policy_for_code(enc))
