from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import pytest

from opennova_jobs import ImportOptions, ImportRequest


ROOT = Path(__file__).resolve().parents[1]
PARITY_FIXTURE = ROOT / "fixtures" / "threedi" / "3di3" / "Shed.3di"
STOCK_ASE_FIXTURE = ROOT / "fixtures" / "3dp" / "StrgateA" / "StrgateA.ase"


def test_ase_signature_parser_handles_stock_fixture() -> None:
    signature = _ase_semantic_signature(STOCK_ASE_FIXTURE)

    assert signature["total_object_count"] > 0
    assert signature["object_names"]
    assert "materials" in signature


def test_blender_and_max_ase_exports_are_semantically_equivalent(tmp_path: Path) -> None:
    from apps.importer.import_runner import execute_import_request
    from opennova_max.runner import MaxBatchRunner

    if not PARITY_FIXTURE.is_file():
        pytest.fail(f"Fixture missing (LFS not pulled?): {PARITY_FIXTURE}")

    max_runner = MaxBatchRunner()
    if not max_runner.available:
        pytest.skip("3dsmaxbatch.exe not available; set OPENNOVA_3DSMAXBATCH to run ASE parity")

    blender_root = tmp_path / "blender"
    max_root = tmp_path / "max"

    blender_request = ImportRequest.for_loose(
        threedi_path=str(PARITY_FIXTURE),
        output_root=str(blender_root),
        options=ImportOptions(
            write_blend=True,
            write_3dp=False,
            write_ase=True,
            write_max=False,
        ),
    )
    blender_result = execute_import_request(blender_request)
    assert blender_result.ok, f"Blender import failed: {blender_result.error}"
    blender_ase = Path(blender_result.output_path) / f"{PARITY_FIXTURE.stem}.ase"
    assert blender_ase.is_file(), f"Blender ASE missing: {blender_ase}"

    max_request = ImportRequest.for_loose(
        threedi_path=str(PARITY_FIXTURE),
        output_root=str(max_root),
        options=ImportOptions(
            write_blend=False,
            write_3dp=False,
            write_ase=True,
            write_max=True,
        ),
    )
    max_results = max_runner.run([max_request])
    assert len(max_results) == 1
    max_result = max_results[0]
    assert max_result.ok, f"Max import failed: {max_result.error}"
    max_ase = Path(max_result.output_path) / f"{PARITY_FIXTURE.stem}.ase"
    assert max_ase.is_file(), f"Max ASE missing: {max_ase}"

    blender_signature = _ase_semantic_signature(blender_ase)
    max_signature = _ase_semantic_signature(max_ase)
    assert blender_signature == max_signature, _first_mismatch(
        blender_signature,
        max_signature,
    )
    _assert_render_normals_close(blender_ase, max_ase)


def _ase_semantic_signature(path: Path) -> dict[str, Any]:
    from pyopennova import ase_ffi

    doc = ase_ffi.parse_file(str(path))
    try:
        object_names = [_cstr(doc.objects[i].name) for i in range(int(doc.object_count))]
        return {
            "total_object_count": int(doc.object_count),
            "object_names": tuple(sorted(object_names)),
            "flags": int(doc.flags),
            "skinned_flags": int(doc.skinned_flags),
            "materials": _materials_signature(doc),
            "render_meshes": tuple(sorted(
                (
                    _render_mesh_signature(doc.objects[i])
                    for i in range(int(doc.object_count))
                    if _is_render_mesh_name(_cstr(doc.objects[i].name))
                ),
                key=lambda item: item["name"],
            )),
            "part_dummies": tuple(sorted(
                (
                    _marker_mesh_signature(doc.objects[i])
                    for i in range(int(doc.object_count))
                    if _is_part_dummy_name(_cstr(doc.objects[i].name))
                ),
                key=lambda item: item["name"],
            )),
            "helper_meshes": tuple(sorted(
                (
                    _marker_mesh_signature(doc.objects[i])
                    for i in range(int(doc.object_count))
                    if _is_helper_mesh_name(_cstr(doc.objects[i].name))
                ),
                key=lambda item: item["name"],
            )),
            "bones": tuple(sorted(
                (
                    _marker_mesh_signature(doc.objects[i])
                    for i in range(int(doc.object_count))
                    if _is_bone_name(_cstr(doc.objects[i].name))
                ),
                key=lambda item: item["name"],
            )),
            "lights": tuple(sorted(
                (_light_signature(doc.lights[i]) for i in range(int(doc.light_count))),
                key=lambda item: item["name"],
            )),
        }
    finally:
        ase_ffi.free_document(doc)


def _materials_signature(doc) -> tuple[dict[str, Any], ...]:
    materials = []
    for i in range(int(doc.material_count)):
        material = doc.materials[i]
        sub_count = int(material.submaterial_count)
        if sub_count:
            for sub_idx in range(sub_count):
                materials.append(_single_material_signature(material.submaterials[sub_idx]))
        else:
            materials.append(_single_material_signature(material))
    return tuple(materials)


def _single_material_signature(material) -> dict[str, Any]:
    return {
        "name": _cstr(material.name),
        "maps": tuple(_normalize_texture_name(_cstr(material.maps[i])) for i in range(4)),
        "uv_u_tiling": _float_tuple(material.uv_u_tiling, 2),
        "uv_v_tiling": _float_tuple(material.uv_v_tiling, 2),
        "ambient": _float_tuple(material.ambient, 3),
        "diffuse": _float_tuple(material.diffuse, 3),
        "specular": _float_tuple(material.specular, 3),
        "shine": _round(material.shine),
        "shine_strength": _round(material.shine_strength),
        "transparency": _round(material.transparency),
        "wiresize": _round(material.wiresize),
        "shading": int(material.shading),
        "extra_flags": int(material.extra_flags),
    }


def _render_mesh_signature(obj) -> dict[str, Any]:
    vertices = _vertices(obj)
    uvs = _uvs(obj)
    faces = []
    for i in range(int(obj.face_count)):
        face = obj.faces[i]
        verts = tuple(vertices[int(face.vert[j])] for j in range(3))
        face_uvs = tuple(
            uvs[int(face.uv[j])]
            for j in range(3)
            if 0 <= int(face.uv[j]) < len(uvs)
        )
        faces.append((verts, face_uvs, int(face.material_id)))

    return {
        "name": _cstr(obj.name),
        "parent_name": _cstr(obj.parent_name),
        "node_id": int(obj.node_id),
        "face_count": int(obj.face_count),
        "skinned": int(obj.skinned),
        "faces": tuple(sorted(faces, key=lambda item: (item[0], item[1], item[2]))),
        "weights": _weights_signature(obj),
    }


def _assert_render_normals_close(left_path: Path, right_path: Path) -> None:
    left = _render_normals_by_name(left_path)
    right = _render_normals_by_name(right_path)
    assert left.keys() == right.keys()

    tolerance = 0.002
    for name in left:
        left_normals = left[name]
        right_normals = right[name]
        assert len(left_normals) == len(right_normals), name
        if not left_normals:
            continue
        max_delta = max(abs(a - b) for a, b in zip(left_normals, right_normals))
        assert max_delta <= tolerance, (
            f"{name} normal drift {max_delta:.6f} exceeds {tolerance:.6f}"
        )


def _render_normals_by_name(path: Path) -> dict[str, tuple[float, ...]]:
    from pyopennova import ase_ffi

    doc = ase_ffi.parse_file(str(path))
    try:
        out = {}
        for i in range(int(doc.object_count)):
            obj = doc.objects[i]
            name = _cstr(obj.name)
            if not _is_render_mesh_name(name):
                continue
            count = int(getattr(obj, "face_normal_count", 0))
            out[name] = tuple(
                float(obj.face_normals[j])
                for j in range(count * 9)
            )
        return out
    finally:
        ase_ffi.free_document(doc)


def _marker_mesh_signature(obj) -> dict[str, Any]:
    vertices = _vertices(obj)
    return {
        "name": _cstr(obj.name),
        "parent_name": _cstr(obj.parent_name),
        "face_count": int(obj.face_count),
        "vertices": tuple(sorted(set(vertices))),
    }


def _vertices(obj) -> tuple[tuple[float, ...], ...]:
    return tuple(
        _float_tuple((obj.verts[i * 3], obj.verts[i * 3 + 1], obj.verts[i * 3 + 2]), 3)
        for i in range(int(obj.vert_count))
    )


def _uvs(obj) -> tuple[tuple[float, ...], ...]:
    return tuple(
        (_round(obj.uvs[i].u), _round(obj.uvs[i].v), _round(obj.uvs[i].w))
        for i in range(int(obj.uv_count))
    )


def _weights_signature(obj) -> tuple[tuple[tuple[int, ...], tuple[float, ...]], ...]:
    if int(obj.skinned) == 0:
        return ()
    return tuple(sorted(
        (
            tuple(int(obj.weights[i].bone_index[j]) for j in range(4)),
            tuple(_round(obj.weights[i].weight[j]) for j in range(4)),
        )
        for i in range(int(obj.weight_count))
    ))


def _light_signature(light) -> dict[str, Any]:
    return {
        "name": _cstr(light.name),
        "type": int(light.type),
        "pos": _float_tuple(light.pos, 3),
        "color": _float_tuple(light.color, 3),
        "intensity": _round(light.intensity),
        "atten_start": _round(light.atten_start),
        "atten_end": _round(light.atten_end),
        "near_atten_start": _round(light.near_atten_start),
        "near_atten_end": _round(light.near_atten_end),
        "hotspot": _round(light.hotspot),
        "falloff": _round(light.falloff),
        "tm_row2": _float_tuple(light.tm_row2, 3),
    }


def _is_render_mesh_name(name: str) -> bool:
    return re.match(r"^\d{2} Mesh\d+$", name) is not None


def _is_part_dummy_name(name: str) -> bool:
    return re.match(r"^PN\d{2}$", name) is not None


def _is_bone_name(name: str) -> bool:
    return re.match(r"^BN\d{2}$", name) is not None


def _is_helper_mesh_name(name: str) -> bool:
    return not (
        _is_render_mesh_name(name)
        or _is_part_dummy_name(name)
        or _is_bone_name(name)
    )


def _normalize_texture_name(name: str) -> str:
    return name.replace("\\", "/").lower()


def _cstr(value) -> str:
    return bytes(value).split(b"\0", 1)[0].decode("utf-8", errors="replace")


def _float_tuple(values, count: int, *, precision: int = 4) -> tuple[float, ...]:
    return tuple(_round(values[i], precision=precision) for i in range(count))


def _round(value, *, precision: int = 4) -> float:
    rounded = round(float(value), precision)
    return 0.0 if rounded == -0.0 else rounded


def _first_mismatch(left, right, path: str = "signature") -> str:
    if type(left) is not type(right):
        return f"{path}: type differs ({type(left).__name__} != {type(right).__name__})"
    if isinstance(left, dict):
        if left.keys() != right.keys():
            return f"{path}: keys differ ({sorted(left.keys())} != {sorted(right.keys())})"
        for key in left:
            if left[key] != right[key]:
                return _first_mismatch(left[key], right[key], f"{path}.{key}")
        return f"{path}: dictionaries differ"
    if isinstance(left, (list, tuple)):
        if len(left) != len(right):
            return f"{path}: length differs ({len(left)} != {len(right)})"
        for index, (left_item, right_item) in enumerate(zip(left, right)):
            if left_item != right_item:
                return _first_mismatch(left_item, right_item, f"{path}[{index}]")
        return f"{path}: sequences differ"
    return f"{path}: {left!r} != {right!r}"
