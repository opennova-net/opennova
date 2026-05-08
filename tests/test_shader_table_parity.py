"""Drift test: C and Python shader-tag tables must agree.

The canonical shader-tag feature table lives in two places:
- C: libs/threedi/src/threedi_shader_tags.c (consumed by C++ scene builders)
- Python: pyopennova/materials.py::_MATERIAL_INFO_FLAGS (consumed by descriptor)

This test parses the C source and asserts every (name, flags) tuple matches
the Python dict. Python carries one synthetic entry (VS_PHONGT_MDT) that is
not in the canonical table; it is allowlisted here.
"""
from __future__ import annotations

import re
from pathlib import Path

from pyopennova.materials import _MATERIAL_INFO_FLAGS

# The single synthetic entry Python keeps that is intentionally not in the
# canonical 41-entry table. It is a heuristic alias used only by the texture
# role classifier when synthesising shader names from GP source data.
_PYTHON_ONLY_TAGS = {"VS_PHONGT_MDT"}

_FLAG_NAMES = {
    "EMISSIVE": 0x00000001,
    "ALPHA": 0x00000002,
    "DIFFUSE": 0x00000004,
    "SECONDARY": 0x00000008,
    "NORMAL_A": 0x00000010,
    "NORMAL_B": 0x00000020,
    "SPECIAL": 0x00001000,
    "GLASS": 0x00002000,
    "FILTER": 0x00004000,
    "SMOOTH": 0x00008000,
    "UI_TOGGLE": 0x00010000,
    "LUMINANCE": 0x10000000,
}

_REPO_ROOT = Path(__file__).resolve().parents[1]
_C_TABLE_SOURCE = _REPO_ROOT / "libs" / "threedi" / "src" / "threedi_shader_tags.c"


def _parse_c_table() -> dict[str, int]:
    """Extract (name, flags) from the C array literal in threedi_shader_tags.c.

    Lines look like:  E("FF_ST_OP", F(DIFFUSE)),
    Multi-flag entries:  E("FF_ST_AB", F(ALPHA) | F(DIFFUSE) | F(SPECIAL)),
    """
    src = _C_TABLE_SOURCE.read_text(encoding="utf-8")
    pattern = re.compile(r'E\("([^"]+)"\s*,\s*([^)]+(?:\)[^,]*)*)\)\s*,', re.MULTILINE)
    out: dict[str, int] = {}
    for match in pattern.finditer(src):
        name = match.group(1)
        flags_expr = match.group(2)
        # Strip whitespace and split on |
        tokens = [t.strip() for t in flags_expr.split("|")]
        flags = 0
        for tok in tokens:
            m = re.match(r"F\((\w+)\)", tok)
            if not m:
                raise AssertionError(
                    f"unparseable flag token in C table for {name!r}: {tok!r}"
                )
            flag_name = m.group(1)
            if flag_name not in _FLAG_NAMES:
                raise AssertionError(
                    f"unknown flag name {flag_name!r} for entry {name!r}"
                )
            flags |= _FLAG_NAMES[flag_name]
        out[name] = flags
    return out


def test_shader_table_parity():
    c_table = _parse_c_table()
    py_table = dict(_MATERIAL_INFO_FLAGS)

    # Python carries synthetic entries we do not require in C.
    for tag in _PYTHON_ONLY_TAGS:
        py_table.pop(tag, None)

    extra_in_c = sorted(set(c_table) - set(py_table))
    extra_in_py = sorted(set(py_table) - set(c_table))
    assert not extra_in_c, f"tags only in C: {extra_in_c}"
    assert not extra_in_py, f"tags only in Python: {extra_in_py}"

    for tag, c_flags in sorted(c_table.items()):
        py_flags = py_table[tag]
        assert c_flags == py_flags, (
            f"flags mismatch for {tag!r}: C=0x{c_flags:X} Python=0x{py_flags:X}"
        )


def test_c_table_has_expected_entry_count():
    c_table = _parse_c_table()
    # Entries dumped from gMaterialInfoTable in the original ModSuperOed binary.
    assert len(c_table) == 45, f"expected 45 entries, got {len(c_table)}"
