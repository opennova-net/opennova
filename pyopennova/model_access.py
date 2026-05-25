"""Small accessors for direct 3DI3 ctypes models."""
from __future__ import annotations

from typing import Any


def occlusion_access(model) -> dict[str, Any] | None:
    """Return occlusion arrays/counts for a direct 3DI3 model.

    Direct 3DI3 exposes occlusion tables as model-level arrays. Some tests and
    older adapters may still provide an aggregate ``occlusion`` pointer; this
    helper keeps consumers focused on the data shape instead of the storage
    location.
    """
    if hasattr(model, "occlusion_object_count"):
        return {
            "vertices": model.occlusion_vertices,
            "vertex_count": int(model.occlusion_vertex_count),
            "faces": model.occlusion_faces,
            "face_count": int(model.occlusion_face_count),
            "objects": model.occlusion_objects,
            "object_count": int(model.occlusion_object_count),
        }

    occ = getattr(model, "occlusion", None)
    if not occ:
        return None
    return {
        "vertices": occ.contents.vertices,
        "vertex_count": int(occ.contents.vertex_count),
        "faces": occ.contents.faces,
        "face_count": int(occ.contents.face_count),
        "objects": occ.contents.objects,
        "object_count": int(occ.contents.object_count),
    }


def collision_volume_metadata(coll) -> tuple[list[int], list[int]]:
    """Return per-volume owner object indices and plane starts.

    Direct 3DI3 bounding-volume records do not repeat their owner object index
    or plane start. Those are implied by the collision object's
    ``num_bounding_volumes`` and each volume's ``plane_count``.
    """
    owners = [-1] * int(getattr(coll, "volume_count", 0))
    plane_starts = [-1] * int(getattr(coll, "volume_count", 0))
    volume_cursor = 0
    plane_cursor = 0
    for object_idx in range(int(getattr(coll, "object_count", 0))):
        obj = coll.objects[object_idx]
        volume_count = max(0, int(getattr(obj, "num_bounding_volumes", 0)))
        for _ in range(volume_count):
            if volume_cursor >= len(owners):
                break
            owners[volume_cursor] = object_idx
            plane_starts[volume_cursor] = plane_cursor
            plane_cursor += max(0, int(coll.volumes[volume_cursor].plane_count))
            volume_cursor += 1
    while volume_cursor < len(owners):
        plane_starts[volume_cursor] = plane_cursor
        plane_cursor += max(0, int(coll.volumes[volume_cursor].plane_count))
        volume_cursor += 1
    return owners, plane_starts


def pattrib_from_collision_material_flags(material_flags: int) -> int:
    """Reverse the pattrib bits encoded into CFAC material_flags."""
    flags = int(material_flags)
    pattrib = 0
    if flags & 0x100:
        pattrib |= 0x100
    if flags & 0x800:
        pattrib |= 0x2000
    if flags & 0x400:
        pattrib |= 0x1000
    return pattrib


def color_rgb(value) -> tuple[float, float, float]:
    """Return RGB floats from either normalized RGB or packed B,G,R bytes."""
    raw = [float(value[i]) for i in range(3)]
    if any(component > 1.0 for component in raw):
        return (
            max(0.0, min(1.0, raw[2] / 255.0)),
            max(0.0, min(1.0, raw[1] / 255.0)),
            max(0.0, min(1.0, raw[0] / 255.0)),
        )
    return (raw[0], raw[1], raw[2])


def light_color_rgb(light, field: str = "color_start") -> tuple[float, float, float]:
    return color_rgb(getattr(light, field))


def material_collision_attributes(model) -> dict[int, tuple[int, int]]:
    """Derive ``material_index -> (surface_type, pattrib)`` from collision faces."""
    return _material_collision_attributes_from_face_groups(model)


def _material_collision_attributes_from_face_groups(model) -> dict[int, tuple[int, int]]:
    """Fallback material collision attrs from first-seen CFAC groups."""
    coll = getattr(model, "collision", None)
    if not coll:
        return {}
    face_count = int(coll.contents.face_count)
    material_count = int(getattr(model, "material_count", 0))
    ordered: list[tuple[int, int]] = []
    seen: set[tuple[int, int]] = set()
    for idx in range(face_count):
        face = coll.contents.faces[idx]
        key = (
            int(face.poly_type),
            pattrib_from_collision_material_flags(int(face.material_flags)),
        )
        if key not in seen:
            seen.add(key)
            ordered.append(key)
            if len(ordered) >= material_count:
                break
    return {idx: value for idx, value in enumerate(ordered)}
