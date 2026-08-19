class_name LocalPlayerPresenter
extends Node

const MissionPresentation := preload("res://game/world/mission_presentation.gd")

# Faithful first-person camera. The on-foot eye is Position + CameraOffset, where
# the local player's CameraOffset is the POSED HEAD BONE minus Position — the eye
# follows the animation (stand/crouch/prone/jump all move it) [orig: the local bone
# path @0x4b6bb3 stores head−Position into CameraOffset(+0x6C); the on-foot person
# camera leg adds it @0x437f9c]. +1.0 is the witnessed NON-person fallback bump
# [orig: @0x437e8f], kept for the no-skeleton case. F4 swaps to the mode-1 chase
# camera [orig: ThirdPersonCamera_Update @0x437af0]. Mouse look is SIM-owned:
# raw pixel deltas feed Simulation.add_local_player_look (the witnessed integer
# pipeline — sensitivity<<11, scoped zoom reduction, ±80° pitch clamp with the +40°
# up-limit while prone) [orig: Input_ProcessMouseAxisBindings @0x499680].
# Calibration values live at engine world/player_view.h (Simulation re-exports).

# The head is bone INDEX 14 (.bad row "BN15 Head") — the rig is index-driven and the
# model bone order IS the BN order [world-wac-ai-re §14.2]; the original reads the
# head row of its bone-matrix array, never a name [orig: the local bone path @0x4b6bb3].

# The FP eye pull-back (-0x3000 along the view forward), the eye floor, and the
# non-person bump compose in the SIM's camera pose now (world/player_view.h
# kFpEyePullback/kEyeMinAbovePosition/kNonPersonEyeBump); the constants below
# remain for this presenter's OWN eye sampler (_eye_position — the aim-ray and
# 3P-anchor legs read the raw head anchor, not the composed camera).
# The chase-camera composition — the reset distance/orbit, the pivot nudge, and
# the collision march's no-collision landing — runs in the SIM per drain
# (world/player_view.h, S8: player_view_compose_camera +
# player_view_tp_effective_distance); this presenter stamps the composed pose.
# Orbit keys (view bits 0x10/0x40 -> orbit_yaw ±0x1000000/tick @0x437c1b), the
# zoom keys (view actions 409/410 @0x49c1c5..0x49c23f), the march's bone-
# collision FORCES (@0x4382d9), and the dead-target 10.0 -> 3.0 distance ease
# @0x437cc0 are tracked deferrals (net-re section 5.39).


var _world: GameWorld = null
var _fly_camera: FlyCamera = null
var _camera: Camera3D
var _third_person := false
var _avatar: ObjectModel = null
var _held_weapon: ObjectModel = null  # the 3P gun; a SIBLING of _avatar (see GameWorld)
var _held_weapon_graphic := ""      # the gfx3 the live node was built from
# --- the equipped-weapon FSM view + the sim-owned view state (net-re §5.62/§5.41) --
# The FSM and the VIEW STATE both tick in the sim at 62.5 Hz (engine/runtime/world
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


## The showhud bit-0 FP-gun gate, forwarded to the rig's visibility decision.
## GameHudPresenter owns the showhud flag cycle and pushes the bit here.
## [orig: g_FpWeaponViewFlags bit 0 read by Player_RenderFirstPersonViewModel
##  @0x4DEDEA]
func set_fp_gun_visible(visible: bool) -> void:
	_viewmodel_rig.set_fp_gun_visible(visible)


func is_debug_force_viewmodel() -> bool:
	return debug_force_viewmodel


func set_debug_body_in_first_person(enabled: bool) -> void:
	debug_body_in_first_person = enabled


func is_debug_body_in_first_person() -> bool:
	return debug_body_in_first_person


## `fly_camera`: the shell's gameplay-lock seam — the game passes its
## FlyCamera (the same node as `camera`); previews that fly no camera
## pass null and the lock is a no-op.
func setup(world: GameWorld, camera: Camera3D,
		fly_camera: FlyCamera = null) -> void:
	_world = world
	_camera = camera
	_fly_camera = fly_camera
	# The weapon-event presentation lives beside the presenter for the same setup ->
	# teardown span; it resolves userpoints against this presenter's live nodes.
	_weapon_effects = PlayerWeaponEffects.new()
	_weapon_effects.setup(world, self)
	_input_router.setup(world, self)
	_camera_saved_fov = camera.fov if camera != null else -1.0
	_camera_saved_cull_mask = camera.cull_mask if camera != null else -1
	# The player camera never draws the FP body layer: retail renders no local
	# body in first person, and the water mirror never draws persons either
	# (its reflected entity waves collect only vehicles above water and it has
	# no player-render leg [orig: Terrain_CollectVisibleEntitiesForReflection
	# @ 0x5c90a0]) — the layer keeps the body a shadow source only. It never
	# draws the viewmodel layer either — the FP arms/weapon render through the
	# dedicated renderfov pass the rig builds (deferred; see
	# PlayerViewmodelRig.setup for the "parent busy" boot shape). Godot's shadow
	# collection also intersects the gameplay camera mask, so the two caster
	# marker layers must remain admitted even though SHADOWS_ONLY geometry on
	# those layers is absent from the beauty pass.
	if _camera != null:
		_camera.cull_mask = (
				_camera.cull_mask
				& ~(Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY
						| Water.VISUAL_LAYER_VIEWMODEL)
				) | Water.VISUAL_LAYER_SHADOW_CASTER_MASK
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
func vm_parts() -> Array[ObjectModel]:
	return _viewmodel_rig.vm_parts()


func viewmodel() -> Node3D:
	return _viewmodel_rig.viewmodel()


func held_weapon() -> ObjectModel:
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
		gameplay_input_active: bool = true) -> MissionFrameInput:
	return _input_router.before_world_tick(
			delta, capture_mouse, gameplay_input_active)


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
	if _fly_camera != null:
		_fly_camera.set_gameplay_locked(locked)


# The crosshair's witnessed anchor. First person PINS the exact screen center — the
# original never projects there [orig: HUD_DrawCrosshair @0x592640 — 1P local takes
# screen_w/2, screen_h/2 @0x5928a0/@0x5928ae]; third person / spectate projects the
# aim ray's far point through the live camera [orig: the else branch @0x592910 —
# Entity_BuildCameraView(entity, 1, 1, 65536000 = 1000.0 q16) transformed + frustum-
# clipped @0x592932..3c; Viewport_ScreenToVirtual @0x5d2c70]. Vector2.INF = "no
# projection" (1P pin, no camera/player, or the far point behind the camera) — the
# HUD falls back to the design center.


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
	var target := eye + forward * Simulation.player_aim_project_range()
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
	var endpoint := eye + forward * Simulation.player_aim_project_range()
	# The terrain surface, through the ported retail raycast
	# (TerrainData.raycast_terrain -> engine/runtime/terrain_query/terrain_raycast.h
	# [orig: Terrain_RaycastHeightmapHiRes_0 @0x60e710]) rather than a Godot
	# physics query. The terrain heightfield was the only thing that query could
	# ever hit in the runtime -- object pick bodies are editor-only -- so this
	# measures the same surface, and it is now the SAME sampler the round the
	# player fires traces, so the readout and the bullet agree by construction.
	var terrain := _world.get_terrain_data()
	if terrain != null:
		var hit := terrain.raycast_terrain(eye, endpoint)
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
# Unported tail: the remote trig approximation @0x4b6984. The 0.125u floor here
# is a DEFENSIVE stand-in — retail floors only the sample-less capsule leg
# (retail: @0x4b6b98); the head-bone legs store unfloored (D-INF-18).]
func _eye_position(pos: Vector3) -> Vector3:
	var head := avatar_head_world()
	if head == Vector3.INF:
		return pos + Vector3(0, Simulation.player_non_person_eye_bump(), 0)  # non-person bump [orig: @0x437e8f]
	head.y = maxf(head.y, pos.y + Simulation.player_eye_min_above_position())
	return head


## The posed head-bone eye (Vector3.INF = no skeleton): the input router feeds
## it to the sim for the 3P anchor chase [orig: the chase target @0x437b70].
func avatar_head_world() -> Vector3:
	if _avatar == null or not is_instance_valid(_avatar):
		return Vector3.INF
	var skel := _find_skeleton(_avatar)
	if skel == null or skel.get_bone_count() <= Simulation.player_head_bone_index():
		return Vector3.INF
	return skel.global_transform * skel.get_bone_global_pose(Simulation.player_head_bone_index()).origin


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
	var attach: Variant = PresentApplier.held_weapon_attach_transform(
			_avatar, overlay.weapon_attach_angles, overlay.weapon_hand_frame) \
			if _avatar != null and is_instance_valid(_avatar) else null
	if attach == null:
		_held_weapon.visible = false
		return
	_held_weapon.global_transform = attach as Transform3D
	_held_weapon.visible = true
	# Same layer rule as the body: first person hides it from every camera by
	# LAYER while keeping it a shadow source (the witnessed mirror never draws
	# persons or their held weapons — the reflection collects vehicles only
	# [orig: Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0]).
	var draw_held_weapon := _third_person or debug_body_in_first_person
	PlayerViewmodelRig.set_visual_layers(_held_weapon, Water.VISUAL_LAYER_WORLD
			if draw_held_weapon else Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY)
	PlayerViewmodelRig.set_shadow_casting(_held_weapon,
			GeometryInstance3D.SHADOW_CASTING_SETTING_ON if draw_held_weapon
			else GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY)


func _find_skeleton(root: Node) -> Skeleton3D:
	if root is Skeleton3D:
		return root
	for child in root.get_children():
		var found := _find_skeleton(child)
		if found != null:
			return found
	return null


# Place the camera from the SIM-COMPOSED pose (world/player_view.h, S8): the
# witnessed composition — the eye floor + 0.1875 pull-back, the recoil-doubled
# FP pitch, the torso+lean/4 roll, the chased-anchor pivot nudge and the
# march-landed third-person back-off — runs in the engine per drain
# [orig: Camera_ComputeThirdPersonView @0x437d10 — the on-foot person leg
# @0x437f9c..0x438031, the TP leg @0x438100..0x4383e2]; this presenter only
# converts the pose to the Godot frame and stamps the node. The mission yaw ->
# Godot forward mirrors the present remap (x,y,z)->(x,z,-y): a mission facing
# yaw faces (sin yaw, cos yaw) -> Godot (sin yaw, 0, -cos yaw), tilted by pitch.
func _update_player_camera() -> void:
	if _world == null or _camera == null:
		return
	var sim = _sim()
	var pos: Vector3 = sim.get_local_player_position() if sim != null else Vector3.ZERO
	if _view != null and _view.camera_pose_valid:
		var yr := deg_to_rad(_view.camera_yaw_deg)
		var pr := deg_to_rad(_view.camera_pitch_deg)
		var forward := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
		_camera.global_position = _view.camera_eye
		_camera.look_at(_view.camera_eye + forward, Vector3.UP)
		# The FP roll (torsoRoll + lean/4, composed in the sim; 0 in third
		# person). Sign pinned presenter-side: lean right (positive lean)
		# tilts the view right. [orig: @0x437fe6]
		if absf(_view.camera_roll_deg) > 0.001:
			_camera.rotate_object_local(
					Vector3(0, 0, -1), deg_to_rad(_view.camera_roll_deg))
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

	# The third-person body and its held gun take the entity's outdoor sun
	# factor like any sector-drawn entity (D-RLIT-3; the sim computes the
	# local quality each occlusion frame). The FP submit deliberately keeps
	# effectScale=1 (retail computes then discards its outdoor sun sample)
	# but still keys the interior group from blink_hits[0]. Updating on every
	# presentation frame makes portal crossings live.
	# [orig: Player_RenderFirstPersonViewModel @0x4DEEA4..0x4DEF52 — the
	# setup_terrain_effect_for_entity return is dropped on the FP leg;
	# Terrain_RenderSectorEntities stacks it for the world body @0x5c7bff]
	var body_scale := 1.0
	if sim != null:
		body_scale = float(sim.get_local_player_sun_quality()) * 0.25
	_set_model_lighting_context(_avatar, interior, transfer, body_scale)
	_set_model_lighting_context(_held_weapon, interior, transfer, body_scale)
	for part in vm_parts():
		_set_model_lighting_context(part, interior, transfer)


func _set_model_lighting_context(model: Node, interior: bool,
		transfer: float, effect_scale := 1.0) -> void:
	if model != null and is_instance_valid(model):
		model.set_entity_lighting_context(effect_scale, interior, transfer)


# The ADS camera: the fov POLICY is sim state (80 base, 80/mag for sighted defs,
# eased by the 15-tick interp, suppressed in third person — engine/runtime/world
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
	_camera.fov = Simulation.fov_vertical_from_horizontal(_view.fov_h_deg, size.x / size.y)


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
		var inv := body_basis.inverse()
		var deltas: Array = []
		for a in overlay.segment_angles:
			deltas.append(inv * MissionObjectPlacer.bms_to_godot_basis(a))
		_avatar.set_aim_overlay(deltas)
	else:
		var sim_yaw = _sim()
		_avatar.global_basis = MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0.0, sim_yaw.get_local_player_yaw_deg() if sim_yaw != null else 0.0, 0.0))
		_avatar.set_aim_overlay([])
	# The body renders only in third person; first person hides it from every
	# camera by LAYER, not by visible = false, so it stays a live shadow
	# source. The 2026-08-05 witness corrected the earlier mirror-visible
	# reading: retail's reflected entity waves collect only vehicles above
	# water, and the reflected world has no player-render leg, so no person —
	# the local body included — ever
	# enters the mirror [orig: Terrain_CollectVisibleEntitiesForReflection
	# @ 0x5c90a0 filterMask 0x400; Entity_InitFromModel @ 0x40e20a; the
	# viewmodel pass stays Player_RenderFirstPersonViewModel @ 0x4ded60].
	# Stamped every frame: ObjectModel.rebuild() recreates its mesh
	# children on the default layer.
	_avatar.visible = true
	var draw_avatar := _third_person or debug_body_in_first_person
	PlayerViewmodelRig.set_visual_layers(_avatar, Water.VISUAL_LAYER_WORLD
			if draw_avatar else Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY)
	PlayerViewmodelRig.set_shadow_casting(_avatar,
			GeometryInstance3D.SHADOW_CASTING_SETTING_ON if draw_avatar
			else GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY)
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
	var weapon_view: PlayerWeaponView = 			_weapon_effects.weapon_view() if _weapon_effects != null else null
	if weapon_view != null:
		_avatar.set_weapon_channel(weapon_view.body_anim_key, weapon_view.body_anim_phase,
				weapon_view.body_anim_prev_key, weapon_view.body_anim_prev_phase,
				weapon_view.body_anim_blend_weight, weapon_view.body_anim_variant,
				weapon_view.body_anim_prev_variant)
	else:
		_avatar.set_weapon_channel("", 0)
	if (not anim_source_key.is_empty() and not anim_key.is_empty()
			and anim_blend_weight < 1.0):
		_avatar.play_body_blend_at(
				anim_source_key, anim_source_phase,
				anim_key, anim_phase, anim_blend_weight)
	elif not anim_key.is_empty():
		_avatar.play_body_clip_at(anim_key, anim_phase)
	else:
		_avatar.play_body_anim_at(
				sim.get_local_player_body_anim_slot() if sim != null else -1, anim_phase)
