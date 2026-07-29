"""Armature building (ported from adm_scene_builder.h): the synthetic
per-part armature, the .bad-skeleton armature, and mesh-to-armature binding.

Mixin part of BlenderSceneBuilder -- instance state lives in core.__init__.
Cross-mixin contracts: the .bad path calls AnimationMixin._normalize_quat
through self, and bind_meshes_to_armature consumes (then clears) the
_mesh_bone_data written by the meshes LOD0 pass.
"""

from __future__ import annotations

import math

import bpy
from mathutils import Vector, Matrix, Quaternion

from blender.math_utils import (
    render_space,
    bone_space,
    conjugate_y_to_z,
    orthonormalize,
)

from .helpers import _set_object_mode


class ArmatureMixin:
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
