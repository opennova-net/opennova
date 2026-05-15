"""Focused unit tests for the channel-1 (uv1) tiling back-calculation pipeline.

Mirrors OED's per-material UV-tiling transform applied in
``WriteRDTA @ 0x459a04`` (ModSuperOed.exe.i64), where the per-vertex uv1
in the output RDTA chunk is computed as
``(uv0 - 0.5) * materialSlot->uv1_u_tiling + 0.5`` (and same for v).

The 3DI3 MTRL chunk doesn't carry uv1 tiling natively, so we recover it
from per-vertex (uv0, uv1) ratios. Without this back-calc and the
plumbing through MaterialDescriptor -> ASE writer, the regenerated
.ase emits ``*UVW_U_TILING 1.0`` on the second sub-map, OED's bake skips
the transform branch, and the second-channel detail/lightmap UVs come
out as a verbatim copy of uv0.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from pyopennova import threedi_ffi
from pyopennova.materials import (
    MaterialDescriptor,
    derive_uv1_tilings,
    describe_material,
)


AKCRATE_3DI = Path("fixtures/stock_3di/akcrate/akcrate.3di")


def test_derive_uv1_tilings_recovers_akcrate_per_material_tilings() -> None:
    """akcrate's three FF_MT_OP materials encode (u,v) tilings of (2,6),
    (5,10), and (6,50) on the second UV channel. Recovered from
    per-vertex (uv0, uv1) ratios across all 5 LODs.
    """
    ir = threedi_ffi.read_model_3di3(str(AKCRATE_3DI))
    try:
        tilings = derive_uv1_tilings(ir)
    finally:
        threedi_ffi.free_model_3di3(ir)

    assert set(tilings.keys()) == {0, 1, 2}
    assert tilings[0] == pytest.approx((2.0, 6.0), rel=1e-3)
    assert tilings[1] == pytest.approx((5.0, 10.0), rel=1e-3)
    assert tilings[2] == pytest.approx((6.0, 50.0), rel=1e-3)


def test_derive_uv1_tilings_defaults_to_identity_when_no_samples() -> None:
    """When every (uv0, uv1) sample sits on the pivot (uv0 == 0.5), no
    ratios are usable â€” the helper falls back to identity (1.0). This
    matches OED's `if (uv1_u_tiling != 0.0)` guard at WriteRDTA's uv1
    transform: identity tiling is harmless because the transform is
    skipped entirely on the bake side too.
    """

    class _PivotVert:
        uv0 = (0.5, 0.5)
        uv1 = (0.5, 0.5)

    class _Prim:
        material_index = 0
        vertex_offset = 0
        vertex_count = 1

    class _Lod:
        primitive_count = 1
        primitives = [_Prim()]
        vertices = [_PivotVert()]

    class _IR:
        material_count = 1
        lod_count = 1
        lods = [_Lod()]

    tilings = derive_uv1_tilings(_IR())
    assert tilings == {0: (1.0, 1.0)}


def test_describe_material_propagates_uv1_tiling_override() -> None:
    """describe_material's ``uv1_tiling_override`` is the bridge from
    derive_uv1_tilings into the descriptor that the ASE writer reads.
    Verifies the override surfaces as ``effective_u1_tiling`` /
    ``effective_v1_tiling`` on the resulting MaterialDescriptor.
    """
    ir = threedi_ffi.read_model_3di3(str(AKCRATE_3DI))
    try:
        ir_mat = ir.materials[0]
        desc = describe_material(
            ir_mat,
            source_format=getattr(ir, "source_format", None),
            uv1_tiling_override=(2.0, 6.0),
        )
    finally:
        threedi_ffi.free_model_3di3(ir)

    assert isinstance(desc, MaterialDescriptor)
    assert desc.effective_u1_tiling == pytest.approx(2.0)
    assert desc.effective_v1_tiling == pytest.approx(6.0)


def test_describe_material_defaults_to_identity_uv1_tiling_when_no_override() -> None:
    """Backward compatibility: callers that don't pass ``uv1_tiling_override``
    get identity tiling (matches the pre-fix behavior since the 3DI3 model has no
    native uv1 tiling field).
    """
    ir = threedi_ffi.read_model_3di3(str(AKCRATE_3DI))
    try:
        desc = describe_material(ir.materials[0])
    finally:
        threedi_ffi.free_model_3di3(ir)

    assert desc.effective_u1_tiling == pytest.approx(1.0)
    assert desc.effective_v1_tiling == pytest.approx(1.0)


def test_bake_preserves_uv1_tiling_through_3dp_seed_then_ase_merge(tmp_path) -> None:
    """End-to-end: regenerate akcrate source from the stock 3DI3 model (which exercises
    derive_uv1_tilings + describe_material + populate_ase_submaterial), bake
    via bake_jo_project (which exercises the C-side apply_material_table_from_project
    .ase tiling preservation in libs/object/src/convert_internal.cpp), and
    verify the baked LOD3 VERT[0].uv1 matches OED's expected
    (uv0 - 0.5) * tiling + 0.5 transform (per WriteRDTA @ 0x459a04).

    Without the .ase tiling preservation in apply_material_table_from_project,
    seed_material_table_from_project's hardcoded ``uv1_u_tiling = 0.0f``
    overwrites the .ase-derived tiling and the bake's rdta.cpp uv1 transform
    branch is skipped, leaving uv1 == uv0.
    """
    import struct
    from pyopennova.legacy_tools.stock_roundtrip import (
        discover_stock_3di_fixtures,
        generate_source_from_stock_3di,
    )
    from pyopennova.legacy_tools.project_baker import bake_jo_project
    from pyopennova.threedi_ffi import read_chunk_tree_3di3

    fx = discover_stock_3di_fixtures(names=["akcrate"])[0]
    src = generate_source_from_stock_3di(fx, tmp_path / "source")
    out = tmp_path / "akcrate_opennova.3di"
    bake_jo_project(src.project_path, out, model_name=fx.name)

    root = read_chunk_tree_3di3(str(out))
    rdta = next(c for c in root.children if c.id == "RDTA")
    rlod3 = rdta.children[3]
    vert = next(c for c in rlod3.children if c.id == "VERT")
    d = bytes(vert.data)
    uv0 = struct.unpack_from("<2f", d, 12 + 24)
    uv1 = struct.unpack_from("<2f", d, 12 + 24 + 8)

    # Material 0 has tiling (u, v) = (2.0, 6.0). Expected transform of
    # uv0 = (0.99950, 0.51420):
    #   uv1.u = (0.99950 - 0.5) * 2.0 + 0.5 = 1.49900
    #   uv1.v = (0.51420 - 0.5) * 6.0 + 0.5 = 0.58520
    expected_uv1_u = (uv0[0] - 0.5) * 2.0 + 0.5
    expected_uv1_v = (uv0[1] - 0.5) * 6.0 + 0.5
    assert uv1[0] == pytest.approx(expected_uv1_u, abs=1e-5)
    assert uv1[1] == pytest.approx(expected_uv1_v, abs=1e-5)
    # Sanity: uv1 must NOT be a verbatim copy of uv0 (the pre-fix bug).
    assert uv1 != uv0
