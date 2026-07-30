class_name LocalPlayerHost
extends Node

const MissionRuntime := preload("res://engine/world/mission_runtime.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

# Faithful first-person camera. The on-foot eye is Position + CameraOffset, where
# the local player's CameraOffset is the POSED HEAD BONE minus Position — the eye
# follows the animation (stand/crouch/prone/jump all move it) [orig: the local bone
# path @0x4b6bb3 stores head−Position into CameraOffset(+0x6C); the on-foot person
# camera leg adds it @0x437f9c]. +1.0 is the witnessed NON-person fallback bump
# [orig: @0x437e8f], kept for the no-skeleton case. F4 swaps to the mode-1 chase
# camera [orig: ThirdPersonCamera_Update @0x437af0]. Mouse look is SIM-owned:
# raw pixel deltas feed NovaSimulation.add_local_player_look (the witnessed integer
# pipeline — sensitivity<<11, scoped zoom reduction, ±80° pitch clamp with the +40°
# up-limit while prone) [orig: Input_ProcessMouseAxisBindings @0x499680].
const PLAYER_EYE_HEIGHT := 1.0          # the non-person +0x10000 bump [orig: @0x437e8f]
const PLAYER_EYE_MIN := 0.125           # CameraOffset.z floor 0x2000 [orig: @0x4b6b98]
# The head is bone INDEX 14 (.bad row "BN15 Head") — the rig is index-driven and the
# model bone order IS the BN order [world-wac-ai-re §14.2]; the original reads the
# head row of its bone-matrix array, never a name [orig: the local bone path @0x4b6bb3].
const PLAYER_HEAD_BONE_INDEX := 14
# The witnessed FP eye pull-back: after adding CameraOffset the eye moves -0x3000
# (0.1875u) along the view FORWARD axis, through the full view rotation (roll
# included) [orig: @0x438001..0x438031 — Math_FixedPointTransformPoint22 of
# (-0x3000, 0, 0) added onto g_view_pos].
const PLAYER_EYE_PULLBACK := 0.1875
# Witnessed chase-camera numbers. The in-play state is the ROUND-START RESET —
# distance 1.0, orbit yaw 0, orbit pitch 0 [orig: Camera_ResetToLocalPlayer
# @0x4a3d30 (distance 0x10000 @0x4a3d4c, orbit zeroed @0x4a3d56/5b), called from
# Game_StartMission @0x525c54 and Game_InitNewRound @0x4227a2] — the tight
# over-the-shoulder view. The 3.0 / 5.625 deg pair is only the tracked-entity-
# CHANGE seed (kill-cam/spectate retarget) [orig: Camera_SetTrackedEntity
# @0x439213/@0x43921d]. The eye is anchor + R(yaw + orbit_yaw, pitch +
# orbit_pitch)*(-dist, 0, 0) with the SAME angles as the view rotation (roll 0)
# [orig: Camera_ComputeThirdPersonView @0x438100..0x438171, offset @0x4383e2];
# the anchor chases Position + CameraOffset (the head-bone eye) quarter-step per
# tick [orig: ThirdPersonCamera_Update @0x437b70/@0x437c8d]. Orbit keys (view
# bits 0x10/0x40 -> orbit_yaw ±0x1000000/tick @0x437c1b), the zoom keys (view
# actions 409/410: dist -/+= max(dist>>6, 0x800), clamped [0.5, 512]
# @0x49c1c5..0x49c23f), the march's bone-collision FORCES (@0x4382d9; its
# no-collision landing IS ported — see tp_effective_distance), and the
# dead-target 10.0 -> 3.0 distance ease @0x437cc0 are tracked deferrals
# (net-re section 5.39).
const PLAYER_TP_DISTANCE := 1.0          # [orig: the reset 0x10000 @0x4a3d4c]
const PLAYER_TP_ORBIT_PITCH_DEG := 0.0   # [orig: the reset zero @0x4a3d5b]
const PLAYER_TP_PIVOT_NUDGE := 0.125     # [orig: R*(0x2000,0x2000,0x2000) @0x43818a]
const PLAYER_TP_MARCH_STEP := 0.25       # [orig: 0.25u march steps @0x438243]


# The collision march's NO-COLLISION landing [orig: @0x438213..0x43832e]: under
# 8.0u the eye marches back in 0.25u steps for stepIndex 1..numSteps-1
# (numSteps = floor(dist/0.25)) and stays on the LAST step — (numSteps-1)*0.25,
# never the full distance (numSteps <= 1 skips the march and leaves the eye at
# the pivot @0x43821f). The reset distance 1.0 therefore lands the retail
# on-foot camera 0.75u back. The per-step bone collision FORCES (the actual
# obstruction pull-in, min back-off 0.25 @0x4383ca) stay deferred.
static func tp_effective_distance(dist: float) -> float:
	if dist >= 8.0:  # [orig: the march gate @0x4381e9]
		return dist
	var steps := int(dist / PLAYER_TP_MARCH_STEP)
	if steps <= 1:
		return 0.0
	return float(steps - 1) * PLAYER_TP_MARCH_STEP
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
# Flags & 2), eased by the sim's scope fraction. The view fields flow from the mounted root's weapon.def
# (_apply_viewmodel_def <- GameWorld.local_player_viewmodel_def, the fixed default weapon until
# equipped-weapon resolution lands); the values below are the witnessed JOX WPN_AK47AUTO line,
# kept as the no-def fallback. (The pre-def constant (10, 0, -201) turned out to be the
# REVX-era WPN_AK47AUTO `pos` — that SKU's def drives the AKM_1st viewmodel.)
const WEAPON_DEF_POS_SCALE := 256.0                                # flt_7D1D70: file unit -> /256 world units
# Tunable (vars, not consts) so debug drivers can sweep placements live; the values are
# the witnessed WPN_AK47AUTO def line + the current best facing.
var PLAYER_VIEWMODEL_POS_UNITS := Vector3(-19.46, 21.19, -161.31)  # weapon.def WPN_AK47AUTO `pos` (hip)
# The ADS/sighted view offset (weapon.def `tpos` -> WeaponDef.AltCamOffset @0x10C), blended
# in by the sim's scope fraction; JOX AK47AUTO = (-62.33, 29.19, -152.56).
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
var _third_person := false
var _avatar: Node3D = null
var _held_weapon: Node3D = null      # the 3P gun; a SIBLING of _avatar (see GameWorld)
var _held_weapon_graphic := ""      # the gfx3 the live node was built from
var _viewmodel: Node3D = null
# The FP render pass nodes (see PLAYER_VIEWMODEL_RENDERFOV_H_DEG).
var _vm_pass_layer: CanvasLayer = null
var _vm_viewport: SubViewport = null
var _vm_camera: Camera3D = null
# --- the equipped-weapon FSM view + the sim-owned view state (net-re §5.62/§5.41) --
# The FSM and the VIEW STATE both tick in the sim at 62.5 Hz (libs/world
# weapon_fsm + player_view; ADR 0016 — policy, state, and cadence live in the
# engine): the ADS engaged bit + 15-step ease, the fov policy, and the 3P anchor
# chase arrive as a PlayerLocalView snapshot each frame. This host samples raw
# input (trigger edges, the RMB toggle REQUEST) and places nodes; the weapon
# EVENT presentation — the batch consume, the FSM event clips on BOTH viewmodel
# parts (arms + gun share the animadm), the muzzle/shell userpoint resolution,
# and the owner-bound effect anchors — lives in PlayerWeaponEffects.
var _vm_parts: Array = []           # NovaObjectModel parts under the viewmodel container
var _weapon_effects: PlayerWeaponEffects = null
var _fire_was_held := false
var _reload_was_down := false
var _scope_was_down := false
# The manual weapon-switch keys — the retail defaults from the shipped binding
# catalog: rows 28-36 Knife '1' / Secondary '2' / Primary '3' / Flashbang '4' /
# FragGrenade '5' / SmokeGrenade '6' / Accessory '7' / Detonator '8' / medpack '9'
# fire the category actions 201-209 (categories 1..9), rows 39/40 cycleweaponP '['
# / cycleweaponN ']' cycle prev/next [orig: input cases 200-210 @ 0x4e1144 ->
# Player_SwitchToWeaponByHandle((action-200)*65); cases 212/214 ->
# Player_CycleWeaponSlot @ 0x4dfe70; libs/controls k_catalog rows].
const _WEAPON_CATEGORY_KEYS: Array[Key] = [KEY_1, KEY_2, KEY_3, KEY_4, KEY_5,
		KEY_6, KEY_7, KEY_8, KEY_9]
var _category_was_down := 0
var _cycle_prev_was_down := false
var _cycle_next_was_down := false
var _view: PlayerLocalView = null   # the sim's per-tick view snapshot (null = no sim)
var _camera_saved_fov := -1.0
# Debug experiments (the F3 overlay's Player page): keep the FP arms drawn in every
# camera mode, and/or draw the player's own body in first person — the "see our
# feet" probe (the §14 aim overlay bends the spine away from the eye, so looking
# down shows your legs; the head/shoulders will clip the near plane until a
# section-hide like the original's bone zeroing @0x4b1f2a is ported).
var debug_force_viewmodel := false
var debug_body_in_first_person := false
var _camera_saved_cull_mask := -1


# The sim, re-resolved per use: mission reloads free the runtime and its sim,
# so a cached reference would go stale (the debug views follow the same rule).
# Untyped: GUT harness worlds serve value-only sim doubles.
func _sim():
	return _world.get_sim() if _world != null else null


func set_debug_force_viewmodel(enabled: bool) -> void:
	debug_force_viewmodel = enabled


func set_debug_body_in_first_person(enabled: bool) -> void:
	debug_body_in_first_person = enabled


func setup(world, camera: Camera3D) -> void:
	_world = world
	_camera = camera
	# The weapon-event presentation lives beside the host for the same setup ->
	# teardown span; it resolves userpoints against this host's live nodes.
	_weapon_effects = PlayerWeaponEffects.new()
	_weapon_effects.setup(world, self)
	_camera_saved_fov = camera.fov if camera != null else -1.0
	_camera_saved_cull_mask = camera.cull_mask if camera != null else -1
	# The player camera never draws the reflection-only body layer: in first
	# person the body lives there for the water mirror alone (see
	# _update_avatar); NovaWater's mirror camera is the one view that keeps it.
	# It never draws the viewmodel layer either — the FP arms/weapon render
	# through the dedicated renderfov pass built below.
	if _camera != null:
		_camera.cull_mask &= ~(
				NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY
				| NovaWater.VISUAL_LAYER_VIEWMODEL
				| NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK)
	# Deferred: the game shell calls setup() from its own _ready, while the root
	# viewport is still mid-scene-setup — a direct add_child into it fails then
	# ("parent busy"), which would leave the viewmodel layer masked off the player
	# camera with NO pass to draw it (an invisible FP viewmodel). The build's own
	# guards make the deferred call a no-op after teardown()/double setup().
	_build_viewmodel_pass.call_deferred()
	_reset_state()
	# Attachment is the adoption boundary: discard presentation history produced
	# before this host existed. Every event produced after setup is live, including
	# a first-tick shot before the first active snapshot is presented.
	if _world != null:
		_world.drain_local_player_weapon_events()
		_world.set_local_player_weapon_tick_consumer(
				Callable(self, "_present_fixed_weapon_tick"))


func teardown() -> void:
	if _world != null:
		_world.set_local_player_weapon_tick_consumer(Callable())
	_set_fly_camera_locked(false)
	_release_mouse_capture()
	_clear_models()
	_free_viewmodel_pass()
	if _camera != null:
		if _camera_saved_cull_mask >= 0:
			_camera.cull_mask = _camera_saved_cull_mask
		if _camera_saved_fov > 0.0:
			_camera.fov = _camera_saved_fov
	if _weapon_effects != null:
		_weapon_effects.teardown()
	_weapon_effects = null
	_world = null
	_camera = null
	_camera_saved_cull_mask = -1
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


## Drop the built FP viewmodel so the next update pass rebuilds gun/arms/FSM from the
## (changed) equipped weapon — the armory ACCEPT re-mount [orig:
## WeaponLoadout_ApplyFromBuffer @0x565cd0 tail -> Player_MountWeaponSlot @0x4dfa40].
func refresh_viewmodel() -> void:
	if _weapon_effects != null:
		_weapon_effects.on_viewmodel_refresh()
	if _viewmodel != null and is_instance_valid(_viewmodel):
		_viewmodel.queue_free()
	_viewmodel = null


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
		_vm_camera.fov = NovaSimulation.fov_vertical_from_horizontal(
				PLAYER_VIEWMODEL_RENDERFOV_H_DEG, float(size.x) / float(size.y))


func set_input_source(source: Callable) -> void:
	_input_source = source


func set_third_person(enabled: bool) -> void:
	_third_person = enabled
	_sync_camera_mode()


# The sim owns the camera-mode-dependent view state (fov suppression + the 3P
# anchor chase) [orig: g_camera_mode @0xA890C8]; tell it whenever the mode flips.
func _sync_camera_mode() -> void:
	var sim = _sim()
	if sim != null:
		sim.set_local_player_camera_third_person(_third_person)


func is_third_person() -> bool:
	return _third_person


# W4-2-style justified accessors: PlayerWeaponEffects resolves action userpoints
# and effect anchors against the host-OWNED presentation nodes (the FP viewmodel
# parts, the 3P gun, the camera). These expose exactly the state it reads, so no
# cross-object _private access crosses the seam; the nodes stay host-owned.
func vm_parts() -> Array:
	return _vm_parts


func viewmodel() -> Node3D:
	return _viewmodel


func held_weapon() -> Node3D:
	return _held_weapon


func camera() -> Camera3D:
	return _camera


func before_world_tick(_delta: float, capture_mouse: bool = false,
		gameplay_input_active: bool = true) -> void:
	if not _has_player():
		_set_fly_camera_locked(false)
		_release_mouse_capture()
		_clear_models()
		return
	_set_fly_camera_locked(true)
	if capture_mouse and Input.get_mouse_mode() != Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	_ensure_models()
	# A live UI overlay keeps the world ticking but must actively submit a neutral
	# movement frame. Skipping this call leaves the sim holding its previous input,
	# so a player who opened the armory while running would keep running under it.
	var state := _read_input_state() if gameplay_input_active else {}
	var sim = _sim()
	if sim != null:
		sim.set_player_input(
			_bool(state, "forward"),
			_bool(state, "back"),
			_bool(state, "left"),
			_bool(state, "right"),
			_bool(state, "lean_left"),
			_bool(state, "lean_right"),
			_bool(state, "jump"))
		# Feed the sim the head-bone eye for the 3P anchor chase [orig: the chase target
		# is Position + CameraOffset @0x437b70; CameraOffset is the posed head bone,
		# computed sim-side in the original @0x4b6bb3 — hosted, the render skeleton is
		# the sample source (D-INF-18)].
		var head := _avatar_head_world()
		sim.set_local_player_eye(head if head != Vector3.INF else Vector3.ZERO,
				head != Vector3.INF)
	_send_weapon_input()


# The weapon trigger input: LMB fire (held + edge), R reload (raw edge — the
# full-magazine/empty-reserve refusal is the SIM's dispatch gate), RMB the ADS
# toggle REQUEST (the sim gates it and owns the engaged state). Only while the
# mouse is captured - UI clicks never fire. [orig: the binding dispatch cases
# 0x95 fire / 0xD3 reload / 6 scope, Input_HandleActionBinding_0 @0x4e0420 —
# ported in libs/world weapon_fsm + NovaSimulation]
func _send_weapon_input() -> void:
	var sim = _sim()
	var captured := Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED
	var fire_held := captured and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	var fire_edge := fire_held and not _fire_was_held
	_fire_was_held = fire_held
	var reload_down := captured and Input.is_physical_key_pressed(KEY_R)
	var reload_edge := reload_down and not _reload_was_down
	_reload_was_down = reload_down
	var scope_down := captured and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
	if scope_down and not _scope_was_down and sim != null:
		sim.request_local_player_scope_toggle()
	_scope_was_down = scope_down
	# Latches update even with no sim (the deleted forwarders no-op'd downstream):
	# a key held across a mission reload must not fire a spurious edge on the
	# first frame the new sim appears.
	if sim != null:
		sim.set_local_player_weapon_input(fire_held, fire_edge, reload_edge)
	_send_weapon_switch_input(captured)


# The category keys (1..9) and the cycle pair ('['/']'): edge-triggered requests
# into the sim's switch walks; the sim applies the witnessed stance/FSM gates and
# answers through the event drain (switch_to_weapon / switch_denied).
func _send_weapon_switch_input(captured: bool) -> void:
	var sim = _sim()
	var down_mask := 0
	for i in _WEAPON_CATEGORY_KEYS.size():
		if captured and Input.is_physical_key_pressed(_WEAPON_CATEGORY_KEYS[i]):
			down_mask |= 1 << i
			if (_category_was_down & (1 << i)) == 0 and sim != null:
				sim.request_local_player_weapon_category(i + 1)
	_category_was_down = down_mask
	var prev_down := captured and Input.is_physical_key_pressed(KEY_BRACKETLEFT)
	if prev_down and not _cycle_prev_was_down and sim != null:
		sim.request_local_player_weapon_cycle(-1)
	_cycle_prev_was_down = prev_down
	var next_down := captured and Input.is_physical_key_pressed(KEY_BRACKETRIGHT)
	if next_down and not _cycle_next_was_down and sim != null:
		sim.request_local_player_weapon_cycle(1)
	_cycle_next_was_down = next_down


func after_world_tick() -> void:
	if not _has_player():
		_set_world_nvg_view(false, 0)
		_set_fly_camera_locked(false)
		_release_mouse_capture()
		_clear_models()
		if _world != null:
			_world.drain_local_player_weapon_events()
		if _weapon_effects != null:
			_weapon_effects.reset()
		_view = null
		return
	_view = _world.local_player_view()
	_set_world_nvg_view(_view != null and _view.nvg_visible,
			_view.nvg_gain if _view != null else 0)
	# Place the camera/viewmodel root for THIS tick before consuming one-shot
	# presentation events. On the first live tick the freshly built model is still
	# at its default transform; on later ticks it otherwise trails movement/look by
	# one frame. Pre-adopt the weapon snapshot so the avatar's body channel remains
	# current while _update_player_camera() stamps all visual roots.
	var weapon_view: PlayerWeaponView = _world.local_player_weapon_view()
	_weapon_effects.set_weapon_view(weapon_view)
	_update_player_camera()
	_weapon_effects.consume_pending(weapon_view)


# Present one simulation tick's weapon batch before EffectWorld advances that
# same tick. Updating only the local camera/avatar/viewmodel here gives action
# user points their production-tick pose; mission/vehicle Nodes retain the
# render-frame-batched present path that prevents 00TRa transform flicker.
func _present_fixed_weapon_tick(events: Array[PlayerWeaponEvent]) -> void:
	if not _has_player():
		return
	var weapon_view: PlayerWeaponView = _world.local_player_weapon_view()
	if events.is_empty():
		# Keep the active clip at this tick's exact pose, but defer the expensive
		# camera/avatar/viewmodel-root presentation to after the catch-up batch.
		_weapon_effects.consume(weapon_view, events, true)
		return
	_view = _world.local_player_view()
	_weapon_effects.set_weapon_view(weapon_view)
	_update_player_camera()
	_weapon_effects.consume(weapon_view, events, true)


# Edge-triggered gameplay keys. F4 toggles first/third person [orig: g_camera_mode
# @ 0xA890C8; view actions 400/402/412 @ 0x49C073; ThirdPersonCamera_Update @0x437af0
# — full 3P camera + torso-bend witness: docs/world/world-wac-ai-re.md §14 (D-INF-11),
# net-re §5.39 2026-07-08 addendum]. Stance is the witnessed 3-key SELECT — Z prone,
# X crouch, C stand (catalog ids 9/10/11, defaults Z/X/C) — each key REQUESTS its
# stance from the sim, which applies the mutual exclusion and the ForceCrouch
# refusal (the C2S 0x1D semantics). [orig: input cases 170/169/172 @0x4e0df3/
# @0x4e0d77/@0x4e0e3e -> NapiNPServerMsg_HandleStanceChange @0x501c60]
func handle_key_input(event: InputEvent, active: bool) -> bool:
	if not active or not _has_player() or not (event is InputEventKey):
		return false
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return false
	if key.keycode == KEY_F4:
		_third_person = not _third_person
		_sync_camera_mode()
		return true
	var physical := key.physical_keycode if key.physical_keycode != 0 else key.keycode
	var sim = _sim()
	if physical == KEY_B:
		if sim != null:
			sim.request_local_player_binoculars_toggle()
		return true
	if physical == KEY_N:
		if sim != null:
			sim.request_local_player_nvg_toggle()
		return true
	if physical == KEY_EQUAL or physical == KEY_PLUS or physical == KEY_KP_ADD:
		if sim != null:
			sim.request_local_player_nvg_gain(1)
		return true
	if physical == KEY_MINUS or physical == KEY_KP_SUBTRACT:
		if sim != null:
			sim.request_local_player_nvg_gain(-1)
		return true
	if key.keycode == KEY_Z:
		_request_stance(2)  # prone [orig: case 170 sends 0xAA]
		return true
	if key.keycode == KEY_X:
		_request_stance(1)  # crouch [orig: case 169 sends 0xA9]
		return true
	if key.keycode == KEY_C:
		_request_stance(0)  # stand [orig: case 172 sends 0xAC]
		return true
	return false


func _request_stance(stance: int) -> void:
	var sim = _sim()
	if sim != null:
		sim.request_local_player_stance(stance)


# Mouse-look: raw pixel deltas into the SIM's witnessed integer pipeline (the sim
# owns sensitivity, the scoped zoom reduction, Y-invert, and the pitch clamps).
# [orig: Input_ProcessMouseAxisBindings @0x499680 -> the axis cases 166/164]
func handle_input(event: InputEvent, active: bool) -> bool:
	if not active or not _has_player() or not (event is InputEventMouseMotion):
		return false
	var sim = _sim()
	if sim == null:
		return false
	var mm := event as InputEventMouseMotion
	sim.add_local_player_look(mm.relative.x, mm.relative.y)
	return true


func _reset_state() -> void:
	_third_person = false
	if _weapon_effects != null:
		_weapon_effects.reset()
	_fire_was_held = false
	_reload_was_down = false
	_scope_was_down = false
	_view = null
	_set_world_nvg_view(false, 0)
	_sync_camera_mode()


func _set_world_nvg_view(active: bool, gain: int) -> void:
	if _world != null:
		_world.set_local_player_nvg_view(active, gain)


func _has_player() -> bool:
	if _world == null:
		return false
	if not _world.is_loaded():
		return false
	var sim = _sim()
	return sim != null and sim.has_local_player()


# WASD is the 8-way move relative to the look (W/S forward/back, A/D strafe);
# Q/E lean (catalog ids 6/7); Space jumps (momentary — the motor jumps once when
# grounded). There is no run key: running is the automatic forward-walk promotion
# in the sim's body selection, suppressed while scoped.
# [orig: Player_PackInputStateToEntity @0x4df450; promotion @0x4b729d]
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
		"lean_left": Input.is_physical_key_pressed(KEY_Q),
		"lean_right": Input.is_physical_key_pressed(KEY_E),
		"jump": Input.is_physical_key_pressed(KEY_SPACE),
	}


func _bool(state: Dictionary, key: String) -> bool:
	return bool(state.get(key, false))


func _ensure_models() -> void:
	if _world == null:
		return
	if _avatar == null or not is_instance_valid(_avatar):
		_avatar = _world.build_local_player_avatar()
	if _viewmodel == null or not is_instance_valid(_viewmodel):
		_viewmodel = _world.build_local_player_viewmodel()
		if _viewmodel != null:
			_apply_viewmodel_def()
			_vm_parts.clear()
			for child in _viewmodel.get_children():
				if child.has_method("play_body_clip"):
					_vm_parts.append(child)
			# re-sync the clip serial: fresh parts replay the active clip
			_weapon_effects.reset_play_serial()


func _clear_models() -> void:
	if _held_weapon != null and is_instance_valid(_held_weapon):
		_held_weapon.queue_free()
	_held_weapon = null
	_held_weapon_graphic = ""
	if _avatar != null and is_instance_valid(_avatar):
		_avatar.queue_free()
	if _viewmodel != null and is_instance_valid(_viewmodel):
		_viewmodel.queue_free()
	_avatar = null
	_viewmodel = null
	_vm_parts.clear()
	if _camera != null and _camera_saved_fov > 0.0:
		_camera.fov = _camera_saved_fov


func _set_fly_camera_locked(locked: bool) -> void:
	if _camera != null and _camera.has_method("set_gameplay_locked"):
		_camera.set_gameplay_locked(locked)


func _release_mouse_capture() -> void:
	if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# The crosshair's witnessed anchor. First person PINS the exact screen center — the
# original never projects there [orig: HUD_DrawCrosshair @0x592640 — 1P local takes
# screen_w/2, screen_h/2 @0x5928a0/@0x5928ae]; third person / spectate projects the
# aim ray's far point through the live camera [orig: the else branch @0x592910 —
# Entity_BuildCameraView(entity, 1, 1, 65536000 = 1000.0 q16) transformed + frustum-
# clipped @0x592932..3c; Viewport_ScreenToVirtual @0x5d2c70]. Vector2.INF = "no
# projection" (1P pin, no camera/player, or the far point behind the camera) — the
# HUD falls back to the design center.
const AIM_PROJECT_RANGE := 1000.0  # [orig: 65536000 q16 = 1000.0 units]

func aim_screen_point() -> Vector2:
	if not _third_person:
		return Vector2.INF  # 1P: the HUD pins the design center [orig: @0x5928a0]
	if _world == null or _camera == null or not _has_player():
		return Vector2.INF
	var angles := _aim_angles_deg()
	var yr := deg_to_rad(angles.x)
	var pr := deg_to_rad(angles.y)
	var forward := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
	var sim = _sim()
	var eye := _eye_position(sim.get_local_player_position() if sim != null else Vector3.ZERO)
	var target := eye + forward * AIM_PROJECT_RANGE
	if _camera.is_position_behind(target):
		return Vector2.INF
	return _camera.unproject_position(target)


# The binocular rangefinder targets the same aim ray as the camera. Retail
# measures from entity Position to the collision/far endpoint, truncates to an
# integer, and clamps the display to 1..1000.
func aim_range_units() -> int:
	if _world == null or _camera == null or not _has_player():
		return 1
	var sim = _sim()
	var pos: Vector3 = sim.get_local_player_position() if sim != null else Vector3.ZERO
	var eye := _eye_position(pos)
	var angles := _aim_angles_deg()
	var yr := deg_to_rad(angles.x)
	var pr := deg_to_rad(angles.y)
	var forward := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
	var endpoint := eye + forward * AIM_PROJECT_RANGE
	# The terrain surface, through the ported retail raycast
	# (NovaTerrainData.raycast_terrain -> libs/terrain_query/terrain_raycast.h
	# [orig: Terrain_RaycastHeightmapHiRes_0 @0x60e710]) rather than a Godot
	# physics query. The terrain heightfield was the only thing that query could
	# ever hit in the runtime -- object pick bodies are editor-only -- so this
	# measures the same surface, and it is now the SAME sampler the round the
	# player fires traces, so the readout and the bullet agree by construction.
	var terrain = _world.get_terrain_data()
	if terrain != null and terrain.has_method("raycast_terrain"):
		var hit: Vector3 = terrain.raycast_terrain(eye, endpoint)
		# A miss reports all-NAN.
		if not (is_nan(hit.x) or is_nan(hit.y) or is_nan(hit.z)):
			endpoint = hit
	return clampi(int(pos.distance_to(endpoint)), 1, 1000)


func _aim_angles_deg() -> Vector2:
	var sim = _sim()
	var yaw := float(sim.get_local_player_yaw_deg()) if sim != null else 0.0
	var pitch := float(sim.get_local_player_pitch_deg()) if sim != null else 0.0
	if _view != null and _view.binoculars_view_active:
		yaw += _view.binocular_yaw_offset_deg
		pitch += _view.binocular_pitch_offset_deg
	return Vector2(yaw, pitch)


# The eye anchor: Position + CameraOffset, where the local player's CameraOffset is
# the POSED HEAD BONE minus Position — sampled from the avatar's render skeleton, the
# same bone matrices the original builds sim-side. Stance, lean, the walk/run bob,
# and the jump arc all move the eye exactly as the animation moves the head.
# [orig: the local bone path @0x4b6bb3 (Entity_BuildBoneTransformMatrices -> head,
# CameraOffset = head - Position); consumed by the on-foot person leg @0x437f9c.
# Unported tails: the 4-sample terrain clamp @0x4b6c1c and the remote trig
# approximation @0x4b6984; the 0.125u floor is the witnessed min @0x4b6b98.]
func _eye_position(pos: Vector3) -> Vector3:
	var head := _avatar_head_world()
	if head == Vector3.INF:
		return pos + Vector3(0, PLAYER_EYE_HEIGHT, 0)  # non-person bump [orig: @0x437e8f]
	head.y = maxf(head.y, pos.y + PLAYER_EYE_MIN)
	return head


func _avatar_head_world() -> Vector3:
	if _avatar == null or not is_instance_valid(_avatar):
		return Vector3.INF
	var skel := _find_skeleton(_avatar)
	if skel == null or skel.get_bone_count() <= PLAYER_HEAD_BONE_INDEX:
		return Vector3.INF
	return skel.global_transform * skel.get_bone_global_pose(PLAYER_HEAD_BONE_INDEX).origin


## The soldier's third-person gun: retail's draw 5. The model is the equipped weapon's
## gfx3 and it is drawn RIGID — one matrix into every bone slot — so it carries no clip
## and no skeleton of its own; everything is the attach transform built here.
##
## Position is bone 16's own pivot, nudged, carried through that bone's posed matrix: the
## original's `M16 · (pivot16 + nudge)`, and since `M16 · pivot16` IS the joint world
## position, that reduces to joint + M16_rotation · nudge, which is what the pose gives us
## directly. Orientation is NOT bone 16's rotation and NOT one of the aim-overlay classes
## — it is the weapon's own attach basis (see PlayerAimOverlay.weapon_attach_angles).
## [orig: draw @0x4e3c87..0x4e3d99; matrix build @0x4b2180..0x4b22f8; gate
##  Entity_CanFireWeapon @0x4dcb10]
func _update_held_weapon(overlay: PlayerAimOverlay) -> void:
	if _world == null:
		return
	var def: PlayerViewmodelDef = _world.local_player_viewmodel_def()
	# 27 of the 94 shipped weapon rows author no gfx3 at all; drawing nothing is the
	# correct, retail behaviour there, not a missing asset.
	var graphic := def.gfx3 if def != null else ""
	if graphic != _held_weapon_graphic:
		if _held_weapon != null and is_instance_valid(_held_weapon):
			_held_weapon.queue_free()
		_held_weapon = null
		_held_weapon_graphic = graphic
		if not graphic.is_empty():
			_held_weapon = _world.build_local_player_held_weapon(graphic)
	if _held_weapon == null or not is_instance_valid(_held_weapon):
		return
	if overlay == null or not overlay.weapon_visible:
		_held_weapon.visible = false
		return
	var attach: Variant = PresentHeldWeapon.attach_transform(
			_avatar, overlay.weapon_attach_angles, overlay.weapon_hand_frame) 			if _avatar != null and is_instance_valid(_avatar) else null
	if attach == null:
		_held_weapon.visible = false
		return
	_held_weapon.global_transform = attach as Transform3D
	_held_weapon.visible = true
	# Same layer rule as the body: first person hides it from the player camera by LAYER,
	# so the water mirror still sees the soldier holding his rifle.
	_set_visual_layers(_held_weapon, NovaWater.VISUAL_LAYER_WORLD
			if (_third_person or debug_body_in_first_person)
			else NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY)


func _find_skeleton(root: Node) -> Skeleton3D:
	if root is Skeleton3D:
		return root
	for child in root.get_children():
		var found := _find_skeleton(child)
		if found != null:
			return found
	return null


# Place the camera from the player's authoritative pose. First person: eye = the
# head-bone anchor pulled back 0.1875u along the view, looking along the facing,
# rolled by torsoRoll + lean/4 [orig: the on-foot person leg @0x437f9c..0x438031 —
# pitch = entPitch + 2*pitchBlend (pitchBlend = the unported recoil impulse),
# roll = torsoRoll + lean/4 @0x437fe6, then eye += R*(-0x3000, 0, 0)].
# Third person (F4): (yaw + orbit_yaw, pitch + orbit_pitch) seeds the R*(-dist,0,0)
# eye offset from the NUDGED pivot (anchor + R*(0.125 fwd/left/up)); the original's
# FINAL rotation is the atan2 look-at from the (collision-pulled) eye back to that
# same nudged point — with no march ported, setting the seed angles directly is
# exactly equal [orig: Camera_ComputeThirdPersonView @0x438100..0x438171, nudge
# @0x43818a, offset @0x4383e2, the look-at recompute per net-re section 5.39].
# The mission yaw -> Godot forward mirrors the
# present remap (x,y,z)->(x,z,-y): a mission facing yaw faces (sin yaw, cos yaw)
# -> Godot (sin yaw, 0, -cos yaw), tilted by pitch.
func _update_player_camera() -> void:
	if _world == null or _camera == null:
		return
	var sim = _sim()
	var pos: Vector3 = sim.get_local_player_position() if sim != null else Vector3.ZERO
	var angles := _aim_angles_deg()
	var yr := deg_to_rad(angles.x)
	var pr := deg_to_rad(angles.y)
	var forward := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
	var eye := _eye_position(pos)
	if _third_person:
		# Chase camera: the smoothed anchor is SIM state — Position + CameraOffset
		# (the head-bone eye) eased a quarter-step per 62.5 Hz TICK (render-rate
		# independent) [orig: ThirdPersonCamera_Update @0x437b70/@0x437c8d; ported
		# in libs/world player_view]. Until the first 3P tick seeds the chase, the
		# eye stands in. The camera keeps the AIM angles pitched up by the orbit
		# default; on-foot orbit_yaw stays 0 until the orbit keys port.
		var anchor := eye
		if _view != null and _view.tp_anchor_valid:
			anchor = _view.tp_anchor
		var opr := pr + deg_to_rad(PLAYER_TP_ORBIT_PITCH_DEG)
		var tp_forward := Vector3(sin(yr) * cos(opr), sin(opr), -cos(yr) * cos(opr))
		var tp_basis := Basis.looking_at(tp_forward, Vector3.UP)
		# The pivot nudge: the camera backs away from — and aims at — the point
		# R*(0x2000, 0x2000, 0x2000) from the anchor, view frame X=fwd/Y=left/Z=up
		# (0.125u each) [orig: @0x43818a..0x4381c4 — the transformed nudge becomes
		# the matrix translation the (-dist,0,0) eye offset and the look-at use].
		# It is what frames the retail view OVER the head with the hat below-right
		# of center instead of body-centered.
		var pivot := anchor + (tp_forward - tp_basis.x + tp_basis.y) * PLAYER_TP_PIVOT_NUDGE
		_camera.global_position = pivot - tp_forward * tp_effective_distance(PLAYER_TP_DISTANCE)
		_camera.global_basis = tp_basis
	else:
		_camera.global_position = eye
		_camera.look_at(eye + forward, Vector3.UP)
		# The FP roll: torsoRoll + lean/4, composed in the sim (fp_roll_deg). Sign
		# pinned host-side: lean right (positive lean) tilts the view right.
		# [orig: @0x437fe6 — g_view_rot_roll = entity+0x2DC + lean>>2]
		var roll_deg := _view.fp_roll_deg if _view != null else 0.0
		if absf(roll_deg) > 0.001:
			_camera.rotate_object_local(Vector3(0, 0, -1), deg_to_rad(roll_deg))
		# The witnessed eye pull-back: -0x3000 (0.1875u) along the view FORWARD,
		# after the roll is in the basis [orig: @0x438001..0x438031].
		_camera.global_position += _camera.global_transform.basis.z * PLAYER_EYE_PULLBACK
	_update_scope_camera()
	_update_avatar(pos)
	_update_model_lighting_context()
	_update_viewmodel()


func _update_model_lighting_context() -> void:
	var sim = _sim()
	var interior_item_id := \
			int(sim.local_player_interior_item_id()) if sim != null else 0
	var transfer := 0.0
	var item_db = _world.get_item_db() if _world != null else null
	if interior_item_id != 0 and item_db != null:
		transfer = float(item_db.get_light_transfer(interior_item_id))
	var interior := interior_item_id != 0

	# Ordinary world models take the player's current entity context. The FP
	# submit deliberately keeps effectScale=1 (retail computes then discards its
	# outdoor sun sample) but still keys the interior group from blink_hits[0].
	# Updating on every presentation frame makes portal crossings live.
	# [orig: Player_RenderFirstPersonViewModel @0x4DEEA4..0x4DEF52]
	_set_model_lighting_context(_avatar, interior, transfer)
	_set_model_lighting_context(_held_weapon, interior, transfer)
	for part in _vm_parts:
		_set_model_lighting_context(part, interior, transfer)


func _set_model_lighting_context(model: Node, interior: bool,
		transfer: float) -> void:
	if model != null and is_instance_valid(model):
		model.set_entity_lighting_context(1.0, interior, transfer)


# The ADS camera: the fov POLICY is sim state (80 base, 80/mag for sighted defs,
# eased by the 15-tick interp, suppressed in third person — libs/world
# player_view [orig: g_cameraFovDeg @0x26C6848; Player_ToggleWeaponScope @0x4df401;
# @0x4df3fa]); this host converts horizontal -> vertical through the live aspect
# via the ONE shared conversion [orig: @0x58d900].
func _update_scope_camera() -> void:
	if _camera == null or _view == null:
		return
	var viewport := _camera.get_viewport()
	if viewport == null:
		return
	var size := viewport.get_visible_rect().size
	if size.x <= 0.0 or size.y <= 0.0:
		return
	_camera.fov = NovaSimulation.fov_vertical_from_horizontal(_view.fov_h_deg, size.x / size.y)


func _update_avatar(pos: Vector3) -> void:
	if _avatar == null or not is_instance_valid(_avatar) or _world == null:
		return
	_avatar.global_position = pos
	# The avatar node carries the BODY frame (the lagged body heading), not the aim yaw:
	# the aim/body split is what the per-segment overlay renders as the torso twist, and
	# the body-class delta is identity by construction so the hips stay glued to the node.
	# [orig: Entity_BuildBoneTransformMatrices @0x4b1290 — every overlay blends toward
	# bodyHeading/bodyPitch; docs/world/world-wac-ai-re.md §14 (D-INF-11)]
	var runtime = _world.get_runtime()
	var overlay: PlayerAimOverlay = runtime.local_player_aim_overlay() \
			if runtime != null else null
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
		var sim_yaw = _sim()
		_avatar.global_basis = MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0.0, sim_yaw.get_local_player_yaw_deg() if sim_yaw != null else 0.0, 0.0))
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
	_update_held_weapon(overlay)
	var sim = _sim()
	var anim_key := String(sim.get_local_player_anim_key()) if sim != null else ""
	var anim_phase := int(sim.get_local_player_anim_phase_ticks()) if sim != null else 0
	# The upper-body weapon channel: the sim's secondary-channel clip (reload etc.) posed
	# at its own playhead onto the mask bones, composed under the aim overlay. Equal
	# state ids still carry the secondary playhead; an empty key means the gate is off.
	# [orig: producer @0x4b5dad, override @0x4b14db; world-wac-ai-re.md §14.8]
	if _avatar.has_method("set_weapon_channel"):
		var weapon_view: PlayerWeaponView = \
				_weapon_effects.weapon_view() if _weapon_effects != null else null
		if weapon_view != null:
			_avatar.set_weapon_channel(weapon_view.body_anim_key, weapon_view.body_anim_phase)
		else:
			_avatar.set_weapon_channel("", 0)
	if not anim_key.is_empty() and _avatar.has_method("play_body_clip_at"):
		_avatar.play_body_clip_at(anim_key, anim_phase)
	elif not anim_key.is_empty() and _avatar.has_method("play_body_clip"):
		_avatar.play_body_clip(anim_key)
	elif _avatar.has_method("play_body_anim_at"):
		_avatar.play_body_anim_at(
				sim.get_local_player_body_anim_slot() if sim != null else -1, anim_phase)
	elif _avatar.has_method("play_body_anim"):
		_avatar.play_body_anim(
				sim.get_local_player_body_anim_slot() if sim != null else -1)


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
	# with the visible ease carried by the 15-step scope-camera interp — SIM state
	# at the world cadence [orig: CNetPlayerInterp_Setup @0x4df36e / g_fpCameraInterp
	# @0x82CE40; libs/world player_view_bias_units states the blend].
	var ads := _view.scope_fraction if _view != null else 0.0
	# The NoCardSwitch reload rule: reloading a card-switching weapon drops the
	# ADS bias for the frame (instant, not eased) [orig: Player_UpdateFirstPersonCamera
	# @0x4dd439/@0x4dd4cc skip the bias add while Player_IsReloadingCardSwitchWeapon].
	if _view != null and _view.suppress_view_bias:
		ads = 0.0
	var view_units := PLAYER_VIEWMODEL_POS_UNITS.lerp(PLAYER_VIEWMODEL_TPOS_UNITS, ads)
	_viewmodel.global_transform = _camera.global_transform * Transform3D(
		vm_basis, bias * _viewmodel_offset(view_units))
	_apply_emplaced_viewmodel_controls()
	# The FP overlay never enters the water mirror OR the main camera: retail draws it
	# as its own renderfov/near-Z pass over the finished frame [orig:
	# Player_RenderFirstPersonViewModel @ 0x4ded60]; hosted, the dedicated layer is drawn
	# only by the pass camera (and excluded by the mirror camera's cull_mask).
	_set_visual_layers(_viewmodel, NovaWater.VISUAL_LAYER_VIEWMODEL)
	# The card switch: while the SIGHTS card is up, the FP model does not draw —
	# the frame shows one or the other [orig: selectors/clear @0x5ca299..0x5ca304;
	# the card path @0x5caaf3..0x5cab15 and the viewmodel candidate @0x5ca32c].
	var carded := _view != null and _view.scope_card_active
	var binoculars := _view != null and _view.binoculars_view_active
	_viewmodel.visible = (
			((not _third_person) and not carded and not binoculars)
			or debug_force_viewmodel)
	_update_viewmodel_pass()


func _apply_emplaced_viewmodel_controls() -> void:
	var weapon_view: PlayerWeaponView = \
			_weapon_effects.weapon_view() if _weapon_effects != null else null
	for part in _vm_parts:
		if part == null or not is_instance_valid(part) or \
				not part.has_method("set_ctrl_value"):
			continue
		if weapon_view != null and weapon_view.emplaced_controls_valid:
			part.set_ctrl_value(
					"EWEAP_GUNYAW", weapon_view.emplaced_gun_yaw)
			part.set_ctrl_value(
					"EWEAP_GUNPITCH", weapon_view.emplaced_gun_pitch)
		elif part.has_method("clear_ctrl_value"):
			part.clear_ctrl_value("EWEAP_GUNYAW")
			part.clear_ctrl_value("EWEAP_GUNPITCH")


# Stamp `layer_mask` onto every VisualInstance3D under `root` (inclusive).
# VisualInstance3D.layers is per-instance - a container's value does not
# propagate to children - and both player models are NovaObjectModel subtrees
# (mesh instances under Robj/Skeleton3D nodes) whose rebuild() recreates them
# on the default layer, so the callers above re-stamp every frame.
func _set_visual_layers(root: Node, layer_mask: int) -> void:
	if root is VisualInstance3D:
		var visual := root as VisualInstance3D
		visual.layers = layer_mask \
				| (visual.layers & NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK)
	for child in root.get_children():
		_set_visual_layers(child, layer_mask)


# Pull the resolved weapon.def view record from the world; null when no weapon.def (or the
# weapon) resolves in the mounted root — the witnessed JOX WPN_AK47AUTO constants above stay
# in force. The def rows carry xyz raw file units + yaw/pitch/roll degrees
# [orig: weapon.def 'pos'/'tpos' handlers @0x54471f; 'renderfov' @0x54482a, default 80.0].
func _apply_viewmodel_def() -> void:
	if _world == null:
		return
	var def: PlayerViewmodelDef = _world.local_player_viewmodel_def()
	if def == null:
		return
	PLAYER_VIEWMODEL_POS_UNITS = def.pos_units
	PLAYER_VIEWMODEL_ROT_BIAS_DEF = def.rot_bias_deg
	PLAYER_VIEWMODEL_TPOS_UNITS = def.tpos_units
	PLAYER_VIEWMODEL_RENDERFOV_H_DEG = def.renderfov_h_deg
	# flags / scope_max_mag / clipsize stay with the SIM (the weapon dict feeds
	# set_local_player_weapon): the ADS gates + fov policy run there (ADR 0016).


# Convert a weapon.def `pos`/`tpos` POSITION (raw file units) into a Godot camera-local offset.
# The witnessed pipeline [orig: Player_UpdateFirstPersonCamera @0x4dd380 — the offset is
# VIEW-LOCAL: Math_FixedPointTransformPoint22(g_view_matrix, &cam_offset, ..) @0x4dd5d8
# rotates it by the view basis before adding onto g_view_pos; at ADS settle (entity
# Flags & 2) the tpos/AltCamOffset REPLACES the offset wholesale @0x4dd58f..0x4dd5c7;
# scale flt_7D1D70=256 @0x544770]. The view/def frame is X = FORWARD, Y = LEFT,
# Z = UP — proven by the aim ray's far point being {+65536000, 0, 0} through the SAME
# transform [orig: HUD_DrawCrosshair @0x592a0f aim_direction = (1000.0, 0, 0) q16].
# Godot camera-local is (x right, y up, -z forward), so:
#   file x (forward) -> Godot -z   (M4 tpos x -50.9 = ~0.2u BACK into the shoulder)
#   file y (left)    -> Godot -x
#   file z (up)      -> Godot  y   (e.g. MP5SD pos.z -183 -> grip ~0.715u below the eye)
# (The 2026-07-11 grill REFUTED the earlier x=right/y=forward reading: the AK's
# |x| ~= |y| masked the swap; the JOX/REVX M4 tpos made it glare — the canted-ADS
# report. oscarmike's onhook-derived map agrees with the witnessed frame.) The
# velocity lead (>>7, clamps @0x4dd4f2..) and the prone Z drop (-1280 @0x4dd578)
# are recorded unported tails.
func _viewmodel_offset(units: Vector3) -> Vector3:
	return Vector3(
		-units.y / WEAPON_DEF_POS_SCALE,
		units.z / WEAPON_DEF_POS_SCALE,
		-units.x / WEAPON_DEF_POS_SCALE)


# Fold degrees into (-180, 180] (def rot columns store e.g. 353 for -7).
func _wrap180(degrees: float) -> float:
	var out := fmod(degrees + 180.0, 360.0)
	if out < 0.0:
		out += 360.0
	return out - 180.0
