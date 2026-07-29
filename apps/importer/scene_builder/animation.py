"""Animation building (ported from adm_scene_builder.h): .bad channel
decode, Action/slot management across Blender's legacy and layered
animation APIs, and the NLA push.

Mixin part of BlenderSceneBuilder -- instance state lives in core.__init__.
"""

from __future__ import annotations

import bpy
from mathutils import Vector, Quaternion


class AnimationMixin:
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
