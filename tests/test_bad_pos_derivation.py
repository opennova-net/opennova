"""bad_positions_from_model: BadBone.position reconstructed from the model.

The relation (docs/net/novaworld-net-re.md section 5.40, corpus-witnessed):

    position[i] = bind_rows[parent(i)] @ (-rel.x, rel.y, rel.z)

The synthetic test runs unconditionally (pure math, no native lib).  The
retail-corpus tests are asset-gated on OPENNOVA_JO_ASSETS (extracted JO
assets, e.g. the JOX dir) and skip-as-pass without it, per
docs/asset-gated-tests.md.
"""
from __future__ import annotations

import math
import os
from pathlib import Path

import pytest

from pyopennova.animation_build import mat_vec_mul, quat_to_matrix
from pyopennova.bad_build import bad_positions_from_model

JO_ASSETS = os.environ.get("OPENNOVA_JO_ASSETS", "").strip()

requires_assets = pytest.mark.skipif(
    not JO_ASSETS, reason="OPENNOVA_JO_ASSETS not set (asset-gated; skip == pass)"
)

# Healthy JO viewmodel rigs (intact shipped positions; the broken 12 are
# catalogued in section 5.40).  Reconstruction must match the shipped bone
# table on every norm-consistent bone.
HEALTHY_RIGS = ["M16_1st", "M24_1st", "M21_1st", "Frag_1st"]
BROKEN_RIG = "ak47_1st"  # ships X-triplicated/zeroed positions


# ---------------------------------------------------------------------------
# synthetic (always runs)
# ---------------------------------------------------------------------------


def test_synthetic_rig_exact():
    rot = [
        quat_to_matrix((1.0, 0.0, 0.0, 0.0)),                    # identity root
        quat_to_matrix((math.cos(0.4), 0.0, 0.0, math.sin(0.4))),
        quat_to_matrix((math.cos(0.7), 0.0, math.sin(0.7), 0.0)),
        quat_to_matrix((0.9, 0.1, -0.3, 0.2)),                   # normalized inside
    ]
    parents = [0, 0, 1, 1]  # bone 0 self-parented == root
    rels = [(0.0, 0.0, 0.0), (0.25, -0.5, 1.0), (-0.125, 0.75, 0.5), (2.0, 0.0, -1.0)]

    got = bad_positions_from_model(rot, parents, rels)

    assert got[0] == (0.0, 0.0, 0.0)  # root: x-negated rel, unrotated
    for i in (1, 2, 3):
        expected = mat_vec_mul(rot[parents[i]], (-rels[i][0], rels[i][1], rels[i][2]))
        assert all(abs(a - b) < 1e-12 for a, b in zip(got[i], expected))


def test_surplus_bones_beyond_parts():
    # AKM_1st shape: more bones than parts; output is paired to the parts.
    rot = [quat_to_matrix((1.0, 0.0, 0.0, 0.0))] * 4
    got = bad_positions_from_model(rot, [0, 0, 1, 2], [(0, 0, 0), (1.0, 2.0, 3.0)])
    assert len(got) == 2
    assert got[1] == (-1.0, 2.0, 3.0)


# ---------------------------------------------------------------------------
# retail corpus (asset-gated)
# ---------------------------------------------------------------------------


def _load_rig(name: str):
    """(bones, parts) for a viewmodel rig: parts from the .3di LOD0 table,
    bones from the .adm's anim_reset .bad — the runtime pairing."""
    from pyopennova.adm_ffi import free_adm, parse_adm
    from pyopennova.bad_ffi import free_bad, parse_bad
    from pyopennova.threedi_ffi import free_model_ir, read_model_ir

    root = Path(JO_ASSETS)
    files = {p.name.lower(): p for p in root.iterdir() if p.is_file()}
    model_path = files[f"{name.lower()}.3di"]
    adm_path = files[f"{name.lower()}.adm"]

    af = parse_adm(str(adm_path))
    reset = None
    try:
        for i in range(af.count):
            if af.entries[i].key.decode("ascii", "replace").strip().lower() == "anim_reset":
                toks = af.entries[i].value.decode("ascii", "replace").replace('"', " ").split()
                if toks:
                    reset = toks[0]
                break
    finally:
        free_adm(af)
    assert reset, f"no anim_reset in {adm_path}"
    bad_path = files.get(reset.lower()) or files[reset.lower() + ".bad"]

    ir = read_model_ir(str(model_path))
    try:
        lod = ir.lods[0]
        parts = [
            dict(
                parent=int(lod.parts[i].parent_index),
                rel=tuple(float(v) for v in lod.parts[i].rel_position),
            )
            for i in range(lod.part_count)
        ]
    finally:
        free_model_ir(ir)

    bf = parse_bad(str(bad_path))
    try:
        bones = [
            dict(
                parent=int(bf.bones[i].parent_index),
                pos=tuple(float(v) for v in bf.bones[i].position),
                rot=tuple(
                    tuple(float(bf.bones[i].rotation[r * 3 + c]) for c in range(3))
                    for r in range(3)
                ),
            )
            for i in range(bf.num_bones)
        ]
    finally:
        free_bad(bf)
    return bones, parts


def _norm(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


@requires_assets
@pytest.mark.parametrize("rig", HEALTHY_RIGS)
def test_healthy_rig_positions_reconstruct(rig):
    bones, parts = _load_rig(rig)
    npair = min(len(bones), len(parts))
    derived = bad_positions_from_model(
        [b["rot"] for b in bones],
        [b["parent"] for b in bones[:npair]],
        [p["rel"] for p in parts[:npair]],
    )
    checked = 0
    for i in range(npair):
        if bones[i]["parent"] < 0 or bones[i]["parent"] == i:
            continue  # root carries no parent-frame constraint
        shipped, nd = bones[i]["pos"], _norm(parts[i]["rel"])
        if nd > 1e-6 and abs(_norm(shipped) - nd) / nd >= 5e-3:
            continue  # shipped value is stale/broken (norm-inconsistent)
        err = _norm(tuple(a - b for a, b in zip(derived[i], shipped)))
        assert err <= 5e-4, f"{rig} bone {i}: |derived-shipped|={err:.2e}"
        checked += 1
    assert checked >= 30, f"{rig}: only {checked} bones exercised"


@requires_assets
def test_broken_rig_triplicated_positions_reconstruct():
    # ak47_1st ships the X-triplicated breakage (pos.x copied into all three
    # slots — section 5.40); reconstruction must produce model-consistent
    # values (|derived| == |rel|, orthogonal transform) instead of echoing it.
    bones, parts = _load_rig(BROKEN_RIG)
    npair = min(len(bones), len(parts))
    derived = bad_positions_from_model(
        [b["rot"] for b in bones],
        [b["parent"] for b in bones[:npair]],
        [p["rel"] for p in parts[:npair]],
    )
    fixed = 0
    for i in range(npair):
        if bones[i]["parent"] < 0 or bones[i]["parent"] == i:
            continue
        pos, nd = bones[i]["pos"], _norm(parts[i]["rel"])
        triplicated = pos[0] == pos[1] == pos[2]  # bit-copied breakage
        if triplicated and nd > 1e-3:
            assert abs(_norm(derived[i]) - nd) < 5e-4 * max(1.0, nd)
            fixed += 1
    assert fixed >= 20, f"only {fixed} triplicated bones reconstructed"
