"""Cross-DCC animation parity: Blender vs 3ds Max .adm/.bad for the same model.

Imports a weapon (default WPN_MP5SD / Mp5_1st) with onimport in BOTH Blender and
3ds Max, exports the animations through each plugin's exporter, and asserts the
two produce equivalent .adm + .bad. Equivalence is checked functionally: the
.adm entries must match, and each .bad must reconstruct the same per-frame bone
world transforms (gauge-invariant), since the two DCCs may legitimately choose
different bone-table/translation gauges.

Requires real DCC apps + the stock game data, so it skips unless all are present:
  * bpy importable (Blender as a module)
  * 3dsmaxbatch.exe (auto-discovered or OPENNOVA_3DSMAXBATCH)
  * OPENNOVA_JO_ASSETS pointing at a JO asset dir containing weapon.def + the model
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

from opennova_jobs import ImportOptions, ImportRequest

ROOT = Path(__file__).resolve().parents[1]
ITEM_NAME = os.environ.get("OPENNOVA_PARITY_WEAPON", "WPN_MP5SD")


def _assets_dir():
    base = os.environ.get("OPENNOVA_JO_ASSETS", "").strip()
    if not base:
        pytest.skip("set OPENNOVA_JO_ASSETS to a JO asset dir to run anim DCC parity")
    p = Path(base)
    if not (p / "weapon.def").is_file():
        pytest.skip(f"weapon.def not found under OPENNOVA_JO_ASSETS={base}")
    return p


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------


def _bone_infos(bf):
    from pyopennova import coords

    out = []
    for i in range(int(bf.num_bones)):
        b = bf.bones[i]
        name = b.name.decode("utf-8", "replace") if isinstance(b.name, bytes) else str(b.name)
        out.append((name, int(b.parent_index),
                    coords.bone_space((b.position[0], b.position[1], b.position[2]))))
    return out


def _reconstruct(reset_path, clip_path):
    """World transforms (positions + rotations) per frame, using the reset rest."""
    from pyopennova import bad_ffi
    from pyopennova.animation_build import sample_bad_clip

    bfr = bad_ffi.parse_bad(str(reset_path))
    bfc = bad_ffi.parse_bad(str(clip_path))
    try:
        sampled = sample_bad_clip(bfc, _bone_infos(bfr), animation_name="x")
        return [
            [(b.name, b.world_position, b.world_rotation) for b in fr.bones]
            for fr in sampled.frames
        ]
    finally:
        bad_ffi.free_bad(bfr)
        bad_ffi.free_bad(bfc)


def _read_adm(path):
    from pyopennova import adm_ffi

    af = adm_ffi.parse_adm(str(path))
    try:
        return [(af.entries[i].key.decode("utf-8", "replace"),
                 af.entries[i].value.decode("utf-8", "replace"))
                for i in range(int(af.count))]
    finally:
        adm_ffi.free_adm(af)


def _reset_bad(adm_path):
    """Resolve the reset .bad next to an .adm (the 'anim_reset' entry's value)."""
    out_dir = Path(adm_path).parent
    for key, val in _read_adm(adm_path):
        if key == "anim_reset":
            cand = out_dir / (val + ".bad")
            return cand if cand.is_file() else None
    return None


def _compare(blender_dir: Path, max_dir: Path) -> None:
    # Each side names its .adm after the model stem; find whatever .adm exists.
    b_adms = list(blender_dir.glob("*.adm"))
    m_adms = list(max_dir.glob("*.adm"))
    assert b_adms, f"no Blender .adm in {blender_dir}"
    assert m_adms, f"no Max .adm in {max_dir}"
    b_entries = dict(_read_adm(b_adms[0]))
    m_entries = dict(_read_adm(m_adms[0]))
    assert b_entries == m_entries, f"ADM entries differ:\n blender={b_entries}\n max={m_entries}"

    b_reset = _reset_bad(b_adms[0])
    m_reset = _reset_bad(m_adms[0])
    assert b_reset and m_reset, "missing reset .bad on one side"

    max_pos = 0.0
    max_rot = 0.0
    worst = ""
    for key, bad_name in b_entries.items():
        b_clip = blender_dir / (bad_name + ".bad")
        m_clip = max_dir / (bad_name + ".bad")
        assert b_clip.is_file() and m_clip.is_file(), f"missing {bad_name}.bad on one side"
        b_world = _reconstruct(b_reset, b_clip)
        m_world = _reconstruct(m_reset, m_clip)
        assert len(b_world) == len(m_world), f"{bad_name}: frame count differs"
        for f in range(len(b_world)):
            for (n0, p0, q0), (n1, p1, q1) in zip(b_world[f], m_world[f]):
                dp = sum((p0[k] - p1[k]) ** 2 for k in range(3)) ** 0.5
                dot = abs(sum(q0[k] * q1[k] for k in range(4)))
                if dp > max_pos:
                    max_pos = dp
                    worst = f"{bad_name} frame {f} {n0} pos={dp:.5f}"
                max_rot = max(max_rot, abs(1.0 - dot))
    assert max_pos < 1e-3, f"world position parity exceeded: {worst}"
    assert max_rot < 1e-3, f"world rotation parity exceeded ({max_rot:.5f})"


# ---------------------------------------------------------------------------
# Blender driver (in-process bpy)
# ---------------------------------------------------------------------------


def _export_blender(assets: Path, out_dir: Path) -> None:
    pytest.importorskip("bpy")
    import bpy  # noqa: F401
    from apps.importer.import_runner import _setup_blender_package, execute_import_request

    _setup_blender_package()
    req = ImportRequest.for_definition(
        base_dir=str(assets),
        item_name=ITEM_NAME,
        item_type="weapon",
        output_root=str(out_dir),
        options=ImportOptions(
            import_animations=True, import_arms=False,
            import_collisions=False, import_occlusion=False, import_lights=False,
            write_blend=False, write_ase=False, write_3dp=False,
        ),
    )
    result = execute_import_request(req)
    assert result.ok, f"Blender import failed: {result.error}"

    from blender.anim_exporter import (  # type: ignore[import]
        NovalogicAnimExporter, _ClipData, _collect_unique_actions_from_nla,
        _get_active_armature, _sanitize_name,
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    ctx = bpy.context
    arm = _get_active_armature(ctx)
    assert arm is not None, "no armature after Blender import"
    actions = _collect_unique_actions_from_nla(arm)
    assert actions, "no NLA actions to export from Blender"

    clips = []
    for action in actions:
        flags = int(action.get("bad_flags", 3))
        clips.append(_ClipData(
            action=action,
            action_name=action.name,
            bad_name=action.get("bad_name", _sanitize_name(action.name)),
            start_frame=int(action.frame_start),
            end_frame=int(action.frame_end),
            is_reset=(action.name == "anim_reset"),
            flags=flags,
        ))
    reset_clip = next((c for c in clips if c.is_reset), None)
    assert reset_clip is not None, "no reset clip on Blender side"

    exporter = NovalogicAnimExporter(ctx, arm)
    exporter.configure_from_reset_clip(reset_clip)
    for clip in clips:
        exporter.write_bad(str(out_dir / (clip.bad_name + ".bad")), clip)
    exporter.write_adm(str(out_dir / f"{Path(ITEM_NAME).stem}.adm"), clips)


# ---------------------------------------------------------------------------
# Max driver (3dsmaxbatch subprocess, forcing the repo code)
# ---------------------------------------------------------------------------

_MAX_SCRIPT = r'''
import os, sys
REPO = {repo!r}
sys.path.insert(0, REPO)
for _m in list(sys.modules):
    if (_m == "opennova_max" or _m.startswith("opennova_max.")
            or _m == "pyopennova" or _m.startswith("pyopennova.")
            or _m == "opennova_jobs" or _m.startswith("opennova_jobs.")):
        del sys.modules[_m]
ASSETS = {assets!r}
OUT = {out!r}
ITEM = {item!r}
from pyopennova.asset_resolver import AssetResolver
from pyopennova.definitions import build_animation_context, process_def_files, ensure_extension
from pyopennova.threedi_ffi import read_model, free_model_3di3
from pyopennova.bad_ffi import parse_bad, free_bad
from opennova_max.scene_builder import MaxSceneBuilder
from opennova_max.anim_scene_exporter import AnimSceneExporter
import pymxs
rt = pymxs.runtime
rt.resetMaxFile(rt.Name("noPrompt"))
resolver = AssetResolver(ASSETS, game="jo"); resolver.__enter__()
weapons, _items = process_def_files(resolver=resolver)
w = next(x for x in weapons if x.name == ITEM)
threedi = resolver.resolve(ensure_extension(w.graphic1.main, ".3di"))
anim_ctx = build_animation_context(w.anim_adm, resolver=resolver)
reset_bad = parse_bad(anim_ctx.reset_animation.bad_filepath)
ir = read_model(str(threedi))
b = MaxSceneBuilder(ir, bad_file=reset_bad, anim_context=anim_ctx, resolver=resolver,
                    import_collisions=False, import_occlusion=False, import_lights=False)
b.build_basic_scene("parity"); b.apply_animations()
os.makedirs(OUT, exist_ok=True)
AnimSceneExporter(rt).export(os.path.join(OUT, os.path.splitext(os.path.basename(ITEM))[0] + ".adm"))
free_model_3di3(ir); free_bad(reset_bad); resolver.__exit__(None, None, None)
'''


def _export_max(assets: Path, out_dir: Path) -> None:
    from opennova_max.discovery import resolve_3dsmaxbatch

    exe = resolve_3dsmaxbatch()
    if exe is None:
        pytest.skip("3dsmaxbatch.exe not available; set OPENNOVA_3DSMAXBATCH")
    out_dir.mkdir(parents=True, exist_ok=True)
    script = _MAX_SCRIPT.format(repo=str(ROOT), assets=str(assets), out=str(out_dir), item=ITEM_NAME)
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as fh:
        fh.write(script)
        script_path = fh.name
    try:
        proc = subprocess.run([str(exe), script_path], capture_output=True, text=True, timeout=1800)
    finally:
        os.unlink(script_path)
    if not list(out_dir.glob("*.adm")):
        raise AssertionError("Max anim export produced no .adm\n%s\n%s" % (proc.stdout[-2000:], proc.stderr[-2000:]))


# ---------------------------------------------------------------------------
# Test
# ---------------------------------------------------------------------------


def test_blender_and_max_anim_exports_are_equivalent(tmp_path):
    assets = _assets_dir()
    blender_dir = tmp_path / "blender"
    max_dir = tmp_path / "max"
    _export_blender(assets, blender_dir)
    _export_max(assets, max_dir)
    _compare(blender_dir, max_dir)
