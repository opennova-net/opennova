"""Editable_Mesh + Dummy hierarchy builders for the Max addon."""
from __future__ import annotations

from typing import Any, Iterable, Sequence

from pyopennova import coords
from pyopennova.mesh_build import FlatMesh, flatten_lod


def _rt():
    """Return ``pymxs.runtime``, raising a clear error outside Max."""
    try:
        import pymxs
    except ImportError as exc:  # pragma: no cover - exercised only inside Max
        raise RuntimeError(
            "pymxs is not available; opennova_max only runs inside 3ds Max 2021+."
        ) from exc
    return pymxs.runtime


def build_part_hierarchy(
    ir,
    lod_index: int,
    name: str,
    *,
    hidden: bool = False,
    material_names: str = "",
) -> tuple[Any, dict[int, Any]]:
    """Create one Dummy per LOD part, parented as in the 3DI3 model."""
    rt = _rt()
    lod = ir.lods[lod_index]
    num_parts = int(lod.part_count)

    root = rt.Dummy()
    root.name = name
    _set_hidden(root, hidden)
    _set_user_prop(rt, root, "_lod_index", lod_index)
    if material_names:
        _set_user_prop(rt, root, "_material_names", material_names)

    part_nodes: dict[int, Any] = {}
    for i in range(num_parts):
        d = rt.Dummy()
        d.name = f"PN{i + 1:02d}"
        _set_hidden(d, hidden)
        part_nodes[i] = d

    for i in range(num_parts):
        part = lod.parts[i]
        node = part_nodes[i]

        abs_pos = coords.render_space((
            float(part.abs_position[0]),
            float(part.abs_position[1]),
            float(part.abs_position[2]),
        ))
        rel_pos = coords.render_space((
            float(part.rel_position[0]),
            float(part.rel_position[1]),
            float(part.rel_position[2]),
        ))

        if 0 <= int(part.parent_index) < num_parts and int(part.parent_index) != i:
            set_parent_and_local_position(node, part_nodes[int(part.parent_index)], rel_pos)
        else:
            set_parent_and_local_position(node, root, abs_pos)

    return root, part_nodes


def build_lod_meshes(
    ir,
    lod_index: int,
    part_nodes: dict[int, Any],
    material_dict: dict[int, Any],
    *,
    include_empty_parts: bool = True,
    track_bone_data: bool = False,
    mesh_bone_data: dict[str, Any] | None = None,
    hidden: bool = False,
) -> list[Any]:
    """Create one Editable_Mesh per part."""
    flat_meshes = flatten_lod(
        ir,
        lod_index,
        include_empty_parts=include_empty_parts,
        track_bone_data=track_bone_data,
        preserve_source_indexing=True,
    )
    rt = _rt()

    mesh_objects: list[Any] = []
    for fm in flat_meshes:
        parent_node = part_nodes.get(fm.part_index)
        if parent_node is None:
            continue
        mesh_obj = _build_max_mesh(rt, fm, material_dict)
        set_parent_and_local_position(mesh_obj, parent_node)
        _set_hidden(mesh_obj, hidden)
        _set_user_prop(rt, mesh_obj, "opennova_part_index", fm.part_index)
        _set_user_prop(rt, mesh_obj, "_part_index", fm.part_index)
        if mesh_bone_data is not None and track_bone_data:
            mesh_bone_data[mesh_obj.name] = fm.vertex_bone_data
        mesh_objects.append(mesh_obj)

    return mesh_objects


def create_mesh_node(
    name: str,
    vertices: Sequence[Sequence[float]],
    faces: Sequence[Sequence[int]],
    *,
    material: Any = None,
    parent: Any = None,
    location: Sequence[float] | None = None,
    hidden: bool = False,
    smoothing_groups: Sequence[int] | None = None,
) -> Any:
    """Create an Editable_Mesh from arbitrary triangles/quads/polygons."""
    rt = _rt()
    tri_faces = _triangulate_faces(faces)
    mesh = rt.mesh(
        vertices=[rt.Point3(float(v[0]), float(v[1]), float(v[2])) for v in vertices],
        faces=[rt.Point3(f[0] + 1, f[1] + 1, f[2] + 1) for f in tri_faces],
    )
    mesh.name = name
    if material is not None:
        mesh.material = material
    if parent is not None:
        set_parent_and_local_position(mesh, parent, location)
    elif location is not None:
        set_world_position(mesh, location)
    if smoothing_groups is not None:
        for fi, sg in enumerate(smoothing_groups[:len(tri_faces)]):
            rt.setFaceSmoothGroup(mesh, fi + 1, int(sg))
    elif tri_faces:
        for fi in range(len(tri_faces)):
            rt.setFaceSmoothGroup(mesh, fi + 1, 0)
    _set_hidden(mesh, hidden)
    return mesh


def _build_max_mesh(rt, fm: FlatMesh, material_dict: dict[int, Any]) -> Any:
    """Build one editable mesh node from a FlatMesh in the active scene."""
    from .materials import _max_material_id, create_multimaterial

    verts_p3 = [rt.Point3(v[0], v[1], v[2]) for v in fm.vertices]
    faces_p3 = [rt.Point3(f[0] + 1, f[1] + 1, f[2] + 1) for f in fm.faces]

    mesh = rt.mesh(vertices=verts_p3, faces=faces_p3)
    mesh.name = fm.name

    source_face_material_ids = _source_face_material_ids(fm)
    max_face_material_ids = [
        _max_material_id(gid) if len(fm.material_id_set) > 1 else 1
        for gid in source_face_material_ids
    ]
    for fi, max_mat_id in enumerate(max_face_material_ids):
        rt.setFaceMatID(mesh, fi + 1, max_mat_id)

    for fi, sg in enumerate(fm.smoothing_groups):
        rt.setFaceSmoothGroup(mesh, fi + 1, int(sg))

    _apply_vertex_normals(rt, mesh, fm)

    if fm.face_uvs0:
        _write_map_channel(rt, mesh, 1, fm.face_uvs0, len(fm.faces))
        _set_user_prop(rt, mesh, "opennova_uv0_map_verts", len(fm.face_uvs0))
        _set_user_prop(rt, mesh, "opennova_uv0_map_faces", len(fm.faces))
    if fm.face_uvs1:
        _write_map_channel(rt, mesh, 2, fm.face_uvs1, len(fm.faces))
        _set_user_prop(rt, mesh, "opennova_uv1_map_verts", len(fm.face_uvs1))
        _set_user_prop(rt, mesh, "opennova_uv1_map_faces", len(fm.faces))

    material = create_multimaterial(
        f"{fm.name}_Materials",
        fm.material_id_set,
        material_dict,
    )
    if material is not None:
        mesh.material = material

    _set_user_prop(rt, mesh, "opennova_material_ids", _csv_ints(fm.material_id_set))
    _set_user_prop(rt, mesh, "opennova_max_material_ids", _csv_ints(_max_material_id(v) for v in fm.material_id_set))
    _set_user_prop(rt, mesh, "opennova_source_face_material_ids", _csv_ints(source_face_material_ids))
    _set_user_prop(rt, mesh, "opennova_face_material_ids", _csv_ints(max_face_material_ids))
    _set_user_prop(
        rt,
        mesh,
        "opennova_face_material_id_mode",
        "global" if len(fm.material_id_set) > 1 else "single",
    )
    return mesh


def _apply_vertex_normals(rt, mesh: Any, fm: FlatMesh) -> None:
    source_normals = _source_vertex_normals(fm)
    for vi, normal in enumerate(source_normals, start=1):
        if normal is None:
            continue
        try:
            rt.setNormal(mesh, vi, rt.Point3(normal[0], normal[1], normal[2]))
        except Exception:
            pass
    _apply_explicit_vertex_normals(rt, mesh, fm, source_normals)
    try:
        rt.update(mesh)
    except Exception:
        pass


def _apply_explicit_vertex_normals(
    rt,
    mesh: Any,
    fm: FlatMesh,
    source_normals: list[tuple[float, float, float] | None],
) -> None:
    if not any(normal is not None for normal in source_normals):
        return
    try:
        normal_mod = rt.Edit_Normals()
        rt.addModifier(mesh, normal_mod)
    except Exception:
        return
    try:
        rt.select(mesh)
        rt.execute("max modify mode")
        rt.modPanel.setCurrentObject(normal_mod)
        rt.update(mesh)
    except Exception:
        pass
    try:
        normal_mod.MakeExplicit(node=mesh)
    except Exception:
        pass
    for fi, face in enumerate(fm.faces, start=1):
        for corner, vertex_index in enumerate(face, start=1):
            vi = int(vertex_index)
            if vi < 0 or vi >= len(source_normals):
                continue
            normal = source_normals[vi]
            if normal is None:
                continue
            try:
                normal_id = int(normal_mod.GetNormalID(fi, corner, node=mesh))
            except Exception:
                continue
            try:
                normal_mod.SetNormal(normal_id, rt.Point3(normal[0], normal[1], normal[2]), node=mesh)
            except Exception:
                pass
            try:
                normal_mod.SetNormalExplicit(normal_id, explicit=True, node=mesh)
            except Exception:
                pass
            try:
                normal_mod.SetFaceNormalSpecified(fi, corner, specified=True, node=mesh)
            except Exception:
                pass


def _source_vertex_normals(fm: FlatMesh) -> list[tuple[float, float, float] | None]:
    normals: list[tuple[float, float, float] | None] = [None] * len(fm.vertices)
    if not fm.face_normals or len(fm.face_normals) < len(fm.faces) * 3:
        return normals
    for fi, face in enumerate(fm.faces):
        for ci, vertex_index in enumerate(face):
            vi = int(vertex_index)
            if vi < 0 or vi >= len(normals) or normals[vi] is not None:
                continue
            n = fm.face_normals[fi * 3 + ci]
            normals[vi] = (float(n[0]), float(n[1]), float(n[2]))
    return normals


def _source_face_material_ids(fm: FlatMesh) -> list[int]:
    fallback = int(fm.material_id_set[0]) if fm.material_id_set else 0
    out: list[int] = []
    for fi in range(len(fm.faces)):
        if fi < len(fm.face_material_ids):
            out.append(int(fm.face_material_ids[fi]))
        else:
            out.append(fallback)
    return out


def _write_map_channel(rt, mesh, channel: int, uvs, face_count: int) -> None:
    _ensure_map_channel(rt, mesh, channel)
    rt.meshop.setNumMapVerts(mesh, channel, len(uvs))
    for ti, uv in enumerate(uvs):
        rt.meshop.setMapVert(mesh, channel, ti + 1, rt.Point3(uv[0], uv[1], 0.0))
    rt.meshop.setNumMapFaces(mesh, channel, face_count)
    for fi in range(face_count):
        base = fi * 3
        rt.meshop.setMapFace(
            mesh,
            channel,
            fi + 1,
            rt.Point3(base + 1, base + 2, base + 3),
        )


def _ensure_map_channel(rt, mesh, channel: int) -> None:
    """Enable a mesh map channel before assigning map verts/faces."""
    if int(rt.meshop.getNumMaps(mesh)) <= channel:
        rt.meshop.setNumMaps(mesh, channel + 1, keep=True)
    rt.meshop.setMapSupport(mesh, channel, True)


def _triangulate_faces(faces: Sequence[Sequence[int]]) -> list[tuple[int, int, int]]:
    tri_faces: list[tuple[int, int, int]] = []
    for face in faces:
        if len(face) < 3:
            continue
        for i in range(1, len(face) - 1):
            tri_faces.append((int(face[0]), int(face[i]), int(face[i + 1])))
    return tri_faces


def _set_hidden(node, hidden: bool) -> None:
    if not hidden:
        return
    try:
        node.isHidden = True
    except Exception:
        try:
            _rt().hide(node)
        except Exception:
            pass


def _set_user_prop(rt, obj, key: str, value) -> None:
    try:
        rt.setUserProp(obj, key, value)
    except Exception:
        pass


def _csv_ints(values: Iterable[int]) -> str:
    return ",".join(str(int(v)) for v in values)


def set_parent_and_local_position(
    node,
    parent,
    local_position: Sequence[float] | None = None,
) -> None:
    """Parent ``node`` and place it as Blender would with ``obj.location``.

    Max preserves world transforms when assigning ``node.parent``.  Blender
    importer code sets local coordinates before parenting, so Max nodes need an
    explicit world position after parenting to produce the same local result.
    """
    if parent is not None:
        node.parent = parent
    world = parented_local_position_to_world(parent, local_position)
    set_world_position(node, world)


def parented_local_position_to_world(
    parent,
    local_position: Sequence[float] | None = None,
) -> tuple[float, float, float]:
    local = _vec3(local_position)
    if parent is None:
        return local
    return vector_add(world_position(parent), local)


def set_parent_and_world_position(node, parent, position: Sequence[float]) -> None:
    if parent is not None:
        node.parent = parent
    set_world_position(node, position)


def set_world_position(node, position: Sequence[float]) -> None:
    rt = _rt()
    p = rt.Point3(float(position[0]), float(position[1]), float(position[2]))
    try:
        node.position = p
        return
    except Exception:
        pass
    try:
        tm = node.transform
        tm.position = p
        node.transform = tm
    except Exception:
        pass


def world_position(node) -> tuple[float, float, float]:
    """Best-effort world position for Dummy/mesh nodes."""
    try:
        p = node.transform.position
    except Exception:
        p = node.position
    return (float(p.x), float(p.y), float(p.z))


def vector_sub(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (float(a[0]) - float(b[0]), float(a[1]) - float(b[1]), float(a[2]) - float(b[2]))


def vector_add(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    return (float(a[0]) + float(b[0]), float(a[1]) + float(b[1]), float(a[2]) + float(b[2]))


def vector_scale(a: Sequence[float], scalar: float) -> tuple[float, float, float]:
    return (float(a[0]) * scalar, float(a[1]) * scalar, float(a[2]) * scalar)


def _vec3(value: Sequence[float] | None) -> tuple[float, float, float]:
    if value is None:
        return (0.0, 0.0, 0.0)
    return (float(value[0]), float(value[1]), float(value[2]))


def point3_tuple(p) -> tuple[float, float, float]:
    return (float(p[0]), float(p[1]), float(p[2]))
