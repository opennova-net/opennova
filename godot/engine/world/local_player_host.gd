class_name LocalPlayerHost
extends Node

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

# Faithful first-person camera. The eye is +1.0 world unit above the player
# [orig: Camera_ComputeThirdPersonView @0x437d10]; F4 swaps to a behind+above
# third person [orig: ThirdPersonCamera_Update @0x437af0]. The mouse drives look
# yaw/pitch (pitch clamped ±80° [orig: Input_HandleActionBinding_0 @0x4e1330]).
const PLAYER_EYE_HEIGHT := 1.0          # +0x10000 = +1.0 world unit above Position
const PLAYER_PITCH_CLAMP_DEG := 80.0    # ±954437120 BAM
const PLAYER_MOUSE_SENS_DEG := 0.12     # degrees per mouse pixel (tunable)
# Witnessed chase-camera numbers: distance 3.0 (0x30000) and orbit pitch 22.5 deg
# (0x4000000), the on-change defaults [orig: Camera_SetTrackedEntity @0x4391d0]; the
# eye is anchor + R(yaw, pitch + orbit)*(-dist) re-aimed at the anchor
# [orig: Camera_ComputeThirdPersonView @0x437d10]; the anchor eases quarter-step
# [orig: ThirdPersonCamera_Update @0x437c8d]. Orbit keys, the bone/terrain collision
# march, and the 0.125u look-at offset are tracked deferrals (net-re section 5.39
# 2026-07-08 addendum).
const PLAYER_TP_DISTANCE := 3.0
const PLAYER_TP_ORBIT_PITCH_DEG := 22.5
# First-person weapon viewmodel placement, witnessed from weapon.def `pos` (hip) / `tpos` (ADS).
# The original adds the equipped weapon's view-bias offset to the eye in view-local space, rotated by
# the view orientation, then draws the gun (gfx1) + character arms at that view root
# [orig: Player_UpdateFirstPersonCamera @0x4dd380 -> g_view_euler_translation_out;
# Player_RenderFirstPersonViewModel @0x4ded60]. The weapon.def parser stores the pos/tpos POSITION as
# `atof(str) * 256.0` (a 16.16 fixed-point world coord; scale flt_7D1D70 @0x544770) and the ROTATION
# as degrees -> 32-bit BAM (`* 0x0B60B60` = 2^32/360) [orig: weapon.def 'tpos' handler @0x54471f].
# The camera ftol's the stored float and adds it straight onto g_view_pos (16.16), so the net WORLD
# offset is simply `file_value / 256` — see _viewmodel_offset for the axis map and derivation.
# The Sighted/ADS path swaps `pos` -> `tpos` (WeaponDef.AltCamOffset @0x10C, read when entity
# Flags & 2): AK47AUTO tpos = (-28.046, 21.531, -187.857), wired when ADS lands.
# Units are WPN_AK47AUTO (REVX02\WEAPON.DEF) — hardcoded with the fixed-default model until a
# weapon.def Godot binding resolves the equipped weapon's pos/tpos per-weapon. (Swapped from WPN_MP5SD
# to confirm the placement generalizes; AK47AUTO pos is a near-pure vertical drop = a clean test.)
const WEAPON_DEF_POS_SCALE := 256.0                            # flt_7D1D70: file unit -> /256 world units
const PLAYER_VIEWMODEL_POS_UNITS := Vector3(10.0, 0.0, -201.0) # weapon.def `pos` (hip)
# FP viewmodel model-facing rotation, euler DEGREES, camera-local. Our NovaObjectModel mesh is
# model-native (Y-up, only X-negated) so it must NOT get the world-object bms_to_godot_basis; this
# lays the gun barrel down-range relative to the view. The small per-weapon `pos`-rotation columns
# (Bone.rot, yaw/pitch/roll BAM — AK47AUTO = 0.0 / 0.0 / 1.0 deg) are a separate fine-tune, deferred.
const PLAYER_VIEWMODEL_ROT := Vector3(0.0, 180.0, 0.0)

var _world
var _camera: Camera3D
var _input_source := Callable()
var _look_yaw := 0.0
var _look_pitch := 0.0
var _look_seeded := false
var _third_person := false
var _crouch := false
var _prone := false
var _avatar: Node3D = null
var _viewmodel: Node3D = null
var _tp_anchor := Vector3.ZERO
var _tp_anchor_valid := false


func setup(world, camera: Camera3D) -> void:
	_world = world
	_camera = camera
	# The player camera never draws the reflection-only body layer: in first
	# person the body lives there for the water mirror alone (see
	# _update_avatar); NovaWater's mirror camera is the one view that keeps it.
	if _camera != null:
		_camera.cull_mask &= ~NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY
	_reset_state()


func teardown() -> void:
	_set_fly_camera_locked(false)
	_release_mouse_capture()
	_clear_models()
	if _camera != null:
		_camera.cull_mask |= NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY
	_world = null
	_camera = null
	_input_source = Callable()
	_reset_state()


func set_input_source(source: Callable) -> void:
	_input_source = source


func set_third_person(enabled: bool) -> void:
	_third_person = enabled


func is_third_person() -> bool:
	return _third_person


func before_world_tick(_delta: float, capture_mouse: bool = false) -> void:
	if not _has_player():
		_set_fly_camera_locked(false)
		_release_mouse_capture()
		_clear_models()
		_look_seeded = false
		return
	_set_fly_camera_locked(true)
	_seed_look_from_world()
	if capture_mouse and Input.get_mouse_mode() != Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	_ensure_models()
	var state := _read_input_state()
	_world.set_local_player_input(
		_bool(state, "forward"),
		_bool(state, "back"),
		_bool(state, "left"),
		_bool(state, "right"),
		_bool(state, "run"),
		_crouch,
		_prone,
		_bool(state, "jump"),
		_look_yaw,
		_look_pitch)


func after_world_tick() -> void:
	if not _has_player():
		_set_fly_camera_locked(false)
		_release_mouse_capture()
		_clear_models()
		return
	_update_player_camera()


# Edge-triggered gameplay keys. F4 toggles first/third person [orig: g_camera_mode
# @ 0xA890C8; view actions 400/402/412 @ 0x49C073; ThirdPersonCamera_Update @0x437af0
# — full 3P camera + torso-bend witness: docs/world/world-wac-ai-re.md §14 (D-INF-11),
# net-re §5.39 2026-07-08 addendum]. C / Z toggle the player's stance
# (crouch / prone), mutually exclusive — the original toggles stance on a key edge
# [orig: stance bits on entity+0x12C; NapiNPServerMsg_HandleStanceChange @0x501c60;
# crouch wins].
func handle_key_input(event: InputEvent, active: bool) -> bool:
	if not active or not _has_player() or not (event is InputEventKey):
		return false
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return false
	if key.keycode == KEY_F4:
		_third_person = not _third_person
		return true
	if key.keycode == KEY_C:
		_crouch = not _crouch
		if _crouch:
			_prone = false
		return true
	if key.keycode == KEY_Z:
		_prone = not _prone
		if _prone:
			_crouch = false
		return true
	return false


# Mouse-look: turn the look yaw (X) and pitch (Y, clamped ±80°). [orig: mouse -> entity
# Yaw@+0x10 / Pitch@+0x14, Input_HandleActionBinding_0 @0x4e1330]. Signs are tunable.
func handle_input(event: InputEvent, active: bool) -> bool:
	if not active or not _has_player() or not (event is InputEventMouseMotion):
		return false
	_seed_look_from_world()
	var mm := event as InputEventMouseMotion
	_look_yaw = _wrap_degrees(_look_yaw + mm.relative.x * PLAYER_MOUSE_SENS_DEG)
	_look_pitch = clampf(_look_pitch - mm.relative.y * PLAYER_MOUSE_SENS_DEG,
		-PLAYER_PITCH_CLAMP_DEG, PLAYER_PITCH_CLAMP_DEG)
	return true


func _reset_state() -> void:
	_look_yaw = 0.0
	_look_pitch = 0.0
	_look_seeded = false
	_third_person = false
	_crouch = false
	_prone = false


func _has_player() -> bool:
	if _world == null:
		return false
	if _world.has_method("is_loaded") and not _world.is_loaded():
		return false
	return _world.has_method("has_local_player") and _world.has_local_player()


func _seed_look_from_world() -> void:
	if _look_seeded or _world == null:
		return
	_look_yaw = _wrap_degrees(float(_world.local_player_yaw_deg()))
	_look_pitch = clampf(float(_world.local_player_pitch_deg()),
		-PLAYER_PITCH_CLAMP_DEG, PLAYER_PITCH_CLAMP_DEG)
	_look_seeded = true


# WASD is the 8-way move relative to the look (W/S forward/back, A/D strafe); Shift
# runs; Space jumps (momentary — the motor jumps once when grounded).
# [orig: Player_PackInputStateToEntity @0x4df450]
func _read_input_state() -> Dictionary:
	if _input_source.is_valid():
		var out = _input_source.call()
		if out is Dictionary:
			return out
	return {
		"forward": Input.is_physical_key_pressed(KEY_W),
		"back": Input.is_physical_key_pressed(KEY_S),
		"left": Input.is_physical_key_pressed(KEY_A),
		"right": Input.is_physical_key_pressed(KEY_D),
		"run": Input.is_physical_key_pressed(KEY_SHIFT),
		"jump": Input.is_physical_key_pressed(KEY_SPACE),
	}


func _bool(state: Dictionary, key: String) -> bool:
	return bool(state.get(key, false))


func _ensure_models() -> void:
	if _world == null:
		return
	if (_avatar == null or not is_instance_valid(_avatar)) and _world.has_method("build_local_player_avatar"):
		_avatar = _world.build_local_player_avatar()
	if (_viewmodel == null or not is_instance_valid(_viewmodel)) and _world.has_method("build_local_player_viewmodel"):
		_viewmodel = _world.build_local_player_viewmodel()


func _clear_models() -> void:
	if _avatar != null and is_instance_valid(_avatar):
		_avatar.queue_free()
	if _viewmodel != null and is_instance_valid(_viewmodel):
		_viewmodel.queue_free()
	_avatar = null
	_viewmodel = null


func _set_fly_camera_locked(locked: bool) -> void:
	if _camera != null and _camera.has_method("set_gameplay_locked"):
		_camera.set_gameplay_locked(locked)


func _release_mouse_capture() -> void:
	if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# Place the camera from the player's authoritative pose. First person: eye = player + 1.0u
# looking along the facing. Third person (F4): behind + above, looking at the player
# [orig: Camera_ComputeThirdPersonView @0x437d10; ThirdPersonCamera_Update @0x437af0]. The
# mission yaw -> Godot forward mirrors the present remap (x,y,z)->(x,z,-y): a mission facing
# yaw faces (sin yaw, cos yaw) -> Godot (sin yaw, 0, -cos yaw), tilted by pitch.
func _update_player_camera() -> void:
	if _world == null or _camera == null:
		return
	var pos: Vector3 = _world.local_player_position()
	var yr := deg_to_rad(_world.local_player_yaw_deg())
	var pr := deg_to_rad(_world.local_player_pitch_deg())
	var forward := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
	var eye := pos + Vector3(0, PLAYER_EYE_HEIGHT, 0)
	if _third_person:
		# Chase camera: the smoothed anchor is the follow target [orig: anchor = Position
		# + CameraOffset, quarter-step ease per 62 Hz tick — ThirdPersonCamera_Update
		# @0x437af0; ours eases per frame], the eye sits back along the look direction
		# pitched up by the orbit default and the rotation re-aims at the anchor
		# [orig: Camera_ComputeThirdPersonView @0x437d10 mode 1].
		if not _tp_anchor_valid:
			_tp_anchor = eye
			_tp_anchor_valid = true
		_tp_anchor += (eye - _tp_anchor) * 0.25
		var opr := pr + deg_to_rad(PLAYER_TP_ORBIT_PITCH_DEG)
		var back := Vector3(sin(yr) * cos(opr), sin(opr), -cos(yr) * cos(opr))
		_camera.global_position = _tp_anchor - back * PLAYER_TP_DISTANCE
		_camera.look_at(_tp_anchor, Vector3.UP)
	else:
		_tp_anchor_valid = false
		_camera.global_position = eye
		_camera.look_at(eye + forward, Vector3.UP)
	_update_avatar(pos)
	_update_viewmodel()


func _update_avatar(pos: Vector3) -> void:
	if _avatar == null or not is_instance_valid(_avatar) or _world == null:
		return
	_avatar.global_position = pos
	# The avatar node carries the BODY frame (the lagged body heading), not the aim yaw:
	# the aim/body split is what the per-segment overlay renders as the torso twist, and
	# the body-class delta is identity by construction so the hips stay glued to the node.
	# [orig: Entity_BuildBoneTransformMatrices @0x4b1290 — every overlay blends toward
	# bodyHeading/bodyPitch; docs/world/world-wac-ai-re.md §14 (D-INF-11)]
	var overlay: Dictionary = _world.local_player_aim_overlay() \
			if _world.has_method("local_player_aim_overlay") else {}
	if bool(overlay.get("valid", false)):
		var body_basis := MissionObjectPlacer.bms_to_godot_basis(overlay["body"])
		_avatar.global_basis = body_basis
		if _avatar.has_method("set_aim_overlay"):
			var inv := body_basis.inverse()
			var deltas: Array = []
			for a in (overlay["angles"] as PackedVector3Array):
				deltas.append(inv * MissionObjectPlacer.bms_to_godot_basis(a))
			_avatar.set_aim_overlay(deltas)
	else:
		_avatar.global_basis = MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0.0, _world.local_player_yaw_deg(), 0.0))
		if _avatar.has_method("set_aim_overlay"):
			_avatar.set_aim_overlay([])
	# The body renders in BOTH modes; first person hides it from the player
	# camera by LAYER, not by visible = false (which would remove it from every
	# camera, the water mirror included). Retail's reflection re-renders the
	# world scene, which CONTAINS the local player's body - the FP arms are a
	# separate overlay pass that never enters it [orig: Water_ReflectionPrerender
	# @ 0x5c2780 -> render_main_scene @ 0x5c1240; the viewmodel pass is
	# Player_RenderFirstPersonViewModel @ 0x4ded60]. setup() masked the
	# reflection-only bit off the player camera; the mirror camera includes it.
	# Stamped every frame: NovaObjectModel.rebuild() recreates its mesh
	# children on the default layer.
	_avatar.visible = true
	_set_visual_layers(_avatar, NovaWater.VISUAL_LAYER_WORLD if _third_person
			else NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY)
	var anim_key := String(_world.local_player_anim_key()) if _world.has_method("local_player_anim_key") else ""
	var anim_phase := int(_world.local_player_anim_phase_ticks()) if _world.has_method("local_player_anim_phase_ticks") else 0
	if not anim_key.is_empty() and _avatar.has_method("play_body_clip_at"):
		_avatar.play_body_clip_at(anim_key, anim_phase)
	elif not anim_key.is_empty() and _avatar.has_method("play_body_clip"):
		_avatar.play_body_clip(anim_key)
	elif _avatar.has_method("play_body_anim_at"):
		_avatar.play_body_anim_at(_world.local_player_body_anim_slot(), anim_phase)
	elif _avatar.has_method("play_body_anim"):
		_avatar.play_body_anim(_world.local_player_body_anim_slot())


# First-person weapon viewmodel: sit it in front of the eye, tracking the camera 1:1,
# shown in first person only (in 3P the body avatar shows instead). The original biases
# the CAMERA by the weapon's `pos`/`tpos` view offset and draws the model at the view
# root [orig: Player_UpdateFirstPersonCamera @0x4dd380]; placing it in camera space is
# the faithful structural equivalent (camera.global_transform == the engine view
# transform here).
func _update_viewmodel() -> void:
	if _viewmodel == null or not is_instance_valid(_viewmodel) or _camera == null:
		return
	var vm_basis := Basis.from_euler(Vector3(
		deg_to_rad(PLAYER_VIEWMODEL_ROT.x),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.y),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.z)))
	_viewmodel.global_transform = _camera.global_transform * Transform3D(
		vm_basis, _viewmodel_offset(PLAYER_VIEWMODEL_POS_UNITS))
	# The FP overlay never enters the water mirror: retail draws it as its own
	# near-Z viewport pass over the finished frame, not as part of the mirrored
	# world scene [orig: Player_RenderFirstPersonViewModel @ 0x4ded60]; hosted,
	# the dedicated layer is what the mirror camera's cull_mask excludes.
	_set_visual_layers(_viewmodel, NovaWater.VISUAL_LAYER_VIEWMODEL)
	_viewmodel.visible = not _third_person


# Stamp `layer_mask` onto every VisualInstance3D under `root` (inclusive).
# VisualInstance3D.layers is per-instance - a container's value does not
# propagate to children - and both player models are NovaObjectModel subtrees
# (mesh instances under Robj/Skeleton3D nodes) whose rebuild() recreates them
# on the default layer, so the callers above re-stamp every frame.
func _set_visual_layers(root: Node, layer_mask: int) -> void:
	if root is VisualInstance3D:
		(root as VisualInstance3D).layers = layer_mask
	for child in root.get_children():
		_set_visual_layers(child, layer_mask)


# Convert a weapon.def `pos`/`tpos` POSITION (raw file units) into a Godot camera-local offset.
# Faithful to the witnessed pipeline [orig: Player_UpdateFirstPersonCamera @0x4dd380; scale
# flt_7D1D70=256 @0x544770]: the camera adds `ftol(Bone.pos)` straight onto g_view_pos, and at a
# level look the view matrix is identity [orig: Math_BuildFixedPointRotationMatrixYXZ @0x615400], so
# component i lands on world axis i (world Z = up). The view-local frame is therefore
# (x = right, y = forward, z = up) — `pos[2]` is the grip's DOWN offset (the dominant term; the barrel
# reaches forward via the model), NOT depth. Godot camera-local is (x right, y up, -z forward), so:
#   file x (right)   -> Godot  x
#   file y (forward) -> Godot -z
#   file z (up)      -> Godot  y      (e.g. MP5SD pos.z -183 -> grip ~0.715u below the eye)
# The two small lateral/forward terms (x, y) are sign-confirmable by drive; the z->y (down) term is
# the certain one. (oscarmike WeaponManager._jo_to_godot_position agrees on /256 + z->up/down.)
func _viewmodel_offset(units: Vector3) -> Vector3:
	return Vector3(
		units.x / WEAPON_DEF_POS_SCALE,
		units.z / WEAPON_DEF_POS_SCALE,
		-units.y / WEAPON_DEF_POS_SCALE)


func _wrap_degrees(degrees: float) -> float:
	var out := fmod(degrees, 360.0)
	if out < 0.0:
		out += 360.0
	return out
