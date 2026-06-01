from __future__ import annotations

import enum
import math
import os

import bpy
from mathutils import Vector, Matrix, Quaternion

from blender.math_utils import (
    render_space,
    bone_space,
    collision_space,
    conjugate_y_to_z,
    orthonormalize,
    compute_polyhedron_controlled,
)
from blender.mesh_primitives import create_cube_mesh, create_direction_arrow_mesh


class OcclusionType(enum.IntEnum):
    OB = 0
    OS = 1
    OP = 2
    OP2 = 3

    @property
    def prefix(self) -> str:
        return _OCCLUSION_PREFIXES[self]

    @property
    def color(self) -> tuple[float, float, float]:
        return _OCCLUSION_COLORS[self]


_OCCLUSION_PREFIXES = {
    OcclusionType.OB: "OB",
    OcclusionType.OS: "OS",
    OcclusionType.OP: "OP",
    OcclusionType.OP2: "OP",
}

_OCCLUSION_COLORS = {
    OcclusionType.OB: (1.0, 0.2, 0.2),
    OcclusionType.OS: (0.2, 0.6, 1.0),
    OcclusionType.OP: (1.0, 0.8, 0.2),
    OcclusionType.OP2: (1.0, 0.0, 0.6),
}


def _mtrx_to_center_rotation(mat_data):
    """Convert MTRX 4x4 matrix → Blender 3x3 rotation for a center point.

    Inverts build_matrix_from_axis (export_3di.cpp) + ASE parser swizzle chain.
    Returns a 4x4 Matrix or None if the matrix contains NaN (zero-axis sentinel).
    """
    m = [mat_data[i] for i in range(16)]
    if any(math.isnan(v) for v in m):
        return None
    ax0 = (m[10], -m[2],  m[6])
    ax1 = (-m[8],  m[0], -m[4])
    ax2 = (m[9],  -m[1],  m[5])
    return Matrix([
        [ ax1[1], -ax0[1],  ax2[1]],
        [-ax1[0],  ax0[0], -ax2[0]],
        [ ax1[2], -ax0[2],  ax2[2]],
    ]).to_4x4()


def _compute_smoothing_groups(faces, normals, epsilon=1e-4):
    """Compute smoothing group bitmasks from per-loop normals.

    Compares normals at shared edges to classify them as smooth or sharp,
    then flood-fills connected smooth regions.  Uses greedy graph coloring
    on the component adjacency graph so that adjacent components never
    share a smoothing-group bit (prevents false smoothing from bit
    collisions).
    """
    from collections import defaultdict, deque

    # Build edge → face adjacency
    edge_faces = defaultdict(list)
    for fi, (v0, v1, v2) in enumerate(faces):
        for a, b in ((v0, v1), (v1, v2), (v2, v0)):
            edge_faces[(min(a, b), max(a, b))].append(fi)

    # Classify edges as smooth or sharp by comparing per-loop normals
    eps_sq = epsilon * epsilon
    smooth_adj = defaultdict(set)
    for (ea, eb), flist in edge_faces.items():
        if len(flist) != 2:
            continue
        fi_a, fi_b = flist[0], flist[1]
        fa, fb = faces[fi_a], faces[fi_b]
        smooth = True
        for sv in (ea, eb):
            na = normals[fi_a * 3 + list(fa).index(sv)]
            nb = normals[fi_b * 3 + list(fb).index(sv)]
            dx = na[0] - nb[0]
            dy = na[1] - nb[1]
            dz = na[2] - nb[2]
            if dx * dx + dy * dy + dz * dz > eps_sq:
                smooth = False
                break
        if smooth:
            smooth_adj[fi_a].add(fi_b)
            smooth_adj[fi_b].add(fi_a)

    # Flood-fill smooth-connected components
    comp = [-1] * len(faces)
    cid = 0
    for fi in range(len(faces)):
        if comp[fi] >= 0:
            continue
        queue = deque([fi])
        while queue:
            f = queue.popleft()
            if comp[f] >= 0:
                continue
            comp[f] = cid
            for adj in smooth_adj.get(f, ()):
                if comp[adj] < 0:
                    queue.append(adj)
        cid += 1

    # Detect flat components: all per-loop normals within the component
    # are identical.  Flat components get SG=0 (no smoothing) because the
    # original model likely used SG=0 for co-planar faces — smoothing has
    # no effect on normals there, but SG=0 gives per-face tangent/bitangent
    # in compute_smoothed_vectors, which matters for vertex dedup.
    comp_varies = [False] * cid
    comp_ref = [None] * cid
    for fi in range(len(faces)):
        c = comp[fi]
        if c < 0 or comp_varies[c]:
            continue
        for j in range(3):
            n = normals[fi * 3 + j]
            if comp_ref[c] is None:
                comp_ref[c] = n
            else:
                ref = comp_ref[c]
                dx = n[0] - ref[0]
                dy = n[1] - ref[1]
                dz = n[2] - ref[2]
                if dx * dx + dy * dy + dz * dz > eps_sq:
                    comp_varies[c] = True
                    break
    flat_comps = set(c for c in range(cid) if not comp_varies[c])

    # Build component adjacency graph (sharp edges between components)
    comp_adj = defaultdict(set)
    for (ea, eb), flist in edge_faces.items():
        if len(flist) != 2:
            continue
        fi_a, fi_b = flist[0], flist[1]
        ca, cb = comp[fi_a], comp[fi_b]
        if ca != cb and ca >= 0 and cb >= 0:
            comp_adj[ca].add(cb)
            comp_adj[cb].add(ca)

    # Greedy graph coloring: assign each component a bit such that no
    # two adjacent components share the same bit (up to 31 distinct bits).
    comp_color = {}
    for c in range(cid):
        if c in flat_comps:
            comp_color[c] = -1  # will map to SG=0
            continue
        used = set()
        for neighbor in comp_adj.get(c, ()):
            if neighbor in comp_color and comp_color[neighbor] >= 0:
                used.add(comp_color[neighbor])
        color = 0
        while color in used:
            color += 1
        comp_color[c] = color

    result = []
    for fi in range(len(faces)):
        c = comp[fi]
        if c < 0:
            result.append(0)
        else:
            color = comp_color.get(c, 0)
            result.append(0 if color < 0 else (1 << (color % 31)))
    return result


def _build_occlusion_name(type_code: int, parent_subobject: int,
                           connecting_subobject: int) -> str:
    display_index = parent_subobject if parent_subobject >= 0 else 0
    try:
        occ = OcclusionType(type_code)
        prefix = occ.prefix
    except ValueError:
        prefix = "OX"
    name = f"{prefix}{display_index + 1:02d}"
    if (type_code == 2 or type_code == 3) and connecting_subobject >= 0:
        name += f"-{connecting_subobject + 1:02d}"
    return name




class CollisionType(enum.IntEnum):
    CB = 1; CS = 2; CC = 3; CL = 4; CV = 5; CA = 6
    VC = 7; BB = 8; CD = 9; CT = 10; CM = 11; VK = 12
    CF = 13; LP = 14; CP = 19

    @property
    def color(self) -> tuple[float, float, float]:
        return _COLLISION_COLORS[self]


_COLLISION_COLORS = {
    CollisionType.CB: (0.0, 1.0, 0.0),
    CollisionType.CS: (0.0, 0.8, 1.0),
    CollisionType.CC: (1.0, 0.5, 0.0),
    CollisionType.CL: (1.0, 1.0, 0.0),
    CollisionType.CV: (0.5, 0.0, 1.0),
    CollisionType.CA: (0.2, 1.0, 0.5),
    CollisionType.VC: (0.6, 0.6, 0.6),
    CollisionType.BB: (1.0, 0.0, 1.0),
    CollisionType.CD: (0.8, 0.4, 0.0),
    CollisionType.CT: (1.0, 0.2, 0.2),
    CollisionType.CM: (0.4, 0.4, 1.0),
    CollisionType.VK: (1.0, 0.0, 0.0),
    CollisionType.CF: (0.3, 1.0, 0.3),
    CollisionType.LP: (1.0, 0.8, 0.2),
    CollisionType.CP: (0.7, 0.7, 1.0),
}


def _blinkbox_enabled_suffix(flags: int) -> str:
    enabled = (~flags) & 0x3E
    if enabled == 0:
        return ""
    out = ""
    if enabled & (1 << 1): out += "V"
    if enabled & (1 << 2): out += "S"
    if enabled & (1 << 3): out += "W"
    if enabled & (1 << 4): out += "L"
    if enabled & (1 << 5): out += "O"
    return out


def _format_duplicate_suffix(occurrence: int) -> str:
    if occurrence <= 1:
        return ""
    index = occurrence - 1
    out = ""
    while index > 0:
        index -= 1
        out = chr(ord('a') + (index % 26)) + out
        index //= 26
    return out


def _build_volume_name(type_code: int, flags: int, index: int, occurrence: int) -> str:
    try:
        base = CollisionType(type_code).name
    except ValueError:
        base = "CX"
    if type_code == 8:  # BB
        suffix = _blinkbox_enabled_suffix(flags)
        if suffix:
            base += suffix
    display_index = index if index >= 0 else 0
    return f"{base}{display_index + 1:02d}{_format_duplicate_suffix(occurrence)}"


def _set_object_mode(obj, mode: str) -> None:
    """Set Blender mode with an explicit Blender 5 operator context."""
    if obj is None:
        raise ValueError("Cannot change Blender mode without an object")

    view_layer = bpy.context.view_layer
    view_layer.update()
    obj.select_set(True)
    view_layer.objects.active = obj

    with bpy.context.temp_override(
        object=obj,
        active_object=obj,
        selected_objects=[obj],
        selected_editable_objects=[obj],
    ):
        bpy.ops.object.mode_set(mode=mode)


def _ensure_object_mode() -> None:
    active = getattr(bpy.context, "active_object", None)
    if active and getattr(active, "mode", "OBJECT") != "OBJECT":
        _set_object_mode(active, "OBJECT")


class BlenderSceneBuilder:
    """Build a Blender scene from a ThreediModelIR + optional BadFile.

    - Part hierarchy using empties
    - Vertices localized to their part's coordinate space
    - Meshes parented to their part node
    """

    def __init__(self, ir, bad_file=None, anim_context=None,
                 resolver=None, import_collisions=True, import_occlusion=True,
                 import_lights=True):
        self.ir = ir
        self.bad_file = bad_file
        self.anim_context = anim_context
        self.resolver = resolver
        self.import_collisions = import_collisions
        self.import_occlusion = import_occlusion
        self.import_lights = import_lights

        # Runtime state
        self.part_nodes: dict[int, object] = {}   # index -> Empty object
        self.mesh_objects: list[object] = []       # mesh objects with _part_index
        self.material_dict: dict[int, object] = {}
        self.root_object = None
        self.armature_object = None
        self._bone_infos = []                      # populated by build_armature_from_bad
        self._mesh_bone_data = {}                  # mesh_obj.name -> per-vertex bone data
        self._world_rot_corrections = []           # BAD world rot -> Blender rest world rot
        self._rest_local_quats = []                # Blender local rest quaternions
        self._rest_local_inv_mats = []             # Blender local rest inverse 3x3 matrices

    def build_basic_scene(self, name: str):
        _ensure_object_mode()

        self.build_part_hierarchy(name)
        mesh_objects = self.create_basic_meshes(name)
        self._build_additional_lods(name)
        self.create_scene_markers()
        self.create_user_points()
        if self.import_occlusion:
            self.create_occlusion_visualization()
        if self.import_collisions:
            self.create_collision_visualization()
        if self.import_lights:
            self.create_scene_lights()

        # Build armature, bind meshes, and build animations if BAD + anim context
        if self.bad_file and self.anim_context:
            try:
                self.build_armature_from_bad(self.bad_file, name)
                self.bind_meshes_to_armature()
                self.build_animations_from_context(self.anim_context)
            except Exception as e:
                import traceback
                traceback.print_exc()
                print(f"Warning: failed to build animations: {e}")
        elif self.anim_context and self._is_lw_animation_context(self.anim_context) and int(self.ir.mesh_type) == 3:
            try:
                self.build_armature_from_parts(name)
                self.bind_meshes_to_armature()
                self.build_lw_animations_from_context(self.anim_context)
            except Exception as e:
                import traceback
                traceback.print_exc()
                print(f"Warning: failed to build LW animations: {e}")
        elif int(self.ir.mesh_type) == 3:
            # Preserve skin weights for static skinned models that have no BAD.
            # This keeps ASE MESH_WEIGHTS data so OED re-export doesn't collapse
            # all vertices onto subobject 0.
            try:
                self.build_armature_from_parts(name)
                self.bind_meshes_to_armature()
            except Exception as e:
                import traceback
                traceback.print_exc()
                print(f"Warning: failed to build synthetic armature: {e}")

        if not mesh_objects:
            print(f"No meshes created for {name}")
            return None

        if self.root_object:
            self.root_object.select_set(True)
            bpy.context.view_layer.objects.active = self.root_object
        for obj in mesh_objects:
            obj.select_set(True)

        print(f"Imported {len(self.part_nodes)} parts + {len(mesh_objects)} meshes for {name}")
        return mesh_objects

    # Armature building (ported from adm_scene_builder.h)

    def build_armature_from_parts(self, name: str):
        """Build a minimal BN## armature from part hierarchy for skinned meshes.

        This path is used when the source model has skin weights but no BAD
        animation payload.  It preserves vertex-weight export without requiring
        gameplay animation data.
        """
        if self.ir.lod_count == 0:
            return
        lod0 = self.ir.lods[0]
        part_count = int(lod0.part_count)
        if part_count <= 0:
            return

        abs_positions = []
        for i in range(part_count):
            part = lod0.parts[i]
            abs_positions.append(render_space(Vector(part.abs_position)))

        children_by_parent = [[] for _ in range(part_count)]
        parent_indices = []
        for i in range(part_count):
            pi = int(lod0.parts[i].parent_index)
            if pi < 0 or pi >= part_count or pi == i:
                pi = -1
            parent_indices.append(pi)
            if pi >= 0:
                children_by_parent[pi].append(i)

        armature_data = bpy.data.armatures.new(f"{name}_Armature")
        armature_obj = bpy.data.objects.new("Skeleton", armature_data)
        bpy.context.collection.objects.link(armature_obj)
        if self.root_object:
            armature_obj.parent = self.root_object
        self.armature_object = armature_obj

        _set_object_mode(armature_obj, "EDIT")

        edit_bones = armature_data.edit_bones
        bl_bones = []
        for i in range(part_count):
            bname = f"BN{i + 1:02d}"
            eb = edit_bones.new(bname)
            head = abs_positions[i]
            eb.head = head

            tail = None
            for child_idx in children_by_parent[i]:
                delta = abs_positions[child_idx] - head
                if delta.length > 1e-5:
                    tail = head + delta
                    break
            if tail is None:
                tail = head + Vector((0.0, 0.05, 0.0))

            eb.tail = tail
            eb.use_connect = False
            bl_bones.append(eb)

        for i, pi in enumerate(parent_indices):
            if pi >= 0:
                bl_bones[i].parent = bl_bones[pi]

        _set_object_mode(armature_obj, "OBJECT")

        self._bone_infos = []
        for i, pi in enumerate(parent_indices):
            rest_origin = (Vector(lod0.parts[i].rel_position)
                           if pi >= 0
                           else Vector(lod0.parts[i].abs_position))
            rest_origin = render_space(rest_origin)
            self._bone_infos.append((
                f"BN{i + 1:02d}",
                pi,
                rest_origin,
                Matrix.Identity(3),
                0.0,
            ))

        print(f"[ARMATURE] Created synthetic armature with {part_count} bones")

    def build_armature_from_bad(self, bad_file, name: str):
        """Build a Blender Armature from BAD bone data.

        Bone positions and rotations are converted from Y-up to Blender Z-up
        via bone_space and conjugate_y_to_z.
        """
        bone_count = bad_file.num_bones
        if bone_count == 0:
            print("[ARMATURE] No bones in BAD file")
            return

        # -- adm_build_bone_data_from_bad --
        # Each entry: (name, parent_idx, rest_origin, local_basis_3x3)
        bone_infos = []

        for i in range(bone_count):
            bone = bad_file.bones[i]
            bone_name = bone.name.decode("utf-8", errors="replace").rstrip("\x00").replace(":", "")

            parent_idx = bone.parent_index
            if parent_idx < 0 or parent_idx >= bone_count or parent_idx == i:
                parent_idx = -1

            # AdmMat3 bone_mat = adm_bad_to_mat3(bone.rotation)
            bone_rot = Matrix((
                (bone.rotation[0], bone.rotation[1], bone.rotation[2]),
                (bone.rotation[3], bone.rotation[4], bone.rotation[5]),
                (bone.rotation[6], bone.rotation[7], bone.rotation[8]),
            ))

            # BAD stores world-space rotations and parent-local position offsets.
            # rest_origin is the parent-local offset (converted to Blender Z-up).
            # The animation accumulation uses: world_pos = parent_pos + parent_rot @ rest_origin.
            # rest.basis = local rotation (bone_rot * inv(parent_rot)) for keyframe conversion.
            rest_origin = bone_space(Vector(bone.position))

            if parent_idx < 0:
                local_rot = conjugate_y_to_z(bone_rot)
            else:
                parent_bone = bad_file.bones[parent_idx]
                parent_rot = Matrix((
                    (parent_bone.rotation[0], parent_bone.rotation[1], parent_bone.rotation[2]),
                    (parent_bone.rotation[3], parent_bone.rotation[4], parent_bone.rotation[5]),
                    (parent_bone.rotation[6], parent_bone.rotation[7], parent_bone.rotation[8]),
                ))
                local_rot = conjugate_y_to_z(bone_rot @ parent_rot.inverted())

            # Orthonormalize to clean up floating-point drift
            local_rot = orthonormalize(local_rot)

            bone_infos.append((bone_name, parent_idx, rest_origin, local_rot, bone.length))

        # root_motion bone (identity transform, no parent)
        bone_infos.append(("root_motion", -1, (0, 0, 0), Matrix.Identity(3), 0.0))

        self._bone_infos = bone_infos

        # -- Compute absolute head positions and rotations --
        # BAD stores world-space rotation matrices and parent-local position offsets.
        # Use BAD world rotations directly (conjugated Y-up → Z-up) for abs_rotations.
        # Accumulate abs_positions through parent world rotations (matching animation logic):
        #   world_pos = parent_world_pos + parent_world_rot @ local_offset
        abs_positions = [None] * len(bone_infos)
        abs_rotations = [None] * len(bone_infos)

        # Pre-compute BAD world rotation matrices (conjugated to Blender Z-up).
        # BAD stores rotation matrices in transposed form (basis vectors in rows),
        # so we transpose after converting to Blender Matrix for correct operation.
        for i in range(bone_count):
            bone = bad_file.bones[i]
            bone_rot = Matrix((
                (bone.rotation[0], bone.rotation[1], bone.rotation[2]),
                (bone.rotation[3], bone.rotation[4], bone.rotation[5]),
                (bone.rotation[6], bone.rotation[7], bone.rotation[8]),
            ))
            world_rot = orthonormalize(conjugate_y_to_z(bone_rot))
            abs_rotations[i] = world_rot.transposed()
        # root_motion bone: identity
        abs_rotations[bone_count] = Matrix.Identity(3)

        # Accumulate world positions from parent-local offsets
        for i, (_bname, parent_idx, rest_origin, _local_rot, _bad_length) in enumerate(bone_infos):
            if parent_idx >= 0 and abs_positions[parent_idx] is not None:
                abs_positions[i] = abs_positions[parent_idx] + abs_rotations[parent_idx] @ Vector(rest_origin)
            else:
                abs_positions[i] = Vector(rest_origin)

        # Child lookup for tail-length estimation (use actual hierarchy spacing,
        # not BAD bone length fields).
        children_by_parent = [[] for _ in range(bone_count)]
        for child_idx in range(bone_count):
            parent_idx = bone_infos[child_idx][1]
            if 0 <= parent_idx < bone_count:
                children_by_parent[parent_idx].append(child_idx)

        # -- adm_create_skeleton_from_bone_data --
        armature_data = bpy.data.armatures.new(f"{name}_Armature")
        armature_obj = bpy.data.objects.new("Skeleton", armature_data)
        bpy.context.collection.objects.link(armature_obj)

        if self.root_object:
            armature_obj.parent = self.root_object

        self.armature_object = armature_obj

        _set_object_mode(armature_obj, "EDIT")

        edit_bones = armature_data.edit_bones
        bl_bones = []

        for i, (bname, parent_idx, rest_origin, _local_rot, _bad_length) in enumerate(bone_infos):
            eb = edit_bones.new(bname)
            eb.head = abs_positions[i]

            if i < bone_count:
                # Use the opposite Y direction from the converted BAD basis.
                # This fixes bones appearing reversed in Edit Mode.
                forward = abs_rotations[i] @ Vector((0, -1.0, 0))
                if forward.length <= 1e-8:
                    forward = Vector((0, -1.0, 0))
                forward.normalize()

                # Prefer child-head distance as display length; fallback to a small
                # constant so leaves still have visible tails.
                tail_len = 0.05
                if children_by_parent[i]:
                    parent_head = abs_positions[i]
                    max_child_dist = max(
                        (abs_positions[c] - parent_head).length
                        for c in children_by_parent[i]
                    )
                    if max_child_dist > 1e-5:
                        tail_len = max_child_dist

                tail_dir = forward * tail_len
            else:
                tail_dir = Vector((0, 0.05, 0))

            eb.tail = eb.head + tail_dir
            eb.use_connect = False
            if i < bone_count:
                eb["bad_length"] = bone_infos[i][4]
            bl_bones.append(eb)

        for i, (_bname, parent_idx, _rest_origin, _local_rot, _bad_length) in enumerate(bone_infos):
            if parent_idx >= 0 and parent_idx < len(bl_bones):
                bl_bones[i].parent = bl_bones[parent_idx]

        # Parent BAD root bones under root_motion so the skeleton follows
        # root_motion displacement.  root_motion is appended last.
        rm_bone_idx = len(bone_infos) - 1  # root_motion is last
        for i in range(bone_count):
            if bone_infos[i][1] < 0:  # BAD root bone (parent_idx == -1)
                bl_bones[i].parent = bl_bones[rm_bone_idx]

        # Align roll so the edit bone's Z axis matches the desired rest orientation.
        # This fully constrains the bone's rest rotation (head/tail sets Y, roll sets X/Z).
        for i, (_bname, _parent_idx, _rest_origin, _local_rot, _bad_length) in enumerate(bone_infos):
            z_axis = abs_rotations[i] @ Vector((0, 0, 1))
            bl_bones[i].align_roll(z_axis)

        _set_object_mode(armature_obj, "OBJECT")

        # Read back Blender's actual rest transforms per bone.
        # These may differ slightly from our computed values due to Blender's
        # internal edit bone → rest matrix conversion. Using Blender's actual
        # values ensures animation conversion is exact.
        self._rest_local_quats = []
        self._rest_local_inv_mats = []

        for i, (bname, parent_idx, rest_origin, _local_rot, _bad_length) in enumerate(bone_infos):
            pose_bone = armature_obj.pose.bones.get(bname)
            if pose_bone is None:
                self._rest_local_quats.append(Quaternion())
                self._rest_local_inv_mats.append(Matrix.Identity(3))
                continue

            bone = pose_bone.bone
            if bone.parent:
                parent_mat = bone.parent.matrix_local
                local_mat = parent_mat.inverted() @ bone.matrix_local
            else:
                local_mat = bone.matrix_local.copy()

            self._rest_local_quats.append(local_mat.to_quaternion())
            self._rest_local_inv_mats.append(local_mat.to_3x3().inverted())

        # Per-bone world-space basis correction:
        # BAD channel rotations are authored in BAD world bone axes; Blender edit-bone
        # display changes (tail direction/roll) can alter rest axes.  Map each BAD
        # world rotation into Blender rest world rotation with:
        #   R_bl_world = R_bad_world * C
        # where C = inv(R_bad_rest_world) * R_bl_rest_world.
        self._world_rot_corrections = []
        corr_errs = []
        for i, (bname, parent_idx, rest_origin, _local_rot, _bad_length) in enumerate(bone_infos):
            pose_bone = armature_obj.pose.bones.get(bname)
            if pose_bone is None:
                self._world_rot_corrections.append(Quaternion((1.0, 0.0, 0.0, 0.0)))
                continue

            bad_rest_world_q = abs_rotations[i].to_quaternion()
            bl_rest_world_q = pose_bone.bone.matrix_local.to_quaternion()
            bad_rest_world_q = self._normalize_quat(bad_rest_world_q)
            bl_rest_world_q = self._normalize_quat(bl_rest_world_q)

            corr_q = bad_rest_world_q.inverted() @ bl_rest_world_q
            corr_q = self._normalize_quat(corr_q)
            self._world_rot_corrections.append(corr_q)

            mapped_rest = bad_rest_world_q @ corr_q
            err_deg = math.degrees(
                bl_rest_world_q.rotation_difference(mapped_rest).angle
            )
            corr_errs.append(err_deg)

        if corr_errs:
            print(
                f"[ARMATURE_CORR] bones={len(corr_errs)} "
                f"rest-map err deg max/avg={max(corr_errs):.4f}/{(sum(corr_errs)/len(corr_errs)):.4f}"
            )

        print(f"[ARMATURE] Created armature with {len(bone_infos)} bones")

    # Mesh-to-armature binding

    def bind_meshes_to_armature(self):
        """Bind mesh objects to the armature via vertex groups + Armature modifier.

        For skinned meshes, each vertex has up to 4 bone influences from the
        IR's bone_indices/bone_weights remapped through the primitive's bone_table.
        For non-skinned meshes, each vertex gets weight 1.0 to its part bone.
        """
        if not self.armature_object:
            return

        bone_infos = self._bone_infos
        mesh_list = self.mesh_objects

        for mesh_obj in mesh_list:
            bone_data = self._mesh_bone_data.get(mesh_obj.name)
            if bone_data is None:
                continue

            vert_count = len(mesh_obj.data.vertices)

            # Collect all bone indices referenced by this mesh's vertices
            # and build vertex groups per bone
            bone_vgroups = {}  # skeleton_bone_idx -> vertex_group

            for vert_idx in range(min(vert_count, len(bone_data))):
                entries = bone_data[vert_idx]
                for bone_idx, weight in entries:
                    if bone_idx < 0 or bone_idx >= len(bone_infos):
                        continue
                    if weight <= 0:
                        continue

                    if bone_idx not in bone_vgroups:
                        bone_name = bone_infos[bone_idx][0]
                        # Reuse existing vertex group if already created
                        vg = mesh_obj.vertex_groups.get(bone_name)
                        if vg is None:
                            vg = mesh_obj.vertex_groups.new(name=bone_name)
                        # Some bpy builds do not allow id-properties on
                        # VertexGroup.  Keep this best-effort so binding
                        # never aborts and skin weights are still exported.
                        try:
                            vg["opennova_bone_index"] = int(bone_idx)
                        except Exception:
                            pass
                        bone_vgroups[bone_idx] = vg

                    bone_vgroups[bone_idx].add([vert_idx], weight, 'REPLACE')

            mod = mesh_obj.modifiers.new(name="Armature", type='ARMATURE')
            mod.object = self.armature_object

        # Clean up stored bone data (no longer needed, saves memory)
        self._mesh_bone_data.clear()

        print(f"[BIND] Bound {len(mesh_list)} meshes to armature")

    # Animation building (ported from adm_scene_builder.h)

    @staticmethod
    def _is_lw_animation_context(anim_context) -> bool:
        try:
            from pyopennova.lw_animation import is_lw_animation_context

            return is_lw_animation_context(anim_context)
        except Exception:
            return False

    @staticmethod
    def _quat_norm_sq(q: Quaternion) -> float:
        """Version-safe quaternion norm squared for mathutils."""
        return q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z

    def _normalize_quat(self, q: Quaternion) -> Quaternion:
        """Return normalized q, or identity if degenerate."""
        if self._quat_norm_sq(q) <= 1e-24:
            return Quaternion((1.0, 0.0, 0.0, 0.0))
        q.normalize()
        return q

    def _bad_channel_to_blender_quat(self, rq) -> Quaternion:
        """Convert BAD channel quaternion sample (xyzw) to Blender (wxyz) in Z-up."""
        q = Quaternion((rq.w, rq.x, -rq.z, rq.y))
        return self._normalize_quat(q)

    @staticmethod
    def _find_or_create_action_slot(action, armature_obj):
        """Return action slot bound to armature (Blender 4.5+ API)."""
        if not hasattr(action, "slots"):
            return None
        target_id = f"OB{armature_obj.name}"
        for slot in action.slots:
            if getattr(slot, "identifier", "") == target_id:
                return slot
        return action.slots.new(id_type='OBJECT', name=armature_obj.name)

    def _get_action_fcurves(self, action, armature_obj, slot=None):
        """Return an FCurve collection for both legacy and layered Action APIs."""
        if hasattr(action, "fcurves"):
            return action.fcurves, slot

        if not hasattr(action, "layers"):
            raise AttributeError("Action has neither fcurves nor layers")

        if slot is None:
            slot = self._find_or_create_action_slot(action, armature_obj)

        if len(action.layers) == 0:
            layer = action.layers.new("Base Layer")
        else:
            layer = action.layers[0]

        strip = None
        for s in layer.strips:
            if hasattr(s, "channelbags"):
                strip = s
                break
        if strip is None:
            strip = layer.strips.new(type='KEYFRAME')

        cb = None
        for candidate in strip.channelbags:
            cslot = getattr(candidate, "slot", None)
            if cslot is None:
                continue
            if cslot == slot:
                cb = candidate
                break
            if getattr(cslot, "identifier", "") == getattr(slot, "identifier", ""):
                cb = candidate
                break
        if cb is None:
            cb = strip.channelbags.new(slot)

        if not hasattr(cb, "fcurves"):
            raise AttributeError("Layered Action channelbag has no fcurves")

        return cb.fcurves, slot

    def build_animations_from_context(self, anim_context):
        """Build Blender Actions from AnimationContext and push to NLA tracks."""
        import traceback
        from pyopennova.bad_ffi import parse_bad, free_bad

        if not self.armature_object:
            print("[ANIM] No armature object, skipping animations")
            return

        armature_obj = self.armature_object

        # Ensure armature has animation data
        if not armature_obj.animation_data:
            armature_obj.animation_data_create()

        # Collect all animations: reset + others
        all_anims = []
        if anim_context.reset_animation:
            all_anims.append(anim_context.reset_animation)
        all_anims.extend(anim_context.animations)

        nla_frame_offset = 0
        reset_action = None
        for anim_meta in all_anims:
            try:
                bad_file = parse_bad(anim_meta.bad_filepath)
            except Exception as e:
                print(f"[ANIM] Failed to parse {anim_meta.bad_filepath}: {e}")
                continue

            try:
                action = self._build_action_from_bad(
                    bad_file, armature_obj, anim_meta.animation_name
                )
                action.use_fake_user = True
                if reset_action is None and anim_context.reset_animation:
                    reset_action = action
                if anim_meta.bad_name:
                    action["bad_name"] = anim_meta.bad_name
                action["bad_flags"] = anim_meta.flags

                # Push action to NLA track
                track = armature_obj.animation_data.nla_tracks.new()
                track.name = action.name
                strip = track.strips.new(action.name, int(nla_frame_offset + 1), action)
                strip.name = action.name
                strip.influence = 1.0

                # Blender 4.5+: bind the strip to the action's slot
                if hasattr(action, 'slots') and len(action.slots) > 0:
                    slot = self._find_or_create_action_slot(action, armature_obj)
                    if slot is not None:
                        strip.action_slot = slot

                frame_count = int(action.frame_range[1] - action.frame_range[0] + 1)
                nla_frame_offset += frame_count

                print(f"[ANIM] Created action '{action.name}' "
                      f"({frame_count} frames) -> NLA track")
            except Exception as e:
                print(f"[ANIM] Failed to build action '{anim_meta.animation_name}': {e}")
                traceback.print_exc()
            finally:
                free_bad(bad_file)

        # Expand scene frame range to cover all NLA strips
        if nla_frame_offset > 0:
            bpy.context.scene.frame_start = 1
            bpy.context.scene.frame_end = int(nla_frame_offset)

        # Set the reset action as active so the armature displays the reset pose.
        if reset_action:
            armature_obj.animation_data.action = reset_action
            if hasattr(reset_action, 'slots') and len(reset_action.slots) > 0:
                armature_obj.animation_data.action_slot = reset_action.slots[0]
        bpy.context.scene.frame_set(1)


    def build_lw_animations_from_context(self, anim_context):
        """Record LW animation inventory without applying unproven transforms.

        SAF/KSA parsing is wired, but the runtime consumer that maps frame
        tokens/angles/root values to sub-object transforms still needs RE.
        Applying guessed transforms corrupts the rest pose, so keep animation
        data as diagnostics until that mapping is proven.
        """

        if not self.armature_object:
            print("[LW_ANIM] No armature object, skipping animations")
            return

        armature_obj = self.armature_object
        if not armature_obj.animation_data:
            armature_obj.animation_data_create()

        named_clips = self._lw_named_clips(anim_context)
        armature_obj["lw_animation_status"] = "parsed_not_applied_pending_re"
        armature_obj["lw_animation_clip_count"] = len(named_clips)
        armature_obj["lw_animation_slot_count"] = len(getattr(anim_context, "clips", {}))
        armature_obj["lw_animation_warning_count"] = len(getattr(anim_context, "warnings", ()))
        if getattr(anim_context, "anm_name", ""):
            armature_obj["lw_anm_name"] = str(anim_context.anm_name)
        if getattr(anim_context, "ksa_name", ""):
            armature_obj["lw_ksa_name"] = str(anim_context.ksa_name)
        if getattr(anim_context, "aca_name", ""):
            armature_obj["lw_aca_name"] = str(anim_context.aca_name)
        print(
            "[LW_ANIM] Parsed "
            f"{len(named_clips)} movement clips / {len(getattr(anim_context, 'clips', {}))} slots; "
            "not applying transforms until SAF frame mapping is RE'd"
        )


    def _lw_named_clips(self, anim_context):
        if getattr(anim_context, "movement_clips", None):
            names = []
            used = set()
            for movement_name, clip in anim_context.movement_clips.items():
                base_name = str(movement_name)
                name = base_name
                suffix = 2
                while name in used:
                    name = f"{base_name}_{suffix}"
                    suffix += 1
                used.add(name)
                names.append((name, clip))
            return names

        return [
            (f"LW_{slot_id:03d}_{clip.source_name}", clip)
            for slot_id, clip in sorted(getattr(anim_context, "clips", {}).items())
        ]


    def _build_action_from_lw_clip(self, clip, armature_obj, action_name: str):
        frame_count = max(0, int(getattr(clip, "frame_count", 0)))
        action = bpy.data.actions.new(name=action_name)
        action["lw_slot_id"] = int(getattr(clip, "slot_id", -1))
        action["lw_source_name"] = str(getattr(clip, "source_name", ""))
        action["lw_source_type"] = str(getattr(clip, "source_type", ""))
        action["lw_loop_frame"] = int(getattr(clip, "loop_frame", 0))
        action["lw_transform_status"] = "partial_token_direct_index"

        slot = None
        if hasattr(action, 'slots'):
            slot = self._find_or_create_action_slot(action, armature_obj)
            ad = armature_obj.animation_data
            old_action = ad.action
            old_slot = getattr(ad, 'action_slot', None)
            ad.action = action
            ad.action_slot = slot

        fcurves, slot = self._get_action_fcurves(action, armature_obj, slot)
        bone_infos = self._bone_infos
        bone_count = len(bone_infos)
        frames = tuple(getattr(clip, "frames", ()))
        if frame_count == 0 or bone_count == 0 or not frames:
            action.frame_start = 1
            action.frame_end = max(frame_count, 1)
            if hasattr(action, 'slots'):
                ad = armature_obj.animation_data
                ad.action = old_action
                if old_slot is not None:
                    ad.action_slot = old_slot
            return action

        pose_bones = armature_obj.pose.bones
        for bone_idx in range(bone_count):
            pb = pose_bones.get(bone_infos[bone_idx][0])
            if pb:
                pb.rotation_mode = 'QUATERNION'

        touched = sorted({
            bone_idx
            for frame in frames
            for token, _angle in getattr(frame, "part_records", ())
            for bone_idx in [self._lw_token_to_bone_index(token, bone_count)]
            if bone_idx is not None
        })
        unresolved = sorted({
            int(token)
            for frame in frames
            for token, _angle in getattr(frame, "part_records", ())
            if self._lw_token_to_bone_index(token, bone_count) is None
        })
        if unresolved:
            action["lw_unresolved_tokens"] = ",".join(str(token) for token in unresolved[:64])
            action["lw_unresolved_token_count"] = len(unresolved)

        bone_rot_fcurves = {}
        for bone_idx in touched:
            bname = bone_infos[bone_idx][0]
            data_path_rot = f'pose.bones["{bname}"].rotation_quaternion'
            curves = (
                fcurves.new(data_path=data_path_rot, index=0),
                fcurves.new(data_path=data_path_rot, index=1),
                fcurves.new(data_path=data_path_rot, index=2),
                fcurves.new(data_path=data_path_rot, index=3),
            )
            for fc in curves:
                fc.keyframe_points.add(frame_count)
            bone_rot_fcurves[bone_idx] = curves

        for frame_idx in range(frame_count):
            frame = frames[min(frame_idx, len(frames) - 1)]
            rotations = {bone_idx: Quaternion((1, 0, 0, 0)) for bone_idx in touched}
            for token, angle in getattr(frame, "part_records", ()):
                bone_idx = self._lw_token_to_bone_index(token, bone_count)
                if bone_idx is None:
                    continue
                rotations[bone_idx] = Quaternion((0, 0, 1), self._lw_angle_to_radians(angle))

            bl_frame = frame_idx + 1
            for bone_idx, curves in bone_rot_fcurves.items():
                q = rotations.get(bone_idx, Quaternion((1, 0, 0, 0)))
                curves[0].keyframe_points[frame_idx].co = (bl_frame, q.w)
                curves[1].keyframe_points[frame_idx].co = (bl_frame, q.x)
                curves[2].keyframe_points[frame_idx].co = (bl_frame, q.y)
                curves[3].keyframe_points[frame_idx].co = (bl_frame, q.z)
                for fc in curves:
                    fc.keyframe_points[frame_idx].interpolation = 'LINEAR'

        for fc in fcurves:
            fc.update()

        action.frame_start = 1
        action.frame_end = frame_count

        if hasattr(action, 'slots'):
            ad = armature_obj.animation_data
            ad.action = old_action
            if old_slot is not None:
                ad.action_slot = old_slot

        return action


    @staticmethod
    def _lw_token_to_bone_index(token: int, bone_count: int) -> int | None:
        token = int(token) & 0xFF
        if 0 <= token < bone_count:
            return token
        return None


    @staticmethod
    def _lw_angle_to_radians(angle: int) -> float:
        return float(angle) * (2.0 * math.pi / 65536.0)


    def _build_action_from_bad(self, bad_file, armature_obj, anim_name: str):
        """Build a single Blender Action from a parsed BAD file.

        Quaternions and translations are converted from Y-up to Blender Z-up.
        Blender pose values are rest-relative (delta from rest pose).
        """
        frame_count = bad_file.frame_count if bad_file.frame_count > 0 else 0

        is_translated = (bad_file.flags & 0x02) != 0

        action = bpy.data.actions.new(name=anim_name)

        # Blender 4.5+ action slot system: create a slot bound to the armature
        # so fcurves are visible in the Action Editor and NLA.
        slot = None
        if hasattr(action, 'slots'):
            slot = self._find_or_create_action_slot(action, armature_obj)
            # Temporarily assign action+slot so fcurves get associated with the slot
            ad = armature_obj.animation_data
            old_action = ad.action
            old_slot = getattr(ad, 'action_slot', None)
            ad.action = action
            ad.action_slot = slot
            # Will be restored after fcurves are built (at end of function)

        fcurves, slot = self._get_action_fcurves(action, armature_obj, slot)

        bone_infos = self._bone_infos
        skeleton_bone_count = len(bone_infos)
        bone_count = min(bad_file.num_bones, skeleton_bone_count)

        if frame_count == 0 or bone_count == 0:
            return action

        # Set pose bones to quaternion mode
        pose_bones = armature_obj.pose.bones
        for bone_idx in range(bone_count):
            bname = bone_infos[bone_idx][0]
            pb = pose_bones.get(bname)
            if pb:
                pb.rotation_mode = 'QUATERNION'

        # Pre-create FCurves for rotation (and position if translated)
        bone_rot_fcurves = []
        bone_pos_fcurves = []

        for bone_idx in range(bone_count):
            bname = bone_infos[bone_idx][0]
            data_path_rot = f'pose.bones["{bname}"].rotation_quaternion'
            fc_w = fcurves.new(data_path=data_path_rot, index=0)
            fc_x = fcurves.new(data_path=data_path_rot, index=1)
            fc_y = fcurves.new(data_path=data_path_rot, index=2)
            fc_z = fcurves.new(data_path=data_path_rot, index=3)
            bone_rot_fcurves.append((fc_w, fc_x, fc_y, fc_z))

            if is_translated:
                data_path_pos = f'pose.bones["{bname}"].location'
                fc_px = fcurves.new(data_path=data_path_pos, index=0)
                fc_py = fcurves.new(data_path=data_path_pos, index=1)
                fc_pz = fcurves.new(data_path=data_path_pos, index=2)
                bone_pos_fcurves.append((fc_px, fc_py, fc_pz))
            else:
                bone_pos_fcurves.append(None)

        # Pre-allocate keyframe points
        for bone_idx in range(bone_count):
            for fc in bone_rot_fcurves[bone_idx]:
                fc.keyframe_points.add(frame_count)
            if bone_pos_fcurves[bone_idx]:
                for fc in bone_pos_fcurves[bone_idx]:
                    fc.keyframe_points.add(frame_count)

        # Build rest origin vectors and parent indices from bone_infos
        rest_origins = [Vector(bone_infos[i][2]) for i in range(bone_count)]
        parent_indices = [bone_infos[i][1] for i in range(bone_count)]

        # Blender rest transforms (read back from armature after edit mode)
        # Used to convert animation values from absolute-local to rest-relative
        rest_local_quats = self._rest_local_quats
        rest_local_inv_mats = self._rest_local_inv_mats

        # Frame-by-frame animation
        world_rots = [Quaternion((1, 0, 0, 0))] * bone_count
        world_positions = [Vector((0, 0, 0))] * bone_count
        prev_local_rots = [Quaternion((1, 0, 0, 0))] * bone_count

        for frame_idx in range(frame_count):
            # First pass: compute world transforms
            for bone_idx in range(bone_count):
                # Blender quaternion order: (w, x, y, z)
                rot_q = Quaternion((1, 0, 0, 0))
                if bone_idx < bad_file.num_channels:
                    channel = bad_file.channels[bone_idx]
                    if frame_idx < channel.frame_count:
                        rot_q = self._bad_channel_to_blender_quat(
                            channel.rotations[frame_idx]
                        )
                        if bone_idx < len(self._world_rot_corrections):
                            rot_q = rot_q @ self._world_rot_corrections[bone_idx]
                            rot_q = self._normalize_quat(rot_q)

                # adm_transform_position is identity, so convert Y-up to Z-up directly
                translation = Vector((0, 0, 0))
                if is_translated:
                    stride = bad_file.num_bones
                    idx = frame_idx * stride + bone_idx
                    if idx < bad_file.num_translations:
                        tl = bad_file.translations[idx]
                        translation = Vector((tl[0], -tl[2], tl[1]))

                parent_idx = parent_indices[bone_idx]

                if parent_idx < 0 or parent_idx >= bone_count:
                    # Root bone: world_pos = rest.origin + translation
                    world_rots[bone_idx] = rot_q
                    world_pos = rest_origins[bone_idx] + translation
                    world_positions[bone_idx] = world_pos
                else:
                    # Child: world_pos = parent_world.xform(rest.origin) + translation
                    world_rots[bone_idx] = rot_q
                    parent_rot_mat = world_rots[parent_idx].to_matrix()
                    world_pos = world_positions[parent_idx] + parent_rot_mat @ rest_origins[bone_idx] + translation
                    world_positions[bone_idx] = world_pos

            # Second pass: compute local transforms and insert keyframes.
            # Blender pose values are relative to the bone's rest pose.
            # Convert from absolute-local to rest-relative:
            #   pose_rot = rest_rot.inverted() @ local_rot
            #   pose_pos = rest_rot_3x3.inverted() @ (local_pos - rest_origin)
            bl_frame = frame_idx + 1

            for bone_idx in range(bone_count):
                parent_idx = parent_indices[bone_idx]

                # local_rot = parent_world_rot.inverse() * world_rot (or world_rot for roots)
                if parent_idx < 0 or parent_idx >= bone_count:
                    local_rot = world_rots[bone_idx]
                else:
                    local_rot = world_rots[parent_idx].inverted() @ world_rots[bone_idx]

                # Convert to Blender rest-relative space
                rest_quat = rest_local_quats[bone_idx]
                pose_rot = rest_quat.inverted() @ local_rot

                # Quaternion flip prevention (dot < 0 → negate)
                if frame_idx == 0:
                    prev_local_rots[bone_idx] = pose_rot
                else:
                    prev = prev_local_rots[bone_idx]
                    dot = pose_rot.dot(prev)
                    if dot < 0:
                        pose_rot = Quaternion((-pose_rot.w, -pose_rot.x,
                                                -pose_rot.y, -pose_rot.z))
                    prev_local_rots[bone_idx] = pose_rot

                # Insert rotation keyframes
                fc_w, fc_x, fc_y, fc_z = bone_rot_fcurves[bone_idx]
                fc_w.keyframe_points[frame_idx].co = (bl_frame, pose_rot.w)
                fc_x.keyframe_points[frame_idx].co = (bl_frame, pose_rot.x)
                fc_y.keyframe_points[frame_idx].co = (bl_frame, pose_rot.y)
                fc_z.keyframe_points[frame_idx].co = (bl_frame, pose_rot.z)
                fc_w.keyframe_points[frame_idx].interpolation = 'LINEAR'
                fc_x.keyframe_points[frame_idx].interpolation = 'LINEAR'
                fc_y.keyframe_points[frame_idx].interpolation = 'LINEAR'
                fc_z.keyframe_points[frame_idx].interpolation = 'LINEAR'

                # Insert position keyframes if translated
                if bone_pos_fcurves[bone_idx]:
                    if parent_idx < 0 or parent_idx >= bone_count:
                        # Root: local_t = world_transform
                        local_pos = world_positions[bone_idx]
                    else:
                        # Child: local_t = parent_world.affine_inverse() * world_transform
                        parent_rot_mat = world_rots[parent_idx].to_matrix()
                        parent_inv_rot = parent_rot_mat.inverted()
                        local_pos = parent_inv_rot @ (world_positions[bone_idx] - world_positions[parent_idx])

                    # Blender pose bone location is in rest-local space:
                    # final_pos = rest_origin + rest_rot @ pose_location
                    # So: pose_location = rest_rot_inv @ (local_pos - rest_origin)
                    delta_pos = rest_local_inv_mats[bone_idx] @ (local_pos - rest_origins[bone_idx])

                    fc_px, fc_py, fc_pz = bone_pos_fcurves[bone_idx]
                    fc_px.keyframe_points[frame_idx].co = (bl_frame, delta_pos.x)
                    fc_py.keyframe_points[frame_idx].co = (bl_frame, delta_pos.y)
                    fc_pz.keyframe_points[frame_idx].co = (bl_frame, delta_pos.z)
                    fc_px.keyframe_points[frame_idx].interpolation = 'LINEAR'
                    fc_py.keyframe_points[frame_idx].interpolation = 'LINEAR'
                    fc_pz.keyframe_points[frame_idx].interpolation = 'LINEAR'

        # Root motion from events
        # adm_transform_root_motion(x,y,z) = (-x, y, -z)
        if bad_file.num_events > 0:
            rm_bone_name = "root_motion"
            data_path_rm = f'pose.bones["{rm_bone_name}"].location'
            fc_rmx = fcurves.new(data_path=data_path_rm, index=0)
            fc_rmy = fcurves.new(data_path=data_path_rm, index=1)
            fc_rmz = fcurves.new(data_path=data_path_rm, index=2)
            # Events include a terminal duplicate (num_events = frame_count + 1).
            # Only keyframe the actual animation frames to match BN* bone keyframes.
            rm_count = min(bad_file.num_events, frame_count)
            fc_rmx.keyframe_points.add(rm_count)
            fc_rmy.keyframe_points.add(rm_count)
            fc_rmz.keyframe_points.add(rm_count)

            rm_pos = Vector((0, 0, 0))
            for i in range(rm_count):
                evt = bad_file.events[i]
                vx, vy, vz = evt.velocity[0], evt.velocity[1], evt.velocity[2]
                rm_pos = rm_pos + Vector((vx, -vz, vy))

                bl_frame = i + 1
                fc_rmx.keyframe_points[i].co = (bl_frame, rm_pos.x)
                fc_rmy.keyframe_points[i].co = (bl_frame, rm_pos.y)
                fc_rmz.keyframe_points[i].co = (bl_frame, rm_pos.z)
                fc_rmx.keyframe_points[i].interpolation = 'LINEAR'
                fc_rmy.keyframe_points[i].interpolation = 'LINEAR'
                fc_rmz.keyframe_points[i].interpolation = 'LINEAR'

        for fc in fcurves:
            fc.update()

        action.frame_start = 1
        action.frame_end = frame_count

        # Restore previous action/slot after building fcurves (Blender 4.5+ slot system)
        if hasattr(action, 'slots'):
            ad = armature_obj.animation_data
            ad.action = old_action
            if old_slot is not None:
                ad.action_slot = old_slot

        return action

    def merge_with_existing_scene(self, main_builder):
        _ensure_object_mode()
        self.root_object = main_builder.root_object
        self.part_nodes = main_builder.part_nodes
        # Share material_dict so secondary models reuse existing Blender
        # material objects instead of creating duplicates (Material_0.001 etc.).
        # This prevents material count explosion during ASE export.
        self.material_dict = main_builder.material_dict
        mesh_objects = self.create_basic_meshes("merged")

        # Bind arms meshes to the main builder's existing armature
        if main_builder.armature_object and hasattr(main_builder, '_bone_infos'):
            self.armature_object = main_builder.armature_object
            self._bone_infos = main_builder._bone_infos
            self.bind_meshes_to_armature()

        return bool(mesh_objects)

    def build_part_hierarchy(self, base_name: str):
        """Create a hierarchy of Empty objects for the part tree."""
        if self.ir.lod_count == 0:
            return

        lod0 = self.ir.lods[0]
        num_parts = int(lod0.part_count)

        root = bpy.data.objects.new(base_name, None)
        root.empty_display_type = 'PLAIN_AXES'
        root.empty_display_size = 0.1
        bpy.context.collection.objects.link(root)
        root["_lod_index"] = 0
        self.root_object = root

        for i in range(num_parts):
            part_name = f"PN{i + 1:02d}"
            part_obj = bpy.data.objects.new(part_name, None)
            part_obj.empty_display_type = 'PLAIN_AXES'
            part_obj.empty_display_size = 0.05
            bpy.context.collection.objects.link(part_obj)
            self.part_nodes[i] = part_obj

        # Parent and position parts
        for i in range(num_parts):
            part = lod0.parts[i]
            part_obj = self.part_nodes[i]

            abs_pos = render_space(Vector(part.abs_position))
            rel_pos = render_space(Vector(part.rel_position))

            parent = root
            if (part.parent_index >= 0 and
                part.parent_index < num_parts and
                part.parent_index != i):
                parent = self.part_nodes[part.parent_index]

            part_obj.parent = parent
            if parent == root:
                part_obj.location = abs_pos
            else:
                part_obj.location = rel_pos

        return root

    def create_basic_meshes(self, base_name: str) -> list:
        """Create meshes from the IR (LOD 0).

        Delegates to _create_meshes_for_lod() which creates one mesh object
        per (part, material) pair.
        """
        if self.ir.lod_count == 0:
            return []

        mesh_objects = self._create_meshes_for_lod(
            self.ir.lods[0], self.part_nodes, track_bone_data=True)
        self.mesh_objects = mesh_objects
        return mesh_objects

    def _create_meshes_for_lod(self, lod, part_nodes, track_bone_data=False):
        """Create meshes for a single LOD level.

        Creates one mesh object per (part, material) pair.  Primitives that
        share the same part and material are merged into a single mesh.
        Each resulting mesh has exactly one material slot.

        Args:
            lod: IR LOD data (self.ir.lods[N])
            part_nodes: dict mapping part index -> Blender Empty object
            track_bone_data: if True, store bone data in self._mesh_bone_data
                             (only needed for LOD 0 armature binding)
        """
        mesh_objects = []
        num_parts = int(lod.part_count)
        num_primitives = int(lod.primitive_count)

        is_skinned = (int(self.ir.mesh_type) == 3)  # THREEDI_IR_MESH_SKINNED
        bind_skinned_meshes = is_skinned and track_bone_data

        # Group primitives by part_index.
        # Creating one mesh per part (not per part+material) ensures that
        # normal smoothing crosses material boundaries within a part,
        # matching the reference tool's per-subobject smoothing behaviour.
        from collections import defaultdict, OrderedDict
        part_prims = defaultdict(list)
        for prim_idx in range(num_primitives):
            prim = lod.primitives[prim_idx]
            part_idx = int(prim.part_index)
            if part_idx < 0 or part_idx >= num_parts:
                continue
            if part_idx not in part_nodes:
                continue
            part_prims[part_idx].append(prim_idx)

        # Create one mesh per part (all materials merged)
        for part_idx in sorted(part_prims.keys()):
            prim_indices = part_prims[part_idx]
            part = lod.parts[part_idx]
            part_abs = Vector(part.abs_position)

            all_vertices = []
            all_faces = []
            all_uvs = []
            all_uvs1 = []
            all_normals = []
            all_bone_data = []
            all_face_mat_indices = []   # per-face material index
            vert_map = {}

            def _bone_entries(v, prim, is_skinned, part_idx):
                if is_skinned:
                    entries = []
                    for bi in range(4):
                        w = v.bone_weights[bi]
                        if w > 0:
                            local_idx = v.bone_indices[bi]
                            if prim.bone_table_length > 0 and local_idx < prim.bone_table_length:
                                skel_idx = prim.bone_table[local_idx]
                            else:
                                skel_idx = 0
                            entries.append((skel_idx, w))
                    if not entries:
                        entries.append((part_idx, 1.0))
                    return entries
                else:
                    return [(part_idx, 1.0)]

            def _bone_key(entries):
                """Build a stable dedup key for skinned per-vertex weights."""
                if not is_skinned:
                    return ()
                return tuple(
                    (int(bone_idx), round(float(weight), 6))
                    for bone_idx, weight in entries
                )

            def get_or_add_vert(pos, bone_data_entry, source_index=None):
                if is_skinned and source_index is not None:
                    # Preserve original vertex indexing for skinned meshes.
                    # Position-only dedup collapses distinct source vertices and
                    # breaks CDTA/COBJ parity on models like US01.
                    key = ("src", int(source_index))
                else:
                    key = (round(pos.x, 6), round(pos.y, 6), round(pos.z, 6),
                           _bone_key(bone_data_entry))
                if key in vert_map:
                    return vert_map[key]
                idx = len(all_vertices)
                all_vertices.append(pos)
                all_bone_data.append(bone_data_entry)
                vert_map[key] = idx
                return idx

            # Collect unique material indices for this part and ensure
            # materials are created.
            mat_idx_set = OrderedDict()
            for prim_idx in prim_indices:
                mi = int(lod.primitives[prim_idx].material_index)
                if mi not in mat_idx_set:
                    mat_idx_set[mi] = len(mat_idx_set)
                if mi not in self.material_dict:
                    if 0 <= mi < int(self.ir.material_count):
                        self.material_dict[mi] = self._create_material(self.ir.materials[mi])

            for prim_idx in prim_indices:
                prim = lod.primitives[prim_idx]
                idx_offset = int(prim.index_offset)
                idx_count = int(prim.index_count)
                vert_offset = int(prim.vertex_offset)
                prim_mat_idx = int(prim.material_index)

                if idx_count < 3:
                    continue

                for j in range(0, idx_count, 3):
                    i0 = int(lod.indices[idx_offset + j + 0]) + vert_offset
                    i1 = int(lod.indices[idx_offset + j + 2]) + vert_offset
                    i2 = int(lod.indices[idx_offset + j + 1]) + vert_offset

                    if i0 >= int(lod.vertex_count) or i1 >= int(lod.vertex_count) or i2 >= int(lod.vertex_count):
                        continue

                    v0 = lod.vertices[i0]
                    v1 = lod.vertices[i1]
                    v2 = lod.vertices[i2]

                    if bind_skinned_meshes:
                        pos0 = render_space(Vector(v0.position))
                        pos1 = render_space(Vector(v1.position))
                        pos2 = render_space(Vector(v2.position))
                    else:
                        pos0 = render_space(Vector(v0.position) - part_abs)
                        pos1 = render_space(Vector(v1.position) - part_abs)
                        pos2 = render_space(Vector(v2.position) - part_abs)

                    bd0 = _bone_entries(v0, prim, is_skinned, part_idx)
                    bd1 = _bone_entries(v1, prim, is_skinned, part_idx)
                    bd2 = _bone_entries(v2, prim, is_skinned, part_idx)

                    vi0 = get_or_add_vert(pos0, bd0, i0)
                    vi1 = get_or_add_vert(pos1, bd1, i1)
                    vi2 = get_or_add_vert(pos2, bd2, i2)

                    # Skip degenerate faces created by vertex merging
                    if vi0 == vi1 or vi1 == vi2 or vi0 == vi2:
                        continue

                    all_faces.append((vi0, vi1, vi2))
                    all_face_mat_indices.append(prim_mat_idx)
                    # UV V-flip: Blender V=0 at bottom, game/DDS V=0 at top
                    all_uvs.extend([
                        (v0.uv0[0], 1.0 - v0.uv0[1]),
                        (v1.uv0[0], 1.0 - v1.uv0[1]),
                        (v2.uv0[0], 1.0 - v2.uv0[1]),
                    ])
                    all_normals.extend([
                        render_space(Vector(v0.normal)),
                        render_space(Vector(v1.normal)),
                        render_space(Vector(v2.normal)),
                    ])
                    all_uvs1.extend([
                        (v0.uv1[0], 1.0 - v0.uv1[1]),
                        (v1.uv1[0], 1.0 - v1.uv1[1]),
                        (v2.uv1[0], 1.0 - v2.uv1[1]),
                    ])

            if not all_vertices or not all_faces:
                continue

            mesh_name = f"{part_idx + 1:02d} Mesh0"
            mesh_data = bpy.data.meshes.new(mesh_name)

            mesh_data.from_pydata(all_vertices, [], all_faces)
            mesh_data.update()

            # Compute smoothing groups from per-loop normals so that
            # the vertex builder reproduces the correct normal splits.
            if all_normals and len(all_normals) == len(all_faces) * 3:
                sg_values = _compute_smoothing_groups(all_faces, all_normals)
            else:
                sg_values = [1] * len(all_faces)

            sg_attr = mesh_data.attributes.new('smoothing_group', 'INT', 'FACE')
            for fi in range(len(mesh_data.polygons)):
                sg_attr.data[fi].value = sg_values[fi] if fi < len(sg_values) else 1
                mesh_data.polygons[fi].use_smooth = True
            if all_normals and len(all_normals) == len(mesh_data.loops):
                mesh_data.normals_split_custom_set(all_normals)

            if all_uvs:
                uv_layer = mesh_data.uv_layers.new(name="UVMap")
                for loop_idx in range(len(mesh_data.loops)):
                    if loop_idx < len(all_uvs):
                        uv_layer.data[loop_idx].uv = all_uvs[loop_idx]

            if all_uvs1:
                uv_layer1 = mesh_data.uv_layers.new(name="UVMap_Lightmap")
                for loop_idx in range(len(mesh_data.loops)):
                    if loop_idx < len(all_uvs1):
                        uv_layer1.data[loop_idx].uv = all_uvs1[loop_idx]

            # Add all materials used by this part as mesh material slots.
            # mat_slot_map maps global mat_idx -> local slot index.
            mat_slot_map = {}
            for mi in mat_idx_set:
                slot_idx = len(mesh_data.materials)
                mat_slot_map[mi] = slot_idx
                if mi in self.material_dict:
                    mesh_data.materials.append(self.material_dict[mi])
                else:
                    mesh_data.materials.append(None)

            # Assign per-face material index
            for fi in range(len(mesh_data.polygons)):
                if fi < len(all_face_mat_indices):
                    global_mi = all_face_mat_indices[fi]
                    mesh_data.polygons[fi].material_index = mat_slot_map.get(global_mi, 0)

            mesh_obj = bpy.data.objects.new(mesh_name, mesh_data)
            bpy.context.collection.objects.link(mesh_obj)

            mesh_obj.parent = self.root_object if bind_skinned_meshes and self.root_object else part_nodes[part_idx]

            mesh_obj["_part_index"] = part_idx
            if track_bone_data:
                self._mesh_bone_data[mesh_obj.name] = all_bone_data

            mesh_objects.append(mesh_obj)

        # Create empty mesh objects for parts with no geometry.
        # The original ASE always had mesh objects for all subobjects, even
        # empty ones.  This preserves subobjectCount in ConvertToInternal.
        for part_idx in sorted(part_nodes.keys()):
            if part_idx not in part_prims:
                mesh_name = f"{part_idx + 1:02d} Mesh0"
                mesh_data = bpy.data.meshes.new(mesh_name)
                mesh_data.from_pydata([], [], [])
                mesh_data.update()
                mesh_obj = bpy.data.objects.new(mesh_name, mesh_data)
                bpy.context.collection.objects.link(mesh_obj)
                mesh_obj.parent = part_nodes[part_idx]
                mesh_obj["_part_index"] = part_idx
                mesh_objects.append(mesh_obj)

        return mesh_objects

    def _build_additional_lods(self, name: str):
        """Import LODs 1+ as separate root hierarchies, hidden by default."""
        if self.ir.lod_count <= 1:
            return

        # Build material name manifest for _material_names property
        mat_names = []
        for i in range(int(self.ir.material_count)):
            mat = self.ir.materials[i]
            shader = mat.shader_name.decode("utf-8", errors="replace").rstrip("\x00")
            if not shader:
                shader = "FF_ST_OP"
            mat_names.append(f"Material_{i}_{shader}")
        mat_names_str = ";".join(mat_names)

        # Store on LOD 0 root too
        if self.root_object:
            self.root_object["_material_names"] = mat_names_str

        for lod_idx in range(1, int(self.ir.lod_count)):
            lod = self.ir.lods[lod_idx]
            num_parts = int(lod.part_count)
            if num_parts == 0:
                continue

            # Create LOD root empty
            lod_root_name = f"{name}_LOD{lod_idx}"
            lod_root = bpy.data.objects.new(lod_root_name, None)
            lod_root.empty_display_type = 'PLAIN_AXES'
            lod_root.empty_display_size = 0.1
            bpy.context.collection.objects.link(lod_root)
            lod_root["_lod_index"] = lod_idx
            lod_root["_material_names"] = mat_names_str
            lod_root.hide_viewport = True

            # Build part hierarchy for this LOD
            lod_part_nodes = {}
            for i in range(num_parts):
                part_name = f"PN{i + 1:02d}"
                part_obj = bpy.data.objects.new(part_name, None)
                part_obj.empty_display_type = 'PLAIN_AXES'
                part_obj.empty_display_size = 0.05
                bpy.context.collection.objects.link(part_obj)
                lod_part_nodes[i] = part_obj

            for i in range(num_parts):
                part = lod.parts[i]
                part_obj = lod_part_nodes[i]

                abs_pos = render_space(Vector(part.abs_position))
                rel_pos = render_space(Vector(part.rel_position))

                parent = lod_root
                if (part.parent_index >= 0 and
                    part.parent_index < num_parts and
                    part.parent_index != i):
                    parent = lod_part_nodes[part.parent_index]

                part_obj.parent = parent
                if parent == lod_root:
                    part_obj.location = abs_pos
                else:
                    part_obj.location = rel_pos

            # Create meshes for this LOD (shared material_dict)
            lod_meshes = self._create_meshes_for_lod(
                lod, lod_part_nodes, track_bone_data=False)

            # Create center markers for each part (mirrors create_scene_markers
            # for LOD 0).  The original OED tool derives per-LOD center axes
            # from each LOD's own _XX center objects.
            for i in range(num_parts):
                if i not in lod_part_nodes:
                    continue
                center_mesh = bpy.data.meshes.new(f"_{i + 1:02d} center")
                center_obj = bpy.data.objects.new(f"_{i + 1:02d} center", center_mesh)
                create_cube_mesh(center_mesh, 0.015)
                bpy.context.collection.objects.link(center_obj)
                center_obj.parent = lod_part_nodes[i]

                # Apply rotation from MTRX if available.
                if (i < int(lod.part_animation_count) and
                        int(self.ir.matrix_count) > 0):
                    mi = lod.part_animations[i].matrix_index
                    if mi != 0xFF and mi < int(self.ir.matrix_count):
                        rot = _mtrx_to_center_rotation(self.ir.matrices[mi].m)
                        if rot is not None:
                            center_obj.matrix_local = rot
                    else:
                        # Preserve "no matrix" centers so export can emit a
                        # PANM matrix index of 0xFF.
                        center_obj["opennova_zero_axis"] = True

            # Create attach point markers (~XXx attach) for child parts
            pi_counter: dict[int, int] = {}
            for i in range(num_parts):
                if i not in lod_part_nodes:
                    continue
                part = lod.parts[i]
                pi = int(part.parent_index)
                if i == 0 or pi < 0:
                    continue
                count = pi_counter.get(pi, 0)
                pi_counter[pi] = count + 1
                suffix = chr(ord('a') + (count % 26))
                attach_name = f"~{pi + 1:02d}{suffix} attach"
                attach_mesh = bpy.data.meshes.new(attach_name)
                attach_obj = bpy.data.objects.new(attach_name, attach_mesh)
                create_cube_mesh(attach_mesh, 0.012)
                bpy.context.collection.objects.link(attach_obj)
                attach_obj.parent = lod_part_nodes[i]

            print(f"[LOD] {lod_root_name}: {num_parts} parts, "
                  f"{len(lod_meshes)} meshes")

    def _resolve_ctrl_reg(self, reg_index):
        """Resolve a control register index to its name string from the IR."""
        if reg_index < 0 or reg_index >= self.ir.control_register_count:
            return None
        if not self.ir.control_registers:
            return None
        name = self.ir.control_registers[reg_index].name
        if isinstance(name, bytes):
            name = name.decode("utf-8", errors="replace").rstrip("\x00")
        return name if name else None

    def _create_material(self, ir_mat):
        mat = bpy.data.materials.new(f"Material_{ir_mat.index}")
        mat.use_nodes = True
        mat.node_tree.nodes.clear()

        bsdf = mat.node_tree.nodes.new("ShaderNodeBsdfPrincipled")
        output = mat.node_tree.nodes.new("ShaderNodeOutputMaterial")
        mat.node_tree.links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
        bsdf.inputs["Base Color"].default_value = (0.8, 0.2, 0.2, 1.0)

        # NovaLogic's engine (D3D8 fixed-function) defaults to diffuse-only
        # with no specular contribution. The gsys_phong lookup texture provides
        # specular only when shader_type explicitly enables it. Set Blender
        # defaults to match: fully rough, no specular.
        bsdf.inputs["Roughness"].default_value = 1.0
        if "Specular IOR Level" in bsdf.inputs:
            bsdf.inputs["Specular IOR Level"].default_value = 0.0
        elif "Specular" in bsdf.inputs:
            bsdf.inputs["Specular"].default_value = 0.0

        shader = ir_mat.shader_name.decode("utf-8", errors="replace").rstrip("\x00")
        if shader:
            mat.name = f"{mat.name}_{shader}"
            mat["opennova_shader"] = shader

        # Store original name before Blender mangles duplicates with .NNN suffixes
        mat["ase_material_name"] = mat.name

        # Store all IR texture names as custom properties for round-trip export.
        # IR texture slots:
        #   1 = DIFFUSE  -> exported as MAP_DIFFUSE
        #   2 = DETAIL   -> lightmap/overlay, NOT exported (causes df4oed crash)
        #   3 = NORMAL   -> stored but not exported to ASE
        # NOTE: We do NOT export slot 2 as MAP_OPACITY because:
        # 1. These are lightmaps, not alpha masks
        # 2. MAP_OPACITY triggers a crash in df4oed's material preview (sub_403750)
        for t_idx in range(ir_mat.texture_count):
            tex = ir_mat.textures[t_idx]
            tex_name = tex.name.decode("utf-8", errors="replace").rstrip("\x00").strip()
            if not tex_name:
                continue
            if tex.slot == 1 or (t_idx == 0 and "ase_diffuse_bitmap" not in mat):
                mat["ase_diffuse_bitmap"] = tex_name
            elif tex.slot == 2:
                mat["ase_detail_bitmap"] = tex_name
            elif tex.slot == 3:
                mat["ase_normal_bitmap"] = tex_name
                mat["ase_normal_type"] = int(tex.type)  # 0=diffuse, 4=MDT, 5=TGA alpha

        # Default diffuse matching 3ds Max Standard material (0.588)
        mat.diffuse_color = (0.588, 0.588, 0.588, 1.0)

        if ir_mat.flags & 0x04:  # TWO_SIDED
            mat.use_backface_culling = False

        # Find and load diffuse texture for Blender viewport display
        tex_node = None
        diffuse_bitmap = mat.get("ase_diffuse_bitmap", "")
        if diffuse_bitmap:
            tex_path = self._resolve_texture(diffuse_bitmap)
            print(f"[TEX] mat={ir_mat.index} diffuse={diffuse_bitmap!r} -> {tex_path}")
            if tex_path:
                tex_node = mat.node_tree.nodes.new("ShaderNodeTexImage")
                tex_node.name = f"Diffuse_{diffuse_bitmap}"
                try:
                    existing = bpy.data.images.get(os.path.basename(tex_path))
                    if existing:
                        tex_node.image = existing
                    else:
                        tex_node.image = bpy.data.images.load(tex_path)
                    img = tex_node.image
                    print(f"[TEX] Loaded image: {img.name} size={img.size[0]}x{img.size[1]} channels={img.channels}")
                    mat.node_tree.links.new(tex_node.outputs["Color"], bsdf.inputs["Base Color"])

                    if ir_mat.flags & 0x01:  # ALPHA_TEST
                        mat.node_tree.links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
                        mat.blend_method = "CLIP"
                except Exception as e:
                    print(f"Failed to load texture {tex_path}: {e}")

        # Blend mode (overrides CLIP for true alpha-blend materials)
        if ir_mat.blend_mode == 1:  # ALPHA
            mat.blend_method = "BLEND"
            mat.show_transparent_back = False
            if tex_node:
                mat.node_tree.links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
        elif ir_mat.blend_mode == 2:  # ADDITIVE
            mat.blend_method = "BLEND"
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = 1.0
            if tex_node and "Emission Color" in bsdf.inputs:
                mat.node_tree.links.new(tex_node.outputs["Color"], bsdf.inputs["Emission Color"])

        mat["blend_mode"] = ir_mat.blend_mode

        # Detail/lightmap texture (slot 2): create MixRGB Multiply node.
        # This gives visual representation in Blender (diffuse * detail) and
        # an exportable node structure (exporter detects MixRGB Multiply → 2 textures).
        detail_bitmap = mat.get("ase_detail_bitmap", "")
        if detail_bitmap and tex_node:
            detail_path = self._resolve_texture(detail_bitmap)
            if detail_path:
                try:
                    detail_tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
                    detail_tex.name = f"Detail_{detail_bitmap}"
                    existing = bpy.data.images.get(os.path.basename(detail_path))
                    if existing:
                        detail_tex.image = existing
                    else:
                        detail_tex.image = bpy.data.images.load(detail_path)

                    # Create MixRGB Multiply node: diffuse * detail → Base Color
                    mix_node = mat.node_tree.nodes.new("ShaderNodeMixRGB")
                    mix_node.blend_type = 'MULTIPLY'
                    mix_node.inputs["Fac"].default_value = 1.0
                    # Disconnect current diffuse → Base Color link
                    for link in list(mat.node_tree.links):
                        if (link.to_socket == bsdf.inputs["Base Color"]
                                and link.from_node == tex_node):
                            mat.node_tree.links.remove(link)
                            break
                    # Wire: diffuse → Color1, detail → Color2, Mix → Base Color
                    mat.node_tree.links.new(tex_node.outputs["Color"], mix_node.inputs["Color1"])
                    mat.node_tree.links.new(detail_tex.outputs["Color"], mix_node.inputs["Color2"])
                    mat.node_tree.links.new(mix_node.outputs["Color"], bsdf.inputs["Base Color"])
                    print(f"[TEX] mat={ir_mat.index} detail={detail_bitmap!r} -> MixRGB Multiply")
                except Exception as e:
                    print(f"Failed to load detail texture {detail_path}: {e}")

        # Wire bump/normal map (slot 3)
        # NovaLogic's engine uses height-based bump mapping, NOT tangent-space
        # normal maps. The bump data comes from:
        #   - Type 4 (NORMAL_MDT): .mdt files — proprietary format, can't load
        #   - Type 5 (NORMAL_TGA): TGA alpha channel contains height data
        #   - Stage1:alpha: diffuse texture alpha = height map (DOT3 shaders)
        # Use ShaderNodeBump (height→normal) instead of ShaderNodeNormalMap.
        normal_bitmap = mat.get("ase_normal_bitmap", "")
        normal_type = mat.get("ase_normal_type", 0)
        bump_wired = False
        if normal_bitmap and normal_type != 4:  # Skip MDT — Blender can't load
            tex_path = self._resolve_texture(normal_bitmap)
            if tex_path:
                try:
                    normal_tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
                    normal_tex.name = f"Bump_{normal_bitmap}"
                    existing = bpy.data.images.get(os.path.basename(tex_path))
                    if existing:
                        normal_tex.image = existing
                    else:
                        normal_tex.image = bpy.data.images.load(tex_path)
                    normal_tex.image.colorspace_settings.name = "Non-Color"
                    bump_node = mat.node_tree.nodes.new("ShaderNodeBump")
                    if normal_type == 5:
                        # NORMAL_TGA: height data is in the alpha channel
                        mat.node_tree.links.new(normal_tex.outputs["Alpha"], bump_node.inputs["Height"])
                    else:
                        mat.node_tree.links.new(normal_tex.outputs["Color"], bump_node.inputs["Height"])
                    mat.node_tree.links.new(bump_node.outputs["Normal"], bsdf.inputs["Normal"])
                    bump_wired = True
                except Exception as e:
                    print(f"Failed to load bump map {tex_path}: {e}")

        # Fallback: for bump shaders with no separate bump texture,
        # the diffuse texture's alpha channel IS the height map.
        # Shader types 1-6 (DOT3, PHONGT, BUMP) all use diffuse alpha as bump.
        _bump_shaders = ("DOT3", "PHONGT", "BUMP")
        if not bump_wired and tex_node and any(s in shader for s in _bump_shaders):
            bump_node = mat.node_tree.nodes.new("ShaderNodeBump")
            mat.node_tree.links.new(tex_node.outputs["Alpha"], bump_node.inputs["Height"])
            mat.node_tree.links.new(bump_node.outputs["Normal"], bsdf.inputs["Normal"])

        # Handle EMISSIVE flag / emissive_type
        if ir_mat.flags & 0x08:
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = 1.0

        # Glass / reflection
        if ir_mat.is_glass:
            if "Transmission Weight" in bsdf.inputs:
                bsdf.inputs["Transmission Weight"].default_value = 0.5
            elif "Transmission" in bsdf.inputs:
                bsdf.inputs["Transmission"].default_value = 0.5
            if "IOR" in bsdf.inputs:
                bsdf.inputs["IOR"].default_value = 1.45
            rc = ir_mat.reflect_color
            if rc[0] != 0.0 or rc[1] != 0.0 or rc[2] != 0.0:
                mat["reflect_color"] = [rc[0], rc[1], rc[2], rc[3]]

        # Specular intensity — in the NovaLogic engine, specular is only active
        # when shader_type selects a bump+specular or phong+specular mode.
        # The gsys_phong lookup uses fixed exponents (pow 4/16/64).
        # Map specular_intensity to both Specular IOR Level and Roughness.
        if ir_mat.specular_intensity > 0:
            spec = min(ir_mat.specular_intensity / 255.0, 1.0)
            if "Specular IOR Level" in bsdf.inputs:
                bsdf.inputs["Specular IOR Level"].default_value = spec
            elif "Specular" in bsdf.inputs:
                bsdf.inputs["Specular"].default_value = spec
            # Derive roughness from specular: higher specular = lower roughness.
            # The phong LUT exponents (4-64) produce relatively tight highlights,
            # so map 0->1.0 roughness, 255->0.3 roughness.
            bsdf.inputs["Roughness"].default_value = 1.0 - spec * 0.7

        # Phong shader minimum specular for round-trip fidelity.
        # Without this, the exporter can't detect Phong (specular=0 → DOT3 path).
        _phong_shaders = ("PHONGT", "PHONGO", "BUMPPHONG", "ENVPHONG")
        if any(s in shader for s in _phong_shaders):
            spec_key = "Specular IOR Level" if "Specular IOR Level" in bsdf.inputs else "Specular"
            if spec_key in bsdf.inputs and bsdf.inputs[spec_key].default_value == 0:
                bsdf.inputs[spec_key].default_value = 0.3
            if bsdf.inputs["Roughness"].default_value >= 1.0:
                bsdf.inputs["Roughness"].default_value = 0.5

        # Luminosity — drives emission in the engine
        if ir_mat.luminosity > 0:
            lum = min(ir_mat.luminosity / 255.0, 1.0)
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = lum

        # UV tiling (apply Mapping node if tiling != 0 and != 1)
        u_tile = ir_mat.u_tiling
        v_tile = ir_mat.v_tiling
        if (u_tile != 0.0 and u_tile != 1.0) or (v_tile != 0.0 and v_tile != 1.0):
            if u_tile == 0.0:
                u_tile = 1.0
            if v_tile == 0.0:
                v_tile = 1.0
            uv_node = mat.node_tree.nodes.new("ShaderNodeUVMap")
            mapping = mat.node_tree.nodes.new("ShaderNodeMapping")
            mapping.inputs["Scale"].default_value = (u_tile, v_tile, 1.0)
            mat.node_tree.links.new(uv_node.outputs["UV"], mapping.inputs["Vector"])
            # Wire mapping output to all existing texture image nodes
            for node in mat.node_tree.nodes:
                if node.type == "TEX_IMAGE":
                    mat.node_tree.links.new(mapping.outputs["Vector"], node.inputs["Vector"])

        # Store shader animation parameters as custom properties for round-trip
        if ir_mat.u_params.style != 0:
            mat["uv_u_style"] = int(ir_mat.u_params.style)
            mat["uv_u_rate"] = ir_mat.u_params.gen_rate
            mat["uv_u_phase"] = ir_mat.u_params.phase
            mat["uv_u_start"] = ir_mat.u_params.start
            mat["uv_u_end"] = ir_mat.u_params.end
        if ir_mat.v_params.style != 0:
            mat["uv_v_style"] = int(ir_mat.v_params.style)
            mat["uv_v_rate"] = ir_mat.v_params.gen_rate
            mat["uv_v_phase"] = ir_mat.v_params.phase
            mat["uv_v_start"] = ir_mat.v_params.start
            mat["uv_v_end"] = ir_mat.v_params.end
        if ir_mat.alpha_gen.style != 0:
            mat["alpha_gen_style"] = int(ir_mat.alpha_gen.style)
            mat["alpha_gen_rate"] = ir_mat.alpha_gen.rate
            mat["alpha_gen_phase"] = ir_mat.alpha_gen.phase
            mat["alpha_gen_start"] = int(ir_mat.alpha_gen.start)
            mat["alpha_gen_end"] = int(ir_mat.alpha_gen.end)
        if ir_mat.rgb_gen.style != 0:
            mat["rgb_gen_style"] = int(ir_mat.rgb_gen.style)
            mat["rgb_gen_rate"] = ir_mat.rgb_gen.rate
            mat["rgb_gen_phase"] = ir_mat.rgb_gen.phase
            sc = ir_mat.rgb_gen.start_color
            ec = ir_mat.rgb_gen.end_color
            mat["rgb_gen_start_color"] = [sc[0], sc[1], sc[2], sc[3]]
            mat["rgb_gen_end_color"] = [ec[0], ec[1], ec[2], ec[3]]
        if ir_mat.animation.num_frames > 0:
            mat["tex_anim_frames"] = int(ir_mat.animation.num_frames)
            mat["tex_anim_type"] = int(ir_mat.animation.animation_type)
            mat["tex_anim_time"] = int(ir_mat.animation.cycle_frame_time)
        if ir_mat.alpha_threshold > 0:
            mat["alpha_threshold"] = ir_mat.alpha_threshold

        # Store emissive_type for round-trip (0=none, 2=full)
        if ir_mat.emissive_type != 0:
            mat["emissive_type"] = int(ir_mat.emissive_type)

        # Store control register names for round-trip (resolved from IR indices)
        if ir_mat.rgb_gen.style > 0x70 and ir_mat.rgb_gen.reg >= 0:
            creg = self._resolve_ctrl_reg(ir_mat.rgb_gen.reg)
            if creg:
                mat["rgb_gen_ctrlreg"] = creg
        if ir_mat.alpha_gen.style > 0x70 and ir_mat.alpha_gen.reg >= 0:
            creg = self._resolve_ctrl_reg(ir_mat.alpha_gen.reg)
            if creg:
                mat["alpha_gen_ctrlreg"] = creg
        if ir_mat.u_params.style > 0x70 and ir_mat.u_params.reg >= 0:
            creg = self._resolve_ctrl_reg(ir_mat.u_params.reg)
            if creg:
                mat["uv_u_ctrlreg"] = creg
        if ir_mat.v_params.style > 0x70 and ir_mat.v_params.reg >= 0:
            creg = self._resolve_ctrl_reg(ir_mat.v_params.reg)
            if creg:
                mat["uv_v_ctrlreg"] = creg

        return mat

    def _resolve_texture(self, texture_name: str) -> str | None:
        if not texture_name or not self.resolver:
            return None
        return self.resolver.resolve_texture(texture_name)

    def _create_marker_material(self, name: str, color: tuple):
        if name in bpy.data.materials:
            return bpy.data.materials[name]
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        mat.node_tree.nodes.clear()
        bsdf = mat.node_tree.nodes.new("ShaderNodeBsdfPrincipled")
        output = mat.node_tree.nodes.new("ShaderNodeOutputMaterial")
        mat.node_tree.links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
        bsdf.inputs["Base Color"].default_value = (*color, 1.0)
        bsdf.inputs["Roughness"].default_value = 1.0
        if "Emission Color" in bsdf.inputs:
            bsdf.inputs["Emission Color"].default_value = (*color, 1.0)
        elif "Emission" in bsdf.inputs:
            bsdf.inputs["Emission"].default_value = (*color, 1.0)
        if "Emission Strength" in bsdf.inputs:
            bsdf.inputs["Emission Strength"].default_value = 0.3
        return mat

    def create_scene_markers(self):
        """Create center and attachment point markers, parented to part nodes."""
        if self.ir.lod_count == 0:
            return
        lod0 = self.ir.lods[0]

        for i in range(int(lod0.part_count)):
            if i not in self.part_nodes:
                continue
            part_node = self.part_nodes[i]

            # Center point (child of part node, at origin = part center)
            center_mesh = bpy.data.meshes.new(f"_{i + 1:02d} center")
            center_obj = bpy.data.objects.new(f"_{i + 1:02d} center", center_mesh)
            create_cube_mesh(center_mesh, 0.015)
            center_obj.data.materials.append(self._create_marker_material("center_magenta", (1.0, 0.0, 1.0)))
            bpy.context.collection.objects.link(center_obj)
            center_obj.parent = part_node

            # Apply rotation from MTRX if available (preserves center axis
            # through the Blender → ASE → convert_internal roundtrip).
            if (i < int(lod0.part_animation_count) and
                    int(self.ir.matrix_count) > 0):
                mi = lod0.part_animations[i].matrix_index
                if mi != 0xFF and mi < int(self.ir.matrix_count):
                    rot = _mtrx_to_center_rotation(self.ir.matrices[mi].m)
                    if rot is not None:
                        center_obj.matrix_local = rot
                else:
                    # Preserve "no matrix" centers so export can emit a
                    # PANM matrix index of 0xFF.
                    center_obj["opennova_zero_axis"] = True

        # Create tilde attachment point objects (~01a, ~01b, etc.)
        num_parts = int(lod0.part_count)
        pi_counter: dict[int, int] = {}
        for i in range(num_parts):
            if i not in self.part_nodes:
                continue
            part = lod0.parts[i]
            pi = int(part.parent_index)

            # Skip root parts (no parent to attach to)
            if i == 0 or pi < 0:
                continue

            count = pi_counter.get(pi, 0)
            pi_counter[pi] = count + 1
            suffix = chr(ord('a') + (count % 26))
            attach_name = f"~{pi + 1:02d}{suffix} attach"

            attach_mesh = bpy.data.meshes.new(attach_name)
            attach_obj = bpy.data.objects.new(attach_name, attach_mesh)
            create_cube_mesh(attach_mesh, 0.012)

            bpy.context.collection.objects.link(attach_obj)
            attach_obj.parent = self.part_nodes[i]

    def create_user_points(self):
        """Create user point markers, localized to parent part space."""
        if self.ir.lod_count == 0:
            return
        lod0 = self.ir.lods[0]

        # Ensure world matrices are computed before reading parent positions.
        bpy.context.view_layer.update()

        for i in range(int(self.ir.userpoint_count)):
            up = self.ir.userpoints[i]
            name = up.name.decode("utf-8", errors="replace").rstrip("\x00")

            # Build display name from type code
            display_idx = up.part_index if up.part_index >= 0 else 0
            # Build display name: always use "UP{type_char}" so classify_name
            # recognizes it as objType=6.
            type_code = up.type_code
            if type_code and 32 <= type_code <= 126:
                prefix = f"UP{chr(type_code)}"
            else:
                prefix = "USR"
            marker_name = f"{prefix}{display_idx + 1:02d}"
            if name:
                marker_name += f" {name}"

            marker_mesh = bpy.data.meshes.new(marker_name)
            marker_obj = bpy.data.objects.new(marker_name, marker_mesh)
            create_cube_mesh(marker_mesh, 0.015)
            marker_obj.data.materials.append(self._create_marker_material("user_point_white", (1.0, 1.0, 1.0)))
            bpy.context.collection.objects.link(marker_obj)

            # Position: IR stores (3di.y, 3di.z, 3di.x) in internal/swizzled
            # space.  Map to Blender via internal→render→render_space:
            # blender = (ir[0], -ir[2], ir[1]).
            raw = Vector(up.position)
            up_pos = Vector((raw[0], -raw[2], raw[1]))
            if up.part_index >= 0 and up.part_index < len(self.part_nodes):
                parent_node = self.part_nodes[up.part_index]
                up_pos = up_pos - Vector(parent_node.matrix_world.translation)

            marker_obj.location = up_pos

            # Direction: same coordinate mapping as position.
            raw_dir = Vector(up.direction)
            z_axis = Vector((raw_dir[0], -raw_dir[2], raw_dir[1])).normalized()
            if z_axis.length_squared > 1e-6:
                # Construct orthonormal basis from Z-axis direction
                world_up = Vector((0.0, 0.0, 1.0))
                if abs(z_axis.dot(world_up)) > 0.999:
                    world_up = Vector((0.0, 1.0, 0.0))
                x_axis = world_up.cross(z_axis).normalized()
                y_axis = z_axis.cross(x_axis).normalized()
                rot_mat = Matrix((
                    (x_axis.x, y_axis.x, z_axis.x),
                    (x_axis.y, y_axis.y, z_axis.y),
                    (x_axis.z, y_axis.z, z_axis.z),
                )).to_3x3()
                marker_obj.rotation_euler = rot_mat.to_euler()

            parent_node = self.root_object
            if up.part_index >= 0 and up.part_index < len(self.part_nodes):
                parent_node = self.part_nodes[up.part_index]
            marker_obj.parent = parent_node

            marker_obj["nl_ase_name"] = marker_name  # original before Blender dedup
            marker_obj["nl_raw_position"] = tuple(up.position)
            marker_obj["nl_raw_direction"] = tuple(up.direction)
            marker_obj["nl_parent_index"] = int(up.part_index)
            marker_obj["nl_type_code"] = int(up.type_code)

    def create_scene_lights(self):
        """Create light objects from the IR's light data."""
        for i in range(int(self.ir.light_count)):
            light = self.ir.lights[i]
            light_idx = light.part_index if light.part_index >= 0 else i
            light_name = f"LP{light_idx + 1:02d}"

            light_data = bpy.data.lights.new(name=f"{light_name}_data", type="POINT")
            light_obj = bpy.data.objects.new(light_name, light_data)

            light_data.color = (light.color_start[0], light.color_start[1], light.color_start[2])
            light_data.energy = 1.0
            if light.attenuation_end > 0:
                light_data.use_custom_distance = True
                light_data.cutoff_distance = light.attenuation_end
            light_data.use_shadow = False

            bpy.context.collection.objects.link(light_obj)

            # Convert position to Blender space
            light_obj.location = render_space(Vector(light.offset))

            # Store light fields as custom properties for round-trip export
            if light.attenuation_start > 0:
                light_obj["atten_start"] = float(light.attenuation_start)
            if light.falloff > 0:
                light_obj["falloff"] = float(light.falloff)
            rot_bl = render_space(Vector((light.rotation[0],
                                        light.rotation[1],
                                        light.rotation[2])))
            light_obj["tm_row2"] = [rot_bl.x, rot_bl.y, rot_bl.z]
            if light.light_type != 0:
                light_obj["light_type"] = int(light.light_type)

            # Store light colorgen fields as custom properties for 3dp round-trip
            if light.style != 0:
                light_obj["colorgen_style"] = int(light.style)
                light_obj["colorgen_rate"] = float(light.rate) / 256.0
                light_obj["colorgen_phase"] = float(light.phase) / 256.0
            # color_end (color_start is already stored in light_data.color)
            ce = light.color_end
            if ce[0] != 0.0 or ce[1] != 0.0 or ce[2] != 0.0:
                light_obj["colorgen_end"] = [
                    min(255, int(ce[0] * 255.0)),
                    min(255, int(ce[1] * 255.0)),
                    min(255, int(ce[2] * 255.0)),
                ]
            # Control register for colorgen (style > 0x70 means phase is reg index)
            if light.style > 0x70:
                creg = self._resolve_ctrl_reg(int(light.phase))
                if creg:
                    light_obj["colorgen_ctrlreg"] = creg
            # Light disable flags
            flags = int(light.flags)
            if flags & 0x01:
                light_obj["disable_corona"] = 1
            if flags & 0x02:
                light_obj["disable_lightterrain"] = 1
            if flags & 0x04:
                light_obj["disable_lightobjects"] = 1

            parent_node = self.root_object
            if light.part_index >= 0 and light.part_index in self.part_nodes:
                parent_node = self.part_nodes[light.part_index]
            light_obj.parent = parent_node

    def create_collision_visualization(self):
        coll = self.ir.collision
        if not coll:
            return

        lod0 = self.ir.lods[0] if self.ir.lod_count > 0 else None
        part_count = int(lod0.part_count) if lod0 else 0
        name_counters = {}

        for vol_idx in range(int(coll.contents.volume_count)):
            vol = coll.contents.volumes[vol_idx]
            try:
                color = CollisionType(vol.type).color
            except ValueError:
                color = (1.0, 1.0, 1.0)

            # Determine parent node and parent position
            parent = self.root_object
            parent_pos = Vector((0, 0, 0))
            parent_set = False
            if vol.object_index >= 0 and vol.object_index < part_count:
                parent = self.part_nodes.get(vol.object_index, self.root_object)
                part = lod0.parts[vol.object_index]
                parent_pos = render_space(Vector(part.abs_position))
                parent_set = True
            if not parent_set and vol.part_index >= 0 and vol.part_index < part_count:
                parent = self.part_nodes.get(vol.part_index, self.root_object)
                part = lod0.parts[vol.part_index]
                parent_pos = render_space(Vector(part.abs_position))

            # Compute world center and local offset
            world_min = collision_space(Vector(vol.min))
            world_max = collision_space(Vector(vol.max))
            world_center = (world_min + world_max) * 0.5
            local_center = world_center - parent_pos

            # Build name: type + object_index + duplicate suffix
            name_index = vol.object_index if vol.object_index >= 0 else vol.part_index
            name_key = name_index if name_index >= 0 else -1
            counter_key = (vol.type, name_key)
            name_counters[counter_key] = name_counters.get(counter_key, 0) + 1
            occurrence = name_counters[counter_key]
            mesh_name = _build_volume_name(vol.type, vol.flags, name_index, occurrence) + "-colonly"

            # Gather planes (respecting plane_start/plane_count bounds)
            hs_list = []
            if (vol.plane_count > 0 and vol.plane_start >= 0 and
                    vol.plane_start + vol.plane_count <= int(coll.contents.plane_count)):
                for p_idx in range(vol.plane_count):
                    gp_idx = vol.plane_start + p_idx
                    plane = coll.contents.planes[gp_idx]
                    n = collision_space(Vector(plane.normal))
                    hs_list.append([n.x, n.y, n.z, plane.distance])

            min_pt = collision_space(Vector(vol.min))
            max_pt = collision_space(Vector(vol.max))

            try:
                hull_ok = False
                if hs_list:
                    vertices, faces = compute_polyhedron_controlled(hs_list, min_pt, max_pt)
                    if vertices is not None and len(vertices) >= 4:
                        vertices = [v - world_center for v in vertices]
                        self._create_collision_mesh(
                            mesh_name, vertices, faces, color,
                            parent=parent, location=local_center,
                        )
                        hull_ok = True

                if not hull_ok:
                    # Fallback box from min/max bounds
                    half = (abs((world_max.x - world_min.x) * 0.5),
                            abs((world_max.y - world_min.y) * 0.5),
                            abs((world_max.z - world_min.z) * 0.5))
                    # Skip degenerate volumes where all dimensions are zero
                    if half[0] <= 0.0 and half[1] <= 0.0 and half[2] <= 0.0:
                        continue
                    # Box vertices: iterate s0 in (-1,1), s1 in (-1,1), s2 in (-1,1)
                    # Index layout: 0=(-,-,-) 1=(-,-,+) 2=(-,+,-) 3=(-,+,+)
                    #               4=(+,-,-) 5=(+,-,+) 6=(+,+,-) 7=(+,+,+)
                    box_pts = [
                        (s0 * half[0], s1 * half[1], s2 * half[2])
                        for s0 in (-1, 1) for s1 in (-1, 1) for s2 in (-1, 1)
                    ]
                    # Hardcoded 6 quad faces, fan-triangulated
                    box_faces = [
                        [0, 2, 3, 1],  # -X face
                        [4, 5, 7, 6],  # +X face
                        [0, 1, 5, 4],  # -Y face
                        [2, 6, 7, 3],  # +Y face
                        [0, 4, 6, 2],  # -Z face
                        [1, 3, 7, 5],  # +Z face
                    ]
                    self._create_collision_mesh(
                        mesh_name, box_pts, box_faces, color,
                        parent=parent, location=local_center,
                    )
            except Exception as e:
                print(f"Failed to create collision mesh {mesh_name}: {e}")

    def _create_collision_mesh(self, name, vertices, polygon_faces, color,
                               parent=None, location=None):
        """Create a collision mesh with shared vertices.

        polygon_faces: list of polygon faces (each a list of vertex indices).
        Fan-triangulated using the shared vertex indices directly.
        """
        mesh_data = bpy.data.meshes.new(name)
        mesh_obj = bpy.data.objects.new(name, mesh_data)
        # Build shared vertex list
        verts = [tuple(vertices[i]) for i in range(len(vertices))]
        # Fan-triangulate each polygon face using shared vertex indices
        tri_faces = []
        for face in polygon_faces:
            for i in range(1, len(face) - 1):
                tri_faces.append((face[0], face[i], face[i + 1]))
        mesh_data.from_pydata(verts, [], tri_faces)
        mesh_data.update()

        # Store constant smoothing group for ASE export (matches reference)
        sg_attr = mesh_data.attributes.new('smoothing_group', 'INT', 'FACE')
        for j in range(len(tri_faces)):
            sg_attr.data[j].value = 1

        bpy.context.collection.objects.link(mesh_obj)

        if parent is not None:
            mesh_obj.parent = parent
        if location is not None:
            mesh_obj.location = location

        mat_name = f"Collision_{name}"
        mat = bpy.data.materials.get(mat_name)
        if mat is None:
            mat = bpy.data.materials.new(mat_name)
            mat.diffuse_color = (*color, 0.5)
        mesh_data.materials.append(mat)
        mesh_obj.display_type = "SOLID"
        mesh_obj.color = (*color, 0.7)

    def create_occlusion_visualization(self):
        """Create occlusion meshes from the IR's occlusion data."""
        occ = self.ir.occlusion
        if not occ or int(occ.contents.object_count) == 0:
            return

        if self.ir.lod_count == 0:
            return
        lod0 = self.ir.lods[0]

        occ_counters = {}
        for i in range(int(occ.contents.object_count)):
            occ_obj = occ.contents.objects[i]
            if occ_obj.num_vertices <= 0 or occ_obj.face_count <= 0:
                continue

            vert_start = occ_obj.vertex_start
            face_start = occ_obj.face_start
            vert_count = occ_obj.num_vertices
            face_count_val = occ_obj.face_count

            if vert_start + vert_count > int(occ.contents.vertex_count):
                continue
            if face_start + face_count_val > int(occ.contents.face_count):
                continue

            # Compute parent abs position for localization
            occ_parent = occ_obj.parent_subobject_index
            part_abs = Vector((0, 0, 0))
            if occ_parent >= 0 and occ_parent < int(lod0.part_count):
                parent_part = lod0.parts[occ_parent]
                part_abs = render_space(Vector(parent_part.abs_position))

            # Vertices: transform_position then subtract parent abs
            vertices = []
            for j in range(vert_count):
                v = occ.contents.vertices[vert_start + j]
                pos = render_space(Vector(v.position))
                vertices.append(pos - part_abs)

            # Faces: unpack raw_indices as 3 x uint8
            faces = []
            face_flags = []
            for j in range(face_count_val):
                face = occ.contents.faces[face_start + j]
                raw = face.raw_indices
                v1 = raw & 0xFF
                v2 = (raw >> 8) & 0xFF
                v3 = (raw >> 16) & 0xFF
                if v1 < len(vertices) and v2 < len(vertices) and v3 < len(vertices):
                    faces.append([v1, v2, v3])
                    face_flags.append(((raw >> 24) & 0xFF) + 1)

            if not vertices or not faces:
                continue

            base_name = _build_occlusion_name(occ_obj.type, occ_obj.parent_subobject_index, occ_obj.connecting_subobject)
            key = (occ_obj.type, int(occ_obj.parent_subobject_index), int(occ_obj.connecting_subobject))
            occ_counters[key] = occ_counters.get(key, 0) + 1
            mesh_name = base_name + _format_duplicate_suffix(occ_counters[key]) + "-occonly"
            mesh_data = bpy.data.meshes.new(mesh_name)
            bl_obj = bpy.data.objects.new(mesh_name, mesh_data)

            mesh_data.from_pydata(vertices, [], faces)
            mesh_data.update()

            # Store face_flag as smoothing group for ASE export
            sg_attr = mesh_data.attributes.new('smoothing_group', 'INT', 'FACE')
            for j, sg_val in enumerate(face_flags):
                sg_attr.data[j].value = sg_val

            bpy.context.collection.objects.link(bl_obj)

            try:
                occ_color = OcclusionType(occ_obj.type).color
            except ValueError:
                occ_color = (1.0, 0.0, 0.0)

            mat_name = f"Occlusion_{mesh_name}"
            mat = bpy.data.materials.get(mat_name)
            if mat is None:
                mat = bpy.data.materials.new(mat_name)
                mat.diffuse_color = (*occ_color, 0.25)
            mesh_data.materials.append(mat)
            bl_obj.display_type = "SOLID"
            bl_obj.color = (*occ_color, 0.25)

            parent_node = self.root_object
            if occ_parent >= 0 and occ_parent in self.part_nodes:
                parent_node = self.part_nodes[occ_parent]
            bl_obj.parent = parent_node
