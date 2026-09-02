class_name WorldPlayerVisuals
extends RefCounted

# The local-player visuals, extracted from GameWorld: the third-person
# avatar/held-gun builders, the first-person viewmodel composition (gun +
# character arms), the armory weapon apply/clear + spawn-loadout projection,
# and the typed local-player view/weapon decodes (ADR 0017 edges). Plain
# RefCounted on the internal-member pattern (OcclusionFramePass /
# ItemEffectDirector): it owns no Nodes — built models parent under the world.
#
# GameWorld keeps a same-name delegate for every public name here (presenters,
# probes, the debug-control table, and the tests all call the world), and the
# harness override points — local_player_viewmodel_def,
# local_player_character_id, _set_local_player_first_person_model_available,
# set_local_player_weapon_by_name, clear_local_player_weapon — are always
# invoked as _world.<name>() from this component so subclass overrides keep
# binding (game_world_test's ViewmodelWorldHarness and armory_presenter_test's
# ArmoryWorldHarness pin that). The _viewmodel_*/_local_weapon_* state stays
# on GameWorld (unload() resets it) and is reached through _world.

# The GameWorld whose local player these visuals present.
var _world: GameWorld


## One-time wiring from the owning GameWorld (constructed in the world's
## _init, before any load).
func setup(world: GameWorld) -> void:
	_world = world


## Build a GameWorld-managed avatar model for the local player (which has no BMS placement of its
## own). The caller (LocalPlayerPresenter) positions it and swaps its visual/shadow policy per
## first/third person. In first person the body remains a live SHADOWS_ONLY source, but neither the
## gameplay beauty camera nor the water mirror renders it: retail's reflected entity collector has
## no player/person leg [orig: Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0]. Null when
## the resource root / item graphic is unavailable. The player runtime type id is the
## bound MissionObjectPlacer.PLAYER_RUNTIME_TYPE_ID [net-re §5.2b].
## The soldier's THIRD-PERSON gun. Built as a SIBLING of the avatar rather than a child:
## ObjectModel.rebuild() frees all of its children, so a weapon parented under the
## avatar would silently vanish whenever the body model rebuilds. It carries no skeleton
## and no clip — the original stamps ONE matrix into every bone slot of this model, i.e.
## it is drawn rigid, posed entirely by its attach basis.
## [orig: BoneCallback_org0_World draw 5 @0x4e3c87..0x4e3d99; model = WeaponDef.tpModel
##  (+0x170, weapon.def gfx3) @0x4e3cd3]
func build_local_player_held_weapon(graphic: String) -> ObjectModel:
	if _world._placer == null or graphic.is_empty():
		return null
	var model: ObjectModel = _world._placer.build_model_from_graphic(
			graphic, "", _world, "", "", true)
	if model != null:
		model.set_shadow_caster_enabled(true)
		# The 3P gun silhouettes inside the AVATAR's render slot, exactly like
		# retail's child walk (RenderSlot_RenderEntityAndChildren renders the
		# held weapon with the person) — never in a slot of its own.
		if _world._local_view_presenter != null:
			model.set_slot_shadow_capture_with(_world._local_view_presenter.avatar())
	return model


func build_local_player_avatar() -> Node3D:
	if _world._placer == null:
		return null
	# _env wires the TOD-reactive lighting/fog stamp — without it the avatar
	# freezes at the noon preview defaults (retail relights every entity per
	# frame; witness: placement_traits.h ledger).
	return _world._placer.build_player_animated_model(
			MissionObjectPlacer.PLAYER_RUNTIME_TYPE_ID, _world,
			_world.local_player_character_id())


func _local_player_visual_spec() -> PlayerVisualSpec:
	if _world._placer == null:
		return null
	return _world._placer.resolve_player_visual_spec(
			MissionObjectPlacer.PLAYER_RUNTIME_TYPE_ID,
			_world.local_player_character_id())


# Resolve the .3DI definitions that LocalPlayerPresenter would otherwise load only on
# its first visible frame. Retail's Game_ReloadEntityModelsAndCallbacks and HUD
# model pass load the player + current weapon overlay before CEffectWorld_RebuildAllModelBuffers freezes
# the C2S 0x3D source; doing the lightweight data lookup here gives our snapshot
# the same boundary without constructing hidden scene nodes. Later builders hit
# the placer's cache, so they cannot introduce a definition just after freeze.
func _prewarm_loaded_model_challenge_definitions() -> void:
	if _world._placer == null:
		return
	# By the deployment/admission boundary the complete initial world stream has
	# populated the joiner's replica snapshot. (S2C 0x11 itself comes earlier and
	# releases the client's C2S 0x0A world request.) Resolve each unique wire type
	# now so the C2S 0x3D loaded-model page freezes before the first visible frame.
	var challenge_sim: Simulation = _world._runtime.get_sim() \
			if _world._runtime != null else null
	if challenge_sim != null:
		var stride := int(challenge_sim.get_present_stride())
		var snapshot: PackedFloat32Array = challenge_sim.get_present_snapshot()
		var warmed_types := {}
		if stride >= Simulation.PF_STRIDE:
			for row in range(int(snapshot.size() / stride)):
				var runtime_type_id := int(
						snapshot[row * stride + Simulation.PF_TYPE_ID])
				if runtime_type_id == 0 or warmed_types.has(runtime_type_id):
					continue
				warmed_types[runtime_type_id] = true
				var visual_item_id := int(
						_world._placer.resolve_player_visual_item_id(runtime_type_id))
				var wire_graphic := String(_world._placer.graphic_for(visual_item_id))
				if not wire_graphic.is_empty():
					_world._placer.object_data_for(wire_graphic)
		# The header-only join learned its entity types from the stream after
		# MissionPresentation's ordinary mission-body setup. Resolve the model-derived
		# seat/emplacement table and world collision/trait consumers now, before
		# admission and before the loaded-model challenge page freezes.
		if _world._loaded_mission != null and _world._loaded_mission.is_wire_header_only():
			var item_db: ItemDatabase = _world._placer.get_item_db()
			if item_db != null:
				# S16: the native extractor reads model userpoints through the
				# sim's own parse cache, so the asset root wires FIRST (the
				# shell extractor that read models render-side is gone).
				if _world._resource_root != null:
					challenge_sim.set_asset_root(_world._resource_root)
				var wire_type_ids := PackedInt32Array()
				for warmed_type in warmed_types.keys():
					wire_type_ids.append(int(warmed_type))
				challenge_sim.install_seat_specs_for_type_ids(
						item_db, wire_type_ids)
				challenge_sim.resolve_item_traits(item_db)
				challenge_sim.resolve_collision_instances(item_db)
				challenge_sim.occlusion_init_mission()
	var player_visual_item_id := int(_world._placer.resolve_player_visual_item_id(
			MissionObjectPlacer.PLAYER_RUNTIME_TYPE_ID))
	var avatar_graphic := String(_world._placer.graphic_for(player_visual_item_id))
	if not avatar_graphic.is_empty():
		_world._placer.object_data_for(avatar_graphic)

	if _world._viewmodel_weapon_cleared:
		return
	var def := _world.local_player_viewmodel_def()
	var character_spec := _local_player_visual_spec()
	var spec := Simulation.fp_viewmodel_spec(def != null,
			def.gfx1 if def != null else "",
			character_spec.arms if character_spec != null else "",
			def.animadm if def != null else "", def.flags if def != null else 0)
	if not spec.gun.is_empty():
		_world._placer.object_data_for(spec.gun)
	if spec.show_arms and not spec.arms.is_empty():
		_world._placer.object_data_for(spec.arms)


## Armory apply, presentation side: point the FP viewmodel + action FSM at `weapon_name`.
## Validates against weapon.def; the caller (main_game) drops the old viewmodel so the
## per-frame pass rebuilds gun/arms/FSM from the new def [orig: the ACCEPT re-mount,
## WeaponLoadout_ApplyFromBuffer @0x565cd0 -> Player_MountWeaponSlot @0x4dfa40].
func set_local_player_weapon_by_name(weapon_name: String,
		preserve_slot_state: bool = false) -> bool:
	if weapon_name.is_empty():
		return false
	var weapon_db := _world.get_weapon_database()
	var index: int = weapon_db.find_weapon(weapon_name) if weapon_db != null else -1
	if index < 0:
		push_warning("GameWorld: armory weapon '%s' not in weapon.def — keeping current" % weapon_name)
		return false
	_world._viewmodel_weapon_override = weapon_name
	_world._viewmodel_weapon_cleared = false
	_world._local_weapon_preserve_slot_state = preserve_slot_state
	# The render-side def record (viewmodel gfx/adm/fov reads + the name guard).
	_world._local_weapon = weapon_db.get_weapon(index)
	var sim := _world.get_sim()
	if sim != null:
		_world._set_local_player_first_person_model_available(false)
		# One-step native mount (S6b, ADR 0028): the sim bakes the FSM from its
		# RETAINED weapon.def row and seeds the clip rings from the rig's own
		# .adm — the mount is not hostage to the FP model load, matching the
		# retail ACCEPT chain exactly [orig: WeaponSlotTable_LoadAllFromDefs
		# @0x5414e0 + Player_MountWeaponSlot @0x4dfa40; the FP model resolve is
		# a separate per-frame render consumer @0x4ded60].
		if not bool(sim.install_local_player_weapon_by_name(
				weapon_name, _world._local_weapon_preserve_slot_state)):
			push_warning("GameWorld: sim has no retained weapon.def row for '%s'" % weapon_name)
			return false
	return true


func _apply_local_player_spawn_loadout() -> void:
	var loadout := _world._local_player_spawn_loadout
	_world._local_player_spawn_loadout = {}
	var sim := _world.get_sim()
	if sim == null:
		return
	var has_loadout := false
	for slot_key in ["primary", "secondary", "accessory"]:
		if loadout.has(slot_key):
			has_loadout = true
			break
	if loadout.has("player_class"):
		sim.set_local_player_class(int(loadout.get("player_class", 0)))
	# Mission-authored kits outrank the profile selection. Unlike the inventory
	# itself, this source bit stays false for load_weapon_table's WPN_M4AUTO
	# fallback, so a real default weapon cannot masquerade as mission policy.
	if bool(sim.has_explicit_spawn_loadout()):
		_sync_local_player_weapon_from_inventory(sim)
		return
	if not has_loadout:
		return
	var kit: Array[Dictionary] = []
	for slot_key in ["primary", "secondary", "accessory"]:
		var weapon_name := String(loadout.get(slot_key, ""))
		if weapon_name.is_empty():
			continue
		kit.append({
			"name": weapon_name,
			"ammo_primary": int(loadout.get(slot_key + "_clips", -1)),
			"ammo_secondary": -1,
			"flags": -1,
		})
	if not bool(sim.apply_local_player_loadout(kit, int(loadout.get("player_class", 0)))):
		return
	if kit.is_empty():
		_world.clear_local_player_weapon()
		return
	_sync_local_player_weapon_from_inventory(sim)


func _sync_local_player_weapon_from_inventory(sim: Simulation) -> void:
	var inventory: Dictionary = sim.get_local_player_inventory()
	if not bool(inventory.get("valid", false)):
		return
	var equipped := String(inventory.get("equipped_name", ""))
	# A syntactically nonempty kit can still be rejected by mission/class rules.
	# Keep the presentation aligned with the resulting authoritative inventory.
	if equipped.is_empty():
		_world.clear_local_player_weapon()
		return
	_world.set_local_player_weapon_by_name(equipped)


## Armory NONE: clear the equipped render/FSM state instead of falling back to the
## pre-armory default model on the next frame.
func clear_local_player_weapon() -> void:
	_world._viewmodel_weapon_override = ""
	_world._viewmodel_weapon_cleared = true
	_world._local_weapon = null
	_world._viewmodel_def_name = ""
	_world._viewmodel_def = null
	_world._local_weapon_preserve_slot_state = false
	var sim := _world.get_sim()
	if sim != null:
		_world._set_local_player_first_person_model_available(false)
		sim.clear_local_player_weapon()


## Build a GameWorld-managed FIRST-PERSON weapon viewmodel for the local player (shown in 1st person; the
## inverse of the 3rd-person avatar). Faithful composition: the equipped weapon's FP gun model PLUS
## the local player's CHARACTER arms, sharing one skeleton [orig: Player_RenderFirstPersonViewModel
## @0x4ded60 draws the weapon FP model, then the CharacterEntity's arms model (blip+8, the Avatars.def
## combo arms graphic @0x4df05f/@0x4deff4) with the same bone matrices, after Avatar_SetArmsCamoCtrl
## @0x4df008/@0x4df070]. The gun + animadm come from the mounted root's weapon.def (gfx1 / animadm
## [orig: WeaponDef_ParseProperty @0x54d730]; the file's gfx1a/gfx1b tokens are parsed-and-discarded
## by retail and never name the arms), the arms from the resolved character. Camera sway / fire-kick /
## ADS [orig: Player_UpdateFirstPersonCamera @0x4dd380] are follow-ups. Null when the placer or both
## models fail to resolve.
func build_local_player_viewmodel() -> Node3D:
	if _world._placer == null:
		_world._set_local_player_first_person_model_available(false)
		return null
	if _world._viewmodel_weapon_cleared:
		_world._set_local_player_first_person_model_available(false)
		return null
	var container := Node3D.new()
	container.name = "PlayerViewmodel"
	_world.add_child(container)
	# anim_wpn_idle = the FP holding pose; without it the arms sit in their bind/T-pose.
	# _env: the viewmodel lights/fogs with the live TOD like every entity
	# (retail draws the FP model through the same lighting constants
	# [orig: Player_RenderFirstPersonViewModel @ 0x4ded60 -> the ctx block]).
	var def := _world.local_player_viewmodel_def()
	# The submit spec (gun/arms/clip-adm + the emplaced arms omission) resolves
	# natively in simassets; the AK set is only the no-definition bring-up
	# fallback and a resolved def with no fpModel intentionally submits no gun.
	# [orig: Player_RenderFirstPersonViewModel @0x4ded60; @0x4dedc7]
	var character_spec := _local_player_visual_spec()
	var spec := Simulation.fp_viewmodel_spec(def != null,
			def.gfx1 if def != null else "",
			character_spec.arms if character_spec != null else "",
			def.animadm if def != null else "",
			def.flags if def != null else 0)
	var gun_name := spec.gun
	var arms_name := spec.arms
	var adm_name := spec.adm
	var show_arms := spec.show_arms
	# Both submits reuse the equipped GUN's model table, while `adm_name` supplies the clips.
	# Some valid retail sets differ (M21B_1st: 42 parts, M21_1st: 40); sizing from the ADM
	# basename truncates late animated parts such as the M14 magazine. [orig: @0x4ded60]
	var arms: ObjectModel = _world._placer.build_model_from_graphic(arms_name,
			adm_name, container, "anim_wpn_idle", gun_name) if show_arms else null
	var gun: ObjectModel = _world._placer.build_model_from_graphic(gun_name,
			adm_name, container, "anim_wpn_idle", gun_name) 			if not gun_name.is_empty() else null
	_world._local_viewmodel_parts.clear()
	if arms != null:
		# The arms' own raw camo triplet, stored by the rig's per-submit FP writer
		# alongside TEX_TEAM/HEAT_GLOW [orig: Avatar_SetArmsCamoCtrl @0x57a3b0
		# immediately before each FP arms submit @0x4df008/@0x4df070].
		arms.set_meta("avatar_part", "arms")
		arms.set_meta("avatar_graphic", arms_name)
		arms.set_meta("avatar_camo",
				character_spec.arms_camo if character_spec != null else Vector3i())
		_world._local_viewmodel_parts.append(arms)
	if gun != null:
		_world._local_viewmodel_parts.append(gun)
	_world._set_local_player_first_person_model_available(gun != null)
	if show_arms and arms == null:
		push_warning("GameWorld: FP arms model '%s' failed to load from the resource root" % arms_name)
	if gun == null and not gun_name.is_empty():
		push_warning("GameWorld: FP gun model '%s' failed to load from the resource root" % gun_name)
	if arms == null and gun == null:
		# A valid definition with no resolved fpModel is a stable, intentionally
		# empty presentation epoch. Returning its container prevents the caller from
		# retrying every frame or substituting a different weapon.
		if def == null:
			container.queue_free()
			return null
		return container
	# The FSM and its clip rings installed natively at ACCEPT time (S6b) — the
	# model resolve is purely presentational now, as in retail [orig: the FP
	# model resolve @0x4ded60 is a render consumer, not a mount].
	return container


## The actual first-person arms submit bound to the authority-stamped character
## identity. Capture probes consume this public semantic witness rather than
## guessing from the profile request or scanning the scene tree.
func local_player_first_person_arms_witness() -> FirstPersonArmsWitness:
	var witness := FirstPersonArmsWitness.new()
	var character_spec := _local_player_visual_spec()
	var expected_graphic := character_spec.arms if character_spec != null else ""
	var expected_camo := character_spec.arms_camo if character_spec != null else Vector3i()
	for part: ObjectModel in _world._local_viewmodel_parts:
		if part == null or not is_instance_valid(part) \
				or String(part.get_meta("avatar_part", "")) != "arms":
			continue
		if not part.is_visible_in_tree():
			witness.error = "submitted first-person arms are not visible in tree"
			return witness
		var actual_graphic := String(part.get_meta("avatar_graphic", ""))
		var actual_camo: Vector3i = part.get_meta("avatar_camo", Vector3i())
		if actual_graphic != expected_graphic or actual_camo != expected_camo:
			witness.error = (
					"submitted first-person arms do not match the resolved character")
			return witness
		witness.character_id = _world.local_player_character_id()
		witness.arms_graphic = actual_graphic
		witness.arms_camo = PackedInt32Array([actual_camo.x, actual_camo.y, actual_camo.z])
		return witness
	witness.error = "no submitted first-person arms are available"
	return witness


## The installed FP weapon's name (empty when none) — the switch-event guard
## against redundant viewmodel reinstalls.
func local_player_weapon_name() -> String:
	return _world._local_weapon.name if _world._local_weapon != null else ""


## Feed only the first-person-visible NVG state into world lighting. The raw
## active state deliberately survives third person in the simulation.
func set_local_player_nvg_view(active: bool, gain: int) -> void:
	if _world._env != null:
		_world._env.set_nvg_view(active, gain)


## The 62.5 Hz view state (ADS ease, fov policy, 3P anchor), decoded once at this
## edge (ADR 0017); null without a sim.
func local_player_view() -> PlayerLocalView:
	var sim := _world.get_sim()
	if sim == null:
		return null
	return sim.get_local_player_view()


## The equipped weapon's HUD slice (error table, HUDCLIPGFX/HUDRNDGFX, clipsize, name),
## decoded from WeaponDatabase's transport dict at this edge (ADR 0017) — the HUD
## reads it per frame, mirroring the original HUD info struct's weapon-def pointer
## [orig: HUD_BuildEntityInfo @0x4b8561 -> hudInfo+552]. Null until a weapon resolves.
func local_player_hud_weapon_def() -> PlayerHudWeaponDef:
	return PlayerHudWeaponDef.from_weapon_def(_world._local_weapon)


## The equipped-weapon FSM view, decoded once at this edge (ADR 0017); null when no
## weapon FSM is installed.
func local_player_weapon_view() -> PlayerWeaponView:
	var sim := _world.get_sim()
	if sim == null:
		return null
	return PlayerWeaponView.from_state_dict(sim.get_local_player_weapon_state())


## Destructively drain the equipped FSM's ordered presentation batch, decoding the
## C++ transport Dictionaries at this one adapter edge (ADR 0017).
func drain_local_player_weapon_events() -> Array[PlayerWeaponEvent]:
	var out: Array[PlayerWeaponEvent] = []
	var sim := _world.get_sim()
	if sim == null:
		return out
	for row in sim.drain_local_player_weapon_events():
		out.append(PlayerWeaponEvent.from_event_dict(row as Dictionary))
	return out


## Register the GameWorld presenter for fixed-tick weapon events. The game
## installs LocalPlayerPresenter here; headless/runtime-only hosts leave it
## invalid and may drain the typed event queue explicitly.
func set_local_player_weapon_tick_consumer(consumer: Callable) -> void:
	_world._local_player_weapon_tick_consumer = consumer


## The resolved weapon.def record driving the FP viewmodel: model/adm names plus the
## witnessed view-bias fields (pos/tpos raw units + rot degrees, renderfov horizontal
## degrees) LocalPlayerPresenter consumes — decoded from WeaponDatabase's transport dict
## at this edge (ADR 0017). Null when the mounted root has no weapon.def or the weapon
## name is absent — callers keep their witnessed JOX AK-47 defaults then. The weapon is
## the bring-up fallback until equipped-weapon resolution lands; the debug
## `set_viewmodel_weapon` control (set_local_player_weapon_by_name over MCP/F3)
## rigs A/B against another SKU's def.
func local_player_viewmodel_def() -> PlayerViewmodelDef:
	if _world._viewmodel_weapon_cleared:
		return null
	var weapon_db := _world.get_weapon_database()
	if weapon_db == null:
		return null
	# Precedence: the armory-equipped (or debug-selected) weapon, else the fixed
	# default until first equip.
	var weapon_name := _world._viewmodel_weapon_override
	if weapon_name.is_empty():
		weapon_name = Simulation.viewmodel_bringup_fallback_weapon()
	# Retail reads the equipped slot's def pointer, resolved when the slot was
	# mounted; the decoded record is keyed on the name it resolved from, so
	# the presenter's per-frame read costs one string compare.
	if weapon_name == _world._viewmodel_def_name and _world._viewmodel_def != null:
		return _world._viewmodel_def
	var index: int = weapon_db.find_weapon(weapon_name)
	if index < 0:
		push_warning("GameWorld: weapon '%s' not in weapon.def — FP viewmodel keeps built-in defaults" % weapon_name)
		return null
	_world._local_weapon = weapon_db.get_weapon(index)
	_world._viewmodel_def_name = weapon_name
	_world._viewmodel_def = PlayerViewmodelDef.from_weapon_def(_world._local_weapon)
	return _world._viewmodel_def
