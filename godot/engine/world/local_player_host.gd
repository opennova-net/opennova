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
# Flags & 2), wired when ADS lands. The view fields now flow from the mounted root's weapon.def
# (_apply_viewmodel_def <- GameWorld.local_player_viewmodel_def, the fixed default weapon until
# equipped-weapon resolution lands); the values below are the witnessed JOX WPN_AK47AUTO line,
# kept as the no-def fallback. (The pre-def constant (10, 0, -201) turned out to be the
# REVX-era WPN_AK47AUTO `pos` — that SKU's def drives the AKM_1st viewmodel.)
const WEAPON_DEF_POS_SCALE := 256.0                                # flt_7D1D70: file unit -> /256 world units
# Tunable (vars, not consts) so debug drivers can sweep placements live; the values are
# the witnessed WPN_AK47AUTO def line + the current best facing.
var PLAYER_VIEWMODEL_POS_UNITS := Vector3(-19.46, 21.19, -161.31)  # weapon.def WPN_AK47AUTO `pos` (hip)
# The ADS/sighted view offset (weapon.def `tpos` -> WeaponDef.AltCamOffset @0x10C), held for
# the ADS swap follow-up; JOX AK47AUTO = (-62.33, 29.19, -152.56).
var PLAYER_VIEWMODEL_TPOS_UNITS := Vector3(-62.33, 29.19, -152.56)
# The FP rig's model->camera AXIS MAP, euler DEGREES in CAMERA space. The FP rig is a
# T-posed character skeleton (BN01 Pelvis at the origin) that the wpn clips POSE into the
# hold facing downrange; the rig renders through the standard skeletal pipeline (import-
# flipped meshes + the bind-rotation rests, positions reconstructed from the model table),
# and the yaw-180 turns the rig's authored forward onto Godot's -Z camera forward — the
# structural equivalent of the original drawing its composed render-frame bone matrices
# with the raw view matrix [orig: Player_RenderFirstPersonViewModel @0x4ded60 root = view
# transform; the S*A^T*S copy loops @0x40c4d8..0x40c57c realize the model->render map
# inside the composition]. Sign pinned against retail: the stock/grip anchor bottom-RIGHT
# at the hip idle (the pre-train build renders identically and was retail-confirmed).
var PLAYER_VIEWMODEL_ROT := Vector3(0.0, 180.0, 0.0)
# Witnessed per-weapon view-rotation bias: weapon.def `pos` rotation columns, DEGREES
# (yaw, pitch, roll) ADDED to the view angles — the weapon cant. AK47AUTO = 5.0 / 3.75 / 353.0.
# [orig: Player_UpdateFirstPersonCamera @0x4dd444: rot = view_rot + Def.Bone.rot; parser stores
# degrees -> BAM @0x54471f.] Sign map to Godot camera axes verified visually.
var PLAYER_VIEWMODEL_ROT_BIAS_DEF := Vector3(5.0, 3.75, 353.0)
# The FP render pass: the original draws the viewmodel through its OWN projection — the
# weapon's `renderfov` (HORIZONTAL degrees; every JO weapon.def omits the key, so all use the
# record default 80.0) converted to vertical via the aspect, with the near plane swapped
# 0.2 -> 0.05 and the viewport depth range remapped so the world never overdraws it, then its
# own flush [orig: Player_RenderFirstPersonViewModel @0x4ded60: Render_SwapProjectionNearZ(0.05)
# @0x4dee29 / restore 0.2 @0x4df0aa, fov = WeaponDef+0x148 @0x4dee71 -> h->v conversion in
# Render_SetViewAndProjectionMatrices @0x58d900, depth remap Render_SetViewportDepth01 @0x58a7b0;
# default 80.0 = flt_7D1898 stored by AdmDef_InitEntryDefaults @0x53ff31; parser key 'renderfov'
# @0x54482a]. Hosted as a SubViewport sharing the world, camera cull-masked to the viewmodel
# layer, composited over the finished frame (the depth-remap's visible equivalent).
var PLAYER_VIEWMODEL_RENDERFOV_H_DEG := 80.0
const VIEWMODEL_PASS_NEAR := 0.05

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
# The FP render pass nodes (see PLAYER_VIEWMODEL_RENDERFOV_H_DEG).
var _vm_pass_layer: CanvasLayer = null
var _vm_viewport: SubViewport = null
var _vm_camera: Camera3D = null
var _tp_anchor := Vector3.ZERO
var _tp_anchor_valid := false
# --- the equipped-weapon FSM view + ADS state (net-re §5.62 / §5.41) ----------
# The FSM ticks in the sim; this host feeds trigger input, plays the event clips on
# BOTH viewmodel parts (arms + gun share the animadm), and owns the ADS camera state:
# the eased pos -> tpos swing and the scoped FOV.
var _vm_parts: Array = []           # NovaObjectModel parts under the viewmodel container
var _weapon_play_serial := -1
var _weapon_unscope_serial := -1
var _weapon_rescope_serial := -1
var _weapon_current_action := 0
var _weapon_clip := 0
var _weapon_reserve := 0
var _fire_was_held := false
var _reload_was_down := false
var _scope_was_down := false
# ADS: engaged is the host's g_scopeEngaged mirror [orig: @0x82CE94]; the fraction is
# the camera interp (0 = hip, 1 = sighted; 15 steps at 62.5 Hz
# [orig: CNetPlayerInterp_Setup steps @0x4df36e]). The pos -> tpos swap itself is
# instant in the original's camera once engaged [orig: Player_UpdateFirstPersonCamera
# @0x4dd380 entity Flags & 2]; the interp carries the visible ease.
var _scope_engaged := false
var _scope_fraction := 0.0
var _frame_delta := 0.0
var _camera_saved_fov := -1.0
const SCOPE_EASE_STEPS := 15.0
const PLAYER_CAMERA_FOV_H_DEG := 80.0  # [orig: g_cameraFovDeg @0x26C6848 default 0x500000]
var PLAYER_VIEWMODEL_FLAGS := 0        # def flags (scoped 1 / sighted 2 gate ADS)
var PLAYER_VIEWMODEL_SCOPE_MAG := 0.0  # def scope_max_mag (scoped FOV = 80 / mag)
var PLAYER_VIEWMODEL_CLIPSIZE := 0
# Debug experiments (the F3 overlay's View tab): keep the FP arms drawn in every
# camera mode, and/or draw the player's own body in first person — the "see our
# feet" probe (the §14 aim overlay bends the spine away from the eye, so looking
# down shows your legs; the head/shoulders will clip the near plane until a
# section-hide like the original's bone zeroing @0x4b1f2a is ported).
var debug_force_viewmodel := false
var debug_body_in_first_person := false


func set_debug_force_viewmodel(enabled: bool) -> void:
	debug_force_viewmodel = enabled


func set_debug_body_in_first_person(enabled: bool) -> void:
	debug_body_in_first_person = enabled


func setup(world, camera: Camera3D) -> void:
	_world = world
	_camera = camera
	_camera_saved_fov = camera.fov if camera != null else -1.0
	# The player camera never draws the reflection-only body layer: in first
	# person the body lives there for the water mirror alone (see
	# _update_avatar); NovaWater's mirror camera is the one view that keeps it.
	# It never draws the viewmodel layer either — the FP arms/weapon render
	# through the dedicated renderfov pass built below.
	if _camera != null:
		_camera.cull_mask &= ~(NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY | NovaWater.VISUAL_LAYER_VIEWMODEL)
	# Deferred: the game shell calls setup() from its own _ready, while the root
	# viewport is still mid-scene-setup — a direct add_child into it fails then
	# ("parent busy"), which would leave the viewmodel layer masked off the player
	# camera with NO pass to draw it (an invisible FP viewmodel). The build's own
	# guards make the deferred call a no-op after teardown()/double setup().
	_build_viewmodel_pass.call_deferred()
	_reset_state()


func teardown() -> void:
	_set_fly_camera_locked(false)
	_release_mouse_capture()
	_clear_models()
	_free_viewmodel_pass()
	if _camera != null:
		_camera.cull_mask |= NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY | NovaWater.VISUAL_LAYER_VIEWMODEL
		if _camera_saved_fov > 0.0:
			_camera.fov = _camera_saved_fov
	_world = null
	_camera = null
	_input_source = Callable()
	_reset_state()


# Build the dedicated FP render pass (see PLAYER_VIEWMODEL_RENDERFOV_H_DEG): a SubViewport
# sharing this host's World3D whose camera draws ONLY the viewmodel layer through the
# weapon renderfov projection, composited over the world frame below the HUD (layer 0 —
# the game HUD CanvasLayers sit at 1+). The container ignores the mouse so gameplay
# input passes through.
func _build_viewmodel_pass() -> void:
	if _camera == null or _vm_pass_layer != null or not _camera.is_inside_tree():
		return
	_vm_pass_layer = CanvasLayer.new()
	_vm_pass_layer.name = "ViewmodelPass"
	_vm_pass_layer.layer = 0
	var container := SubViewportContainer.new()
	container.stretch = true
	container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_vm_viewport = SubViewport.new()
	# Render the CAMERA's World3D: the pass re-renders the SAME scene, culled to the
	# viewmodel layer [orig: one scene, second projection + depth window @0x4ded60].
	# Assigned explicitly — this host node may live OUTSIDE the play viewport (ONED
	# play-in-editor), so tree-inherited world/canvas targets would be the editor
	# window's, not the game's.
	_vm_viewport.world_3d = _camera.get_world_3d()
	_vm_viewport.transparent_bg = true
	_vm_viewport.handle_input_locally = false
	_vm_camera = Camera3D.new()
	_vm_camera.cull_mask = NovaWater.VISUAL_LAYER_VIEWMODEL
	_vm_camera.near = VIEWMODEL_PASS_NEAR  # [orig: Render_SwapProjectionNearZ(0.05) @0x4dee29]
	_vm_viewport.add_child(_vm_camera)
	container.add_child(_vm_viewport)
	_vm_pass_layer.add_child(container)
	# Composite INTO the viewport the player camera renders (the play viewport), sized
	# to it via the full-rect container — not into this node's own ancestor viewport.
	_camera.get_viewport().add_child(_vm_pass_layer)


func _free_viewmodel_pass() -> void:
	if _vm_pass_layer != null and is_instance_valid(_vm_pass_layer):
		_vm_pass_layer.queue_free()
	_vm_pass_layer = null
	_vm_viewport = null
	_vm_camera = null


# Track the player camera 1:1 and rebuild the witnessed projection: renderfov is a
# HORIZONTAL fov in degrees, converted to Godot's vertical fov through the live aspect
# [orig: Render_SetViewAndProjectionMatrices @0x58d900 fovY = 2*atan(tan(fovX/2)/aspect)].
func _update_viewmodel_pass() -> void:
	if _vm_camera == null or _camera == null:
		return
	_vm_camera.global_transform = _camera.global_transform
	_vm_camera.far = _camera.far
	_vm_camera.attributes = _camera.attributes
	_vm_camera.environment = _camera.environment
	var size := _vm_viewport.size
	if size.x > 0 and size.y > 0:
		var aspect := float(size.x) / float(size.y)
		var half_h := deg_to_rad(PLAYER_VIEWMODEL_RENDERFOV_H_DEG) * 0.5
		_vm_camera.fov = rad_to_deg(2.0 * atan(tan(half_h) / aspect))


func set_input_source(source: Callable) -> void:
	_input_source = source


func set_third_person(enabled: bool) -> void:
	_third_person = enabled


func is_third_person() -> bool:
	return _third_person


func before_world_tick(_delta: float, capture_mouse: bool = false) -> void:
	_frame_delta = _delta
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
	_send_weapon_input()


# The weapon trigger input: LMB fire (held + edge), R reload (edge, refused on a full
# magazine or empty reserve), RMB the ADS toggle (edge). Only while the mouse is
# captured - UI clicks never fire. [orig: the binding dispatch cases 0x95 fire /
# 0xD3 reload / 6 scope, Input_HandleActionBinding_0 @0x4e0420]
func _send_weapon_input() -> void:
	if _world == null or not _world.has_method("set_local_player_weapon_input"):
		return
	var captured := Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED
	var fire_held := captured and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	var fire_edge := fire_held and not _fire_was_held
	_fire_was_held = fire_held
	var reload_down := captured and Input.is_physical_key_pressed(KEY_R)
	var reload_edge := reload_down and not _reload_was_down
	_reload_was_down = reload_down
	# [orig: reload case 0xD3 refuses when clip == clipsize or the reserve is empty]
	if reload_edge and (PLAYER_VIEWMODEL_CLIPSIZE <= 0
			or _weapon_clip == PLAYER_VIEWMODEL_CLIPSIZE or _weapon_reserve <= 0):
		reload_edge = false
	var scope_down := captured and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
	if scope_down and not _scope_was_down:
		_toggle_scope()
	_scope_was_down = scope_down
	_world.set_local_player_weapon_input(fire_held, fire_edge, reload_edge, _scope_engaged)


# The ADS toggle [orig: input case 6 @0x4e0420 gates currentAction not in {RELOAD,
# SWITCHFROM}; Player_ToggleWeaponScope @0x4df0c0 gates def Flags & 3 (scoped/sighted)].
# Engaging queues the scopeup FSM state; disengaging scopedown
# [orig: WeaponSlot_TryQueueScopeUp @0x53f050 / ..ScopeDown @0x53f080].
func _toggle_scope() -> void:
	if _weapon_current_action == 4 or _weapon_current_action == 7:
		return
	if (PLAYER_VIEWMODEL_FLAGS & 3) == 0:
		return
	_scope_engaged = not _scope_engaged
	if _world.has_method("queue_local_player_weapon_scope"):
		_world.queue_local_player_weapon_scope(_scope_engaged)


func after_world_tick() -> void:
	if not _has_player():
		_set_fly_camera_locked(false)
		_release_mouse_capture()
		_clear_models()
		return
	_consume_weapon_view()
	_update_player_camera()


# Drain the FSM's per-tick events (monotonic serials - several 62.5 Hz ticks can run
# per frame): clip starts land on BOTH viewmodel parts, forced unscope (one-shot /
# reload stash) and the pump's rescope-after-reload drive the host scope state.
# [orig: ActionSlot_BeginActivePhase @0x53f830 plays the action clip on the owner's
# animadm channel; the rescope block @0x54139e]
func _consume_weapon_view() -> void:
	if _world == null or not _world.has_method("local_player_weapon_view"):
		return
	var view: PlayerWeaponView = _world.local_player_weapon_view()
	if view == null:
		return
	_weapon_current_action = view.current_action
	_weapon_clip = view.clip
	_weapon_reserve = view.reserve
	if view.play_serial != _weapon_play_serial:
		_weapon_play_serial = view.play_serial
		_play_viewmodel_clip(view.anim_key)
	if _weapon_unscope_serial < 0 and _weapon_rescope_serial < 0:
		# First snapshot for these parts (cursors reset to -1): ADOPT the scope
		# serials without treating them as edges — the sim's counters start at 0,
		# and a phantom "rescope" here would auto-ADS a sighted weapon at spawn.
		# (The play cursor above intentionally DOES fire: fresh parts need the
		# FSM's active clip restarted on them.)
		_weapon_unscope_serial = view.unscope_serial
		_weapon_rescope_serial = view.rescope_serial
		return
	if view.unscope_serial != _weapon_unscope_serial:
		_weapon_unscope_serial = view.unscope_serial
		_scope_engaged = false
	if view.rescope_serial != _weapon_rescope_serial:
		_weapon_rescope_serial = view.rescope_serial
		if (PLAYER_VIEWMODEL_FLAGS & 3) != 0:
			_scope_engaged = true
			if _world.has_method("queue_local_player_weapon_scope"):
				_world.queue_local_player_weapon_scope(true)


# Start an FSM clip on every viewmodel part (arms + gun share the animadm) - a replay
# of the active key restarts it (fire/recoil re-triggers), unlike play_body_clip's
# same-key resume.
func _play_viewmodel_clip(key: String) -> void:
	if key.is_empty():
		return
	for part in _vm_parts:
		if part == null or not is_instance_valid(part):
			continue
		part.play_body_clip(key)
		if part.has_method("set_animation_time"):
			part.set_animation_time(0.0)


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
		if _viewmodel != null:
			_apply_viewmodel_def()
			_vm_parts.clear()
			for child in _viewmodel.get_children():
				if child.has_method("play_body_clip"):
					_vm_parts.append(child)
			_weapon_play_serial = -1  # re-sync the FSM event serials to the new parts
			_weapon_unscope_serial = -1
			_weapon_rescope_serial = -1


func _clear_models() -> void:
	if _avatar != null and is_instance_valid(_avatar):
		_avatar.queue_free()
	if _viewmodel != null and is_instance_valid(_viewmodel):
		_viewmodel.queue_free()
	_avatar = null
	_viewmodel = null
	_vm_parts.clear()
	_scope_engaged = false
	_scope_fraction = 0.0
	if _camera != null and _camera_saved_fov > 0.0:
		_camera.fov = _camera_saved_fov


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
	_update_scope_camera()
	_update_avatar(pos)
	_update_viewmodel()


# The ADS camera: step the 15-tick scope ease toward engaged/hip and drive the main
# camera FOV — 80 horizontal base [orig: g_cameraFovDeg @0x26C6848 default 0x500000],
# scoped = 80 / zoom for sighted weapons [orig: Player_ToggleWeaponScope @0x4df401 ->
# 80.0 / Player_GetClampedWeaponElevation], suppressed in third person
# [orig: @0x4df3fa g_camera_mode -> 80.0]. Horizontal -> vertical through the live
# aspect, the same conversion as the render setup [orig: @0x58d900].
func _update_scope_camera() -> void:
	var target := 1.0 if _scope_engaged else 0.0
	_scope_fraction = move_toward(_scope_fraction, target, _frame_delta * 62.5 / SCOPE_EASE_STEPS)
	if _camera == null:
		return
	var mag := 1.0
	if (PLAYER_VIEWMODEL_FLAGS & 2) != 0 and PLAYER_VIEWMODEL_SCOPE_MAG > 1.0:
		mag = PLAYER_VIEWMODEL_SCOPE_MAG
	var zoom_fraction := 0.0 if _third_person else _scope_fraction
	var fov_h := lerpf(PLAYER_CAMERA_FOV_H_DEG, PLAYER_CAMERA_FOV_H_DEG / mag, zoom_fraction)
	var viewport := _camera.get_viewport()
	if viewport == null:
		return
	var size := viewport.get_visible_rect().size
	if size.x <= 0.0 or size.y <= 0.0:
		return
	var aspect := size.x / size.y
	var half_h := deg_to_rad(fov_h) * 0.5
	_camera.fov = rad_to_deg(2.0 * atan(tan(half_h) / aspect))


func _update_avatar(pos: Vector3) -> void:
	if _avatar == null or not is_instance_valid(_avatar) or _world == null:
		return
	_avatar.global_position = pos
	# The avatar node carries the BODY frame (the lagged body heading), not the aim yaw:
	# the aim/body split is what the per-segment overlay renders as the torso twist, and
	# the body-class delta is identity by construction so the hips stay glued to the node.
	# [orig: Entity_BuildBoneTransformMatrices @0x4b1290 — every overlay blends toward
	# bodyHeading/bodyPitch; docs/world/world-wac-ai-re.md §14 (D-INF-11)]
	var overlay: PlayerAimOverlay = _world.local_player_aim_overlay() \
			if _world.has_method("local_player_aim_overlay") else null
	if overlay != null:
		var body_basis := MissionObjectPlacer.bms_to_godot_basis(overlay.body_angles)
		_avatar.global_basis = body_basis
		if _avatar.has_method("set_aim_overlay"):
			var inv := body_basis.inverse()
			var deltas: Array = []
			for a in overlay.segment_angles:
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
	_set_visual_layers(_avatar, NovaWater.VISUAL_LAYER_WORLD
			if (_third_person or debug_body_in_first_person)
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
	# The engine draws the FP model with the RAW VIEW MATRIX as its world transform, i.e. the
	# model lives in VIEW space [orig: Player_RenderFirstPersonViewModel @0x4ded60]. So the
	# viewmodel is parented to the CAMERA transform (view-relative), NOT oriented in world space —
	# a viewmodel must stay fixed to the view, not swing with the aim. PLAYER_VIEWMODEL_ROT lays
	# the model's forward down-range (and picks the correct handedness so a right-handed weapon
	# sits on the right); PLAYER_VIEWMODEL_POS_UNITS is the weapon.def `pos` view offset.
	# camera x bias(def rot, about the eye in view axes) x axis map(rig -> camera).
	# [orig: Player_UpdateFirstPersonCamera @0x4dd380 adds Def.Bone.rot to the view angles and
	# rotates Def.Bone.pos into view orientation before adding to the eye.]
	var b := PLAYER_VIEWMODEL_ROT_BIAS_DEF
	var bias := Basis.from_euler(Vector3(
		deg_to_rad(_wrap180(b.y)),    # their pitch -> Godot x
		deg_to_rad(_wrap180(b.x)),    # their yaw   -> Godot y
		deg_to_rad(-_wrap180(b.z))))  # their roll  -> Godot z (opposite sense)
	var vm_basis := bias * Basis.from_euler(Vector3(
		deg_to_rad(PLAYER_VIEWMODEL_ROT.x),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.y),
		deg_to_rad(PLAYER_VIEWMODEL_ROT.z)))
	# The ADS pos -> tpos swap: instant once sighted in the original's camera
	# [orig: Player_UpdateFirstPersonCamera @0x4dd380, entity Flags & 2 -> AltCamOffset],
	# with the visible ease carried by the 15-step scope-camera interp
	# [orig: CNetPlayerInterp_Setup @0x4df36e / g_fpCameraInterp @0x82CE40] - realized
	# here as the eased blend toward the tpos view bias.
	var view_units := PLAYER_VIEWMODEL_POS_UNITS.lerp(PLAYER_VIEWMODEL_TPOS_UNITS, _scope_fraction)
	_viewmodel.global_transform = _camera.global_transform * Transform3D(
		vm_basis, bias * _viewmodel_offset(view_units))
	# The FP overlay never enters the water mirror OR the main camera: retail draws it
	# as its own renderfov/near-Z pass over the finished frame [orig:
	# Player_RenderFirstPersonViewModel @ 0x4ded60]; hosted, the dedicated layer is drawn
	# only by the pass camera (and excluded by the mirror camera's cull_mask).
	_set_visual_layers(_viewmodel, NovaWater.VISUAL_LAYER_VIEWMODEL)
	_viewmodel.visible = (not _third_person) or debug_force_viewmodel
	_update_viewmodel_pass()


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


# Pull the resolved weapon.def view record from the world; null when no weapon.def (or the
# weapon) resolves in the mounted root — the witnessed JOX WPN_AK47AUTO constants above stay
# in force. The def rows carry xyz raw file units + yaw/pitch/roll degrees
# [orig: weapon.def 'pos'/'tpos' handlers @0x54471f; 'renderfov' @0x54482a, default 80.0].
func _apply_viewmodel_def() -> void:
	if _world == null or not _world.has_method("local_player_viewmodel_def"):
		return
	var def: PlayerViewmodelDef = _world.local_player_viewmodel_def()
	if def == null:
		return
	PLAYER_VIEWMODEL_POS_UNITS = def.pos_units
	PLAYER_VIEWMODEL_ROT_BIAS_DEF = def.rot_bias_deg
	PLAYER_VIEWMODEL_TPOS_UNITS = def.tpos_units
	PLAYER_VIEWMODEL_RENDERFOV_H_DEG = def.renderfov_h_deg
	PLAYER_VIEWMODEL_FLAGS = def.flags
	PLAYER_VIEWMODEL_SCOPE_MAG = def.scope_max_mag
	PLAYER_VIEWMODEL_CLIPSIZE = def.clipsize


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


# Fold degrees into (-180, 180] (def rot columns store e.g. 353 for -7).
func _wrap180(degrees: float) -> float:
	var out := fmod(degrees + 180.0, 360.0)
	if out < 0.0:
		out += 360.0
	return out - 180.0


func _wrap_degrees(degrees: float) -> float:
	var out := fmod(degrees, 360.0)
	if out < 0.0:
		out += 360.0
	return out
