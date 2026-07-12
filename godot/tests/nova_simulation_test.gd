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

const ANIM_FIXTURES := "res://../fixtures/anim"

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
				"delayend": 0, "soundset": "RECOIL_BEGIN"},
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
	assert_true(sim.drain_local_player_weapon_events().is_empty(), "the drain is destructive")
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

	assert_true(sim.apply_local_player_loadout("WPN_M4AUTO", 6))
	assert_eq(sim.get_local_player_class(), 6, "accepted class is authoritative on reopen")
	assert_true(sim.apply_local_player_loadout("", 9), "the authored NONE row is a valid apply")
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
	var pos := sim.get_entity_position(0)
	assert_true(pos.is_equal_approx(Vector3(10, 2, -1)),
		"command-125 soldier uses the IDA-priority ctrlx seat, converted to Godot axes")
	assert_almost_eq(sim.get_entity_yaw_deg(0), 45.0, 0.01,
		"non-gunner mounted seats carry their local yaw offset")
	# The mounted anim state (100 = anim_sit_24) is asserted via the debug card below; the present
	# snapshot is the listen-server ClientState now (covered by nova_listen_server_test).
	var card: Dictionary = sim.get_entity_debug(0)
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
	var pos := sim.get_entity_position(0)
	var expected := Vector3(10.1189880, 2.1048889, 0.7148895)
	assert_lt(pos.distance_to(expected), 0.001,
		"mounted seat local follows the same rotated side as the selected model userpoint")
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
	for _i in range(16):
		sim.step()

	var card: Dictionary = sim.get_entity_debug(0)
	assert_false(card.is_empty(), "the AI entity outlives its registry slot")
	assert_true(card.has("kind") and card.has("alive") and card.has("name"),
		"the registry half keeps its keys")
	assert_eq(int(card["kind"]), -1, "...with typed defaults (kind -1)")
	assert_false(bool(card["alive"]), "...alive false")
	assert_eq(int(card["net_id"]), ssn, "the AI half still reports its scalars")
	sim.free()


func test_ai_state_name_static_lookup() -> void:
	assert_eq(NovaSimulation.ai_state_name(16), "GROUND_FOLLOWWP")
	assert_eq(NovaSimulation.ai_state_name(13), "?", "id gaps read as unknowns")
	assert_eq(NovaSimulation.ai_state_name(23), "GROUND_DEAD")
