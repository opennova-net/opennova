"""Assertions for generated DCC ASE files accepted by original OED."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re


_QUOTED_VALUE_RE = re.compile(r'"(.*)"')


@dataclass(frozen=True)
class AseGeomObject:
    path: Path
    index: int
    name: str
    face_count: int
    material_ref: int | None
    has_bone_number: bool


def assert_oed_render_mesh_names(ase_paths: tuple[Path, ...]) -> None:
    """Validate the generated source shape original OED needs.

    OED assigns render geometry to subobjects by the leading digits in
    ``*NODE_NAME``. A material-bound mesh with a blank or non-digit name can
    parse syntactically but still contribute no render faces to the project.
    """
    for ase_path in ase_paths:
        objects = _parse_geom_objects(ase_path)
        mesh_objects = [obj for obj in objects if obj.face_count > 0]
        blank_meshes = [obj for obj in mesh_objects if not obj.name]
        assert not blank_meshes, (
            f"{ase_path} has mesh-bearing GEOMOBJECTs with blank *NODE_NAME: "
            f"{_format_objects(blank_meshes)}"
        )

        render_meshes = [
            obj
            for obj in mesh_objects
            if obj.material_ref is not None and not obj.has_bone_number
        ]
        assert render_meshes, f"{ase_path} has no material-bound render GEOMOBJECTs"

        invalid_render_names = [
            obj for obj in render_meshes if not obj.name[:1].isdigit()
        ]
        assert not invalid_render_names, (
            f"{ase_path} has render GEOMOBJECT names OED will not map to "
            f"subobjects: {_format_objects(invalid_render_names)}"
        )


def _parse_geom_objects(ase_path: Path) -> list[AseGeomObject]:
    objects: list[AseGeomObject] = []
    current: dict[str, object] | None = None
    brace_depth = 0
    object_index = -1

    for raw_line in ase_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if current is None:
            if line.startswith("*GEOMOBJECT"):
                object_index += 1
                current = {
                    "name": "",
                    "face_count": 0,
                    "material_ref": None,
                    "has_bone_number": False,
                }
                brace_depth = line.count("{") - line.count("}")
            continue

        if line.startswith("*NODE_NAME"):
            match = _QUOTED_VALUE_RE.search(line)
            current["name"] = match.group(1) if match else ""
        elif line.startswith("*NODE_BONENUMBER"):
            current["has_bone_number"] = True
        elif line.startswith("*MATERIAL_REF"):
            current["material_ref"] = int(line.split()[-1])
        elif line.startswith("*MESH_FACE"):
            current["face_count"] = int(current["face_count"]) + 1

        brace_depth += line.count("{") - line.count("}")
        if brace_depth <= 0:
            objects.append(
                AseGeomObject(
                    path=ase_path,
                    index=object_index,
                    name=str(current["name"]),
                    face_count=int(current["face_count"]),
                    material_ref=current["material_ref"],  # type: ignore[arg-type]
                    has_bone_number=bool(current["has_bone_number"]),
                )
            )
            current = None

    return objects


def _format_objects(objects: list[AseGeomObject]) -> str:
    return ", ".join(
        f"#{obj.index} name={obj.name!r} faces={obj.face_count} "
        f"mat={obj.material_ref} bone={obj.has_bone_number}"
        for obj in objects
    )
