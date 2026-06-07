"""
Novalogic animation exporter (.adm + .bad) for Blender.

This is intentionally simple:
- source clips from armature NLA strips (unique by action name, in strip order)
- user picks per-clip include + loop + translation + reset designation
- writes one BAD per included clip and one ADM file listing clips

The Blender-side math (inverse of the importer's bone-space conventions) lives
here; the actual byte serialization is delegated to the shared C writer via
opennova.bad_build (write_bad / write_adm).
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass

import bpy
from mathutils import Matrix, Quaternion, Vector

from .opennova import bad_build


ANIM_FLAG_LOOPED = 0x01
ANIM_FLAG_TRANSLATION = 0x02
LEAF_BONE_LENGTH_FALLBACK = 0.05
ADM_ANIM_KEY_RE = re.compile(r"^anim_[A-Za-z0-9_]+$")
BAD_PI_Y_MATRIX = Matrix(((-1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, -1.0)))
# The importer points edit-bones along -Y, baking a 180° Z-rotation D=diag(-1,-1,1)
# into the rest pose.  _Y_FLIP undoes this factor on local positions.
_Y_FLIP = Matrix(((-1.0, 0.0, 0.0), (0.0, -1.0, 0.0), (0.0, 0.0, 1.0)))


def _log(msg: str):
    print(f"[ANIM_EXPORT] {msg}")


def _sanitize_name(name: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_]+", "_", name.strip())
    return safe or "anim_clip"


def _is_valid_adm_anim_key(name: str) -> bool:
    return ADM_ANIM_KEY_RE.match(name) is not None


def _bl_to_bad_vec(v: Vector) -> Vector:
    # Inverse of importer bone-space conversion: (x, -z, y)
    # -> BAD: (x, z, -y)
    return Vector((v.x, v.z, -v.y))


def _bad_to_bl_vec(v: Vector) -> Vector:
    # Importer bone_space: BAD(x,y,z) -> BL(x,-z,y)
    return Vector((v.x, -v.z, v.y))


def _bl_to_bad_quat(q_bl: Quaternion) -> Quaternion:
    # Pure inverse of importer's _bad_channel_to_blender_quat:
    #   import: BAD(x,y,z,w) -> BL(w, x, -z, y)
    #   export: BL(w,x,y,z) -> BAD(x, z, -y, w)
    # The per-bone corr (=D) already bridges rest-pose conventions.
    q = Quaternion((q_bl.w, q_bl.x, q_bl.z, -q_bl.y))
    q.normalize()
    return q


def _bl_to_bad_matrix3(m_bl: Matrix) -> Matrix:
    # Importer path for BAD bone table is:
    #   R_bl_world = conjugate_y_to_z(R_bad_raw).transposed()
    # Therefore inverse export mapping is:
    #   R_bad_raw = S^-1 * (R_bl_world.transposed()) * S
    # where S maps BAD->Blender (x, y, z) -> (x, -z, y).
    s = Matrix(((1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0)))
    s_inv = s.inverted()
    raw = s_inv @ m_bl.transposed() @ s
    return BAD_PI_Y_MATRIX @ raw


def _bad_raw_matrix_to_bl_world_quat(raw: Matrix) -> Quaternion:
    # Same reconstruction path used by importer:
    # conjugate_y_to_z(raw).transposed().to_quaternion()
    conj = Matrix((
        (raw[0][0], -raw[0][2], raw[0][1]),
        (-raw[2][0], raw[2][2], -raw[2][1]),
        (raw[1][0], -raw[1][2], raw[1][1]),
    ))
    q = conj.transposed().to_quaternion()
    q.normalize()
    return q


def _orthonormalize_matrix3(m: Matrix) -> Matrix:
    q = m.to_quaternion()
    q.normalize()
    return q.to_matrix()


@dataclass
class _ExportBone:
    name: str
    parent_index: int
    position_bad: Vector
    rot_bad: Matrix
    length: float


@dataclass
class _ClipData:
    action: bpy.types.Action
    action_name: str
    bad_name: str
    start_frame: int
    end_frame: int
    is_reset: bool
    flags: int


def _get_active_armature(context) -> bpy.types.Object | None:
    obj = context.active_object
    if obj and obj.type == 'ARMATURE':
        return obj
    selected_armatures = [o for o in context.selected_objects if o.type == 'ARMATURE']
    if len(selected_armatures) == 1:
        return selected_armatures[0]
    return None


def _collect_unique_actions_from_nla(armature_obj: bpy.types.Object):
    ad = armature_obj.animation_data
    if not ad:
        return []

    seen = set()
    ordered_actions = []
    for track in ad.nla_tracks:
        for strip in track.strips:
            action = strip.action
            if action is None:
                continue
            if action.name in seen:
                continue
            seen.add(action.name)
            ordered_actions.append(action)
    return ordered_actions


def _sorted_export_pose_bones(armature_obj: bpy.types.Object):
    pose_bones = [pb for pb in armature_obj.pose.bones if pb.name.upper().startswith("BN")]

    def key_fn(pb):
        m = re.match(r"^BN(\d+)", pb.name.upper())
        if m:
            return (0, int(m.group(1)), pb.name)
        return (1, 0, pb.name)

    pose_bones.sort(key=key_fn)
    return pose_bones


class NovalogicAnimExporter:
    def __init__(self, context, armature_obj: bpy.types.Object):
        self.context = context
        self.scene = context.scene
        self.armature = armature_obj
        self.pose_bones = _sorted_export_pose_bones(armature_obj)
        if not self.pose_bones:
            raise RuntimeError("No BN* bones found on the armature")
        self.bone_index = {pb.name: i for i, pb in enumerate(self.pose_bones)}
        self._reset_world_rot_corrections = [Quaternion((1.0, 0.0, 0.0, 0.0)) for _ in self.pose_bones]
        self._reset_rest_origins_bl = [Vector((0.0, 0.0, 0.0)) for _ in self.pose_bones]
        self._capsule_offset = 0.0  # ground-to-foot gap, computed from reset clip

    def _set_eval_action(self, action: bpy.types.Action):
        ad = self.armature.animation_data
        if ad is None:
            ad = self.armature.animation_data_create()
        old = (ad.action, ad.use_nla)
        ad.use_nla = False
        ad.action = action
        return old

    def _restore_eval_action(self, old_state):
        ad = self.armature.animation_data
        if ad is None:
            return
        old_action, old_use_nla = old_state
        ad.action = old_action
        ad.use_nla = old_use_nla

    def _extract_clip_bones(self, start_frame: int):
        self.scene.frame_set(start_frame)
        bones = []
        raw_bones = []

        for i, pb in enumerate(self.pose_bones):
            parent_idx = -1
            if pb.parent:
                if pb.parent.name == "root_motion":
                    # root_motion is the Blender parent but BAD expects BN01 as root
                    parent_idx = -1
                elif pb.parent.name not in self.bone_index:
                    raise RuntimeError(
                        f"Bone '{pb.name}' has non-BN parent '{pb.parent.name}'"
                    )
                else:
                    parent_idx = self.bone_index[pb.parent.name]
                    if parent_idx >= i:
                        raise RuntimeError(
                            f"Bone hierarchy is not topological: '{pb.name}' parent '{pb.parent.name}'"
                        )

            # Armature-space local translation at reference frame
            if pb.parent and pb.parent.name == "root_motion":
                # BN01 parented to root_motion: compute local relative to rm
                local_mat = pb.parent.matrix.inverted() @ pb.matrix
                local_pos = (_Y_FLIP @ local_mat.translation).copy()
                pos_bad = _bl_to_bad_vec(local_pos)
            elif parent_idx >= 0:
                parent_pb = self.pose_bones[parent_idx]
                local_mat = parent_pb.matrix.inverted() @ pb.matrix
                # _Y_FLIP cancels the D factor in local_mat, giving the
                # true BL rest offset; plain _bl_to_bad_vec is the correct
                # inverse of the importer's bone_space conversion.
                local_pos = (_Y_FLIP @ local_mat.translation).copy()
                pos_bad = _bl_to_bad_vec(local_pos)
            else:
                local_mat = pb.matrix.copy()
                local_pos = pb.matrix.translation.copy()
                pos_bad = _bl_to_bad_vec(local_pos)
            if pb.name.upper().startswith("BN01"):
                pos_bad = Vector((0.0, 0.0, 0.0))

            # BAD stores world-space reference rotations per clip.
            world_rot_bl = pb.matrix.to_quaternion()
            world_rot_bl.normalize()
            world_rot_bl = world_rot_bl.to_matrix()
            world_rot_bad = _orthonormalize_matrix3(_bl_to_bad_matrix3(world_rot_bl))

            raw_bones.append(
                {
                    "name": pb.name[:31],
                    "parent_index": parent_idx,
                    "position_bad": pos_bad,
                    "rot_bad": world_rot_bad,
                    "head_bl": pb.matrix.translation.copy(),
                }
            )

        children = {i: [] for i in range(len(raw_bones))}
        for i, b in enumerate(raw_bones):
            if b["parent_index"] >= 0:
                children[b["parent_index"]].append(i)

        for i, b in enumerate(raw_bones):
            # Prefer original BAD bone length stored during import (roundtrip).
            # Fall back to child-distance computation for user-created armatures.
            pb = self.pose_bones[i]
            bad_len = pb.bone.get("bad_length", None)
            if bad_len is not None:
                length = float(bad_len)
            else:
                child_idxs = children[i]
                length = LEAF_BONE_LENGTH_FALLBACK
                if child_idxs:
                    parent_head = b["head_bl"]
                    max_child_dist = max(
                        (raw_bones[c]["head_bl"] - parent_head).length for c in child_idxs
                    )
                    if max_child_dist > 1e-5:
                        length = max_child_dist

            bones.append(
                _ExportBone(
                    name=b["name"],
                    parent_index=b["parent_index"],
                    position_bad=b["position_bad"],
                    rot_bad=b["rot_bad"],
                    length=float(length),
                )
            )

        return bones

    def _extract_channels(self, clip: _ClipData, has_translation: bool):
        frame_total = clip.end_frame - clip.start_frame + 1
        channels = []
        translations_per_frame = []

        prev_quats = [None] * len(self.pose_bones)
        per_bone_rots = [[] for _ in self.pose_bones]

        for f in range(frame_total):
            frame = clip.start_frame + f
            self.scene.frame_set(frame)

            frame_trans = []

            for i, pb in enumerate(self.pose_bones):
                q_bl_world = pb.matrix.to_quaternion()
                corr = self._reset_world_rot_corrections[i] if i < len(self._reset_world_rot_corrections) else Quaternion((1.0, 0.0, 0.0, 0.0))
                # Inverse of importer: world = bad_channel * corr
                q_bl_channel = q_bl_world @ corr.inverted()
                q_bad = _bl_to_bad_quat(q_bl_channel)

                prev = prev_quats[i]
                if prev is not None and prev.dot(q_bad) < 0.0:
                    q_bad = Quaternion((-q_bad.w, -q_bad.x, -q_bad.y, -q_bad.z))
                prev_quats[i] = q_bad.copy()
                per_bone_rots[i].append((q_bad.x, q_bad.y, q_bad.z, q_bad.w))

                if has_translation:
                    parent_idx = self.bone_index.get(pb.parent.name, -1) if pb.parent else -1
                    rest_origin = self._reset_rest_origins_bl[i] if i < len(self._reset_rest_origins_bl) else Vector((0.0, 0.0, 0.0))
                    if pb.parent and pb.parent.name == "root_motion":
                        # BN01: compute translation relative to root_motion
                        rm_pb = self.armature.pose.bones["root_motion"]
                        pmat = rm_pb.matrix.to_3x3()
                        base_world = rm_pb.matrix.translation + (pmat @ (_Y_FLIP @ rest_origin))
                        translation_bl = pb.matrix.translation - base_world
                    elif parent_idx >= 0:
                        pmat = self.pose_bones[parent_idx].matrix.to_3x3()
                        # pmat includes the edit-bone D factor; re-apply D to
                        # rest_origin so that D^2=I cancels inside pmat @ origin.
                        base_world = self.pose_bones[parent_idx].matrix.translation + (pmat @ (_Y_FLIP @ rest_origin))
                        translation_bl = pb.matrix.translation - base_world
                    else:
                        base_world = rest_origin
                        translation_bl = pb.matrix.translation - base_world
                    delta_bad = _bl_to_bad_vec(translation_bl)
                    frame_trans.append((delta_bad.x, delta_bad.y, delta_bad.z))

            if has_translation:
                translations_per_frame.append(frame_trans)

        # BAD convention: translations have frame_count+1 entries (terminal duplicate)
        if has_translation and translations_per_frame:
            translations_per_frame.append(translations_per_frame[-1])

        for i in range(len(self.pose_bones)):
            rots = per_bone_rots[i]
            # BAD convention: channels have frame_count+1 entries (terminal duplicate)
            terminal = rots[-1] if rots else (0.0, 0.0, 0.0, 1.0)
            channels.append(
                {
                    "frame_lengths": [1] * frame_total + [1],
                    "rotations": rots + [terminal],
                }
            )

        # Legacy convention writes one root velocity event per sampled frame.
        # Each event carries velocity + bottom/top bone extents from ground.
        root_vel_events = []
        if self.pose_bones:
            root_positions = []
            bone_extents = []  # (min_bad_y, max_bad_y) per frame
            rm_pb = self.armature.pose.bones.get("root_motion")
            for f in range(frame_total):
                self.scene.frame_set(clip.start_frame + f)
                # Root position for velocity
                if rm_pb:
                    root_positions.append(rm_pb.matrix.translation.copy())
                else:
                    root_positions.append(self.pose_bones[0].matrix.translation.copy())
                # Bone Y-extents in BAD space (Y-up) for bottom/top.
                # Blender Z = BAD Y (height).  Measure relative to BN01 origin
                # since BAD world positions are relative to skeleton root.
                bn01_z = self.pose_bones[0].matrix.translation.z
                min_y = 0.0
                max_y = 0.0
                for pb in self.pose_bones:
                    bad_y = pb.matrix.translation.z - bn01_z
                    if bad_y < min_y:
                        min_y = bad_y
                    if bad_y > max_y:
                        max_y = bad_y
                bone_extents.append((min_y, max_y))

            for i in range(frame_total):
                if i == 0:
                    vel_bl = root_positions[0].copy()  # displacement from origin
                else:
                    vel_bl = root_positions[i] - root_positions[i - 1]
                vel_bad = _bl_to_bad_vec(vel_bl)
                min_y, max_y = bone_extents[i]
                bottom = abs(min_y) + self._capsule_offset
                top = max_y + abs(min_y) + self._capsule_offset
                root_vel_events.append((vel_bad.x, vel_bad.y, vel_bad.z, bottom, top, 0))

            # BAD convention: events have frame_count+1 entries (terminal duplicate)
            if root_vel_events:
                root_vel_events.append(root_vel_events[-1])

        return channels, translations_per_frame, root_vel_events

    def write_bad(self, out_bad_path: str, clip: _ClipData):
        sample_count = clip.end_frame - clip.start_frame + 1
        if sample_count < 1:
            raise RuntimeError(f"Clip '{clip.action_name}' has too few frames")
        num_frames_header = sample_count

        has_translation = (clip.flags & ANIM_FLAG_TRANSLATION) != 0

        old_state = self._set_eval_action(clip.action)
        try:
            bones = self._extract_clip_bones(clip.start_frame)
            channels, translations, events = self._extract_channels(clip, has_translation)
        finally:
            self._restore_eval_action(old_state)
        num_bones = len(bones)
        num_events = len(events)

        expected_ch = sample_count + 1  # +1 terminal duplicate sample
        for i, ch in enumerate(channels):
            if len(ch["frame_lengths"]) != expected_ch or len(ch["rotations"]) != expected_ch:
                raise RuntimeError(
                    f"Clip '{clip.action_name}' has inconsistent key counts for bone index {i}"
                )
        if has_translation and len(translations) != sample_count + 1:
            raise RuntimeError(
                f"Clip '{clip.action_name}' translation sample count mismatch"
            )
        if num_events != sample_count + 1:  # +1 terminal duplicate event
            raise RuntimeError(
                f"Clip '{clip.action_name}' root event sample count mismatch"
            )

        # Diagnostic: matrix-vs-channel[0] consistency in Blender world space.
        if num_bones > 0 and channels:
            max_err = 0.0
            sum_err = 0.0
            cnt = 0
            for i, b in enumerate(bones):
                qb = channels[i]["rotations"][0]
                q_bl = Quaternion((qb[3], qb[0], -qb[2], qb[1]))
                corr = self._reset_world_rot_corrections[i] if i < len(self._reset_world_rot_corrections) else Quaternion((1.0, 0.0, 0.0, 0.0))
                q_bl = q_bl @ corr
                q_bl.normalize()

                # Reconstruct Blender-world rot from exported BAD matrix using importer logic.
                # Strip BAD_PI_Y_MATRIX (its own inverse) before reconstruction.
                raw = BAD_PI_Y_MATRIX @ b.rot_bad
                conj = Matrix((
                    (raw[0][0], -raw[0][2], raw[0][1]),
                    (-raw[2][0], raw[2][2], -raw[2][1]),
                    (raw[1][0], -raw[1][2], raw[1][1]),
                ))
                m_bl = conj.transposed()
                m_bl = m_bl.to_quaternion()
                m_bl.normalize()

                err = m_bl.rotation_difference(q_bl).angle
                max_err = max(max_err, err)
                sum_err += err
                cnt += 1
            if cnt:
                _log(
                    f"{clip.action_name}: matrix/channel0 mismatch "
                    f"max={max_err * 57.2957795:.4f}deg avg={(sum_err / cnt) * 57.2957795:.4f}deg"
                )

        # Build the neutral clip and hand it to the shared C serializer.
        bones_out = []
        for b in bones:
            m = b.rot_bad
            bones_out.append(bad_build.BadBoneOut(
                name=b.name,
                parent_index=int(b.parent_index),
                length=float(b.length),
                position_bad=(float(b.position_bad.x), float(b.position_bad.y), float(b.position_bad.z)),
                rotation_rows_bad=(
                    (float(m[0][0]), float(m[0][1]), float(m[0][2])),
                    (float(m[1][0]), float(m[1][1]), float(m[1][2])),
                    (float(m[2][0]), float(m[2][1]), float(m[2][2])),
                ),
            ))

        channels_out = [
            bad_build.BadChannelOut(
                frame_lengths=tuple(int(x) for x in ch["frame_lengths"]),
                rotations_xyzw=tuple(tuple(float(c) for c in q) for q in ch["rotations"]),
            )
            for ch in channels
        ]

        events_out = [
            bad_build.BadEventOut(
                velocity_bad=(float(vx), float(vy), float(vz)),
                bottom=float(bottom),
                top=float(top),
                trigger=int(trig),
            )
            for (vx, vy, vz, bottom, top, trig) in events
        ]

        translations_out = ()
        if has_translation:
            translations_out = tuple(
                tuple((float(t[0]), float(t[1]), float(t[2])) for t in frame)
                for frame in translations
            )

        clip_out = bad_build.BadClipOut(
            frame_count=num_frames_header,
            bones=tuple(bones_out),
            channels=tuple(channels_out),
            translations=translations_out,
            events=tuple(events_out),
            version=1,
            fps=30,  # legacy BAD exporters and stock assets
            flags=clip.flags,
            name=clip.action_name,
            bad_name=clip.bad_name,
            is_reset=clip.is_reset,
        )
        bad_build.write_bad(out_bad_path, clip_out)

        _log(
            f"Wrote {os.path.basename(out_bad_path)} "
            f"(bones={num_bones}, frames={num_frames_header}, flags=0x{clip.flags:02X}, events={num_events})"
        )

    def configure_from_reset_clip(self, reset_clip: _ClipData):
        """Compute reset-based corrections needed to invert importer math."""
        old_state = self._set_eval_action(reset_clip.action)
        try:
            reset_bones = self._extract_clip_bones(reset_clip.start_frame)
            # Also capture the actual reset-frame Blender transforms for diagnostics.
            reset_world_quats = [pb.matrix.to_quaternion().normalized() for pb in self.pose_bones]
            reset_local_pos = []
            for i, pb in enumerate(self.pose_bones):
                if pb.parent and pb.parent.name == "root_motion":
                    # BN01 parented to root_motion: compute local relative to rm
                    lmat = pb.parent.matrix.inverted() @ pb.matrix
                    reset_local_pos.append((_Y_FLIP @ lmat.translation).copy())
                else:
                    parent_idx = self.bone_index.get(pb.parent.name, -1) if pb.parent else -1
                    if parent_idx >= 0:
                        pmat_inv = self.pose_bones[parent_idx].matrix.inverted()
                        lmat = pmat_inv @ pb.matrix
                        reset_local_pos.append((_Y_FLIP @ lmat.translation).copy())
                    else:
                        reset_local_pos.append(pb.matrix.translation.copy())
        finally:
            self._restore_eval_action(old_state)

        self._reset_rest_origins_bl = [_bad_to_bl_vec(b.position_bad) for b in reset_bones]
        # For bones parented to root_motion, rest origin is the local offset
        # from root_motion (includes height above capsule center).
        for i, pb in enumerate(self.pose_bones):
            if pb.parent and pb.parent.name == "root_motion":
                self._reset_rest_origins_bl[i] = reset_local_pos[i].copy()
        self._reset_world_rot_corrections = []

        # Diagnostics: exported BAD bone-table values roundtrip through importer math.
        pos_err_max = 0.0
        pos_err_sum = 0.0
        rot_err_max = 0.0
        rot_err_sum = 0.0
        diag_count = 0

        for i, pb in enumerate(self.pose_bones):
            if i >= len(reset_bones):
                self._reset_world_rot_corrections.append(Quaternion((1.0, 0.0, 0.0, 0.0)))
                continue

            raw = reset_bones[i].rot_bad
            bad_rest_world_q = _bad_raw_matrix_to_bl_world_quat(raw)

            bl_rest_world_q = pb.bone.matrix_local.to_quaternion()
            bl_rest_world_q.normalize()

            corr = bad_rest_world_q.inverted() @ bl_rest_world_q
            corr.normalize()
            self._reset_world_rot_corrections.append(corr)

            # Position roundtrip check (BAD position -> importer bone_space)
            bl_pos_from_bad = _bad_to_bl_vec(reset_bones[i].position_bad)
            p_err = (bl_pos_from_bad - reset_local_pos[i]).length
            pos_err_max = max(pos_err_max, p_err)
            pos_err_sum += p_err

            # Rotation roundtrip check: strip BAD_PI_Y_MATRIX (its own
            # inverse) so reconstruction matches the importer convention.
            raw_diag = BAD_PI_Y_MATRIX @ raw
            diag_q = _bad_raw_matrix_to_bl_world_quat(raw_diag)
            r_err = diag_q.rotation_difference(reset_world_quats[i]).angle
            rot_err_max = max(rot_err_max, r_err)
            rot_err_sum += r_err
            diag_count += 1

        if diag_count:
            _log(
                "reset bone-table roundtrip "
                f"pos_err max/avg={pos_err_max:.6f}/{(pos_err_sum / diag_count):.6f} "
                f"rot_err_deg max/avg={(rot_err_max * 57.2957795):.4f}/{((rot_err_sum / diag_count) * 57.2957795):.4f}"
            )

        # Compute capsule offset: the gap between the lowest bone and ground.
        # In the engine, ground is at Y=0 in world space and the skeleton is placed
        # so that feet rest on the ground.  bottom/top event fields measure bone
        # extents from the ground plane upward.  The capsule offset is the small gap
        # between the lowest bone tip and the actual ground contact point (roughly
        # the capsule radius).
        self._capsule_offset = 0
        _log(f"capsule_offset={self._capsule_offset:.4f}")

    def write_adm(self, out_adm_path: str, clips: list[_ClipData]):
        reset = next((c for c in clips if c.is_reset), None)
        if reset is None:
            raise RuntimeError("No reset clip selected")

        refs = [
            bad_build.AdmClipRef(
                animation_name=c.action_name,
                bad_name=c.bad_name,
                is_reset=c.is_reset,
            )
            for c in clips
        ]
        bad_build.write_adm(out_adm_path, refs)

        _log(f"Wrote {os.path.basename(out_adm_path)} ({len(clips)} entries)")


def build_clip_items_for_armature(op):
    arm = _get_active_armature(bpy.context)
    op.clip_items.clear()
    if arm is None:
        return None

    actions = _collect_unique_actions_from_nla(arm)
    for action in actions:
        item = op.clip_items.add()
        item.action_name = action.name
        item.include = True
        item.is_reset = (action.name == "anim_reset")
        # Use original BAD filename if stored, otherwise derive from action name
        item.filename = action.get("bad_name", _sanitize_name(action.name))
        # Use original BAD flags if stored, otherwise default both on
        stored_flags = action.get("bad_flags", -1)
        if stored_flags >= 0:
            item.loop = bool(stored_flags & 0x01)
            item.translation = bool(stored_flags & 0x02)
        else:
            item.loop = True
            item.translation = True
    return arm
