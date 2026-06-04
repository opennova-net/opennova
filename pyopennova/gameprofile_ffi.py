"""ctypes binding for the per-game profile table (libs/gameprofile, in opennova.dll / libopennova.so).

The game->SCR-policy mapping is the single source of truth in C (libs/gameprofile); Python resolves
through it rather than duplicating the table, so the importer keys SCR payloads exactly like the
engine. Only the code->policy lookup the importer needs is wrapped here.
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
    """The SCR decode policy (an SCR_POLICY_* value) for a game code (e.g. "jo", "jodemo").
    A None/unknown code resolves to version-detect (the JO default), matching the C helper."""
    _bind()
    enc = code.encode("utf-8") if isinstance(code, str) else None
    return int(load_lib().gameprofile_scr_policy_for_code(enc))
