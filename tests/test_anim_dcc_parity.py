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

from opennova_jobs import ImportOptions, ImportRequest, validate_import_request

ROOT = Path(__file__).resolve().parents[1]
ITEM_NAME = os.environ.get("OPENNOVA_PARITY_WEAPON", "WPN_MP5SD")
_BPY_UNAVAILABLE_EXIT = 5
_BLENDER_CHILD_CODE = f"""
import importlib.util
import runpy
import sys
from pathlib import Path

if importlib.util.find_spec("bpy") is None:
    raise SystemExit({_BPY_UNAVAILABLE_EXIT})

namespace = runpy.run_path(sys.argv[1])
namespace["_export_blender_in_child"](
    Path(sys.argv[2]),
    Path(sys.argv[3]),
    sys.argv[4],
)
"""


def _assets_dir():
    base = os.environ.get("OPENNOVA_JO_ASSETS", "").strip()
    if not base:
        pytest.skip("set OPENNOVA_JO_ASSETS to a JO asset dir to run anim DCC parity")
    p = Path(base)
    if not (p / "weapon.def").is_file():
        pytest.skip(f"weapon.def not found under OPENNOVA_JO_ASSETS={base}")
    return p


def test_blender_animation_request_writes_real_scene(tmp_path):
    request = _blender_import_request(tmp_path / "assets", tmp_path / "out", ITEM_NAME)

    assert request.options.write_blend
    assert request.options.writes_any_output_file()
    assert validate_import_request(request, check_paths=False) == []


def test_blender_animation_export_runs_in_subprocess(monkeypatch, tmp_path):
    calls = []
    real_importorskip = pytest.importorskip
    compile(_BLENDER_CHILD_CODE, "<blender-animation-child>", "exec")

    def fail_parent_bpy_import(name, *args, **kwargs):
        if name == "bpy":
            raise AssertionError("bpy must not be imported in the pytest parent")
        return real_importorskip(name, *args, **kwargs)

    def fake_run(command, **kwargs):
        calls.append((command, kwargs))
        return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

    monkeypatch.delitem(sys.modules, "bpy", raising=False)
    monkeypatch.setattr(pytest, "importorskip", fail_parent_bpy_import)
    monkeypatch.setattr(subprocess, "run", fake_run)

    _export_blender(tmp_path / "assets", tmp_path / "out")

    assert "bpy" not in sys.modules
    assert len(calls) == 1
    command, kwargs = calls[0]
    assert command[0] == sys.executable
    assert command[1] == "-c"
    assert command[-3:] == [
        str(tmp_path / "assets"),
        str(tmp_path / "out"),
        ITEM_NAME,
    ]
    assert kwargs["cwd"] == ROOT


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
# Blender driver (isolated bpy subprocess)
# ---------------------------------------------------------------------------


def _blender_import_request(
    assets: Path,
    out_dir: Path,
    item_name: str,
) -> ImportRequest:
    return ImportRequest.for_definition(
        base_dir=str(assets),
        item_name=item_name,
        item_type="weapon",
        output_root=str(out_dir),
        options=ImportOptions(
            import_animations=True, import_arms=False,
            import_collisions=False, import_occlusion=False, import_lights=False,
            write_blend=True, write_ase=False, write_3dp=False,
        ),
    )


def _export_blender(assets: Path, out_dir: Path) -> None:
    command = [
        sys.executable,
        "-c",
        _BLENDER_CHILD_CODE,
        str(Path(__file__).resolve()),
        str(assets),
        str(out_dir),
        ITEM_NAME,
    ]
    proc = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=1800,
    )
    if proc.returncode == _BPY_UNAVAILABLE_EXIT:
        pytest.skip("bpy is not importable; install the standalone Blender module")
    if proc.returncode != 0:
        raise AssertionError(
            "Blender anim export subprocess failed\n"
            f"stdout:\n{proc.stdout[-4000:]}\n"
            f"stderr:\n{proc.stderr[-4000:]}"
        )


def _export_blender_in_child(
    assets: Path,
    out_dir: Path,
    item_name: str,
) -> None:
    """Import the scene and export animations inside the dedicated child only."""
    from apps.importer.dispatcher import ImportDispatcher
    from apps.importer.import_runner import _setup_blender_package

    request = _blender_import_request(assets, out_dir, item_name)
    with ImportDispatcher(max_workers=1) as dispatcher:
        result = dispatcher.submit(request).result()
    assert result.ok, f"Blender import failed: {result.error}"

    blend_files = [
        Path(path)
        for path in result.written_files
        if Path(path).suffix.lower() == ".blend"
    ]
    assert len(blend_files) == 1, (
        f"expected one Blender scene output, got {result.written_files}"
    )

    _setup_blender_package()
    import bpy

    bpy.ops.wm.open_mainfile(filepath=str(blend_files[0]))
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
    exporter.write_adm(str(out_dir / f"{Path(item_name).stem}.adm"), clips)


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
