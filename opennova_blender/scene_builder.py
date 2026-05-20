from __future__ import annotations

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
from pyopennova.mesh_utils import (
    mtrx_to_center_rotation as _mtrx_to_center_rotation_raw,
)
from pyopennova.model_access import (
    collision_volume_metadata,
    light_color_rgb,
    occlusion_access,
)
from pyopennova.scene_naming import (
    CollisionType,
    OcclusionType,
    build_occlusion_name as _build_occlusion_name,
    build_volume_name as _build_volume_name,
    format_duplicate_suffix as _format_duplicate_suffix,
)


def _mtrx_to_center_rotation(mat_data):
    """Convert MTRX 16-float buffer to a Blender 4x4 rotation matrix.

    Wraps the host-agnostic 3x3 rotation from
    ``pyopennova.mesh_utils.mtrx_to_center_rotation`` in a mathutils
    Matrix and extends to 4x4 (identity translation row). Returns
    ``None`` if the source matrix contains NaN (zero-axis sentinel).
    """
    rot = _mtrx_to_center_rotation_raw(mat_data)
    if rot is None:
        return None
    return Matrix(rot).to_4x4()


def _userpoint_local_matrix(up, translation: Vector) -> Matrix:
    """Build the exact local transform used by the host-neutral ASE writer."""
    zx = int(up.rot_y) / 65536.0
    zy = -(int(up.rot_x) / 65536.0)
    zz = int(up.rot_z) / 65536.0
    z_len = math.sqrt(zx * zx + zy * zy + zz * zz)
    if z_len > 1.0e-12:
        zx, zy, zz = zx / z_len, zy / z_len, zz / z_len
    else:
        zx, zy, zz = 0.0, 0.0, 1.0

    upx, upy, upz = 0.0, 0.0, 1.0
    if abs(zx * upx + zy * upy + zz * upz) > 0.999:
        upx, upy, upz = 0.0, 1.0, 0.0

    xx = upy * zz - upz * zy
    xy = upz * zx - upx * zz
    xz = upx * zy - upy * zx
    x_len = math.sqrt(xx * xx + xy * xy + xz * xz)
    if x_len > 1.0e-12:
        xx, xy, xz = xx / x_len, xy / x_len, xz / x_len

    yx = zy * xz - zz * xy
    yy = zz * xx - zx * xz
    yz = zx * xy - zy * xx
    y_len = math.sqrt(yx * yx + yy * yy + yz * yz)
    if y_len > 1.0e-12:
        yx, yy, yz = yx / y_len, yy / y_len, yz / y_len

    # AseExporter transposes Blender's matrix into TM rows, so store the
    # direct-writer TM rows as Blender matrix columns.
    basis = Matrix((
        (xx, yx, zx, 0.0),
        (xy, yy, zy, 0.0),
        (xz, yz, zz, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    ))
    return Matrix.Translation(translation) @ basis


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
    """Build a Blender scene from a Threedi3di3 + optional BadFile.

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
        self.mesh_objects: list[object] = []
        self.material_dict: dict[int, object] = {}
        self.root_object = None
        self.armature_object = None
        self._bone_infos = []                      # populated by build_armature_from_bad
        self._mesh_bone_data = {}                  # mesh_obj.name -> per-vertex bone data
        self._world_rot_corrections = []           # BAD world rot -> Blender rest world rot
        self._rest_local_quats = []                # Blender local rest quaternions
        self._rest_local_inv_mats = []             # Blender local rest inverse 3x3 matrices
        try:
            from pyopennova.materials import derive_uv1_tilings
            self._uv1_tilings = derive_uv1_tilings(ir)
        except Exception:
            self._uv1_tilings = {}

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
        model's bone_indices/bone_weights remapped through the primitive's bone_table.
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
                        bone_vgroups[bone_idx] = vg

                    bone_vgroups[bone_idx].add([vert_idx], weight, 'REPLACE')

            mod = mesh_obj.modifiers.new(name="Armature", type='ARMATURE')
            mod.object = self.armature_object

        # Clean up stored bone data (no longer needed, saves memory)
        self._mesh_bone_data.clear()

        print(f"[BIND] Bound {len(mesh_list)} meshes to armature")

    # Animation building (ported from adm_scene_builder.h)

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
        from pyopennova.animation_build import bad_channel_to_zup_quat

        q = Quaternion(bad_channel_to_zup_quat(rq))
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


    def _build_action_from_bad(self, bad_file, armature_obj, anim_name: str):
        """Build a single Blender Action from a parsed BAD file.

        Quaternions and translations are converted from Y-up to Blender Z-up.
        Blender pose values are rest-relative (delta from rest pose).
        """
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

        from pyopennova.animation_build import sample_bad_clip

        world_rot_corrections = []
        for q in self._world_rot_corrections[:bone_count]:
            world_rot_corrections.append((q.w, q.x, q.y, q.z))

        sampled_clip = sample_bad_clip(
            bad_file,
            bone_infos[:bone_count],
            animation_name=anim_name,
            world_rot_corrections=world_rot_corrections,
        )
        frame_count = sampled_clip.frame_count
        is_translated = (sampled_clip.flags & 0x02) != 0

        if frame_count == 0 or bone_count == 0:
            if hasattr(action, 'slots'):
                ad = armature_obj.animation_data
                ad.action = old_action
                if old_slot is not None:
                    ad.action_slot = old_slot
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

        # Build rest origin vectors from bone_infos
        rest_origins = [Vector(bone_infos[i][2]) for i in range(bone_count)]

        # Blender rest transforms (read back from armature after edit mode)
        # Used to convert animation values from absolute-local to rest-relative
        rest_local_quats = self._rest_local_quats
        rest_local_inv_mats = self._rest_local_inv_mats

        # Frame-by-frame animation
        prev_local_rots = [Quaternion((1, 0, 0, 0))] * bone_count

        for sampled_frame in sampled_clip.frames:
            frame_idx = sampled_frame.frame_index
            # Second pass: compute local transforms and insert keyframes.
            # Blender pose values are relative to the bone's rest pose.
            # Convert from absolute-local to rest-relative:
            #   pose_rot = rest_rot.inverted() @ local_rot
            #   pose_pos = rest_rot_3x3.inverted() @ (local_pos - rest_origin)
            bl_frame = sampled_frame.frame

            for bone_idx, sampled_bone in enumerate(sampled_frame.bones[:bone_count]):
                local_rot = Quaternion(sampled_bone.local_rotation)

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
                    local_pos = Vector(sampled_bone.local_position)

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

            for i, sampled_frame in enumerate(sampled_clip.frames[:rm_count]):
                rm_pos = Vector(sampled_frame.root_motion_position)
                bl_frame = sampled_frame.frame
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
        self.root_object = root

        for i in range(num_parts):
            part_name = f"PN{i + 1:02d}"
            part_obj = bpy.data.objects.new(part_name, None)
            part_obj.empty_display_type = 'PLAIN_AXES'
            part_obj.empty_display_size = 0.05
            bpy.context.collection.objects.link(part_obj)
            self.part_nodes[i] = part_obj

        abs_positions = [
            render_space(Vector(lod0.parts[i].abs_position))
            for i in range(num_parts)
        ]

        # Parent and position parts
        for i in range(num_parts):
            part = lod0.parts[i]
            part_obj = self.part_nodes[i]

            abs_pos = abs_positions[i]

            part_obj.parent = root
            part_obj.location = abs_pos

        return root

    def create_basic_meshes(self, base_name: str) -> list:
        """Create meshes from the 3DI3 model (LOD 0).

        Delegates to _create_meshes_for_lod() which creates one mesh object
        per (part, material) pair.
        """
        if self.ir.lod_count == 0:
            return []

        mesh_objects = self._create_meshes_for_lod(
            0, self.part_nodes, track_bone_data=True)
        self.mesh_objects = mesh_objects
        return mesh_objects

    def _create_meshes_for_lod(self, lod_index, part_nodes, track_bone_data=False):
        """Create meshes for a single LOD level.

        Uses ``pyopennova.mesh_build.flatten_lod`` to produce per-part
        ``FlatMesh`` data (host-agnostic strip-walking, source vertex indexing,
        UV V-flip, render_space transforms, smoothing groups), then
        wraps each FlatMesh in a ``bpy.types.Mesh``.  Material creation,
        slot mapping, parenting to ``part_nodes``, and bone-data tracking
        stay here (bpy-specific work).

        Pre-Phase-I this function did its own strip-walking (~240 LOC);
        ``opennova_max/mesh.py:build_lod_meshes`` was already using
        ``flatten_lod`` -- this brings the Blender side to parity.

        Args:
            lod_index: index into self.ir.lods
            part_nodes: dict mapping part index -> Blender Empty object
            track_bone_data: if True, store bone data in self._mesh_bone_data
                             (only needed for LOD 0 armature binding)
        """
        from collections import OrderedDict
        from pyopennova.mesh_build import flatten_lod

        is_skinned = (int(self.ir.mesh_type) == 3)

        # ``include_empty_parts=True`` reproduces the original behaviour of
        # emitting placeholder mesh objects for parts with no geometry,
        # preserving subobjectCount in ConvertToInternal.
        flat_meshes = flatten_lod(
            self.ir,
            lod_index,
            include_empty_parts=True,
            track_bone_data=track_bone_data and is_skinned,
            preserve_source_indexing=True,
        )
        cpp_face_normals: dict[int, list[tuple[float, float, float]]] = {}
        cpp_vertices: dict[int, list[tuple[float, float, float]]] = {}
        try:
            from pyopennova.flat_mesh_ffi import flat_meshes_from_3di3, free_array

            cpp_arr = flat_meshes_from_3di3(
                self.ir,
                lod_index,
                include_empty_parts=True,
                track_bone_data=track_bone_data and is_skinned,
                preserve_source_indexing=True,
            )
            try:
                for ci in range(int(cpp_arr.count)):
                    cfm = cpp_arr.meshes[ci]
                    part_index = int(cfm.part_index)
                    vertex_count = int(cfm.vertex_count)
                    if vertex_count > 0 and cfm.vertices:
                        cpp_vertices[part_index] = [
                            (
                                float(cfm.vertices[vi].x),
                                float(cfm.vertices[vi].y),
                                float(cfm.vertices[vi].z),
                            )
                            for vi in range(vertex_count)
                        ]
                    face_count = int(cfm.face_count)
                    if face_count <= 0 or not cfm.corners:
                        continue
                    normals = []
                    for corner_idx in range(face_count * 3):
                        corner = cfm.corners[corner_idx]
                        normals.append((float(corner.nx), float(corner.ny), float(corner.nz)))
                    cpp_face_normals[part_index] = normals
            finally:
                free_array(cpp_arr)
        except Exception:
            cpp_face_normals = {}
            cpp_vertices = {}

        mesh_objects = []
        bpy.context.view_layer.update()
        for fm in flat_meshes:
            part_idx = int(fm.part_index)
            if part_idx not in part_nodes:
                continue

            mesh_name = fm.name
            mesh_data = bpy.data.meshes.new(mesh_name)

            # Empty-part case: create an empty mesh object and parent only.
            if not fm.vertices or not fm.faces:
                mesh_data.from_pydata([], [], [])
                mesh_data.update()
                mesh_obj = bpy.data.objects.new(mesh_name, mesh_data)
                bpy.context.collection.objects.link(mesh_obj)
                mesh_obj.parent = part_nodes[part_idx]
                mesh_objects.append(mesh_obj)
                continue

            verts = cpp_vertices.get(part_idx)
            uses_cpp_vertices = bool(verts and len(verts) == len(fm.vertices))
            if not uses_cpp_vertices:
                verts = [tuple(v) for v in fm.vertices]
            faces = [tuple(f) for f in fm.faces]
            mesh_data.from_pydata(verts, [], faces)
            mesh_data.update()

            # Smoothing groups already computed by flatten_lod via
            # mesh_utils.compute_smoothing_groups.
            sg_attr = mesh_data.attributes.new('smoothing_group', 'INT', 'FACE')
            for fi in range(len(mesh_data.polygons)):
                sg_val = int(fm.smoothing_groups[fi]) if fi < len(fm.smoothing_groups) else 1
                sg_attr.data[fi].value = sg_val
                mesh_data.polygons[fi].use_smooth = True

            # Per-loop normals (face_normals is 3 entries per face,
            # already render_space-transformed by flatten_lod).
            face_normals = cpp_face_normals.get(part_idx, fm.face_normals)
            if face_normals and len(face_normals) == len(mesh_data.loops):
                mesh_data.normals_split_custom_set([tuple(n) for n in face_normals])
                normal_attr = mesh_data.attributes.new("ase_corner_normals", "FLOAT_VECTOR", "CORNER")
                for loop_idx, normal in enumerate(face_normals):
                    normal_attr.data[loop_idx].vector = tuple(normal)

            # UV channels (per-corner, 3 entries per face, V-flipped by flatten_lod).
            if fm.face_uvs0:
                uv_layer = mesh_data.uv_layers.new(name="UVMap")
                for loop_idx in range(len(mesh_data.loops)):
                    if loop_idx < len(fm.face_uvs0):
                        uv_layer.data[loop_idx].uv = fm.face_uvs0[loop_idx]
            if fm.face_uvs1:
                uv_layer1 = mesh_data.uv_layers.new(name="UVMap_Lightmap")
                for loop_idx in range(len(mesh_data.loops)):
                    if loop_idx < len(fm.face_uvs1):
                        uv_layer1.data[loop_idx].uv = fm.face_uvs1[loop_idx]

            # Materials: ensure each global material in fm.material_id_set
            # is created, then add as a slot on this mesh, then assign per-face.
            mat_slot_map: "OrderedDict[int, int]" = OrderedDict()
            for mi in fm.material_id_set:
                mi = int(mi)
                if mi in mat_slot_map:
                    continue
                if mi not in self.material_dict:
                    if 0 <= mi < int(self.ir.material_count):
                        self.material_dict[mi] = self._create_material(self.ir.materials[mi])
                slot_idx = len(mesh_data.materials)
                mat_slot_map[mi] = slot_idx
                mesh_data.materials.append(self.material_dict.get(mi))

            for fi in range(len(mesh_data.polygons)):
                if fi < len(fm.face_material_ids):
                    global_mi = int(fm.face_material_ids[fi])
                    mesh_data.polygons[fi].material_index = mat_slot_map.get(global_mi, 0)

            mesh_obj = bpy.data.objects.new(mesh_name, mesh_data)
            bpy.context.collection.objects.link(mesh_obj)
            mesh_obj.parent = part_nodes[part_idx]
            if uses_cpp_vertices:
                # Native flat vertices are already in render/world space.
                # Keep display/export geometry at identity while retaining PN
                # parenting for scene organization and ASE NODE_PARENT.
                mesh_obj.matrix_parent_inverse = part_nodes[part_idx].matrix_world.inverted()
                mesh_obj.matrix_world = Matrix.Identity(4)
            if track_bone_data:
                self._mesh_bone_data[mesh_obj.name] = fm.vertex_bone_data

            mesh_objects.append(mesh_obj)

        return mesh_objects

    def _build_additional_lods(self, name: str):
        """Import LODs 1+ as separate root hierarchies, hidden by default."""
        if self.ir.lod_count <= 1:
            return

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

            abs_positions = [
                render_space(Vector(lod.parts[i].abs_position))
                for i in range(num_parts)
            ]

            for i in range(num_parts):
                part = lod.parts[i]
                part_obj = lod_part_nodes[i]

                abs_pos = abs_positions[i]

                part_obj.parent = lod_root
                part_obj.location = abs_pos

            # Create meshes for this LOD (shared material_dict)
            lod_meshes = self._create_meshes_for_lod(
                lod_idx, lod_part_nodes, track_bone_data=False)

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
                            center_obj.matrix_basis = Matrix.Identity(4)
                            center_obj.matrix_parent_inverse = rot
                        else:
                            center_obj.scale = (0.0, 0.0, 0.0)
                    else:
                        center_obj.scale = (0.0, 0.0, 0.0)

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
        """Resolve a control register index to its name string from the 3DI3 model."""
        if reg_index < 0 or reg_index >= self.ir.control_register_count:
            return None
        if not self.ir.control_registers:
            return None
        name = self.ir.control_registers[reg_index].name
        if isinstance(name, bytes):
            name = name.decode("utf-8", errors="replace").rstrip("\x00")
        return name if name else None

    def _create_material(self, ir_mat):
        from pyopennova.materials import describe_material

        desc = describe_material(
            ir_mat,
            resolver=self.resolver,
            ctrl_resolver=self._resolve_ctrl_reg,
            source_format=getattr(self.ir, "source_format", None),
            uv1_tiling_override=self._uv1_tilings.get(int(getattr(ir_mat, "index", 0))),
        )
        mat = bpy.data.materials.new(f"Material_{desc.index}")
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

        mat.name = desc.name
        # Default diffuse matching 3ds Max Standard material (0.588)
        mat.diffuse_color = (0.588, 0.588, 0.588, 1.0)

        mat.use_backface_culling = not desc.two_sided

        def _assign_image(node, tex_desc, tex_path):
            if tex_path:
                existing = bpy.data.images.get(os.path.basename(tex_path))
                if existing:
                    node.image = existing
                else:
                    node.image = bpy.data.images.load(tex_path)
                return node.image

            existing = bpy.data.images.get(tex_desc.name)
            if existing:
                node.image = existing
            else:
                node.image = bpy.data.images.new(tex_desc.name, width=1, height=1, alpha=True)
                node.image.filepath = tex_desc.name
            return node.image

        # Find and load diffuse texture for Blender viewport display
        tex_node = None
        diffuse_bitmap = desc.diffuse.name
        if diffuse_bitmap:
            tex_path = desc.diffuse.path
            print(f"[TEX] mat={desc.index} diffuse={diffuse_bitmap!r} -> {tex_path}")
            tex_node = mat.node_tree.nodes.new("ShaderNodeTexImage")
            tex_node.name = f"Diffuse_{diffuse_bitmap}"
            try:
                img = _assign_image(tex_node, desc.diffuse, tex_path)
                print(f"[TEX] Image: {img.name} size={img.size[0]}x{img.size[1]} channels={img.channels}")
                mat.node_tree.links.new(tex_node.outputs["Color"], bsdf.inputs["Base Color"])

                if desc.alpha_test:
                    mat.node_tree.links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
                    mat.blend_method = "CLIP"
            except Exception as e:
                print(f"Failed to load texture {tex_path or diffuse_bitmap}: {e}")

        # Blend mode (overrides CLIP for true alpha-blend materials).
        # Drive from desc.renderer_blend so glass tags (FFP_GLASS, VS_BMTXMIRRT,
        # etc.) and FF_*_AB shaders end up on the same path. Numeric blend_mode
        # is a fallback only.
        renderer_blend = desc.renderer_blend
        is_alpha = renderer_blend == "alpha_blend" or desc.blend_mode == 1
        is_additive = renderer_blend == "additive" or desc.blend_mode == 2
        if is_alpha:
            mat.blend_method = "BLEND"
            mat.show_transparent_back = False
            if tex_node:
                mat.node_tree.links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
        elif is_additive:
            mat.blend_method = "BLEND"
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = 1.0
            if tex_node and "Emission Color" in bsdf.inputs:
                mat.node_tree.links.new(tex_node.outputs["Color"], bsdf.inputs["Emission Color"])

        # Detail/lightmap texture (slot 2): add a named detail image node.
        # Diffuse Color -> Base Color stays linked for viewport display.
        # The ASE exporter discovers detail from scene state rather than metadata.
        if desc.detail.name:
            tex_path = desc.detail.path
            try:
                detail_node = mat.node_tree.nodes.new("ShaderNodeTexImage")
                detail_node.name = f"Detail_{desc.detail.name}"
                detail_node.label = f"Detail {desc.detail.name}"
                _assign_image(detail_node, desc.detail, tex_path)
                if (abs(desc.effective_u1_tiling - 1.0) > 1e-6 or
                        abs(desc.effective_v1_tiling - 1.0) > 1e-6):
                    uv_node = mat.node_tree.nodes.new("ShaderNodeUVMap")
                    uv_node.name = "Detail_UVMap"
                    uv_node.label = "Detail UVMap"
                    mapping = mat.node_tree.nodes.new("ShaderNodeMapping")
                    mapping.name = "Detail_UV_Tiling"
                    mapping.label = "Detail UV Tiling"
                    mapping.inputs["Scale"].default_value = (
                        desc.effective_u1_tiling,
                        desc.effective_v1_tiling,
                        1.0,
                    )
                    mat.node_tree.links.new(uv_node.outputs["UV"], mapping.inputs["Vector"])
                    mat.node_tree.links.new(mapping.outputs["Vector"], detail_node.inputs["Vector"])
            except Exception as e:
                print(f"Failed to load detail texture {tex_path or desc.detail.name}: {e}")

        # Wire bump/normal map (slot 3)
        # NovaLogic's engine uses height-based bump mapping, NOT tangent-space
        # normal maps. The bump data comes from:
        #   - Type 4 (NORMAL_MDT): .mdt files — proprietary format, can't load
        #   - Type 5 (NORMAL_TGA): TGA alpha channel contains height data
        #   - Stage1:alpha: diffuse texture alpha = height map (DOT3 shaders)
        # Use ShaderNodeBump (height→normal) instead of ShaderNodeNormalMap.
        normal_desc = desc.normal if desc.normal.name else desc.secondary_normal
        normal_bitmap = normal_desc.name
        normal_type = normal_desc.type
        bump_wired = False
        from pyopennova.materials import NORMAL_TYPE_MDT, NORMAL_TYPE_TGA_ALPHA
        if normal_bitmap and normal_type != NORMAL_TYPE_MDT:  # Skip MDT — Blender can't load
            tex_path = normal_desc.path
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
                    if normal_type == NORMAL_TYPE_TGA_ALPHA:
                        # NORMAL_TGA: height data is in the alpha channel
                        mat.node_tree.links.new(normal_tex.outputs["Alpha"], bump_node.inputs["Height"])
                    else:
                        mat.node_tree.links.new(normal_tex.outputs["Color"], bump_node.inputs["Height"])
                    mat.node_tree.links.new(bump_node.outputs["Normal"], bsdf.inputs["Normal"])
                    bump_wired = True
                except Exception as e:
                    print(f"Failed to load bump map {tex_path}: {e}")

        # No fabricated bump from the diffuse alpha. Phong/DOT3 materials with
        # no slot-3 texture import flat-shaded; the original engine's
        # height-from-alpha gsys_phong path needs a per-material flag
        # indicating that the alpha channel actually contains height data.

        # Handle EMISSIVE flag / emissive_type
        if desc.emissive:
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = 1.0

        # Glass / reflection
        if desc.glass:
            if "Transmission Weight" in bsdf.inputs:
                bsdf.inputs["Transmission Weight"].default_value = 0.5
            elif "Transmission" in bsdf.inputs:
                bsdf.inputs["Transmission"].default_value = 0.5
            if "IOR" in bsdf.inputs:
                bsdf.inputs["IOR"].default_value = 1.45
            rc = desc.reflect_color
            if rc[0] != 0.0 or rc[1] != 0.0 or rc[2] != 0.0:
                mat.diffuse_color = (rc[0], rc[1], rc[2], rc[3] if rc[3] else 1.0)

        # Specular intensity — in the NovaLogic engine, specular is only active
        # when shader_type selects a bump+specular or phong+specular mode.
        # The gsys_phong lookup uses fixed exponents (pow 4/16/64).
        # Map specular_intensity to both Specular IOR Level and Roughness.
        if desc.specular_strength > 0:
            spec = desc.specular_strength
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
        if desc.phong_shader:
            spec_key = "Specular IOR Level" if "Specular IOR Level" in bsdf.inputs else "Specular"
            if spec_key in bsdf.inputs and bsdf.inputs[spec_key].default_value == 0:
                bsdf.inputs[spec_key].default_value = 0.3
            if bsdf.inputs["Roughness"].default_value >= 1.0:
                bsdf.inputs["Roughness"].default_value = 0.5

        # Luminosity — drives emission in the engine
        if desc.luminosity_strength > 0.0:
            lum = desc.luminosity_strength
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = lum

        # UV tiling (apply Mapping node if tiling != 0 and != 1)
        if desc.has_custom_tiling:
            u_tile = desc.effective_u_tiling
            v_tile = desc.effective_v_tiling
            uv_node = mat.node_tree.nodes.new("ShaderNodeUVMap")
            mapping = mat.node_tree.nodes.new("ShaderNodeMapping")
            mapping.inputs["Scale"].default_value = (u_tile, v_tile, 1.0)
            mat.node_tree.links.new(uv_node.outputs["UV"], mapping.inputs["Vector"])
            # Wire mapping output to all existing texture image nodes
            for node in mat.node_tree.nodes:
                if node.type == "TEX_IMAGE":
                    marker = f"{node.name} {getattr(node, 'label', '')}".lower()
                    if "detail" in marker:
                        continue
                    mat.node_tree.links.new(mapping.outputs["Vector"], node.inputs["Vector"])

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

            # Apply rotation from MTRX if available.
            if (i < int(lod0.part_animation_count) and
                    int(self.ir.matrix_count) > 0):
                mi = lod0.part_animations[i].matrix_index
                if mi != 0xFF and mi < int(self.ir.matrix_count):
                    rot = _mtrx_to_center_rotation(self.ir.matrices[mi].m)
                    if rot is not None:
                        center_obj.matrix_basis = Matrix.Identity(4)
                        center_obj.matrix_parent_inverse = rot
                    else:
                        center_obj.scale = (0.0, 0.0, 0.0)
                else:
                    center_obj.scale = (0.0, 0.0, 0.0)

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

            # Position: 3DI3 stores (3di.y, 3di.z, 3di.x) in internal/swizzled
            # space.  Map to Blender via internal→render→render_space:
            # blender = (ir[0], -ir[2], ir[1]).
            raw = Vector(up.position)
            up_pos = Vector((raw[0], -raw[2], raw[1]))
            if up.part_index >= 0 and up.part_index < len(self.part_nodes):
                parent_node = self.part_nodes[up.part_index]
                up_pos = up_pos - Vector(parent_node.matrix_world.translation)

            parent_node = self.root_object
            if up.part_index >= 0 and up.part_index < len(self.part_nodes):
                parent_node = self.part_nodes[up.part_index]
            marker_obj.parent = parent_node
            marker_obj.matrix_basis = Matrix.Identity(4)
            marker_obj.matrix_parent_inverse = _userpoint_local_matrix(up, up_pos)

    def create_scene_lights(self):
        """Create light objects from the 3DI3 model's light data."""
        for i in range(int(self.ir.light_count)):
            light = self.ir.lights[i]
            light_idx = light.part_index if light.part_index >= 0 else i
            light_name = f"LP{light_idx + 1:02d}"

            light_kind = "SPOT" if light.light_type != 0 or light.falloff > 0 else "POINT"
            light_data = bpy.data.lights.new(name=f"{light_name}_data", type=light_kind)
            light_obj = bpy.data.objects.new(light_name, light_data)

            light_data.color = light_color_rgb(light)
            light_data.energy = 1.0
            if light.attenuation_end > 0:
                light_data.use_custom_distance = True
                light_data.cutoff_distance = light.attenuation_end
            light_data.use_shadow = False

            bpy.context.collection.objects.link(light_obj)

            # Convert position to Blender space
            light_obj.location = render_space(Vector(light.offset))

            rot_bl = render_space(Vector((light.rotation[0],
                                        light.rotation[1],
                                        light.rotation[2])))
            if rot_bl.length_squared > 1e-8:
                light_obj.rotation_euler = rot_bl.normalized().to_track_quat("-Z", "Y").to_euler()
            if light_kind == "SPOT":
                light_data.spot_size = math.radians(float(light.falloff) * 2.0)

            # Store light colorgen fields as custom properties for 3dp round-trip
            if light.style != 0:
                light_obj["colorgen_style"] = int(light.style)
                light_obj["colorgen_rate"] = float(light.rate) / 256.0
                light_obj["colorgen_phase"] = float(light.phase) / 256.0
            # color_end (color_start is already stored in light_data.color)
            ce = light_color_rgb(light, "color_end")
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
        volume_owners, volume_plane_starts = collision_volume_metadata(coll.contents)

        for vol_idx in range(int(coll.contents.volume_count)):
            vol = coll.contents.volumes[vol_idx]
            owner_idx = volume_owners[vol_idx] if vol_idx < len(volume_owners) else -1
            plane_start = volume_plane_starts[vol_idx] if vol_idx < len(volume_plane_starts) else -1
            object_idx = int(getattr(vol, "object_index", owner_idx))
            part_idx = int(getattr(vol, "part_index", object_idx))
            try:
                color = CollisionType(vol.type).color
            except ValueError:
                color = (1.0, 1.0, 1.0)

            # Determine parent node and parent position
            parent = self.root_object
            parent_pos = Vector((0, 0, 0))
            parent_set = False
            if object_idx >= 0 and object_idx < part_count:
                parent = self.part_nodes.get(object_idx, self.root_object)
                part = lod0.parts[object_idx]
                parent_pos = render_space(Vector(part.abs_position))
                parent_set = True
            if not parent_set and part_idx >= 0 and part_idx < part_count:
                parent = self.part_nodes.get(part_idx, self.root_object)
                part = lod0.parts[part_idx]
                parent_pos = render_space(Vector(part.abs_position))

            # Compute world center and local offset
            world_min = collision_space(Vector(vol.min))
            world_max = collision_space(Vector(vol.max))
            world_center = (world_min + world_max) * 0.5
            local_center = world_center - parent_pos

            # Build name: type + object_index + duplicate suffix
            name_index = object_idx if object_idx >= 0 else part_idx
            name_key = name_index if name_index >= 0 else -1
            counter_key = (vol.type, name_key)
            name_counters[counter_key] = name_counters.get(counter_key, 0) + 1
            occurrence = name_counters[counter_key]
            mesh_name = _build_volume_name(vol.type, vol.flags, name_index, occurrence) + "-colonly"

            # Gather planes (respecting plane_start/plane_count bounds)
            hs_list = []
            if (vol.plane_count > 0 and plane_start >= 0 and
                    plane_start + vol.plane_count <= int(coll.contents.plane_count)):
                for p_idx in range(vol.plane_count):
                    gp_idx = plane_start + p_idx
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
        """Create occlusion meshes from the 3DI3 model's occlusion data."""
        occ = occlusion_access(self.ir)
        if occ is None or occ["object_count"] == 0:
            return

        if self.ir.lod_count == 0:
            return
        lod0 = self.ir.lods[0]

        occ_counters = {}
        vertex_cursor = 0
        face_cursor = 0
        for i in range(occ["object_count"]):
            occ_obj = occ["objects"][i]
            vert_count = int(occ_obj.num_vertices)
            face_count_val = int(occ_obj.face_count)
            if vert_count <= 0 or face_count_val <= 0:
                vertex_cursor += max(0, vert_count)
                face_cursor += max(0, face_count_val)
                continue

            vert_start = int(getattr(occ_obj, "vertex_start", vertex_cursor))
            face_start = int(getattr(occ_obj, "face_start", face_cursor))
            vertex_cursor += max(0, vert_count)
            face_cursor += max(0, face_count_val)

            if vert_start + vert_count > occ["vertex_count"]:
                continue
            if face_start + face_count_val > occ["face_count"]:
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
                v = occ["vertices"][vert_start + j]
                pos = render_space(Vector(v.position))
                vertices.append(pos - part_abs)

            # Faces: unpack raw_indices as 3 x uint8
            faces = []
            face_flags = []
            for j in range(face_count_val):
                face = occ["faces"][face_start + j]
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
