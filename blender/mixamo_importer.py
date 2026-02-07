"""
Mixamo FBX importer for Novalogic pipeline.

Imports one or more Mixamo .fbx files (one with skin mesh + rest pose, the
rest animation-only) and produces a Blender scene fully compatible with the
existing ASE exporter (mesh + bones) and ADM/BAD animation exporter (NLA
actions with metadata).

Bone naming follows the BN## convention used by the Novalogic exporters:
  - DFS traversal from root bones assigns BN01, BN02, BN03, ...
  - Display names: "BN01 Hips", "BN11 LeftArm", etc.
  - ASE exporter's _bone_export_name() trims to just "BN01"
  - Anim exporter's _sorted_export_pose_bones() matches ^BN(\d+) prefix
"""

from __future__ import annotations

import os
import re

import bpy
from mathutils import Vector


BONE_PATH_RE = re.compile(r'pose\.bones\["([^"]+)"\]')


def _log(msg: str):
    print(f"[MIXAMO] {msg}")


def _sanitize_filename(name: str) -> str:
    """Convert a filename into a safe action/anim name component."""
    base = os.path.splitext(os.path.basename(name))[0]
    safe = re.sub(r"[^A-Za-z0-9_]+", "_", base.strip())
    safe = safe.strip("_")
    return safe or "clip"


def _dfs_bone_order(armature_data):
    """Depth-first traversal of edit bones, returning ordered list of bone names.

    Starts from root bones (no parent), visits children in the order they
    appear in armature_data.edit_bones.
    """
    edit_bones = armature_data.edit_bones
    children_map = {}
    roots = []
    for eb in edit_bones:
        if eb.parent is None:
            roots.append(eb.name)
        else:
            parent_name = eb.parent.name
            if parent_name not in children_map:
                children_map[parent_name] = []
            children_map[parent_name].append(eb.name)

    ordered = []
    stack = list(reversed(roots))
    while stack:
        name = stack.pop()
        ordered.append(name)
        children = children_map.get(name, [])
        for child_name in reversed(children):
            stack.append(child_name)

    return ordered


def _strip_bone_prefix(name: str, prefix: str) -> str:
    """Remove the Mixamo prefix from a bone name."""
    if prefix and name.startswith(prefix):
        return name[len(prefix):]
    return name


def _new_objects_since(before_names: set[str]) -> list:
    """Return objects that are new since before_names snapshot."""
    return [obj for obj in bpy.data.objects if obj.name not in before_names]


def _delete_objects_safe(objects: list):
    """Delete a list of Blender objects without using ops (context-safe)."""
    for obj in objects:
        if obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)


def _rotate_location_fcurves_y_up_to_z_up(action):
    """Rotate location fcurve values by -90° X to match applied armature rotation.

    After transform_apply bakes the FBX -90° X object rotation into edit bones,
    location fcurves still encode motion in the original Y-up bone-local space.
    This swaps Y/Z channels: (x, y, z) → (x, z, -y) to match the now Z-up bones.
    """
    # Group location fcurves by bone data_path prefix
    loc_groups = {}  # data_path_prefix -> {channel_index: fcurve}
    for fcurve in action.fcurves:
        if '.location' in fcurve.data_path:
            loc_groups.setdefault(fcurve.data_path, {})[fcurve.array_index] = fcurve

    rotated = 0
    for dp, channels in loc_groups.items():
        if 1 not in channels or 2 not in channels:
            continue
        fc_y = channels[1]
        fc_z = channels[2]

        # Read all Y and Z values
        y_vals = [(kp.co[0], kp.co[1], kp.handle_left[:], kp.handle_right[:])
                  for kp in fc_y.keyframe_points]
        z_vals = [(kp.co[0], kp.co[1], kp.handle_left[:], kp.handle_right[:])
                  for kp in fc_z.keyframe_points]

        # new Y = old Z
        for i, (frame, val, hl, hr) in enumerate(z_vals):
            kp = fc_y.keyframe_points[i]
            kp.co[1] = val
            kp.handle_left[1] = hl[1]
            kp.handle_right[1] = hr[1]

        # new Z = -(old Y)
        for i, (frame, val, hl, hr) in enumerate(y_vals):
            kp = fc_z.keyframe_points[i]
            kp.co[1] = -val
            kp.handle_left[1] = -hl[1]
            kp.handle_right[1] = -hr[1]

        rotated += 1

    if rotated:
        _log(f"  Rotated {rotated} location fcurve groups Y-up -> Z-up in '{action.name}'")


def _scale_location_fcurves(action, scale_factor: float):
    """Scale location fcurve keyframe values to convert units (e.g., cm->m)."""
    scaled = 0
    for fcurve in action.fcurves:
        if '.location' in fcurve.data_path:
            for kp in fcurve.keyframe_points:
                kp.co[1] *= scale_factor
                kp.handle_left[1] *= scale_factor
                kp.handle_right[1] *= scale_factor
            scaled += 1
    if scaled:
        _log(f"  Scaled {scaled} location fcurves by {scale_factor:.4f}")


def _remap_action_fcurves(action, bone_mapping: dict[str, str]):
    """Remap fcurve data_paths from old bone names to new BN## names."""
    remapped = 0
    for fcurve in action.fcurves:
        match = BONE_PATH_RE.search(fcurve.data_path)
        if match:
            old_name = match.group(1)
            if old_name in bone_mapping:
                new_name = bone_mapping[old_name]
                fcurve.data_path = fcurve.data_path.replace(
                    f'"{old_name}"', f'"{new_name}"'
                )
                remapped += 1
    if remapped:
        _log(f"  Remapped {remapped} fcurves in '{action.name}'")


def _fix_action_slot_for_armature(action, armature_name: str):
    """Fix Blender 4.5+ action slot to target the given armature.

    FBX-imported actions have slots bound to their temp armature.  The
    fcurves live inside a channelbag tied to that slot.  Creating a *new*
    slot doesn't move the fcurves, so the animation silently fails.

    Instead, we rename the existing slot's identifier to match our target
    armature, which makes Blender route the fcurves correctly.
    """
    if not hasattr(action, 'slots'):
        return  # Blender < 4.5

    target_id = f"OB{armature_name}"

    # Find the slot that owns the channelbag (i.e. the one with fcurves)
    cb_slot = None
    if hasattr(action, 'layers') and action.layers:
        layer = action.layers[0]
        if layer.strips:
            for strip in layer.strips:
                if hasattr(strip, 'channelbags'):
                    for cb in strip.channelbags:
                        cb_slot = cb.slot
                        break
                if cb_slot:
                    break

    if cb_slot is None:
        return

    if cb_slot.identifier == target_id:
        return  # already correct

    # Remove any other slot that already claims our target identifier
    # (e.g., one we accidentally created earlier) so the rename succeeds
    # without Blender auto-appending ".001".
    to_remove = [s for s in action.slots
                 if s.identifier == target_id and s is not cb_slot]
    for s in to_remove:
        action.slots.remove(s)

    cb_slot.identifier = target_id
    _log(f"  Fixed slot for '{action.name}' -> {target_id}")


def _bake_root_motion(armature_obj, anim_actions, bn01_name):
    """Bake BN01 world-space translation onto root_motion keyframes.

    For each animation action, evaluates each frame to capture BN01's
    world-space position, writes those positions (minus BN01's rest offset)
    onto root_motion location fcurves, and zeros BN01's location fcurves.

    This mirrors the Novalogic import path where root_motion is driven by
    accumulated BAD velocity events (scene_builder.py:751-778).
    """
    scene = bpy.context.scene
    ad = armature_obj.animation_data
    if ad is None:
        ad = armature_obj.animation_data_create()

    # Save current state
    orig_action = ad.action
    orig_slot = getattr(ad, 'action_slot', None)
    orig_frame = scene.frame_current

    # Get BN01 rest-pose head position (edit bone head in armature local space).
    # With identity armature transform, this equals world space.
    bn01_rest_head = None
    for bone in armature_obj.data.bones:
        if bone.name == bn01_name:
            bn01_rest_head = bone.head_local.copy()
            break

    if bn01_rest_head is None:
        _log(f"  WARNING: Could not find rest head for {bn01_name}")
        return

    for action in anim_actions:
        # Assign action for evaluation
        ad.action = action

        # Handle Blender 4.5+ slot system
        if hasattr(action, 'slots'):
            target_id = f"OB{armature_obj.name}"
            for slot in action.slots:
                if slot.identifier == target_id:
                    ad.action_slot = slot
                    break

        frame_start = int(action.frame_range[0])
        frame_end = int(action.frame_range[1])
        frame_count = frame_end - frame_start + 1

        # Evaluate each frame to capture BN01 world position
        positions = []
        for f in range(frame_start, frame_end + 1):
            scene.frame_set(f)
            bn01_pb = armature_obj.pose.bones.get(bn01_name)
            if bn01_pb:
                positions.append(bn01_pb.matrix.translation.copy())
            else:
                positions.append(Vector((0, 0, 0)))

        # Create root_motion location fcurves
        rm_dp = 'pose.bones["root_motion"].location'
        rm_fcs = []
        for ci in range(3):
            existing = action.fcurves.find(rm_dp, index=ci)
            if existing:
                action.fcurves.remove(existing)
            rm_fcs.append(action.fcurves.new(data_path=rm_dp, index=ci))

        for fc in rm_fcs:
            fc.keyframe_points.add(frame_count)

        for i, pos in enumerate(positions):
            bl_frame = frame_start + i
            rm_pos = pos - bn01_rest_head
            for ci in range(3):
                rm_fcs[ci].keyframe_points[i].co = (bl_frame, rm_pos[ci])
                rm_fcs[ci].keyframe_points[i].interpolation = 'LINEAR'

        # Zero BN01's location fcurves (pose location 0 = at rest position)
        bn01_dp = f'pose.bones["{bn01_name}"].location'
        for ci in range(3):
            fc = action.fcurves.find(bn01_dp, index=ci)
            if fc:
                for kp in fc.keyframe_points:
                    kp.co[1] = 0.0
                    kp.handle_left[1] = 0.0
                    kp.handle_right[1] = 0.0

        for fc in rm_fcs:
            fc.update()

        _log(f"  Baked root_motion for '{action.name}' ({frame_count} frames)")

    # Restore original state
    ad.action = orig_action
    if orig_slot is not None and hasattr(ad, 'action_slot'):
        ad.action_slot = orig_slot
    scene.frame_set(orig_frame)


def import_mixamo(
    directory: str,
    files: list[str],
    strip_prefix: str = "mixamorig:",
    create_root_motion: bool = True,
    default_looped: bool = True,
    default_translation: bool = True,
):
    """Main entry point for Mixamo FBX import.

    Args:
        directory: Directory containing the FBX files.
        files: List of filenames (relative to directory).
        strip_prefix: Prefix to strip from Mixamo bone names.
        create_root_motion: Whether to add a root_motion bone.
        default_looped: Default looped flag for imported animations.
        default_translation: Default translation flag for imported animations.
    """
    if not files:
        raise RuntimeError("No files selected")

    # Ensure we're in object mode
    if bpy.context.active_object and bpy.context.active_object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")

    # ── Step 1: Import all FBXs, classify skin vs anim-only ──────────
    skin_path = None
    skin_armature = None
    skin_meshes = []
    skin_extra_objs = []  # FBX empties like "Geo" to clean up later
    anim_imports = []      # (filepath, fname, new_objs, new_armatures)

    for fname in files:
        filepath = os.path.join(directory, fname)
        if not os.path.isfile(filepath):
            _log(f"Skipping missing file: {filepath}")
            continue

        before = set(bpy.data.objects.keys())
        bpy.ops.import_scene.fbx(
            filepath=filepath,
            use_anim=True,
            ignore_leaf_bones=False,
        )
        new_objs = _new_objects_since(before)

        new_meshes = [o for o in new_objs if o.type == 'MESH']
        new_armatures = [o for o in new_objs if o.type == 'ARMATURE']

        if skin_path is None and new_meshes:
            # First file with meshes = skin FBX
            skin_path = filepath
            skin_meshes = new_meshes
            if new_armatures:
                skin_armature = new_armatures[0]
            # Track non-mesh, non-armature objects (empties like "Geo") for cleanup
            skin_extra_objs = [o for o in new_objs
                               if o.type not in ('MESH', 'ARMATURE')]
            _log(f"Skin FBX: {fname} ({len(new_meshes)} meshes)")
        else:
            # Anim-only FBX
            anim_imports.append((filepath, fname, new_objs, new_armatures))

    if skin_armature is None:
        raise RuntimeError(
            "No skin FBX found among selected files. At least one FBX must "
            "contain mesh objects."
        )

    # ── Step 2: Clean up FBX import helper empties (e.g. "Geo") ─────
    _delete_objects_safe(skin_extra_objs)

    # ── Step 3: Discard skin FBX T-pose action ────────────────────────
    if skin_armature.animation_data and skin_armature.animation_data.action:
        tpose_action = skin_armature.animation_data.action
        skin_armature.animation_data.action = None
        _log(f"Discarded skin T-pose action: {tpose_action.name}")
        bpy.data.actions.remove(tpose_action)

    # ── Step 4: Extract actions from anim-only FBXs ───────────────────
    anim_actions = []
    for filepath, fname, new_objs, new_armatures in anim_imports:
        if not new_armatures:
            _log(f"No armature in anim FBX: {fname}, skipping")
            _delete_objects_safe(new_objs)
            continue

        temp_arm = new_armatures[0]
        temp_action = None
        if temp_arm.animation_data and temp_arm.animation_data.action:
            temp_action = temp_arm.animation_data.action

        if temp_action is None:
            _log(f"No action in anim FBX: {fname}, skipping")
            _delete_objects_safe(new_objs)
            continue

        # Name the action from the filename
        sanitized = _sanitize_filename(fname)
        action_name = f"anim_{sanitized}"
        temp_action.name = action_name
        temp_action.use_frame_range = True
        frame_range = temp_action.frame_range
        temp_action.frame_start = frame_range[0]
        temp_action.frame_end = frame_range[1]
        temp_action["bad_name"] = sanitized
        flags = 0
        if default_looped:
            flags |= 0x01
        if default_translation:
            flags |= 0x02
        temp_action["bad_flags"] = flags

        # Detach action from temp armature so it survives cleanup
        temp_action.use_fake_user = True
        temp_arm.animation_data.action = None

        anim_actions.append(temp_action)
        _log(f"Action: {action_name} (frames {int(frame_range[0])}-{int(frame_range[1])})")

        # Delete temp armature and all associated objects
        _delete_objects_safe(new_objs)

    # ── Step 5: Record scale factor, rename bones in edit mode ────────
    scale_factor = skin_armature.scale.x  # typically 0.01 for cm->m

    bpy.context.view_layer.objects.active = skin_armature
    bpy.ops.object.mode_set(mode='EDIT')

    dfs_order = _dfs_bone_order(skin_armature.data)

    # Build mapping: old_name -> "BN## CleanName"
    bone_mapping = {}
    for idx, old_name in enumerate(dfs_order):
        clean = _strip_bone_prefix(old_name, strip_prefix)
        new_name = f"BN{idx + 1:02d} {clean}"
        bone_mapping[old_name] = new_name

    _log(f"Bone mapping: {len(bone_mapping)} bones")

    # Rename bones
    for eb in skin_armature.data.edit_bones:
        if eb.name in bone_mapping:
            eb.name = bone_mapping[eb.name]

    # Create root_motion bone
    if create_root_motion:
        rm_bone = skin_armature.data.edit_bones.new("root_motion")
        rm_bone.head = Vector((0, 0, 0))
        rm_bone.tail = Vector((0, 0.05, 0))
        rm_bone.use_connect = False

        bn01_name = bone_mapping.get(dfs_order[0]) if dfs_order else None
        if bn01_name:
            bn01_eb = skin_armature.data.edit_bones.get(bn01_name)
            if bn01_eb:
                bn01_eb.parent = rm_bone
                _log(f"Parented {bn01_name} to root_motion")

    bpy.ops.object.mode_set(mode='OBJECT')

    # ── Step 6: Rename vertex groups on meshes ────────────────────────
    for mesh_obj in skin_meshes:
        for vg in mesh_obj.vertex_groups:
            if vg.name in bone_mapping:
                vg.name = bone_mapping[vg.name]

    # ── Step 7: Apply rotation + scale to armature + meshes ─────────
    # Blender's FBX importer sets armature scale to 0.01 (cm->m) and a
    # -90° X rotation (Y-up to Z-up conversion) on the object transform.
    # We must bake BOTH into the data so the armature object has identity
    # transform.  This is critical for the anim exporter, which reads
    # pb.bone.matrix_local (armature-local) and pb.matrix (world-space)
    # and assumes they agree at rest frame.  Without applying rotation,
    # matrix_local stays in Y-up space while pb.matrix is in Z-up space,
    # causing a 90° export error (character flat on back).
    bpy.ops.object.select_all(action='DESELECT')
    skin_armature.select_set(True)
    for mesh_obj in skin_meshes:
        mesh_obj.select_set(True)
    bpy.context.view_layer.objects.active = skin_armature
    bpy.ops.object.transform_apply(
        location=False, rotation=True, scale=True,
    )
    _log(f"Applied rotation + scale to armature + meshes")

    # ── Step 7.5: Rotate location fcurves Y-up -> Z-up ────────────────
    # transform_apply baked the -90° X object rotation into the edit bones
    # but did NOT touch animation fcurves.  Location fcurves still encode
    # motion in the original Y-up bone-local space.  Rotate them to match
    # the now Z-up bone orientations: (x, y, z) → (x, z, -y).
    for action in anim_actions:
        _rotate_location_fcurves_y_up_to_z_up(action)

    # ── Step 8: Remap fcurve bone names in all animation actions ──────
    for action in anim_actions:
        _remap_action_fcurves(action, bone_mapping)

    # ── Step 9: Scale location fcurves (cm -> m) ──────────────────────
    # transform_apply scaled bone rest positions but not animation fcurve
    # values.  Location fcurves are in bone-local space and still hold cm
    # offsets — multiply by scale_factor to convert to meters.
    if abs(scale_factor - 1.0) > 1e-6:
        for action in anim_actions:
            _scale_location_fcurves(action, scale_factor)

    # ── Step 10: Fix Blender 4.5+ action slots ────────────────────────
    # FBX-imported actions have channelbag slots bound to their now-deleted
    # temp armatures.  Rename those slots to target our skin armature so
    # Blender can route the fcurves correctly.
    for action in anim_actions:
        _fix_action_slot_for_armature(action, skin_armature.name)

    # ── Step 10.5: Bake BN01 translation onto root_motion ─────────────
    if create_root_motion and anim_actions:
        bn01_name = bone_mapping.get(dfs_order[0]) if dfs_order else None
        if bn01_name:
            _bake_root_motion(skin_armature, anim_actions, bn01_name)

    # ── Step 11: Create anim_reset action ─────────────────────────────
    reset_action = bpy.data.actions.new(name="anim_reset")
    reset_action.use_fake_user = True
    reset_action.use_frame_range = True
    reset_action.frame_start = 1
    reset_action.frame_end = 1
    reset_action["bad_name"] = "reset"
    reset_action["bad_flags"] = 0x03  # looped + translation

    # Blender 4.5+: create a slot for keyframing
    if hasattr(reset_action, 'slots'):
        slot = reset_action.slots.new(
            id_type='OBJECT', name=skin_armature.name,
        )
        ad = skin_armature.animation_data
        if ad is None:
            ad = skin_armature.animation_data_create()
        old_action = ad.action
        old_slot = getattr(ad, 'action_slot', None)
        ad.action = reset_action
        ad.action_slot = slot

    # Single-frame identity keyframes for all bones
    for pb in skin_armature.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        if pb.name == "root_motion":
            dp = f'pose.bones["{pb.name}"].location'
            for ci in range(3):
                fc = reset_action.fcurves.new(data_path=dp, index=ci)
                kf = fc.keyframe_points.insert(1, 0.0)
                kf.interpolation = 'LINEAR'
        else:
            dp_rot = f'pose.bones["{pb.name}"].rotation_quaternion'
            identity = [1.0, 0.0, 0.0, 0.0]
            for ci in range(4):
                fc = reset_action.fcurves.new(data_path=dp_rot, index=ci)
                kf = fc.keyframe_points.insert(1, identity[ci])
                kf.interpolation = 'LINEAR'
            dp_loc = f'pose.bones["{pb.name}"].location'
            for ci in range(3):
                fc = reset_action.fcurves.new(data_path=dp_loc, index=ci)
                kf = fc.keyframe_points.insert(1, 0.0)
                kf.interpolation = 'LINEAR'

    # Restore previous action/slot
    if hasattr(reset_action, 'slots'):
        ad.action = old_action
        if old_slot is not None:
            ad.action_slot = old_slot

    # ── Step 12: Create PN01 part empty ────────────────────────────────
    pn01 = bpy.data.objects.new("PN01", None)
    pn01.empty_display_type = 'PLAIN_AXES'
    pn01.empty_display_size = 0.05
    bpy.context.collection.objects.link(pn01)

    # ── Step 13: Push all actions to NLA tracks ───────────────────────
    if skin_armature.animation_data is None:
        skin_armature.animation_data_create()

    ad = skin_armature.animation_data
    ad.action = None

    all_actions = [reset_action] + anim_actions

    nla_frame_offset = 0
    for action in all_actions:
        action.use_fake_user = True
        track = ad.nla_tracks.new()
        track.name = action.name
        strip = track.strips.new(
            action.name, int(nla_frame_offset + 1), action,
        )
        strip.name = action.name
        strip.influence = 1.0
        # HOLD_FORWARD: don't pre-extrapolate before the strip starts.
        # Without this, higher-track anim strips bleed into the reset
        # strip's frame range and the T-pose never appears in playback.
        strip.extrapolation = 'HOLD_FORWARD'

        # Blender 4.5+: point the NLA strip at the correct slot
        if hasattr(action, 'slots'):
            target_id = f"OB{skin_armature.name}"
            for slot in action.slots:
                if slot.identifier == target_id:
                    strip.action_slot = slot
                    break

        frame_count = int(action.frame_range[1] - action.frame_range[0] + 1)
        nla_frame_offset += frame_count
        _log(f"NLA track: {action.name} ({frame_count} frames)")

    if nla_frame_offset > 0:
        bpy.context.scene.frame_start = 1
        bpy.context.scene.frame_end = int(nla_frame_offset)

    # Set all pose bones to quaternion mode
    for pb in skin_armature.pose.bones:
        pb.rotation_mode = 'QUATERNION'

    # Select armature
    bpy.ops.object.select_all(action='DESELECT')
    skin_armature.select_set(True)
    bpy.context.view_layer.objects.active = skin_armature

    _log(
        f"Import complete: {len(bone_mapping)} bones, "
        f"{len(all_actions)} actions, "
        f"{len(skin_meshes)} meshes"
    )

    return skin_armature
