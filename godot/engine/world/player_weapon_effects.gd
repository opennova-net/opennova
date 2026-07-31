class_name PlayerWeaponEffects
extends RefCounted

const MissionRuntime := preload("res://engine/world/mission_runtime.gd")

# The local player's weapon-EVENT presentation, split out of LocalPlayerPresenter
# (W4-4): the fixed-tick event batch consume (switch/clear/fire/recoil/
# end-sound), the FSM event clips on the viewmodel parts, the muzzle/shell
# userpoint resolution (FP + 3P), and the owner-bound effect anchors. The presenter
# keeps the camera/avatar/viewmodel NODES and the registered fixed-tick
# consumer (_present_fixed_weapon_tick — the cross-concern interlock that
# stamps the camera at production-tick pose); this class presents each batch
# against the presenter's live nodes through its public accessors.

const WEAPON_TICK_DT := MissionRuntime.TICK_DT  # the weapon FSM runs on the engine tick

# The world serves the presentation seams: the event drain, mission audio, the
# effect world, and the effect-anchor registry. Untyped for the same reason as
# the presenter's _world: GUT harness worlds serve value-only doubles.
var _world
# The owning LocalPlayerPresenter. Untyped: the class reads presenter presentation state
# (viewmodel parts, held weapon, camera, 1P/3P mode) through the presenter's public
# accessors, and GUT harnesses serve minimal presenter doubles in its place.
var _presenter
var _viewmodel_generation := 0      # action-slot owner identity across weapon re-mounts
# Live owner-bound effect anchors registered on the world (slot_key -> true);
# dropped whenever the viewmodel generation turns over.
var _registered_effect_anchor_keys: Dictionary = {}
var _weapon_play_serial := -1
var _weapon_view: PlayerWeaponView = null  # this tick's FSM view (body channel rides it)


func setup(world, presenter) -> void:
	_world = world
	_presenter = presenter


func teardown() -> void:
	# Drop our anchors from the world-side ItemEffectDirector BEFORE losing the
	# world reference: the director's registry is world-lifetime and its reset()
	# deliberately leaves owner keys to their owners — a discarded effects object
	# would otherwise leave its bound-resolver entries pinned there forever (the
	# Callable keeps this object alive, so the invalid-callable reap never fires).
	_unregister_effect_anchors()
	_world = null
	_presenter = null


# The sim, re-resolved per use: mission reloads free the runtime and its sim,
# so a cached reference would go stale (the presenter follows the same rule).
func _sim():
	return _world.get_sim() if _world != null else null


## This tick's FSM view snapshot — the presenter's avatar body channel and the
## emplaced viewmodel controls read it here.
func weapon_view() -> PlayerWeaponView:
	return _weapon_view


## The presenter's pre-adoption stamp: adopt the tick's snapshot before the camera
## pass so the avatar's body channel is current while visual roots are placed.
func set_weapon_view(view: PlayerWeaponView) -> void:
	_weapon_view = view


## Drop the presentation latches (the presenter's no-player / reset-state path).
func reset() -> void:
	_weapon_play_serial = -1
	_weapon_view = null


## Re-sync the clip serial: fresh viewmodel parts replay the active clip.
func reset_play_serial() -> void:
	_weapon_play_serial = -1


## A viewmodel re-mount (the presenter's refresh_viewmodel): the action slots get a
## new owner generation and the old generation's live anchors drop.
func on_viewmodel_refresh() -> void:
	_viewmodel_generation += 1
	_unregister_effect_anchors()


## A committed weapon switch from the sim: reinstall the FP viewmodel/FSM for the
## newly equipped def [orig: the mount's model re-resolve — the FP render model
## follows the equipped slot, count_weapon_effects_and_update_viewmodel @ 0x4dc9e0].
## Redundant reinstalls (the installed def already IS the target and its viewmodel
## exists) are skipped so the queued SWITCHTO draw-in survives.
func _apply_weapon_switch(weapon_name: String,
		preserve_slot_state: bool = false) -> void:
	if _world == null:
		return
	var viewmodel: Node3D = _presenter.viewmodel()
	if (String(_world.local_player_weapon_name()).nocasecmp_to(weapon_name) == 0
			and viewmodel != null and is_instance_valid(viewmodel)):
		return
	var switched := bool(_world.set_local_player_weapon_by_name(
			weapon_name, true)) if preserve_slot_state else bool(
					_world.set_local_player_weapon_by_name(weapon_name))
	if switched:
		_presenter.refresh_viewmodel()


func _apply_weapon_clear() -> void:
	if _world == null:
		return
	_world.clear_local_player_weapon()
	_presenter.refresh_viewmodel()


func _unregister_effect_anchors() -> void:
	if _world != null:
		for key in _registered_effect_anchor_keys:
			_world.unregister_effect_anchor(key)
	_registered_effect_anchor_keys.clear()


# Drain any ordered presentation events left for owners that do not install the
# fixed-tick callback. In the game, _present_fixed_weapon_tick consumes
# each 62.5 Hz batch before that tick's particle update. Every clip/begin/end
# payload survives either route; pre-aged fallback clips resume at their source age.
# Clip starts land on BOTH viewmodel parts. Scope side effects
# (forced unscope, rescope-after-reload) flip the SIM's own engaged bit — they
# arrive here already folded into the view snapshot.
# [orig: ActionSlot_BeginActivePhase @0x53f830 plays the action clip on the owner's
# animadm channel; the rescope block @0x54139e]
func consume_pending(view: PlayerWeaponView) -> void:
	if _world == null:
		return
	var events: Array[PlayerWeaponEvent] = _world.drain_local_player_weapon_events()
	consume(view, events)


func consume(view: PlayerWeaponView,
		events: Array[PlayerWeaponEvent],
		authoritative_phase: bool = false) -> void:
	_weapon_view = view
	if view == null:
		# Slot selection is control state, not viewmodel presentation. In
		# particular, an unarmed player has no view until UseGun installs one.
		for event in events:
			if event.clear_weapon:
				_apply_weapon_clear()
			elif not event.switch_to_weapon.is_empty():
				_apply_weapon_switch(
						event.switch_to_weapon, event.preserve_slot_state)
		_weapon_play_serial = -1
		return
	var batch_started_clip := false
	for event in events:
		if not event.anim_key.is_empty():
			batch_started_clip = true
			break
	if authoritative_phase and not batch_started_clip:
		# Advance the already-playing clip before same-tick direct effects sample
		# an action user point. A clip event below replaces this pose first.
		_play_viewmodel_clip(view.anim_key, view.anim_variant,
				view.anim_age_ticks, true)
	for event in events:
		if not event.anim_key.is_empty():
			_play_viewmodel_clip(event.anim_key, event.anim_variant,
					event.age_ticks, authoritative_phase)
		if event.action_started >= 0:
			_fire_action_effects(event)
		if event.action_effect >= 0:
			_fire_direct_action_effect(event)
		if event.action_finished >= 0:
			_fire_action_end_sound(event)
		if event.clear_weapon:
			_apply_weapon_clear()
		elif not event.switch_to_weapon.is_empty():
			_apply_weapon_switch(event.switch_to_weapon, event.preserve_slot_state)
		# event.switch_denied is the deny-sound seam [orig: PlaySoundOnDedicatedServer
		# (dword_24E08C4) @ 0x4e0354] — the shipped set name is unwitnessed (D-WPN-22).
	if batch_started_clip:
		_weapon_play_serial = view.play_serial
	# First adoption and a fresh viewmodel both synchronize to the latest snapshot,
	# but never replay the snapshot's historical sound/effect payloads.
	if view.play_serial != _weapon_play_serial:
		_weapon_play_serial = view.play_serial
		if not authoritative_phase:
			_play_viewmodel_clip(view.anim_key, view.anim_variant,
					view.anim_age_ticks, false)


# The FIRE action kind [orig: libs/world weapon_fsm.h weapon_action::kFire = 2] —
# the only local action-begin that takes the with-effect (muzzle) shim.
const WEAPON_ACTION_FIRE := 2


# Weapon particles always enter the global EffectWorld and render in the later
# world particle brackets, even when their position came from the first-person
# gun. Retail flushes the viewmodel mini-scene, restores the world projection,
# then runs EffectWorld_RenderParticlePass; there is no separate FP particle
# pass. [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0; particle pass
# @ 0x5f7240; ActionSlot_SpawnEffect @ 0x401f20]
const WEAPON_EFFECT_RENDER_DOMAIN := NovaEffectScene.RENDER_DOMAIN_WORLD


# EffectPose carries forward in basis column 2 (rather than Godot's camera -Z
# convention). Build a complete orthonormal frame so every generic producer
# reaches the same portable spawn contract.
func _weapon_effect_transform(position: Vector3, forward: Vector3) -> Transform3D:
	if forward.length_squared() <= 0.0001:
		return Transform3D(Basis.IDENTITY, position)
	var z_axis := forward.normalized()
	var seed_up := Vector3.UP
	if absf(z_axis.dot(seed_up)) > 0.999:
		seed_up = Vector3.FORWARD
	var x_axis := seed_up.cross(z_axis).normalized()
	var y_axis := z_axis.cross(x_axis).normalized()
	return Transform3D(Basis(x_axis, y_axis, z_axis), position)


# The action-begin SOUND + MUZZLE legs: play the started ACTION's soundset
# 3D-positional at the firing entity and spawn its particle effect at the weapon
# model's user point [orig: ActionSlot_ExecuteActionWithEffect @0x541860 plays the
# row's soundset and calls ActionSlot_SpawnEffect @0x401f20 with the row's
# particle + userpoint; the one-shot 3D placement is Sound_Play3DPositional
# @0x527cb0]. The ordered event batch preserves each begin leg when several ticks
# land in one frame.
#
# Particle gating is the witnessed local-player routing [orig:
# ActionSlot_ExecuteActionTick @0x541a70]: for the LOCAL player only the FIRE
# action takes the with-effect shim, and only in third person, from a vehicle, or
# un-scoped in first person (the FP muzzle-flash config dword_24D20C0 bit 0 rides
# that leg; treated always-on here) — every other local begin routes through the
# no-effect shim @0x5419e0 (no casing ejects in your own FP view; remote views
# spawn them via the remote leg @0x541a83, an MP seam).
func _fire_action_effects(event: PlayerWeaponEvent) -> void:
	if _world == null:
		return
	if not event.action_soundset.is_empty():
		var audio = _world.get_mission_audio()
		if audio != null:
			audio.fire_soundset(event.action_soundset, event.world_position, -1)
	if event.action_particle.is_empty():
		return
	if event.action_started != WEAPON_ACTION_FIRE:
		return  # local non-fire begins are the no-effect shim [orig: @0x541b17]
	# The suppression reads the event's SETTLED scope state. Retail promotes
	# g_weaponScopeActive before weapon actions on every tick; one render frame can
	# drain several ticks spanning that boundary, so the final render snapshot is
	# not a valid substitute. [orig: promoter @0x4de4f7 before weapon pump call
	# @0x526786; gate @0x541aba !g_weaponScopeActive]
	if event.scope_settled and not event.third_person and not event.vehicle_attack_context:
		return  # settled-scoped FP fire shows no muzzle flash [orig: @0x541aba]
	var fx = _world.get_effect_world()
	if fx == null:
		return
	var pos := _action_particle_world_position(event.action_particle_userpoint)
	var forward := _action_particle_world_forward(event.action_particle_userpoint)
	# The live handle belongs to the runtime ACTION slot, not the whole player
	# presenter. A weapon re-mount creates a new slot generation. (The instance id is
	# this presentation object's — it lives setup-to-teardown with the presenter.)
	var slot_key := "%d:%d:%d" % [get_instance_id(), _viewmodel_generation, event.action_started]
	var anchor_transform := _weapon_effect_transform(pos, forward)
	# The live group follows the SPAWNING action's userpoint for its whole life:
	# retail records the handle + action index on the slot and the pump re-anchors
	# the emitter to that action's bone every tick, releasing it only on death
	# [orig: ActionSlot_SpawnEffect handle/action record @ 0x40208f/0x402092 ->
	# the +0x18 tracker leg in WeaponAction_ProcessFrame @ 0x540edf ->
	# CEffectEmitter_UpdatePositionAndParams @ 0x5f6810]. The presenter analog is an
	# owner-bound group whose anchor resolver re-reads the live userpoint pose.
	_world.register_effect_anchor(slot_key,
			_weapon_effect_anchor_transform.bind(event.action_particle_userpoint))
	_registered_effect_anchor_keys[slot_key] = true
	fx.spawn_effect_request(event.action_particle, anchor_transform, {
		"admission": NovaEffectScene.ADMISSION_SUPPRESS_WHILE_OWNED,
		"binding": NovaEffectScene.BINDING_FOLLOW_OWNER,
		"render_domain": WEAPON_EFFECT_RENDER_DOMAIN,
		"slot_key": slot_key,
		"owner_key": slot_key,
		"owner_transform": anchor_transform,
		"initial_age_ticks": maxi(event.age_ticks, 0),
	})


# The recoil-row DIRECT effect leg is entirely data-defined. Retail submits
# every authored particle (muzzle, casing, smoke, or another user point) with
# param7=0: no scope gate, name/user-point classification, or live-slot handle.
# Each event is therefore an Always transient in its production tick's render
# domain, pre-aged when multiple fixed ticks are presented together.
# [orig: WeaponAction_Recoil @ 0x542dd0, spawn @ 0x542f64 with param7=0]
func _fire_direct_action_effect(event: PlayerWeaponEvent) -> void:
	if _world == null or event.effect_particle.is_empty():
		return
	var fx = _world.get_effect_world()
	if fx == null:
		return
	var pos := _action_particle_world_position(event.effect_particle_userpoint)
	var forward := _action_particle_world_forward(event.effect_particle_userpoint)
	fx.spawn_effect_transient(event.effect_particle, pos, forward,
			maxi(event.age_ticks, 0), WEAPON_EFFECT_RENDER_DOMAIN, 0, 0)


## The live anchor for an owner-bound weapon-effect group: the spawning action's
## userpoint through the CURRENT viewmodel pose, or null once the viewmodel is
## gone (the effect world then unpins the group at its last pose)
## [orig: the actionEffectHandle tracker @ 0x540edf re-reads
## actionTable[slot+0x28]'s bone until the emitter dies].
func _weapon_effect_anchor_transform(userpoint: String) -> Variant:
	if _presenter == null:
		return null  # torn down; a stale resolver poll must degrade, not error
	var viewmodel: Node3D = _presenter.viewmodel()
	if viewmodel == null or not is_instance_valid(viewmodel):
		return null
	var pos := _action_particle_world_position(userpoint)
	var forward := _action_particle_world_forward(userpoint)
	return _weapon_effect_transform(pos, forward)


# Map a model-space action userpoint through the live fake-skinned weapon bone.
# Rigid first-person gun parts ride the .adm skeleton by subobject/bone index, so
# applying only the model root leaves authored muzzle points in the rest pose (and,
# for the AK, behind the gameplay camera). Convert model space into the bone's rest
# frame, then back through its current global pose — the ported equivalent of the
# original action-bone transform.
# [orig: Entity_ComputeActionTransform @0x401310 -> ActionSlot_SpawnEffect @0x401f20]
func _action_particle_model_to_world(part: Node3D, info: Dictionary) -> Transform3D:
	if part != null and part.has_method("get_skeleton"):
		var skeleton := part.call("get_skeleton") as Skeleton3D
		var subobject := int(info.get("subobject", -1))
		if skeleton != null and subobject >= 0 and subobject < skeleton.get_bone_count():
			return (skeleton.global_transform
					* skeleton.get_bone_global_pose(subobject)
					* skeleton.get_bone_global_rest(subobject).affine_inverse())
	return part.global_transform if part != null else Transform3D.IDENTITY


# World-space spawn point for an ACTION particle: the named user point on a
# viewmodel part (the gun carries the muzzle points), composed through its live
# subobject/bone pose. Falls back to the first part's origin, then the player eye.
# The THIRD-PERSON action-particle anchor: the same authored userpoint name resolved
# against the gfx3 world gun instead of the first-person viewmodel. Retail keeps two
# resolved indices for one authored name — ActionDef+56 against gfx1 and +57 against
# gfx3 — and picks by the first-person bit, which requires the camera to be in first
# person at all [orig: the FP bit gate @0x540e8c..0x540eca requires g_camera_mode == 0;
# the gfx1/gfx3 resolvers @0x54039e/@0x54040f].
#
# This matters because the presenter's vm_parts are the FIRST-PERSON viewmodel: it is
# re-pinned to the camera every frame and merely HIDDEN in third person, never
# detached, so resolving against it while in third person anchors the muzzle flash
# to the player's own eye.
# The weapon model is drawn rigid at its attach transform, so a model-space userpoint
# just rides that transform.
# Returns { "pos": Vector3, "dir": Vector3 } or an empty Dictionary when unresolved.
func _third_person_action_particle(userpoint: String) -> Dictionary:
	if not _presenter.is_third_person() or userpoint.is_empty():
		return {}
	var held_weapon: Node3D = _presenter.held_weapon()
	if held_weapon == null or not is_instance_valid(held_weapon) \
			or not held_weapon.visible or not held_weapon.has_method("get_object_data"):
		return {}
	var data = held_weapon.get_object_data()
	if data == null or not data.has_method("get_user_point_count"):
		return {}
	var xform: Transform3D = held_weapon.global_transform
	for i in range(int(data.get_user_point_count())):
		var info: Dictionary = data.get_user_point_info(i)
		if String(info.get("name", "")).nocasecmp_to(userpoint) != 0:
			continue
		var direction: Vector3 = xform.basis * Vector3(info.get("rotation", Vector3(0, 0, 1)))
		return {
			"pos": xform * Vector3(info.get("position", Vector3.ZERO)),
			"dir": direction.normalized() if direction.length_squared() > 0.000001
					else -xform.basis.z.normalized(),
		}
	return {}


func _action_particle_world_position(userpoint: String) -> Vector3:
	var tp := _third_person_action_particle(userpoint)
	if not tp.is_empty():
		return tp["pos"]
	var fallback := Vector3.INF
	for part in _presenter.vm_parts():
		if part == null or not is_instance_valid(part) or not part.has_method("get_object_data"):
			continue
		if fallback == Vector3.INF:
			fallback = part.global_transform.origin
		if userpoint.is_empty():
			continue
		var data = part.get_object_data()
		if data == null:
			continue
		for i in range(data.get_user_point_count()):
			var info: Dictionary = data.get_user_point_info(i)
			if String(info.get("name", "")).nocasecmp_to(userpoint) == 0:
				var model_to_world := _action_particle_model_to_world(part as Node3D, info)
				return model_to_world * Vector3(info.get("position", Vector3.ZERO))
	if fallback != Vector3.INF:
		return fallback
	# Retail's deepest fallback is the ENTITY ORIGIN [orig: loc_401867 @0x401867..0x401887
	# copies entity+4/+8/+0xC]. The eye was our own invention and put the flash on the
	# player's face whenever a userpoint failed to resolve.
	var sim = _sim()
	return sim.get_local_player_position() if sim != null else Vector3.ZERO


func _action_particle_world_forward(userpoint: String) -> Vector3:
	var tp := _third_person_action_particle(userpoint)
	if not tp.is_empty():
		return tp["dir"]
	for part in _presenter.vm_parts():
		if part == null or not is_instance_valid(part) or not part.has_method("get_object_data"):
			continue
		var data = part.get_object_data()
		if data == null:
			continue
		for i in range(data.get_user_point_count()):
			var info: Dictionary = data.get_user_point_info(i)
			if String(info.get("name", "")).nocasecmp_to(userpoint) != 0:
				continue
			var direction := Vector3(info.get("rotation", Vector3(0, 0, 1)))
			var model_to_world := _action_particle_model_to_world(part as Node3D, info)
			var world_direction: Vector3 = model_to_world.basis * direction
			if world_direction.length_squared() > 0.000001:
				return world_direction.normalized()
	var camera: Camera3D = _presenter.camera()
	if camera != null:
		return -camera.global_transform.basis.z.normalized()
	return Vector3(0, 0, 1)


# The action-END sound leg: the finished ACTION's soundsetend, 3D-positional at the
# firing entity — the fire rows' per-shot gunshot (GS_*) and the reload completion.
# [orig: ActionSlot_FinishActivePhase @0x53f7b0 -> the end shim @0x401100 plays
#  ActionDef+12 at the owner entity, gated on the phase byte being 2 (ACTIVE); its
#  dupsound repeat loop (+44/+48) is data-dead in the JOX/REVX corpora]
func _fire_action_end_sound(event: PlayerWeaponEvent) -> void:
	if _world == null or event.action_end_soundset.is_empty():
		return
	var audio = _world.get_mission_audio()
	if audio != null:
		audio.fire_soundset(event.action_end_soundset, event.world_position, -1)


# Start an FSM clip on every viewmodel part (arms + gun share the animadm) - a replay
# of the active key restarts it (fire/recoil re-triggers), unlike play_body_clip's
# same-key resume. `variant` is the sim ring's latched serve for multi-clip .adm
# rows — both parts follow the ONE latch, so arms and gun never split variants
# [orig: AnimMap_PlayAnimBySlot @0x40bda0 latches the served entry at animState+68].
func _play_viewmodel_clip(key: String, variant: int = 0, age_ticks: int = 0,
		authoritative_phase: bool = false) -> void:
	if key.is_empty():
		return
	var seconds := float(maxi(age_ticks, 0)) * WEAPON_TICK_DT
	for part in _presenter.vm_parts():
		if part == null or not is_instance_valid(part):
			continue
		if authoritative_phase and part.has_method("play_body_clip_variant_at_time"):
			part.play_body_clip_variant_at_time(key, variant, seconds)
			continue
		if part.has_method("play_body_clip_variant"):
			part.play_body_clip_variant(key, variant)
		else:
			part.play_body_clip(key)
		if part.has_method("set_animation_time"):
			part.set_animation_time(seconds)
