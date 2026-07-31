class_name LocalPlayerPresenter
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


var _world
var _camera: Camera3D
var _third_person := false
var _avatar: Node3D = null
var _held_weapon: Node3D = null      # the 3P gun; a SIBLING of _avatar (see GameWorld)
var _held_weapon_graphic := ""      # the gfx3 the live node was built from
# --- the equipped-weapon FSM view + the sim-owned view state (net-re §5.62/§5.41) --
# The FSM and the VIEW STATE both tick in the sim at 62.5 Hz (libs/world
# weapon_fsm + player_view; ADR 0016 — policy, state, and cadence live in the
# engine): the ADS engaged bit + 15-step ease, the fov policy, and the 3P anchor
# chase arrive as a PlayerLocalView snapshot each frame. This presenter places the
# camera/avatar nodes; its collaborators carry the rest of the old monolith
# (W4-4): raw input sampling (trigger edges, the RMB toggle REQUEST, movement,
# gameplay keys) lives in PlayerInputRouter; the FP viewmodel node/parts, the
# renderfov render pass, and the weapon.def placement live in PlayerViewmodelRig;
# the weapon EVENT presentation — the batch consume, the FSM event clips on BOTH
# viewmodel parts (arms + gun share the animadm), the muzzle/shell userpoint
# resolution, and the owner-bound effect anchors — lives in PlayerWeaponEffects.
# All three live beside the presenter for the same setup -> teardown span; the pinned
# presenter surface (before_world_tick / handle_key_input / handle_input / the model
# accessors) delegates to them.
var _weapon_effects: PlayerWeaponEffects = null
var _input_router := PlayerInputRouter.new()
var _viewmodel_rig := PlayerViewmodelRig.new()
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


func is_debug_force_viewmodel() -> bool:
	return debug_force_viewmodel


func set_debug_body_in_first_person(enabled: bool) -> void:
	debug_body_in_first_person = enabled


func is_debug_body_in_first_person() -> bool:
	return debug_body_in_first_person


func setup(world, camera: Camera3D) -> void:
	_world = world
	_camera = camera
	# The weapon-event presentation lives beside the presenter for the same setup ->
	# teardown span; it resolves userpoints against this presenter's live nodes.
	_weapon_effects = PlayerWeaponEffects.new()
	_weapon_effects.setup(world, self)
	_input_router.setup(world, self)
	_camera_saved_fov = camera.fov if camera != null else -1.0
	_camera_saved_cull_mask = camera.cull_mask if camera != null else -1
	# The player camera never draws the reflection-only body layer: in first
	# person the body lives there for the water mirror alone (see
	# _update_avatar); NovaWater's mirror camera is the one view that keeps it.
	# It never draws the viewmodel layer either — the FP arms/weapon render
	# through the dedicated renderfov pass the rig builds (deferred; see
	# PlayerViewmodelRig.setup for the "parent busy" boot shape).
	if _camera != null:
		_camera.cull_mask &= ~(
				NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY
				| NovaWater.VISUAL_LAYER_VIEWMODEL
				| NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK)
	_viewmodel_rig.setup(world, self, camera)
	_reset_state()
	# Attachment is the adoption boundary: discard presentation history produced
	# before this presenter existed. Every event produced after setup is live, including
	# a first-tick shot before the first active snapshot is presented.
	if _world != null:
		_world.drain_local_player_weapon_events()
		_world.set_local_player_weapon_tick_consumer(
				Callable(self, "_present_fixed_weapon_tick"))


func teardown() -> void:
	if _world != null:
		_world.set_local_player_weapon_tick_consumer(Callable())
	set_fly_camera_locked(false)
	_input_router.release_mouse_capture()
	clear_models()
	_viewmodel_rig.teardown()
	if _camera != null:
		if _camera_saved_cull_mask >= 0:
			_camera.cull_mask = _camera_saved_cull_mask
		if _camera_saved_fov > 0.0:
			_camera.fov = _camera_saved_fov
	if _weapon_effects != null:
		_weapon_effects.teardown()
	_weapon_effects = null
	_input_router.teardown()
	_world = null
	_camera = null
	_camera_saved_cull_mask = -1
	_reset_state()


## Drop the built FP viewmodel so the next update pass rebuilds gun/arms/FSM from the
## (changed) equipped weapon — the armory ACCEPT re-mount [orig:
## WeaponLoadout_ApplyFromBuffer @0x565cd0 tail -> Player_MountWeaponSlot @0x4dfa40].
func refresh_viewmodel() -> void:
	if _weapon_effects != null:
		_weapon_effects.on_viewmodel_refresh()
	_viewmodel_rig.refresh_viewmodel()


func set_input_source(source: Callable) -> void:
	_input_router.set_input_source(source)


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
# and effect anchors against the presentation nodes (the FP viewmodel parts —
# rig-owned, delegated here — the 3P gun, the camera). These expose exactly the
# state it reads, so no cross-object _private access crosses the seam.
func vm_parts() -> Array:
	return _viewmodel_rig.vm_parts()


func viewmodel() -> Node3D:
	return _viewmodel_rig.viewmodel()


func held_weapon() -> Node3D:
	return _held_weapon


func camera() -> Camera3D:
	return _camera


## The weapon-event presentation object: the rig resyncs the clip serial through
## it when fresh viewmodel parts are built.
func weapon_effects() -> PlayerWeaponEffects:
	return _weapon_effects


## The FP viewmodel/render-pass owner (tests and probes inspect the pass nodes
## and sweep the placement tunables through it).
func viewmodel_rig() -> PlayerViewmodelRig:
	return _viewmodel_rig


func before_world_tick(delta: float, capture_mouse: bool = false,
		gameplay_input_active: bool = true) -> void:
	_input_router.before_world_tick(delta, capture_mouse, gameplay_input_active)


func after_world_tick() -> void:
	if not has_player():
		_set_world_nvg_view(false, 0)
		set_fly_camera_locked(false)
		_input_router.release_mouse_capture()
		clear_models()
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
	if not has_player():
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


# The gameplay keys (F4/B/N/NVG gain/stance) live in the input router; this
# pinned presenter name delegates (main_game calls it).
func handle_key_input(event: InputEvent, active: bool) -> bool:
	return _input_router.handle_key_input(event, active)


# Mouse-look rides the input router: raw pixel deltas into the SIM's witnessed
# integer pipeline [orig: Input_ProcessMouseAxisBindings @0x499680].
func handle_input(event: InputEvent, active: bool) -> bool:
	return _input_router.handle_input(event, active)


func _reset_state() -> void:
	_third_person = false
	if _weapon_effects != null:
		_weapon_effects.reset()
	_input_router.reset()
	_view = null
	_set_world_nvg_view(false, 0)
	_sync_camera_mode()


func _set_world_nvg_view(active: bool, gain: int) -> void:
	if _world != null:
		_world.set_local_player_nvg_view(active, gain)


func has_player() -> bool:
	if _world == null:
		return false
	if not _world.is_loaded():
		return false
	var sim = _sim()
	return sim != null and sim.has_local_player()


## The model lifetime around the input router's before-tick sample: build the
## avatar when missing and let the rig (re)build the FP viewmodel.
func ensure_models() -> void:
	if _world == null:
		return
	if _avatar == null or not is_instance_valid(_avatar):
		_avatar = _world.build_local_player_avatar()
	_viewmodel_rig.ensure_viewmodel()


func clear_models() -> void:
	if _held_weapon != null and is_instance_valid(_held_weapon):
		_held_weapon.queue_free()
	_held_weapon = null
	_held_weapon_graphic = ""
	if _avatar != null and is_instance_valid(_avatar):
		_avatar.queue_free()
	_avatar = null
	_viewmodel_rig.clear_viewmodel()
	if _camera != null and _camera_saved_fov > 0.0:
		_camera.fov = _camera_saved_fov


func set_fly_camera_locked(locked: bool) -> void:
	if _camera != null and _camera.has_method("set_gameplay_locked"):
		_camera.set_gameplay_locked(locked)


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
	if _world == null or _camera == null or not has_player():
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
	if _world == null or _camera == null or not has_player():
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
	var head := avatar_head_world()
	if head == Vector3.INF:
		return pos + Vector3(0, PLAYER_EYE_HEIGHT, 0)  # non-person bump [orig: @0x437e8f]
	head.y = maxf(head.y, pos.y + PLAYER_EYE_MIN)
	return head


## The posed head-bone eye (Vector3.INF = no skeleton): the input router feeds
## it to the sim for the 3P anchor chase [orig: the chase target @0x437b70].
func avatar_head_world() -> Vector3:
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
			_avatar, overlay.weapon_attach_angles, overlay.weapon_hand_frame) \
			if _avatar != null and is_instance_valid(_avatar) else null
	if attach == null:
		_held_weapon.visible = false
		return
	_held_weapon.global_transform = attach as Transform3D
	_held_weapon.visible = true
	# Same layer rule as the body: first person hides it from the player camera by LAYER,
	# so the water mirror still sees the soldier holding his rifle.
	PlayerViewmodelRig.set_visual_layers(_held_weapon, NovaWater.VISUAL_LAYER_WORLD
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
# pitch = entPitch + 2*pitchBlend,
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
	# The live recoil accumulator is doubled only by retail's first-person camera.
	# Third-person orbit, projectile aim, and the HUD anchor retain the base look
	# pitch. [orig: Player_UpdateFirstPersonCamera @0x437fdb]
	if not _third_person and _view != null:
		angles.y += _view.fp_pitch_recoil_deg
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
		# pinned presenter-side: lean right (positive lean) tilts the view right.
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
	_viewmodel_rig.update_viewmodel(_view,
			_weapon_effects.weapon_view() if _weapon_effects != null else null,
			_third_person, debug_force_viewmodel)


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
	for part in vm_parts():
		_set_model_lighting_context(part, interior, transfer)


func _set_model_lighting_context(model: Node, interior: bool,
		transfer: float) -> void:
	if model != null and is_instance_valid(model):
		model.set_entity_lighting_context(1.0, interior, transfer)


# The ADS camera: the fov POLICY is sim state (80 base, 80/mag for sighted defs,
# eased by the 15-tick interp, suppressed in third person — libs/world
# player_view [orig: g_cameraFovDeg @0x26C6848; Player_ToggleWeaponScope @0x4df401;
# @0x4df3fa]); this presenter converts horizontal -> vertical through the live aspect
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
	PlayerViewmodelRig.set_visual_layers(_avatar, NovaWater.VISUAL_LAYER_WORLD
			if (_third_person or debug_body_in_first_person)
			else NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY)
	_update_held_weapon(overlay)
	var sim = _sim()
	var anim_key := String(sim.get_local_player_anim_key()) if sim != null else ""
	var anim_phase := int(sim.get_local_player_anim_phase_ticks()) if sim != null else 0
	var anim_source_key := (
			String(sim.get_local_player_anim_source_key())
			if sim != null else "")
	var anim_source_phase := (
			int(sim.get_local_player_anim_source_phase_ticks())
			if sim != null else 0)
	var anim_blend_weight := (
			float(sim.get_local_player_anim_blend_weight())
			if sim != null else 1.0)
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
	if (not anim_source_key.is_empty() and not anim_key.is_empty()
			and anim_blend_weight < 1.0
			and _avatar.has_method("play_body_blend_at")):
		_avatar.play_body_blend_at(
				anim_source_key, anim_source_phase,
				anim_key, anim_phase, anim_blend_weight)
	elif not anim_key.is_empty() and _avatar.has_method("play_body_clip_at"):
		_avatar.play_body_clip_at(anim_key, anim_phase)
	elif not anim_key.is_empty() and _avatar.has_method("play_body_clip"):
		_avatar.play_body_clip(anim_key)
	elif _avatar.has_method("play_body_anim_at"):
		_avatar.play_body_anim_at(
				sim.get_local_player_body_anim_slot() if sim != null else -1, anim_phase)
	elif _avatar.has_method("play_body_anim"):
		_avatar.play_body_anim(
				sim.get_local_player_body_anim_slot() if sim != null else -1)
