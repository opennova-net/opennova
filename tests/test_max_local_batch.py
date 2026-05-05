"""Local-only 3ds Max batch validation.

CI intentionally skips this because 3dsmaxbatch.exe is not available there.
Set OPENNOVA_RUN_MAX_LOCAL=1 to run it on a developer machine.
"""
from __future__ import annotations

import base64
import os
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"
VEHICLE_FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "3di3" / "JetSki.3di"
SKINNED_FIXTURE_3DI = ROOT / "fixtures" / "threedi" / "Fsldr03.3di"
BHD_STOCK_DIR = Path(r"C:\Users\taylor\Desktop\BHD_STock2")
BHD_PFF_DIR = Path(r"C:\Users\taylor\Desktop\Delta Force Black Hawk Down")
DEFAULT_BATCH = Path(r"C:\Program Files\Autodesk\3ds Max 2022\3dsmaxbatch.exe")


def test_max_batch_loose_import_writes_shared_outputs(tmp_path: Path) -> None:
    if os.environ.get("OPENNOVA_RUN_MAX_LOCAL") != "1":
        pytest.skip("set OPENNOVA_RUN_MAX_LOCAL=1 to run local 3ds Max validation")
    batch = Path(os.environ.get("OPENNOVA_3DSMAXBATCH", str(DEFAULT_BATCH)))
    if not batch.is_file():
        pytest.skip(f"3dsmaxbatch.exe not found: {batch}")
    if not FIXTURE_3DI.is_file():
        pytest.skip("fixture missing (LFS not pulled?)")
    if not VEHICLE_FIXTURE_3DI.is_file():
        pytest.skip("vehicle fixture missing (LFS not pulled?)")
    if not SKINNED_FIXTURE_3DI.is_file():
        pytest.skip("skinned fixture missing (LFS not pulled?)")

    output_dir = tmp_path / "Shed"
    vehicle_output_dir = tmp_path / "JetSki"
    bhd_output_dir = tmp_path / "ESomal03"
    pff_mog_output_dir = tmp_path / "MogBld3_PFF"
    pff_room_output_dir = tmp_path / "rmlivinA_PFF"
    skinned_output_dir = tmp_path / "Fsldr03"
    texture_dir = tmp_path / "texture_assets"
    texture_dir.mkdir()
    one_pixel_png = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
    )
    for texture_name in ("Jrubble1.png", "oceil1.png", "KBulb.png"):
        (texture_dir / texture_name).write_bytes(one_pixel_png)
    script = tmp_path / "opennova_max_local_validate.py"
    listener_log = tmp_path / "listener.log"
    max_log = tmp_path / "max.log"
    script.write_text(
        "\n".join([
            "import sys",
            f"sys.path.insert(0, {str(ROOT)!r})",
            "for _module in list(sys.modules):",
            "    if (",
            "        _module == 'opennova_max'",
            "        or _module.startswith('opennova_max.')",
            "        or _module == 'pyopennova'",
            "        or _module.startswith('pyopennova.')",
            "    ):",
            "        del sys.modules[_module]",
            "from pathlib import Path",
            "from opennova_max import run_loose_import",
            "from opennova_max import qt_ui",
            "import pymxs",
            "print('OPENNOVA_MAX_LOCAL_START')",
            "assert qt_ui.is_available()",
            "assert qt_ui.dialog_title('9.8.7') == 'OpenNova Importer v9.8.7'",
            "assert qt_ui.can_import_loose('C:/asset.3di', 'C:/out', True, False, False)",
            "assert not qt_ui.can_import_loose('C:/asset.3di', 'C:/out', False, False, False)",
            "print('OPENNOVA_MAX_QT_UI_MODULE_OK', qt_ui.QT_BINDING)",
            "rt = pymxs.runtime",
            "def _pos_tuple(obj):",
            "    p = obj.transform.position",
            "    return (float(p.x), float(p.y), float(p.z))",
            "def _near(a, b, eps=0.0002):",
            "    return all(abs(float(a[i]) - float(b[i])) <= eps for i in range(3))",
            "def _find_obj(name):",
            "    for obj in list(rt.objects):",
            "        if str(obj.name) == name:",
            "            return obj",
            "    raise RuntimeError('object not found: ' + name)",
            "def _user_prop(obj, key):",
            "    try:",
            "        return rt.getUserProp(obj, key)",
            "    except Exception:",
            "        return None",
            "def _collect_materials(mat, out):",
            "    if mat is None:",
            "        return",
            "    name = str(getattr(mat, 'name', ''))",
            "    if name and name in out:",
            "        return",
            "    if name:",
            "        out[name] = mat",
            "    try:",
            "        numsubs = int(mat.numsubs)",
            "    except Exception:",
            "        numsubs = 0",
            "    for slot in range(1, numsubs + 1):",
            "        try:",
            "            sub = rt.getSubMtl(mat, slot)",
            "        except Exception:",
            "            sub = None",
            "        if sub is not None:",
            "            _collect_materials(sub, out)",
            "def _scene_materials():",
            "    out = {}",
            "    for obj in list(rt.objects):",
            "        try:",
            "            mat = obj.material",
            "        except Exception:",
            "            mat = None",
            "        _collect_materials(mat, out)",
            "    return out",
            "def _bitmap_filename(mat):",
            "    try:",
            "        bitmap = mat.diffuseMap",
            "    except Exception:",
            "        bitmap = None",
            "    if bitmap is None:",
            "        return ''",
            "    try:",
            "        return str(bitmap.filename)",
            "    except Exception:",
            "        return ''",
            "def _assert_mesh_uvs_and_materials():",
            "    checked = 0",
            "    for obj in list(rt.objects):",
            "        if not str(obj.name).endswith('Mesh0'):",
            "            continue",
            "        if _user_prop(obj, 'opennova_part_index') in (None, ''):",
            "            continue",
            "        try:",
            "            face_count = int(rt.getNumFaces(obj))",
            "        except Exception:",
            "            continue",
            "        if face_count <= 0:",
            "            continue",
            "        uv0 = int(rt.meshop.getNumMapVerts(obj, 1))",
            "        if uv0 != face_count * 3:",
            "            raise RuntimeError(f'{obj.name} UV0 count {uv0} != {face_count * 3}')",
            "        try:",
            "            mat = obj.material",
            "        except Exception:",
            "            mat = None",
            "        try:",
            "            numsubs = int(mat.numsubs)",
            "        except Exception:",
            "            numsubs = 0",
            "        for slot in range(1, numsubs + 1):",
            "            try:",
            "                sub = rt.getSubMtl(mat, slot)",
            "            except Exception:",
            "                sub = None",
            "            if sub is None:",
            "                raise RuntimeError(f'{obj.name} missing MultiMaterial slot {slot}')",
            "        checked += 1",
            "    if checked <= 0:",
            "        raise RuntimeError('no non-empty Mesh0 objects checked')",
            (
                "ok = run_loose_import("
                f"{str(FIXTURE_3DI)!r}, "
                f"{str(output_dir)!r}, "
                f"asset_base_dir={str(texture_dir)!r}, "
                "write_ase=True, write_3dp=True)"
            ),
            "print('OPENNOVA_MAX_LOCAL_RESULT', ok)",
            f"expected = {[str(output_dir / 'Shed.ase'), str(output_dir / 'Shed.3dp'), str(output_dir / 'Shed.3da'), str(output_dir / 'Shed.max')]!r}",
            "missing = [p for p in expected if not Path(p).is_file() or Path(p).stat().st_size <= 0]",
            "if missing:",
            "    raise RuntimeError('missing outputs: ' + repr(missing))",
            "root = _find_obj('Shed')",
            "diffuse_manifest = str(_user_prop(root, 'opennova_material_diffuse_paths') or '')",
            "if 'jrubble1.png' not in diffuse_manifest.lower():",
            "    raise RuntimeError('resolved diffuse texture diagnostics missing: ' + diffuse_manifest)",
            "missing_diffuse = str(_user_prop(root, 'opennova_material_missing_diffuse') or '')",
            "if missing_diffuse:",
            "    raise RuntimeError('unexpected missing diffuse diagnostics: ' + missing_diffuse)",
            "_assert_mesh_uvs_and_materials()",
            "print('OPENNOVA_MAX_MATERIALS_OK', diffuse_manifest)",
            "from pyopennova.threedi_ffi import free_model_ir, read_model_ir",
            "from pyopennova import coords",
            f"vehicle_ir = read_model_ir({str(VEHICLE_FIXTURE_3DI)!r})",
            "try:",
            "    vehicle_lod = vehicle_ir.lods[0]",
            "    expected_parts = {",
            "        f'PN{i + 1:02d}': coords.render_space(vehicle_lod.parts[i].abs_position)",
            "        for i in range(int(vehicle_lod.part_count))",
            "    }",
            "finally:",
            "    free_model_ir(vehicle_ir)",
            (
                "ok = run_loose_import("
                f"{str(VEHICLE_FIXTURE_3DI)!r}, "
                f"{str(vehicle_output_dir)!r}, "
                "write_ase=True, write_3dp=False)"
            ),
            "print('OPENNOVA_MAX_VEHICLE_RESULT', ok)",
            "if not ok:",
            "    raise RuntimeError('vehicle import produced no geometry')",
            f"if not Path({str(vehicle_output_dir / 'JetSki.max')!r}).is_file() or Path({str(vehicle_output_dir / 'JetSki.max')!r}).stat().st_size <= 0:",
            "    raise RuntimeError('vehicle .max output missing')",
            "for part_name, expected_pos in expected_parts.items():",
            "    actual_pos = _pos_tuple(_find_obj(part_name))",
            "    if not _near(actual_pos, expected_pos):",
            "        raise RuntimeError(f'{part_name} position {actual_pos} != {expected_pos}')",
            "mesh02 = _find_obj('02 Mesh0')",
            "mesh02_parent_pos = _pos_tuple(mesh02.parent)",
            "mesh02_pos = _pos_tuple(mesh02)",
            "if not _near(mesh02_pos, mesh02_parent_pos):",
            "    raise RuntimeError(f'02 Mesh0 position {mesh02_pos} != parent {mesh02_parent_pos}')",
            "print('OPENNOVA_MAX_VEHICLE_PLACEMENT_OK')",
            f"bhd_dir = Path({str(BHD_STOCK_DIR)!r})",
            "bhd_model = bhd_dir / 'ESOMAL03.3di'",
            "if bhd_dir.is_dir() and bhd_model.is_file():",
            "    ok = run_loose_import(",
            "        str(bhd_model),",
            f"        {str(bhd_output_dir)!r},",
            "        output_stem='ESomal03',",
            "        asset_base_dir=str(bhd_dir),",
            "        write_ase=True,",
            "        write_3dp=False,",
            "    )",
            "    print('OPENNOVA_MAX_BHD_ESOMAL03_RESULT', ok)",
            "    if not ok:",
            "        raise RuntimeError('ESomal03 import produced no geometry')",
            f"    if not Path({str(bhd_output_dir / 'ESomal03.max')!r}).is_file() or Path({str(bhd_output_dir / 'ESomal03.max')!r}).stat().st_size <= 0:",
            "        raise RuntimeError('ESomal03 .max output missing')",
            "    esomal_root = _find_obj('ESomal03')",
            "    esomal_manifest = str(_user_prop(esomal_root, 'opennova_material_diffuse_paths') or '').lower()",
            "    if 'irpger.dds' not in esomal_manifest or 'lrpggun.dds' not in esomal_manifest:",
            "        raise RuntimeError('ESomal03 normalized diffuse diagnostics missing: ' + esomal_manifest)",
            "    esomal_missing = str(_user_prop(esomal_root, 'opennova_material_missing_diffuse') or '')",
            "    if esomal_missing:",
            "        raise RuntimeError('ESomal03 unexpected missing diffuse diagnostics: ' + esomal_missing)",
            "    print('OPENNOVA_MAX_BHD_ESOMAL03_TEXTURES_OK', esomal_manifest)",
            "else:",
            "    print('OPENNOVA_MAX_BHD_ESOMAL03_SKIPPED')",
            f"pff_dir = Path({str(BHD_PFF_DIR)!r})",
            "if pff_dir.is_dir():",
            "    from pyopennova.asset_resolver import AssetResolver",
            "    with AssetResolver(str(pff_dir)) as resolver:",
            "        mog_path = resolver.resolve('MogBld3.3di')",
            "        room_path = resolver.resolve('rmlivinA.3di')",
            "    if not mog_path or not room_path:",
            "        raise RuntimeError('PFF validation models missing')",
            "    ok = run_loose_import(",
            "        str(mog_path),",
            f"        {str(pff_mog_output_dir)!r},",
            "        output_stem='MogBld3_PFF',",
            "        asset_base_dir=str(pff_dir),",
            "        write_ase=False,",
            "        write_3dp=False,",
            "    )",
            "    print('OPENNOVA_MAX_PFF_MOGBLD3_RESULT', ok)",
            "    if not ok:",
            "        raise RuntimeError('MogBld3 PFF import produced no geometry')",
            "    mog_root = _find_obj('MogBld3_PFF')",
            "    mog_manifest = str(_user_prop(mog_root, 'opennova_material_diffuse_paths') or '').lower()",
            "    if 'qmog05.dds' not in mog_manifest or 'qmult02.dds' not in mog_manifest or 'qmult05.dds' not in mog_manifest:",
            "        raise RuntimeError('MogBld3 PFF diffuse diagnostics missing: ' + mog_manifest)",
            "    mog_missing = str(_user_prop(mog_root, 'opennova_material_missing_diffuse') or '')",
            "    if mog_missing:",
            "        raise RuntimeError('MogBld3 PFF unexpected missing diffuse diagnostics: ' + mog_missing)",
            "    mog_detail_manifest = str(_user_prop(mog_root, 'opennova_material_detail_paths') or '').lower()",
            "    if 'qmult02.dds' not in mog_detail_manifest or 'qmult05.dds' not in mog_detail_manifest:",
            "        raise RuntimeError('MogBld3 PFF detail diagnostics missing: ' + mog_detail_manifest)",
            "    mog_missing_detail = str(_user_prop(mog_root, 'opennova_material_missing_detail') or '')",
            "    if mog_missing_detail:",
            "        raise RuntimeError('MogBld3 PFF unexpected missing detail diagnostics: ' + mog_missing_detail)",
            "    mog_materials = _scene_materials()",
            "    for mat_name, diffuse_name in (",
            "        ('Material_0_VS_PHONGT', 'qmog05.dds'),",
            "        ('Material_2_VS_PHONGT', 'qmog05.dds'),",
            "    ):",
            "        mat = mog_materials.get(mat_name)",
            "        if mat is None:",
            "            raise RuntimeError('MogBld3 PFF material missing: ' + mat_name)",
            "        bitmap_path = _bitmap_filename(mat).lower()",
            "        if diffuse_name not in bitmap_path:",
            "            raise RuntimeError(f'{mat_name} diffuseMap {bitmap_path!r} does not contain {diffuse_name}')",
            "    print('OPENNOVA_MAX_PFF_MOGBLD3_TEXTURES_OK', mog_manifest)",
            "    ok = run_loose_import(",
            "        str(room_path),",
            f"        {str(pff_room_output_dir)!r},",
            "        output_stem='rmlivinA_PFF',",
            "        asset_base_dir=str(pff_dir),",
            "        write_ase=False,",
            "        write_3dp=False,",
            "    )",
            "    print('OPENNOVA_MAX_PFF_RMLIVINA_RESULT', ok)",
            "    if not ok:",
            "        raise RuntimeError('rmlivinA PFF import produced no geometry')",
            "    room_root = _find_obj('rmlivinA_PFF')",
            "    room_manifest = str(_user_prop(room_root, 'opennova_material_diffuse_paths') or '').lower()",
            "    if 'ndeco.dds' not in room_manifest or 'ndeco1.dds' not in room_manifest:",
            "        raise RuntimeError('rmlivinA PFF diffuse diagnostics missing: ' + room_manifest)",
            "    room_missing = str(_user_prop(room_root, 'opennova_material_missing_diffuse') or '')",
            "    if room_missing:",
            "        raise RuntimeError('rmlivinA PFF unexpected missing diffuse diagnostics: ' + room_missing)",
            "    room_materials = _scene_materials()",
            "    room_mat = room_materials.get('Material_0_VS_PHONGT')",
            "    if room_mat is None or 'ndeco.dds' not in _bitmap_filename(room_mat).lower():",
            "        raise RuntimeError('rmlivinA PFF primary diffuseMap missing')",
            "    print('OPENNOVA_MAX_PFF_RMLIVINA_TEXTURES_OK', room_manifest)",
            "else:",
            "    print('OPENNOVA_MAX_PFF_TEXTURES_SKIPPED')",
            (
                "ok = run_loose_import("
                f"{str(SKINNED_FIXTURE_3DI)!r}, "
                f"{str(skinned_output_dir)!r}, "
                "write_ase=False, write_3dp=True)"
            ),
            "print('OPENNOVA_MAX_SKIN_RESULT', ok)",
            "if not ok:",
            "    raise RuntimeError('skinned import produced no geometry')",
            f"if not Path({str(skinned_output_dir / 'Fsldr03.max')!r}).is_file() or Path({str(skinned_output_dir / 'Fsldr03.max')!r}).stat().st_size <= 0:",
            "    raise RuntimeError('skinned .max output missing')",
            "skin_summaries = []",
            "skin_warnings = []",
            "for obj in list(pymxs.runtime.objects):",
            "    try:",
            "        bound = pymxs.runtime.getUserProp(obj, 'opennova_skin_bound_vertices')",
            "    except Exception:",
            "        bound = None",
            "    if bound not in (None, ''):",
            "        skin_summaries.append((obj.name, int(str(bound))))",
            "    try:",
            "        warning = pymxs.runtime.getUserProp(obj, 'opennova_skin_warning')",
            "    except Exception:",
            "        warning = None",
            "    if warning not in (None, ''):",
            "        skin_warnings.append((obj.name, str(warning)))",
            "if not skin_summaries:",
            "    raise RuntimeError('no skinned meshes recorded binding metadata')",
            "bound_total = sum(count for _name, count in skin_summaries)",
            "if bound_total <= 0:",
            "    raise RuntimeError('skinned meshes recorded zero weighted vertices: ' + repr(skin_summaries))",
            "if skin_warnings:",
            "    raise RuntimeError('skin warnings: ' + repr(skin_warnings))",
            "print('OPENNOVA_MAX_SKIN_OK', len(skin_summaries), bound_total)",
            "print('OPENNOVA_MAX_LOCAL_OK')",
        ]),
        encoding="utf-8",
    )

    proc = subprocess.run(
        [
            str(batch),
            str(script),
            "-v",
            "3",
            "-log",
            str(max_log),
            "-listenerlog",
            str(listener_log),
        ],
        text=True,
        capture_output=True,
        timeout=600,
    )
    listener = listener_log.read_text(errors="replace") if listener_log.exists() else ""
    assert proc.returncode == 0, proc.stdout + proc.stderr + listener
    assert "OPENNOVA_MAX_LOCAL_OK" in listener
