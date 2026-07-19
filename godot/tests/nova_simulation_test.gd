extends GutTest

# NovaSimulation (the GDExtension binding): promote a synthetic BMS mission into a live
# world + AI system, tick it, and confirm the AI walks entities along their authored route.
# This is the in-Godot end of step 1 (promotion) + step 2 (locomotion).

func test_demo_mission_promotes() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_true(sim.is_loaded(), "demo mission promoted")
	assert_eq(sim.get_entity_count(), 2, "two organics got AI brains")
	assert_eq(sim.get_brain_count(), 2, "two AI brains attached")
	assert_eq(sim.get_spawned_count(), 6, "one building + three markers + two organics spawned into pools")
	assert_eq(sim.get_entity_state(0), 16, "a routed organic starts in GROUND_FOLLOWWP (16)")
	sim.free()


# The HUD waypoint track: the demo mission's BLUE route becomes the player track;
# a spawned local player latches waypoint 0 on the first tick and walking into the
# radius advances. [orig chain: NetPacket_WriteWorldStateLoad0x0F @0x502e41 list ->
# Player_UpdatePerFrame @0x4de5f7 advance; docs/interface/hud-re.md §Waypoint HUD]
func test_waypoint_hud_view_tracks_the_demo_route() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var wp: Dictionary = sim.get_waypoint_hud_view()
	assert_true(bool(wp.get("show", false)), "waypoints visible by default")
	assert_eq(int(wp.get("count", 0)), 3, "the blue route's three markers")
	assert_eq(int(wp.get("current", 0)), -1, "no selection before a player tick")

	# Spawn the local player far from marker 0 (demo marker 0 = mission (100,0,0)
	# = Godot (100, 0, 0); radius 25). The first tick latches entry 0.
	assert_true(sim.spawn_local_player(Vector3(0, 0, 0), 0.0, 1))
	sim.step()
	wp = sim.get_waypoint_hud_view()
	assert_eq(int(wp.get("current", -1)), 0, "first tick latches waypoint 0")
	assert_eq(int(wp.get("number", 0)), 1, "1-based display number")
	assert_eq(int(wp.get("name_id", -1)), 1, "marker 0's authored name id")
	var pos: Vector3 = wp.get("position", Vector3.ZERO)
	assert_almost_eq(pos.x, 100.0, 0.01, "marker 0 world X")

	# Teleport inside the 25 u radius (mission space; the player is AI index 2,
	# after the demo's two organics): the next tick advances to waypoint 1.
	sim.debug_set_entity_position(2, Vector3(95, 0, 0))
	sim.step()
	wp = sim.get_waypoint_hud_view()
	assert_eq(int(wp.get("current", -1)), 1, "proximity advance onto waypoint 1")
	sim.free()

const ANIM_FIXTURES := "res://../fixtures/anim"


class ObjectDataPlacerStub:
	extends RefCounted
	var data: NovaObjectData

	func _init(p_data: NovaObjectData) -> void:
		data = p_data

	func object_data_for(_graphic: String) -> NovaObjectData:
		return data


class SkeletalDataPlacerStub:
	extends RefCounted
	var data: NovaObjectData
	var skeletal: NovaSkeletalAnim

	func _init(p_data: NovaObjectData, p_skeletal: NovaSkeletalAnim) -> void:
		data = p_data
		skeletal = p_skeletal

	func object_data_for(_graphic: String) -> NovaObjectData:
		return data

	func skeletal_anim_for(
			_item_id: int, _graphic: String) -> NovaSkeletalAnim:
		return skeletal


class PlayerOnlySkeletalPlacerStub:
	extends RefCounted
	var data: NovaObjectData
	var skeletal: NovaSkeletalAnim

	func _init(p_data: NovaObjectData, p_skeletal: NovaSkeletalAnim) -> void:
		data = p_data
		skeletal = p_skeletal

	func object_data_for(graphic: String) -> NovaObjectData:
		return data if graphic.nocasecmp_to("US01") == 0 else null

	func skeletal_anim_for(
			_item_id: int, graphic: String) -> NovaSkeletalAnim:
		return skeletal if graphic.nocasecmp_to("US01") == 0 else null

func _anim_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES))
	return root


func _minimal_weapon(name: String, animadm: String) -> Dictionary:
	return {
		"name": name,
		"animadm": animadm,
		"actions": [],
		"flags": 0,
		"clipsize": 0,
		"startrounds": 0,
	}


func _weapon_arm_pitch_deg(sim: NovaSimulation) -> float:
	var overlay: Dictionary = sim.get_local_player_aim_overlay()
	var angles: PackedVector3Array = overlay.get("angles", PackedVector3Array())
	return float(angles[4].x) if angles.size() > 4 else 0.0


func test_aim_overlay_exports_the_retail_authored_pitch_sign() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	sim.set_local_player_mouse(511, false)
	sim.add_local_player_look(0.0, 100.0)
	sim.step()
	var authored_pitch := sim.get_local_player_pitch_deg()
	var arm_pitch := _weapon_arm_pitch_deg(sim)
	assert_gt(absf(authored_pitch), 0.5, "look input produced a signed pitch witness")
	assert_almost_eq(arm_pitch, authored_pitch, 0.01,
		"overlay pitch stays in authored sign for MissionObjectPlacer")
	sim.free()


func test_weapon_channel_keeps_own_phase_and_switch_identity_per_entity() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	sim.set_local_player_weapon(_minimal_weapon("WPN_A", "shared.adm"), {})
	sim.step()
	var state: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(String(state.get("body_anim_key", "")), "anim_idle",
		"equal state ids still export the secondary channel's independent playhead")
	assert_gt(absf(_weapon_arm_pitch_deg(sim)), 1.0,
		"the first AnimMap observed by this entity stamps the arms dip")

	for _i in range(200):
		sim.step()
	assert_lt(absf(_weapon_arm_pitch_deg(sim)), 0.001, "the first dip settled")

	sim.set_local_player_weapon(_minimal_weapon("WPN_B", "SHARED.ADM"), {})
	sim.step()
	assert_lt(absf(_weapon_arm_pitch_deg(sim)), 0.001,
		"a differently named weapon sharing the resolved AnimMap does not dip")

	sim.set_local_player_weapon(_minimal_weapon("WPN_C", "different.adm"), {})
	sim.step()
	assert_gt(absf(_weapon_arm_pitch_deg(sim)), 1.0,
		"a changed AnimMap stamps the dip")

	# Replacing the world/player keeps the equipped host state, but the new entity's
	# observed serial starts empty and must receive its own initial stamp.
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.set_local_player_weapon(_minimal_weapon("WPN_C", "different.adm"), {})
	sim.step()
	assert_gt(absf(_weapon_arm_pitch_deg(sim)), 1.0,
		"a replacement local entity observes the current AnimMap as new")
	sim.free()

func test_weapon_clip_variant_ring_rotates_bake_reads_and_plays() -> void:
	# Multi-clip .adm variant rings, end to end through the public binding: clip
	# lengths arrive as per-key VARIANT arrays; the bake consumes ONE ring entry per
	# 'auto' delay field (serve-then-advance), and every play consumes + latches the
	# served variant into the state dict. A both-auto reload over a 3-ring therefore
	# eats entries 0 and 1 at bake — the FIRST reload PLAY serves variant 2, the
	# next serves 0 (the REVVY M4 "m4_1r" "m4_1r" "m4_1r2" shape).
	# [orig: Anim_InitActions reads @0x5421c5/@0x5421d8 via Anim_GetDurationTicks
	#  @0x53ee10; AnimMap_PlayAnimBySlot @0x40bda0 latches at animState+68]
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_RING", "animadm": "ring.adm",
		"actions": [
			{"name": "idle", "anim": "anim_wpn_idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "anim": "anim_wpn_fire", "delaystart": 0, "delayend": 0},
			{"name": "reload", "anim": "anim_wpn_reload", "delaystart": -1, "delayend": -1},
		],
		"flags": 0, "clipsize": 30, "startrounds": 60,
	}
	sim.set_local_player_weapon(def, {
		"anim_wpn_idle": PackedFloat32Array([0.2]),
		"anim_wpn_fire": PackedFloat32Array([0.05]),
		"anim_wpn_reload": PackedFloat32Array([0.5, 1.0, 0.25]),
	})
	sim.step()
	var state: Dictionary = sim.get_local_player_weapon_state()
	assert_eq(String(state.get("anim_key", "")), "anim_wpn_idle", "fresh slot idles")
	assert_eq(int(state.get("anim_variant", -1)), 0, "single-entry rings always serve 0")

	# Spend a round (letting the fire+recoil chain settle back to idle — the reload
	# dispatch gate refuses the edge mid-FIRE), then reload: the bake left the reload
	# ring's head at 2 (two 'auto' reads), so the FIRST reload serves variant 2.
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(6):
		sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("clip", 0)), 29, "one round spent")
	sim.set_local_player_weapon_input(false, false, true)
	var reload_variant := -1
	var first_reload_serial := -1
	for _i in range(90):
		sim.step()
		state = sim.get_local_player_weapon_state()
		if String(state.get("anim_key", "")) == "anim_wpn_reload":
			reload_variant = int(state.get("anim_variant", -1))
			first_reload_serial = int(state.get("play_serial", 0))
			break
	assert_eq(reload_variant, 2,
		"the first reload serves variant 2 — the both-auto bake consumed entries 0+1")

	# Let the reload finish (ds 32 + de 32 ticks and the transitions), spend another
	# round, reload again: the ring wrapped, so the play serves variant 0.
	for _i in range(90):
		sim.step()
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(6):
		sim.step()
	sim.set_local_player_weapon_input(false, false, true)
	reload_variant = -1
	for _i in range(90):
		sim.step()
		state = sim.get_local_player_weapon_state()
		if String(state.get("anim_key", "")) == "anim_wpn_reload" 				and int(state.get("play_serial", 0)) != first_reload_serial:
			reload_variant = int(state.get("anim_variant", -1))
			break
	assert_eq(reload_variant, 0, "the second reload wraps the ring back to variant 0")
	sim.free()


func test_weapon_event_batch_preserves_three_undrained_ticks() -> void:
	# Game_MainLoop catch-up presents once after N fixed ticks. The sim must retain
	# each tick's clip/begin/end payload in order, including its age at the drain.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_EVENT_BATCH",
		"actions": [
			{"name": "idle", "anim": "anim_wpn_idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "anim": "anim_wpn_fire", "delaystart": 0, "delayend": 0,
				"soundset": "FIRE_BEGIN", "soundsetend": "FIRE_END"},
			{"name": "recoil", "anim": "anim_wpn_recoil", "delaystart": 0,
				"delayend": 0, "soundset": "RECOIL_BEGIN",
				"particle": "Effect_TestCas", "particleuserpoint": "bcasing"},
		],
		"flags": 0x100,
		"clipsize": 30,
		"startrounds": 60,
	}
	sim.set_local_player_weapon(def, {
		"anim_wpn_idle": 0.1,
		"anim_wpn_fire": 0.1,
		"anim_wpn_recoil": 0.1,
	})
	sim.step()
	sim.drain_local_player_weapon_events() # discard the initial idle play

	sim.set_local_player_weapon_input(true, true, false)
	sim.step()
	sim.step()
	sim.step()
	var events: Array = sim.drain_local_player_weapon_events()
	assert_eq(events.size(), 3, "FIRE, RECOIL, FIRE survive one three-tick catch-up")
	if events.size() == 3:
		assert_eq([
			String((events[0] as Dictionary).get("anim_key", "")),
			String((events[1] as Dictionary).get("anim_key", "")),
			String((events[2] as Dictionary).get("anim_key", "")),
		], ["anim_wpn_fire", "anim_wpn_recoil", "anim_wpn_fire"])
		assert_eq([
			int((events[0] as Dictionary).get("action_started", -1)),
			int((events[1] as Dictionary).get("action_started", -1)),
			int((events[2] as Dictionary).get("action_started", -1)),
		], [2, 3, 2])
		assert_eq([
			int((events[0] as Dictionary).get("action_finished", -1)),
			int((events[1] as Dictionary).get("action_finished", -1)),
			int((events[2] as Dictionary).get("action_finished", -1)),
		], [2, -1, 2])
		assert_eq([
			int((events[0] as Dictionary).get("age_ticks", -1)),
			int((events[1] as Dictionary).get("age_ticks", -1)),
			int((events[2] as Dictionary).get("age_ticks", -1)),
		], [2, 1, 0])
		assert_eq([
			String((events[0] as Dictionary).get("action_soundset", "")),
			String((events[1] as Dictionary).get("action_soundset", "")),
			String((events[2] as Dictionary).get("action_soundset", "")),
		], ["FIRE_BEGIN", "RECOIL_BEGIN", "FIRE_BEGIN"],
			"payloads are copied before a later tick or remount can overwrite them")
		assert_eq([
			String((events[0] as Dictionary).get("action_end_soundset", "")),
			String((events[1] as Dictionary).get("action_end_soundset", "")),
			String((events[2] as Dictionary).get("action_end_soundset", "")),
		], ["FIRE_END", "", "FIRE_END"])
		assert_eq(int((events[1] as Dictionary).get("action_effect", -1)), 3,
				"the recoil arbiter event survives the catch-up batch")
		assert_eq(String((events[1] as Dictionary).get("effect_particle", "")),
				"Effect_TestCas")
		assert_eq(String((events[1] as Dictionary).get("effect_particle_userpoint", "")),
				"bcasing")
	assert_true(sim.drain_local_player_weapon_events().is_empty(), "the drain is destructive")
	sim.free()


func test_weapon_event_batch_snapshots_the_scope_settle_tick() -> void:
	# Retail promotes the view before weapon actions. Across one catch-up batch,
	# an auto-fire event before 15/15 remains unsuppressed while the later event
	# at 15/15 carries the settled gate.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_SCOPE_BATCH",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0,
				"particle": "Effect_TestMF", "particleuserpoint": "muzzle1"},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
		],
		"flags": 0x102, # Auto + Sighted
		"clipsize": 30,
		"startrounds": 60,
	}
	sim.set_local_player_weapon(def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(12):
		sim.step()
	sim.drain_local_player_weapon_events()

	sim.set_local_player_weapon_input(true, true, false)
	sim.step() # scope 13/15, FIRE
	sim.step() # scope 14/15, RECOIL
	sim.step() # scope 15/15, FIRE
	var fire_events: Array = sim.drain_local_player_weapon_events().filter(
			func(event: Dictionary) -> bool: return int(event.get("action_started", -1)) == 2)
	assert_eq(fire_events.size(), 2)
	if fire_events.size() == 2:
		assert_false(bool((fire_events[0] as Dictionary).get("scope_settled", true)),
				"the earlier catch-up tick still shows its muzzle")
		assert_true(bool((fire_events[1] as Dictionary).get("scope_settled", false)),
				"the 15/15 tick alone suppresses its muzzle")
	sim.free()


func test_reload_during_scope_raise_does_not_stash_an_unpromoted_scope() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.set_local_player_weapon({
		"name": "WPN_SCOPE_RELOAD",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
			{"name": "reload", "delaystart": 1, "delayend": 1},
		],
		"flags": 0x2, "clipsize": 30, "startrounds": 60,
	}, {})
	sim.step()
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(6):
		sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("clip", 0)), 29)
	assert_true(sim.request_local_player_scope_toggle())
	sim.step()
	assert_lt(float(sim.get_local_player_view().get("scope_fraction", 1.0)), 1.0)
	var before := int(sim.get_local_player_weapon_state().get("unscope_serial", 0))
	sim.set_local_player_weapon_input(false, false, true)
	for _i in range(3):
		sim.step()
	assert_eq(int(sim.get_local_player_weapon_state().get("unscope_serial", 0)), before)
	assert_true(bool(sim.get_local_player_view().get("scope_engaged", false)))
	sim.free()


func test_local_fire_spawns_the_authoritative_round_and_impact() -> void:
	# The listen-server loopback handler skips C2S 0x06 because retail local fire
	# already appends/spawns synchronously. Pin that local action seam end-to-end:
	# FSM fired -> RoundSim -> organic tag-2 effects_table row.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	# Spawn yaw 0 = mission yaw 0 = engine heading 90 BAM-deg, which faces
	# mission +y (bearing 90). The target sits 8 m along +y so the shot connects
	# only when the round bearing rides the heading frame directly — the old
	# (90 - heading) flip flew the shot along +x and only an east-side target
	# could pass (the compensating-error pair the fp_impact_probe pinned;
	# ledger D-WPN-18).
	assert_false(md.add_entity(NovaMissionData.KIND_ORGANIC, 102072,
			Vector3(0, 8, 0), Vector3.ZERO).is_empty())
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO")
	var fire_def := {
		"name": "WPN_M4AUTO",
		"actions": [
			{"name": "idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "delaystart": 0, "delayend": 0},
			{"name": "recoil", "delaystart": 0, "delayend": 0},
		],
		"flags": 0,
		"clipsize": 30,
		"startrounds": 300,
	}
	sim.set_local_player_weapon(fire_def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	sim.set_local_player_weapon_input(false, true, false)
	var impacts: Array = []
	for _i in range(4):
		sim.step()
		impacts.append_array(sim.drain_round_impacts())

	var weapon_state := sim.get_local_player_weapon_state()
	assert_eq(int(weapon_state.get("round_ring_count", 0)), 1,
			"local FIRE appends exactly one tag-2 fan-out record")
	# The fixture M4's first shot samples the pre-consume 30-round magazine, so
	# ((30 & 3) << 4) | 2 produces 0x22, not a hard-coded 0x02 and not the
	# unrelated category/rank weapon-slot combo.
	# [orig: WeaponAction_Fire @ 0x542c11; net-re §5.9.1 capture cross-witness]
	assert_eq(int(weapon_state.get("last_round_flags", 0)), 0x22)
	assert_eq(int(weapon_state.get("last_round_subtype", 0)), 12,
			"ordinary on-foot hip fire carries the retail default zoom subtype")
	assert_eq(int(weapon_state.get("last_round_slot_byte", -1)), 0,
			"the sole modeled local weapon slot has retail slot id zero")
	assert_eq(int(weapon_state.get("last_round_seq", 0)), 1)
	assert_eq(impacts.size(), 1, "one local shot reaches the target and emits one impact")
	if impacts.size() == 1:
		assert_eq(String((impacts[0] as Dictionary).get("effect", "")), "Effect_AmHitBody")
		assert_eq(String((impacts[0] as Dictionary).get("sound", "")), "IMP_BULLET_PLAYER")
		# The drained position is Godot-space (x, z_up, -y): the +y_m flight lands
		# near (0, ~eye, -8). Pins the local fire bearing = the engine heading
		# frame (D-WPN-18; RoundSim's wire-validated (cos, sin) mapping).
		var impact_pos: Vector3 = (impacts[0] as Dictionary).get("position", Vector3.ZERO)
		assert_almost_eq(impact_pos.x, 0.0, 0.75,
				"the shot flies the aim bearing, not its 90-deg mirror")
		assert_between(-impact_pos.z, 6.0, 8.5,
				"the impact lands at the north-side target range")

	# Presentation generations reset when the weapon is remounted, but the wire
	# round sequence belongs to the shooter and stays monotonic across adm/slot
	# changes. [orig: word_B7C670; net-re §5.9.1 capture 0x020b..0x0217]
	sim.set_local_player_weapon(fire_def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	sim.set_local_player_weapon_input(false, true, false)
	sim.step()
	weapon_state = sim.get_local_player_weapon_state()
	assert_eq(int(weapon_state.get("round_ring_count", 0)), 2)
	assert_eq(int(weapon_state.get("last_round_seq", 0)), 2,
			"weapon remount does not reset the shooter-lifetime sequence")
	sim.free()


func test_weapon_event_batch_does_not_cross_lifecycle_boundaries() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := {
		"name": "WPN_EVENT_LIFECYCLE",
		"actions": [
			{"name": "idle", "anim": "anim_wpn_idle", "delaystart": 0, "delayend": 0},
		],
	}
	var clips := {"anim_wpn_idle": 0.1}

	sim.set_local_player_weapon(def, clips)
	sim.step()
	sim.set_local_player_weapon(def, clips)
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"remount discards the previous weapon's queued presentation")
	sim.step()
	sim.clear_local_player_weapon()
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"clear discards the unmounted weapon's queued presentation")
	sim.set_local_player_weapon(def, clips)
	sim.step()
	sim.restart()
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"restart cannot age a pre-rewind event across the logic-tick reset")
	sim.free()


func test_armory_reads_and_clears_authoritative_local_loadout() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/def")), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)

	assert_eq(sim.get_local_player_class(), 8, "spawned player exposes its rifleman class")
	assert_eq(sim.get_local_player_team(), 2, "spawned player exposes the assigned red team")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"weapon-table load exposes the entity's stamped default instead of the FP fallback")

	assert_true(sim.apply_local_player_loadout([{"name": "WPN_M4AUTO"}], 6))
	assert_eq(sim.get_local_player_class(), 6, "accepted class is authoritative on reopen")
	var inv: Dictionary = sim.get_local_player_inventory()
	assert_true(bool(inv.get("valid", false)), "the ACCEPT rebuilt the slot pool")
	assert_eq(String(inv.get("equipped_name", "")), "WPN_M4AUTO",
		"the ACCEPT re-selected the accepted weapon")
	assert_true(sim.apply_local_player_loadout([], 9), "the all-NONE kit is a valid apply")
	assert_eq(sim.get_local_player_class(), 9, "NONE still commits the selected class")
	assert_eq(sim.get_local_player_weapon_name(), "", "NONE clears the equipped AdmDef")
	sim.free()


func test_entities_walk_their_route() -> void:
	# Soldiers are anim-driven [orig: Entity_UpdateInfantryAI @0x4b9910]: their motion
	# comes from .bad root-motion clips resolved through a model's .adm. Without a clip
	# set they hold and stand; with one they walk the route.
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var start: Vector3 = sim.get_entity_position(0)
	for _i in range(20):
		sim.step()
	assert_lt(start.distance_to(sim.get_entity_position(0)), 0.01,
		"no clip set -> soldiers stand still (faithful: motion comes from clips)")

	var clips := int(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"))
	assert_gt(clips, 0, "fixture soldier.adm resolved root-motion clips")
	for _i in range(120):
		sim.step()
	var moved: Vector3 = sim.get_entity_position(0)
	# The walk fixture steps ~0.033u/tick toward the first route marker
	# (mission (100,0,0) -> Godot +x).
	assert_gt(start.distance_to(moved), 1.0, "entity walked away from its spawn")
	assert_gt(moved.x, start.x + 1.0, "walked toward the first route marker (+x)")
	sim.free()

func test_infantry_anim_map_failure_paths() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_eq(int(sim.set_infantry_anim_map(null, "soldier.adm")), 0, "null root -> 0 clips")
	assert_eq(int(sim.set_infantry_anim_map(_anim_root(), "missing.adm")), 0, "absent .adm -> 0 clips")
	assert_eq(sim.get_infantry_clip_count(), 0, "failed load leaves no stale clip set")
	sim.free()

func test_load_from_editor_mission_data() -> void:
	# The editor-integration path: promote a live NovaMissionData (what the mission editor holds),
	# not a file or the synthetic demo. KIND_ORGANIC = 3.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK, "empty in-memory mission created")
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md), "promoted the editor's live mission")
	assert_eq(sim.get_brain_count(), 2, "both organics got AI brains")
	assert_eq(sim.get_entity_kind(0), 3, "entity 0 maps back to KIND_ORGANIC")
	sim.free()

func test_item_seat_specs_mount_command_125_spawn() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(11, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	assert_false(soldier.is_empty())
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(vehicle["bms_id"])))

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{"type": 1, "position": Vector3(9, 0, 0), "yaw_offset": 0, "source_name": "sitex00"},
				{"type": 2, "position": Vector3(0, 1, 2), "yaw_offset": 45, "pose_index": 24, "source_name": "ctrlx24"}
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded command-125 mission with seat specs")
	var soldier_idx := _first_organic_ai_index(sim)
	assert_true(soldier_idx >= 0, "found the soldier's AI row")
	var pos := sim.get_entity_position(soldier_idx)
	assert_true(pos.is_equal_approx(Vector3(10, 2, -1)),
		"command-125 soldier uses the IDA-priority ctrlx seat, converted to Godot axes")
	assert_almost_eq(sim.get_entity_yaw_deg(soldier_idx), 45.0, 0.01,
		"non-gunner mounted seats carry their local yaw offset")
	# The mounted anim state (100 = anim_sit_24) is asserted via the debug card below; the present
	# snapshot is the listen-server ClientState now (covered by nova_listen_server_test).
	var card: Dictionary = sim.get_entity_debug(soldier_idx)
	assert_true(bool(card["mounted"]), "debug card marks mounted occupants")
	assert_eq(int(card["mount_target_net_id"]), int(vehicle["bms_id"]))
	assert_eq(int(card["mount_seat"]), 1, "ctrlx seat was selected by original priority")
	assert_eq(int(card["mount_type"]), 2, "seat type is ctrlx/controller")
	assert_eq(int(card["mount_seat_bone"]), 0)
	assert_eq(int(card["mount_seat_pose_index"]), 24)
	assert_eq(String(card["mount_seat_source_name"]), "ctrlx24")
	assert_eq(Vector3(card["mount_seat_local"]), Vector3(0, 1, 2))
	assert_eq(int(card["mount_seat_yaw_offset"]), 45)
	var target_seats: Array = card["mount_target_seats"]
	assert_eq(target_seats.size(), 2, "debug card carries every target seat candidate")
	assert_eq(String((target_seats[0] as Dictionary)["source_name"]), "sitex00")
	assert_eq(int((target_seats[0] as Dictionary)["type"]), 1)
	assert_eq(String((target_seats[1] as Dictionary)["source_name"]), "ctrlx24")
	assert_eq(int((target_seats[1] as Dictionary)["pose_index"]), 24)
	assert_eq(int(card["anim_state"]), 100)
	assert_eq(String(card["anim_key"]), "anim_sit_24")
	sim.free()



# The floating attach labels [orig: draw_vehicle_seat_and_armory_labels @0x5a3290
# selection half]: free seats in the 4.0 u radius label with exactly one nearest
# highlight; the unarmed local player sees every candidate; the armory zone flag is
# absent here so seat mode applies and no armory labels appear.
func test_attach_labels_seats() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{"type": 1, "position": Vector3(0, 2, 0), "source_name": "sitex00"},
				{"type": 3, "position": Vector3(0, -1, 1), "source_name": "UseGun"},
			],
			"armory_points": [Vector3(0, 0, 1.5)],
			"primary_weapon": "WPN_EMPLCD50",
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded the labels mission")
	assert_true(sim.spawn_local_player(Vector3(12, 0, 0), 0.0, 1), "spawned the local player")
	var labels: Array = sim.get_attach_labels()
	assert_eq(labels.size(), 2, "both free seats label inside 4.0 u (armory points stay out of seat mode)")
	var nearest_count := 0
	var seat_types: Array = []
	for raw in labels:
		var l: Dictionary = raw
		seat_types.append(int(l["seat_type"]))
		if bool(l["nearest"]):
			nearest_count += 1
		assert_false(bool(l["armory"]), "no armory labels out of the zone")
		# WPN_EMPLCD50 is not in a loaded weapon table here -> the key stays absent
		# and the HUD falls to the STROVER_USEGUN default.
		assert_eq(String(l["attach_text_key"]), "")
	assert_eq(nearest_count, 1, "exactly the scan winner is highlighted")
	assert_true(seat_types.has(1) and seat_types.has(3), "sit + UseGun seats both reported")
	sim.free()


func test_attach_labels_hide_occupied_and_out_of_range() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(11, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	assert_false(soldier.is_empty())
	# Command-125 mounts the soldier into the best seat at promote — that seat must not label.
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(vehicle["bms_id"])))
	# Same team as the local player: a live ENEMY occupant would reject the whole
	# vehicle instead [orig: Vehicle_HasEnemyOccupant @0x4359f0].
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "team", 1))
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{"type": 2, "position": Vector3(0, 1, 1), "source_name": "ctrlx00"},
				{"type": 1, "position": Vector3(0, -2, 1), "source_name": "sitex00"},
			],
		}
	])
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3(12, 0, 0), 0.0, 1))
	var labels: Array = sim.get_attach_labels()
	assert_eq(labels.size(), 1, "the AI-occupied ctrlx seat never labels [orig: @0x5a348f]")
	assert_eq(int((labels[0] as Dictionary)["seat_type"]), 1, "the free sitex remains")
	sim.free()


func test_attach_labels_empty_out_of_range() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{"type_id": 1294, "seats": [{"type": 1, "position": Vector3.ZERO, "source_name": "sitex00"}]}
	])
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3(40, 0, 0), 0.0, 1), "spawned far away")
	assert_eq(sim.get_attach_labels().size(), 0,
		"outside the 4.0 u gate the nearest scan fails and no labels emit [orig: @0x5a32e2]")
	sim.free()


# Drivable items (control-seat specs) attach AI brains at promote since the vehicle
# pass, so organics no longer sit at AI index 0 — resolve the first pool-0 row.
func _first_organic_ai_index(sim: NovaSimulation) -> int:
	for i in 64:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		if int(d.get("pool", -1)) == 0:
			return i
	return -1

func test_mounted_seat_local_matches_rotated_vehicle_userpoint() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3(0, -90, 0))
	var soldier := md.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(11, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	assert_false(soldier.is_empty())
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(vehicle["bms_id"])))

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{
					"type": 2,
					"position": Vector3(-0.7148895, -0.1189880, 2.1048889),
					"source_name": "ctrlx10",
				}
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded rotated command-125 mount")
	var soldier_idx := _first_organic_ai_index(sim)
	assert_true(soldier_idx >= 0, "found the soldier's AI row")
	var pos := sim.get_entity_position(soldier_idx)
	var expected := Vector3(10.1189880, 2.1048889, 0.7148895)
	assert_lt(pos.distance_to(expected), 0.001,
		"mounted seat local follows the same rotated side as the selected model userpoint")
	sim.free()


# The USE-ITEM toggle's weapon-busy gate at the sim binding [orig: @0x436958-0x436977]:
# a fire in flight swallows the toggle; back at idle the same toggle mounts.
func test_local_player_toggle_mount_weapon_busy_gate() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(2, 0, 0), Vector3.ZERO)
	assert_false(vehicle.is_empty())
	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"seats": [
				{"type": 1, "position": Vector3(0, -1, 1), "yaw_offset": 0, "source_name": "sitex00"},
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded the one-truck mission")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.set_local_player_weapon({
		"name": "WPN_GATE", "animadm": "gate.adm",
		"actions": [
			{"name": "idle", "anim": "anim_wpn_idle", "delaystart": 0, "delayend": 0},
			{"name": "fire", "anim": "anim_wpn_fire", "delaystart": 0, "delayend": 0},
		],
		"flags": 0, "clipsize": 30, "startrounds": 60,
	}, {
		"anim_wpn_idle": PackedFloat32Array([0.2]),
		"anim_wpn_fire": PackedFloat32Array([0.2]),
	})
	sim.step()
	# Pull the trigger: the FSM leaves idle this step; the in-flight fire swallows
	# the toggle.
	sim.set_local_player_weapon_input(false, true, false)
	sim.step()
	assert_false(sim.local_player_toggle_mount(), "a fire in flight swallows the toggle")
	# Release and settle back to idle: the same toggle now passes the gate and mounts.
	sim.set_local_player_weapon_input(false, false, false)
	for _i in range(40):
		sim.step()
	assert_true(sim.local_player_toggle_mount(), "the idle toggle mounts")
	var card: Dictionary = sim.get_world_entity_debug(int(vehicle["bms_id"]))
	var seats: Array = card.get("seats", [])
	assert_true(seats.size() == 1 and bool(seats[0]["occupied"]),
		"the scan took the truck's one sitex seat")
	sim.free()


func test_command_125_usegun_mount_renders_emplaced_pose() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(NovaMissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(NovaMissionData.KIND_ORGANIC, 102072, Vector3(10, 0, 0), Vector3.ZERO)
	assert_false(gun.is_empty())
	assert_false(soldier.is_empty())
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "waypoint_id", 125))
	assert_true(md.set_entity_property_int(NovaMissionData.KIND_ORGANIC, int(soldier["index"]), "wp_number", int(gun["bms_id"])))

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([
		{
			"type_id": 1294,
			"emplaced_pose_variant": 3,
			"seats": [
				{"type": 3, "position": Vector3.ZERO, "yaw_offset": 0}
			],
		}
	])
	assert_true(sim.load_from_mission_data(md), "loaded command-125 UseGun mount")
	# The mounted anim state (67 = anim_emplaced) is asserted via the debug card below; the present
	# snapshot is the listen-server ClientState now (covered by nova_listen_server_test).
	var card: Dictionary = sim.get_entity_debug(0)
	assert_true(bool(card["mounted"]), "debug card marks UseGun occupant mounted")
	assert_eq(int(card["mount_type"]), 3, "seat type is UseGun/gunner")
	assert_eq(int(card["mount_target_emplaced_pose_variant"]), 3)
	assert_eq(int(card["anim_state"]), 67)
	assert_eq(String(card["anim_key"]), "anim_emplaced")
	sim.free()


# (P7: the 3 no-net AI-pool present-snapshot tests were deleted — the present is now the listen-
#  server ClientState, covered by nova_listen_server_test; the editor no-net preview is retired.)



func test_foliage_mask_anchors_track_local_player_stance() -> void:
	# The hide-in-grass selection: only infantry with a stance bit set
	# ((net_stance_bits & 0x3) != 0) and no groundEntity anchor the distant
	# MODEL/depth-mask foliage tier [orig: Terrain_RenderSectorEntitiesBySide
	# @ 0x5c7dc2/0x5c7ded (MoveOrder & 0x300), groundEntity gate
	# @ 0x5c7dd5..0x5c7df7]. The local player's SELECT latches are the stance
	# writer [orig: Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd].
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	var spawn := Vector3(24.0, 0.0, -12.0)
	assert_true(sim.spawn_local_player(spawn, 0.0, 1))

	sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"a STANDING infantry entity never anchors the silhouette tier")

	assert_true(sim.request_local_player_stance(1))  # crouch (SELECT 169)
	sim.step()
	var crouched: PackedVector3Array = sim.get_foliage_mask_anchor_positions()
	assert_eq(crouched.size(), 1, "the crouched local player anchors the silhouette tier")
	if crouched.size() == 1:
		var player := sim.get_local_player_position()
		assert_lt(Vector2(crouched[0].x, crouched[0].z).distance_to(Vector2(player.x, player.z)), 0.1,
			"the anchor is the entity's own Godot-space ground position")

	assert_true(sim.request_local_player_stance(2))  # prone (SELECT 170)
	sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 1,
		"prone anchors too - both MoveOrder stance bits gate the tier")

	assert_true(sim.request_local_player_stance(0))  # stand (SELECT 172)
	sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"standing back up empties the anchor list")
	sim.free()


func test_foliage_mask_anchors_ignore_standing_npcs() -> void:
	# Routed organics keep net_stance_bits 0 (the mirror only writes the LOCAL
	# player's SELECT latches; NPC stance never reaches the wire bits here), so
	# a demo mission full of standing walkers produces no anchors - matching
	# retail, where placed objects and standing soldiers leave MoveOrder's
	# stance bits clear [orig: the 0x5c7dc2 (flags & 0x300) reject].
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_eq(sim.get_entity_count(), 2, "the demo mission has AI infantry to reject")
	for _i in range(4):
		sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"standing NPCs never anchor the hide-in-grass tier")
	sim.free()


func test_transport_play_flag() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_playing(), "starts paused")
	sim.set_playing(true)
	assert_true(sim.is_playing(), "play flag toggles")
	sim.free()

func test_bms_event_fires_through_binding() -> void:
	# The capability consolidation adds: a BMS event evaluates through the SAME binding that
	# runs the AI (the editor preview used to walk AI but never fire events). Build an
	# unconditional OutputText(77) event, tick once, and confirm the host-presentation effect
	# drains out of the shared World EffectLog.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {"action_type": 6, "param1": 77}).is_empty())

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md), "loaded the scripted mission")
	assert_eq(sim.get_event_count(), 1, "one BMS event registered in the runtime")

	# A normal event's first processing pass is the 16th tick (the faithful quarter-list
	# round-robin cadence; see tests/mission/event_runtime_test.cpp).
	for _i in range(16):
		sim.step()
	var effects := sim.drain_effects()
	assert_eq(effects.size(), 1, "one presentation effect drained")
	assert_eq(String((effects[0] as Dictionary)["kind"]), "text", "OutputText -> text effect")
	assert_eq(int((effects[0] as Dictionary)["a"]), 77, "carries the string id")
	assert_true(sim.has_event_fired(0), "the event is marked fired")
	assert_true(sim.drain_effects().is_empty(), "drain cleared the log")
	sim.free()


# --- Read-only introspection (C8: the debug overlay's data feeds) ---

func test_logic_tick_advances_per_step_and_rewinds_on_restart() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	# The pre-mission pass already ran one tick at load, so pin DELTAS, never
	# absolutes.
	var t0: int = sim.get_logic_tick()
	sim.step()
	assert_eq(sim.get_logic_tick(), t0 + 1, "one step advances the logic tick by one")
	sim.step()
	sim.step()
	assert_eq(sim.get_logic_tick(), t0 + 3)
	sim.restart()
	assert_eq(sim.get_logic_tick(), t0, "Stop rewinds the clock to the play-start baseline")
	sim.free()


func test_variable_snapshots_are_bank_sized_and_track_writes() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var mission: PackedInt32Array = sim.get_mission_variables_snapshot()
	var globals: PackedInt32Array = sim.get_global_variables_snapshot()
	var music: PackedInt32Array = sim.get_music_variables_snapshot()
	assert_eq(mission.size(), 512, "V0..V511")
	assert_eq(globals.size(), 256, "G0..G255")
	assert_eq(music.size(), 16, "M0..M15")

	sim.set_mission_variable(5, 42)
	sim.set_global_variable(3, -7)
	assert_eq(sim.get_mission_variables_snapshot()[5], 42, "snapshot reflects V writes")
	assert_eq(sim.get_global_variables_snapshot()[3], -7, "snapshot reflects G writes")
	assert_eq(sim.get_global_variable(3), -7, "the scalar G getter agrees")
	sim.free()


func test_fired_events_snapshot_matches_scalar() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {"action_type": 6, "param1": 77}).is_empty())
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))

	var before: PackedByteArray = sim.get_fired_events_snapshot()
	assert_eq(before.size(), sim.get_event_count(), "one flag per event")
	assert_eq(int(before[0]), 0, "nothing fired before the first quarter pass")

	for _i in range(16):
		sim.step()
	var after: PackedByteArray = sim.get_fired_events_snapshot()
	assert_eq(int(after[0]), 1, "the fired flag sets")
	assert_eq(int(after[0]) == 1, sim.has_event_fired(0), "bulk and scalar reads agree")
	sim.free()


func test_entity_debug_card_carries_named_scalars() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var card: Dictionary = sim.get_entity_debug(0)
	assert_false(card.is_empty(), "a live entity has a card")
	assert_eq(int(card["state"]), 16, "routed organic starts in GROUND_FOLLOWWP")
	assert_eq(String(card["state_name"]), "GROUND_FOLLOWWP", "...with its readable name")
	assert_eq(card["position"], sim.get_entity_position(0), "position matches the scalar getter")
	assert_almost_eq(float(card["yaw_deg"]), sim.get_entity_yaw_deg(0), 0.01)
	assert_eq(int(card["net_id"]), sim.get_entity_net_id(0))
	assert_eq(int(card["kind"]), sim.get_entity_kind(0))
	assert_true(bool(card["alive"]))
	assert_true(card.has("health") and card.has("ai_health"),
		"both health mirrors ride the card (they diverge under damage)")
	assert_true(bool(card["infantry"]), "demo organics route through the infantry motor")

	assert_true(sim.get_entity_debug(-1).is_empty(), "invalid index reads an empty card")
	assert_true(sim.get_entity_debug(999).is_empty())
	sim.free()


func test_effect_state_lookup_uses_the_live_registry_not_the_ai_pool() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_BUILDING, 0, Vector3(3, 4, 5), Vector3(10, 20, 30))
	assert_false(placed.is_empty())
	var ssn := int(placed.get("bms_id", 0))
	assert_gt(ssn, 0)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 0,
			"a building has a registry slot but no AI-pool row")
	var state: PackedVector3Array = sim.get_entity_effect_state_for_ssn(ssn)
	assert_eq(state.size(), NovaSimulation.EFFECT_STATE_COUNT,
			"the SSN query reaches non-AI registry entities")
	if state.size() == NovaSimulation.EFFECT_STATE_COUNT:
		assert_eq(state[NovaSimulation.EFFECT_STATE_POSITION], Vector3(3, 5, -4),
				"effect position uses the canonical mission-to-Godot frame")
		assert_eq(state[NovaSimulation.EFFECT_STATE_ROTATION_DEG], Vector3(10, 20, 30),
				"effect orientation remains mission Euler degrees for the host adapter")
	assert_true(sim.get_entity_effect_state_for_ssn(0).is_empty(), "SSN zero is invalid")
	assert_true(sim.get_entity_effect_state_for_ssn(65536).is_empty(),
			"out-of-range SSNs must not wrap onto a different registry entity")
	sim.free()


func test_collision_backed_building_without_oobj_keeps_batch_visibility() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001, Vector3(0, 20, 0), Vector3.ZERO)
	assert_false(placed.is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(
		ProjectSettings.globalize_path("res://../fixtures/threedi/3di3/House.3di")), OK)
	assert_true(data.has_collision())
	assert_false(data.has_occlusion(), "fixture must exercise collision without OOBJ")
	var placer := ObjectDataPlacerStub.new(data)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, placer), 1)
	sim.occlusion_init_mission()
	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
	var visibility: PackedInt64Array = sim.get_building_visibility()
	assert_eq(visibility.size(), 2, "collision-backed no-OOBJ building stays in the host batch")
	if visibility.size() == 2:
		assert_eq(int(visibility[0]), int(placed.get("bms_id", 0)))
		var packed := int(visibility[1])
		assert_eq(packed & 0xFFFFFFFF, 0xFFFFFFFF,
			"without a section map the host preserves every de-batched render part")
		assert_ne(packed & (1 << 32), 0, "the in-frustum building is visible")
	sim.free()


func test_face_only_cfac_model_attaches_for_projectile_raycast() -> void:
	# Retail collision construction and the projectile face walker do not
	# require BVOL. Bird1 is a committed face-only witness (242 CFAC, 0 BVOL);
	# rejecting it here silently degrades authored bullet geometry to a sphere.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001, Vector3(0, 200, 0), Vector3.ZERO)
	assert_false(placed.is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(
		ProjectSettings.globalize_path("res://../fixtures/threedi/3di3/Bird1.3di")), OK)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, ObjectDataPlacerStub.new(data)), 1,
		"face-only CFAC remains a real collision model")
	var entities: Array = sim.get_hitbox_debug().get("entities", [])
	assert_eq(entities.size(), 1)
	if entities.size() == 1:
		assert_eq(int((entities[0] as Dictionary).get("face_total", 0)), 242,
			"all authored Bird1 faces reach the projectile walker")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().get("entities", []) as Array).is_empty(),
		"the heavier non-organic mesh view retains its local 80-unit range")
	sim.free()


func test_panm_liveness_is_scoped_to_the_active_transform_family() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
		"res://../fixtures/3dp/armry01/Armry01.3di")), OK)
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	var anim := data.add_part_anim(0, 0)
	assert_eq(anim, 0)
	var tracks := [
		"rotation_x", "rotation_y", "rotation_z",
		"scale_x", "scale_y", "scale_z",
		"translation",
	]
	var case := func(flags: int, live_tracks: Array) -> bool:
		assert_eq(data.set_part_animation_flags(0, anim, flags), OK)
		for track in tracks:
			assert_true(data.set_part_anim_track_field(
				0, anim, track, "control", 0))
		for track in live_tracks:
			assert_true(data.set_part_anim_track_field(
				0, anim, track, "control", 0x10))
		return data.has_live_panm_for_lod(0)

	assert_true(case.call(1 << 8, []), "spinner uses raw coefficients")
	assert_true(case.call(3 << 8, []), "view rotation type 3 is evaluated")
	assert_true(case.call(4 << 8, []), "view rotation type 4 is evaluated")
	assert_true(case.call(2 << 8, ["rotation_z"]),
		"rotation family samples rotation tracks")
	assert_false(case.call(2 << 8, ["scale_x"]),
		"rotation ignores an unrelated live scale track")
	assert_false(case.call(1, ["scale_y"]),
		"uniform scale type samples only scale_x")
	assert_true(case.call(1, ["scale_x"]))
	assert_true(case.call(2, ["scale_y"]),
		"axis scale samples all three scale tracks")
	assert_true(case.call(1 << 24, ["translation"]))
	assert_false(case.call(1 << 16, []),
		"rotation-reversed without a rotation family is inert")


func test_collision_uses_effective_lod0_and_never_first_live_lod() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
		"res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di")), OK)
	var lod_count := int(data.get_summary().get("lod_count", 0))
	assert_gt(lod_count, 1, "fixture needs a second visual LOD")
	if lod_count <= 1:
		return

	# A nonempty local block wins even when inert; model-level live rows must
	# not leak through it for canonical LOD0.
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	var inert := data.add_part_anim(0, 0)
	assert_eq(inert, 0)
	assert_false(data.has_live_panm_for_lod(0))
	assert_eq(Array(data.get_effective_panm_targets(0)), [0],
		"inert local row suppresses the model-level fallback")

	# Make only LOD1 live. Visual de-batching may see it, but retail Generic
	# collision always uses canonical LOD0 COBJ ordinals.
	for i in range(data.get_part_anim_count(1) - 1, -1, -1):
		assert_true(data.delete_part_anim(1, i))
	var live := data.add_part_anim(1, 0)
	assert_eq(live, 0)
	assert_true(data.set_part_anim_channel_enabled(1, live, "rotation", true))
	assert_true(data.set_part_anim_channel_mode(
		1, live, "rotation", "z", "sine_wave", -1))
	assert_true(data.set_part_anim_channel_values(
		1, live, "rotation", "z", 0.0, 90.0, 1.0))
	assert_true(data.has_live_panm_for_lod(1))
	assert_eq(data.get_live_panm_lod(), 1)

	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001,
		Vector3.ZERO, Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		"res://../fixtures/def/items.def")), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(
		item_db, ObjectDataPlacerStub.new(data)), 1)
	sim.debug_set_panm_time_ms(0)
	var before: PackedVector3Array = (
		(sim.get_hitbox_debug().get("entities", [])[0] as Dictionary)
		.get("tris", PackedVector3Array()))
	sim.debug_set_panm_time_ms(640)
	var after: PackedVector3Array = (
		(sim.get_hitbox_debug().get("entities", [])[0] as Dictionary)
		.get("tris", PackedVector3Array()))
	assert_eq(after, before, "LOD1 PANM never transforms model-level COBJ")
	sim.free()


func test_animated_collision_uses_retail_section_ordinal_headlessly() -> void:
	# Armry COBJ parents are all 0; face counts are [24, 213, 1, 12].
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		NovaMissionData.KIND_ITEM, 105004, Vector3.ZERO, Vector3.ZERO)
	var ssn := int(placed.get('bms_id', 0))
	assert_gt(ssn, 0)
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {
		'action_type': 21, 'action_sub_type': 34,
		'param1': ssn, 'param2': 1,
		'param3': 1, 'param4': 65536,
	}).is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		'res://../fixtures/def/items.def')), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
		'res://../fixtures/3dp/armry01/Armry01.3di')), OK)
	assert_true(data.has_collision())
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	var anim := data.add_part_anim(0, 1)
	assert_eq(anim, 0)
	assert_true(data.set_part_anim_channel_enabled(
		0, anim, 'translation', true))
	assert_true(data.set_part_anim_channel_mode(
		0, anim, 'translation', 'x',
		'control_register', 0))
	assert_true(data.set_part_anim_channel_values(
		0, anim, 'translation', 'x',
		0.0, 4.0, 0.0))

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs([{
		'type_id': 5004,
		'seats': [{
			'type': 2,
			'position': Vector3.ZERO,
			'source_name': 'ctrlx00',
		}],
	}])
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 1)
	assert_eq(sim.resolve_collision_instances(
		item_db, ObjectDataPlacerStub.new(data)), 1)
	var before_debug: Array = sim.get_hitbox_debug().get(
		'entities', [])
	assert_eq(before_debug.size(), 1)
	assert_eq(int((before_debug[0] as Dictionary).get(
		'face_total', 0)), 250)
	var before: PackedVector3Array = (before_debug[0] as Dictionary).get(
		'tris', PackedVector3Array())
	assert_eq(before.size(), 250 * 3)

	# No present pass/render node: collision reads authoritative AI state.
	for _tick in range(80):
		sim.step()
	assert_eq(sim.get_entity_part_anim_phase(0, 1), 65535)
	var after_debug: Array = sim.get_hitbox_debug().get(
		'entities', [])
	assert_eq(after_debug.size(), 1)
	var after: PackedVector3Array = (after_debug[0] as Dictionary).get(
		'tris', PackedVector3Array())
	assert_eq(after.size(), before.size())
	var moved := 0
	var stayed := 0
	var partial := 0
	for i in before.size():
		var distance := before[i].distance_to(after[i])
		if distance > 3.99:
			assert_almost_eq(distance, 4.0, 0.002)
			moved += 1
		elif distance < 0.002:
			stayed += 1
		else:
			partial += 1
	assert_eq(moved, 639, "only ordinal 1 moves")
	assert_eq(stayed, 111)
	assert_eq(partial, 0)
	sim.free()


func test_organic_collision_samples_current_skeletal_pose_headlessly() -> void:
	# BINOC is a committed 19-bone, three-frame BAD. Pair it with CharModel's
	# canonical 19-row model table/COBJ block so the real NovaSkeletalAnim ->
	# NovaSimulation -> CollisionWorld path can be tested without retail assets.
	var tmp := ProjectSettings.globalize_path(
			"res://.godot/person_collision_pose_test")
	assert_eq(DirAccess.make_dir_recursive_absolute(tmp), OK)
	var bad_out := FileAccess.open(tmp.path_join("BINOC.bad"), FileAccess.WRITE)
	assert_not_null(bad_out)
	if bad_out == null:
		return
	bad_out.store_buffer(FileAccess.get_file_as_bytes(
			"res://../fixtures/bad/BINOC.bad"))
	bad_out.close()
	# BINOC is a static three-frame clip. Turn BN15/head frame 1 into an
	# identity quaternion at its parser-pinned rotation offset (1324 + 16)
	# to make a deterministic moving-bone fixture while retaining its real
	# 19-bone hierarchy and every other shipped byte.
	var moving_bad := FileAccess.open(
			tmp.path_join("BINOC.bad"), FileAccess.READ_WRITE)
	assert_not_null(moving_bad)
	if moving_bad == null:
		return
	moving_bad.seek(1340)
	moving_bad.store_float(0.0)
	moving_bad.store_float(0.0)
	moving_bad.store_float(0.0)
	moving_bad.store_float(1.0)
	moving_bad.close()
	var adm_out := FileAccess.open(
			tmp.path_join("person_collision.adm"), FileAccess.WRITE)
	assert_not_null(adm_out)
	if adm_out == null:
		return
	var quote := String.chr(34)
	adm_out.store_string(
			"anim_reset %sBINOC.bad%s\n" % [quote, quote] +
			"anim_idle %sBINOC.bad%s\n" % [quote, quote] +
			"anim_idle_2 %sBINOC.bad%s\n" % [quote, quote])
	adm_out.close()

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(tmp), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	assert_true(data.has_collision())
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()),
			"19-bone collision rig loads: %s" % skeletal.get_last_error())
	assert_eq(skeletal.get_bone_count(), 19)
	var direct_a: Array = skeletal.eval_pose("anim_idle", 0.0)
	var direct_b: Array = skeletal.eval_pose("anim_idle", 1.0 / 30.0)
	var fixture_moved := 0
	for i in mini(direct_a.size(), direct_b.size()):
		if not (direct_a[i] as Transform3D).is_equal_approx(
				direct_b[i] as Transform3D):
			fixture_moved += 1
	assert_gt(fixture_moved, 0, "fixture must contain an animated bone")

	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3(10, 0, 0), Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_gt(sim.set_infantry_anim_map(root, "person_collision.adm"), 0)
	assert_eq(sim.resolve_collision_instances(
			item_db, SkeletalDataPlacerStub.new(data, skeletal)), 1)

	var before: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(before.size(), 19, "one posed sphere per CharModel COBJ/bone")
	var before_by_section := {}
	for value in before:
		var row: Dictionary = value
		assert_false(bool(row.get("fallback", true)))
		var pos := row.get("pos", Vector3.ZERO) as Vector3
		assert_lt(pos.distance_to(Vector3(10, 0, 0)), 3.0,
				"bind pose applies entity translation exactly once")
		before_by_section[int(row.get("section", -1))] = pos
	assert_true(before_by_section.has(14), "head COBJ/bone is present")

	# No presentation node or Skeleton3D is involved: advancing authoritative
	# clip_phase must move the CollisionWorld/F3 matrices directly.
	for _tick in 2:
		sim.step()
	var after: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(after.size(), 19)
	var moved_sections := 0
	for value in after:
		var row: Dictionary = value
		var section := int(row.get("section", -1))
		if before_by_section.has(section) and (
				before_by_section[section] as Vector3).distance_to(
						row.get("pos", Vector3.ZERO) as Vector3) > 0.0001:
			moved_sections += 1
	assert_gt(moved_sections, 0,
			"current BAD pose, not bind/entity-only matrices, drives collision")
	sim.free()

	DirAccess.remove_absolute(tmp.path_join("BINOC.bad"))
	DirAccess.remove_absolute(tmp.path_join("person_collision.adm"))
	DirAccess.remove_absolute(tmp)


func test_late_spawned_player_resolves_posed_collision_on_demand() -> void:
	# Mission collision is resolved before deploy in production. A player added
	# afterward must demand the same authored COBJ + ADM source instead of
	# becoming the one-sphere fallback until the next explicit sweep.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()),
			"late-spawn rig loads: %s" % skeletal.get_last_error())

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(
			item_db, SkeletalDataPlacerStub.new(data, skeletal)), 0,
			"the initial pre-deploy sweep has no player to attach")
	assert_true(sim.spawn_local_player(Vector3(10, 0, 0), 0.0, 1))

	# F3 still exercises CollisionWorld's demand provider for late targets, but
	# presentation filters the local avatar before any posed/fallback row escapes.
	assert_true((sim.get_hitbox_debug().get("organics", []) as Array).is_empty(),
			"the local avatar never renders posed or fallback hitboxes")
	var local_bms_id := int(sim.get_entity_debug(
			sim.get_entity_count() - 1).get("bms_id", -1))
	assert_true(bool(sim.get_destruction_debug(local_bms_id).get(
			"has_collision_instance", false)),
			"the hidden local avatar was nevertheless attached on demand")
	sim.free()


func test_f3_hides_local_player_and_keeps_distant_posed_organic() -> void:
	# The F3 person view is for inspecting targets. It must not wrap the local
	# avatar in debug spheres, and its payload must not silently discard a
	# valid remote target merely because it is more than 80 mission units away.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3(200, 0, 0), Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()))

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(
			item_db, SkeletalDataPlacerStub.new(data, skeletal)), 1)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	var rows: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(rows.size(), 19, "only the distant target's authored sections remain")
	for value in rows:
		var row: Dictionary = value
		assert_eq(int(row.get("entity_handle", -1)), 0,
				"F3 includes the distant target and excludes local handle 1")
		assert_false(bool(row.get("fallback", true)))
	sim.free()


func test_f3_hides_unresolved_local_player_fallback() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().get("organics", []) as Array).is_empty(),
			"an unresolved local avatar never leaks through the fallback path")
	sim.free()


func test_reused_player_slot_invalidates_old_collision_attempt_identity() -> void:
	# US02 intentionally cannot resolve through this provider, so the mission
	# soldier leaves a negative collision attempt on pool-0 slot 0. After WAC
	# removes it, the local US01 player reuses that exact packed handle. The new
	# registry spawn identity must invalidate the negative cache and resolve all
	# authored person sections on demand.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3.ZERO, Vector3.ZERO).is_empty())
	var probe := NovaSimulation.new()
	assert_true(probe.load_from_mission_data(md))
	var old_ssn := probe.get_entity_net_id(0)
	probe.free()
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(
			0, {"action_type": 22, "param1": old_ssn}).is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()))
	var placer := PlayerOnlySkeletalPlacerStub.new(data, skeletal)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, placer), 0,
			"US02 records one unresolved attempt on slot 0")
	for _tick in 16:
		sim.step()
	assert_true(sim.get_entity_effect_state_for_ssn(old_ssn).is_empty(),
			"the original slot occupant was removed")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1),
			"US01 reuses the freed pool-0 slot")

	assert_true((sim.get_hitbox_debug().get("organics", []) as Array).is_empty(),
			"the newly resolved local avatar remains hidden from F3")
	var local_bms_id := int(sim.get_entity_debug(
			sim.get_entity_count() - 1).get("bms_id", -1))
	assert_true(bool(sim.get_destruction_debug(local_bms_id).get(
			"has_collision_instance", false)),
			"the old negative attempt cannot suppress the new slot identity")
	sim.free()


func test_restart_re_resolves_the_restored_collision_identity() -> void:
	# Collision caches live outside World::Snapshot. Reusing slot 0 during play
	# must not leave the restored baseline actor unbound after Stop/Restart.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			NovaMissionData.KIND_ORGANIC, 105311,
			Vector3.ZERO, Vector3.ZERO)
	assert_false(placed.is_empty())
	var bms_id := int(placed.get("bms_id", 0))
	var probe := NovaSimulation.new()
	assert_true(probe.load_from_mission_data(md))
	var old_ssn := probe.get_entity_net_id(0)
	probe.free()
	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(
			0, {"action_type": 22, "param1": old_ssn}).is_empty())

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/3di3/CharModel.3di")), OK)
	var bad_root := NovaResourceRoot.new()
	assert_eq(bad_root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/bad")), OK)
	var skeletal := NovaSkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_idle": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()))
	var placer := SkeletalDataPlacerStub.new(data, skeletal)

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db, placer), 1)
	assert_true(bool(sim.get_destruction_debug(
			bms_id).get("has_collision_instance", false)))
	for _tick in 16:
		sim.step()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().get("organics", []) as Array).is_empty())
	var local_bms_id := int(sim.get_entity_debug(
			sim.get_entity_count() - 1).get("bms_id", -1))
	assert_true(bool(sim.get_destruction_debug(local_bms_id).get(
			"has_collision_instance", false)),
			"the replacement local occupant receives the cached graphic")

	sim.restart()
	var restored := sim.get_destruction_debug(bms_id)
	assert_true(bool(restored.get("has_collision_instance", false)),
			"restart rebinds the baseline before any F3 or round demand query")
	var restored_rows: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(restored_rows.size(), 19,
			"the restored non-local actor exposes every authored section")
	for value in restored_rows:
		var row: Dictionary = value
		assert_eq(int(row.get("entity_handle", -1)), 0)
		assert_false(bool(row.get("fallback", true)))
	sim.free()


func test_f3_organic_fallbacks_match_live_filtering_bounds() -> void:
	# F3 must describe the same unresolved pool-0 actors RoundSim can hit:
	# engine-flag filtering only (dead bodies remain solid), mission-wide except
	# for the local avatar, and bounded by the shared 96-entity debug budget.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	for i in range(101):
		var pos := Vector3(200, 0, 0) if i == 0 else Vector3(i % 10, 0, i % 7)
		assert_false(md.add_entity(
				NovaMissionData.KIND_ORGANIC, 105311, pos,
				Vector3.ZERO).is_empty())

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.debug_set_entity_health(1, 0)

	var rows: Array = sim.get_hitbox_debug().get("organics", [])
	assert_eq(rows.size(), 96, "fallback entities obey the F3 target cap")
	var handles := {}
	for value in rows:
		var row: Dictionary = value
		assert_true(bool(row.get("fallback", false)))
		handles[int(row.get("entity_handle", -1))] = true
	assert_true(handles.has(0), "the 200-unit actor remains visible mission-wide")
	assert_true(handles.has(1), "a zero-health corpse retains its bullet fallback")
	sim.free()


func test_time_driven_collision_advances_without_an_ai_brain() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	assert_false(md.add_entity(
		NovaMissionData.KIND_BUILDING, 102001,
		Vector3.ZERO, Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		'res://../fixtures/def/items.def')), OK)
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
		'res://../fixtures/3dp/Pmpjk01/Pmpjk01.3di')), OK)
	assert_gt(data.get_part_anim_count(0), 0)
	# Remove the IR-local copy only. The parsed model-level PANM remains the
	# canonical fallback used when effective LOD0 has no local rows.
	for i in range(data.get_part_anim_count(0) - 1, -1, -1):
		assert_true(data.delete_part_anim(0, i))
	assert_eq(data.get_part_anim_count(0), 0)
	assert_true(data.has_live_panm_for_lod(0),
		'model-level PANM remains live as effective LOD0 fallback')
	var targets := data.get_effective_panm_targets(0)
	assert_gt(targets.size(), 0)
	var pose_a: Dictionary = data.evaluate_panm(0, 0, {})
	var pose_b: Dictionary = data.evaluate_panm(0, 640, {})
	var moved_target := -1
	for target_value in targets:
		var target := int(target_value)
		if pose_a.has(target) and pose_b.has(target) and not (
				pose_a[target] as Transform3D).is_equal_approx(
					pose_b[target] as Transform3D):
			moved_target = target
			break
	assert_gte(moved_target, 0,
		'effective model-level PANM advances with the shared time')

	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 0, 'static has no AiEntity controls')
	assert_eq(sim.resolve_collision_instances(
		item_db, ObjectDataPlacerStub.new(data)), 1)
	var fallback_tick := sim.get_logic_tick()
	var fallback_before: PackedVector3Array = (
		(sim.get_hitbox_debug().get('entities', [])[0] as Dictionary)
		.get('tris', PackedVector3Array()))
	for _tick in range(40):
		sim.step()
	assert_eq(sim.get_logic_tick() - fallback_tick, 40)
	var fallback_after: PackedVector3Array = (
		(sim.get_hitbox_debug().get('entities', [])[0] as Dictionary)
		.get('tris', PackedVector3Array()))
	var fallback_moved := 0
	for i in fallback_before.size():
		if fallback_before[i].distance_to(fallback_after[i]) > 0.002:
			fallback_moved += 1
	assert_gt(fallback_moved, 0,
		'direct/headless simulation uses deterministic logic_tick * 16')

	sim.debug_set_panm_time_ms(0)
	var before_debug: Array = sim.get_hitbox_debug().get('entities', [])
	assert_eq(before_debug.size(), 1)
	var before: PackedVector3Array = (before_debug[0] as Dictionary).get(
		'tris', PackedVector3Array())
	assert_gt(before.size(), 0)
	sim.debug_set_panm_time_ms(640)
	var after_debug: Array = sim.get_hitbox_debug().get('entities', [])
	assert_eq(after_debug.size(), 1)
	var after: PackedVector3Array = (after_debug[0] as Dictionary).get(
		'tris', PackedVector3Array())
	assert_eq(after.size(), before.size())
	var moved := 0
	for i in before.size():
		if before[i].distance_to(after[i]) > 0.002:
			moved += 1
	assert_gt(moved, 0,
		'free-running PANM uses retail milliseconds with zero controls')
	sim.free()


func test_entity_debug_card_keeps_its_shape_after_a_scripted_remove() -> void:
	# VaporizeSingle (action 22) despawns the registry slot while the AI entity
	# stays in the pool - the card must keep a STABLE key set with typed
	# defaults for the registry half, never a partial dictionary.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	var probe := NovaSimulation.new()
	assert_true(probe.load_from_mission_data(md))
	var ssn := probe.get_entity_net_id(0)
	probe.free()

	assert_false(md.add_event(0, 0, 0).is_empty())
	assert_false(md.add_event_action(0, {"action_type": 22, "param1": ssn}).is_empty())
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_effect_state_for_ssn(ssn).size(), NovaSimulation.EFFECT_STATE_COUNT,
			"the effect lookup sees the live registry slot before VaporizeSingle")
	for _i in range(16):
		sim.step()

	var card: Dictionary = sim.get_entity_debug(0)
	assert_false(card.is_empty(), "the AI entity outlives its registry slot")
	assert_true(card.has("kind") and card.has("alive") and card.has("name"),
		"the registry half keeps its keys")
	assert_eq(int(card["kind"]), -1, "...with typed defaults (kind -1)")
	assert_false(bool(card["alive"]), "...alive false")
	assert_eq(int(card["net_id"]), ssn, "the AI half still reports its scalars")
	assert_true(sim.get_entity_effect_state_for_ssn(ssn).is_empty(),
			"attached effects detach as soon as VaporizeSingle removes the registry slot")
	sim.free()


func test_ai_state_name_static_lookup() -> void:
	assert_eq(NovaSimulation.ai_state_name(16), "GROUND_FOLLOWWP")
	assert_eq(NovaSimulation.ai_state_name(13), "?", "id gaps read as unknowns")
	assert_eq(NovaSimulation.ai_state_name(23), "GROUND_DEAD")
