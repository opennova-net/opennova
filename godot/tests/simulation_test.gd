extends GutTest

const NATIVE_RUNTIME_TIMING_KEYS := [
	"sim_tick_us",
	"net_tick_us",
	"present_snapshot_us",
	"occlusion_build_us",
	"occlusion_probe_us",
]

# The world-update rows every role's logic tick lands on the frame-stats
# board through the kernel's one tick profile (ADR 0043 d5), plus the bare
# local role's frame tail: the direct (no-net) tick fills these; the
# host/joiner-only rows stay unsampled.
const WORLD_PHASE_SLOTS := [
	FrameStats.SIM_SERVER_WORLD, FrameStats.SIM_WORLD_SETUP,
	FrameStats.SIM_WORLD_SCRIPTS, FrameStats.SIM_UPDATE_ENTITIES,
	FrameStats.SIM_UPDATE_ATTACHMENTS, FrameStats.SIM_UPDATE_PRECIPITATION,
	FrameStats.SIM_UPDATE_PROJECTILES, FrameStats.SIM_UPDATE_EXPLOSIONS,
	FrameStats.SIM_WORLD_HOUSEKEEPING, FrameStats.SIM_PLAYER_TAIL,
	FrameStats.SIM_WEAPON_WALK, FrameStats.SIM_ADM_RESOLVE,
]
const HOST_ONLY_SLOTS := [
	FrameStats.SIM_HOST_PUMP, FrameStats.SIM_SERVER_TICK,
	FrameStats.SIM_SERVER_INPUT, FrameStats.SIM_SERVER_REPLICATION,
]

# Simulation (the GDExtension binding): promote a synthetic BMS mission into a live
# world + AI system, tick it, and confirm the AI walks entities along their authored route.
# This is the in-Godot end of step 1 (promotion) + step 2 (locomotion).

# --- Native fixture roots (S16) ---------------------------------------------
# The Dictionary seat seam (set_item_seat_specs) and the render-placer
# collision sources are gone: a sim poses/attaches ONLY from its own asset
# root (set_asset_root) and the native extractor
# (install_seat_specs_for_type_ids over items.def rows + model userpoints).
# Tests compose a flat loose dir of committed fixtures. ResourceRoot
# rejects user:// paths, so the dirs live under OS.get_cache_dir().

var _native_fixture_dirs: Array[String] = []


func after_each() -> void:
	for dir in _native_fixture_dirs:
		TestFs.remove_dir_recursive(dir)
	_native_fixture_dirs.clear()


# Authored 3DI variants minted once from the retired edit surface
# (fixtures/README.md): CTRL names, PANM rows and flags the
# sim's own parse-once cache consumes from disk.
const SYN_MOUNT_HEAT_GLOW_SLIDE := "res://../fixtures/threedi/synth/mount_heat_glow_slide_part1.3di"
const SYN_ARMRY_SPECIAL1_SLIDE := "res://../fixtures/threedi/synth/armory_special1_slide_part1.3di"
const SYN_ARMRY_SPECIAL2_SLIDE := "res://../fixtures/threedi/synth/armory_special2_slide_part1.3di"
const SYN_TANK_SPECIAL1_SLIDE_EWEP01 := "res://../fixtures/threedi/synth/tank_special1_slide_ewep01.3di"
const SYN_PMP_LOD0_INERT_LOD1_LIVE := "res://../fixtures/threedi/synth/pump_lod0_inert_lod1_sine_rotz.3di"
const SYN_PANM_LIVENESS_DIR := "res://../fixtures/threedi/synth/"
# The USE scan admits a seat only inside the player's view cone (just under
# 90 deg standing, 5 deg seated); a test that presses USE looks at the seat first.
const MountLook := preload("res://tests/support/mount_look.gd")


func _native_fixture_dir() -> String:
	var dir := OS.get_cache_dir().path_join("sim_native_%d_%d" % [
			Time.get_ticks_usec(), _native_fixture_dirs.size()])
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_native_fixture_dirs.append(dir)
	return dir


func _write_fixture_bytes(dir: String, name: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(dir.path_join(name), FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return
	file.store_buffer(bytes)
	file.close()


func _copy_fixture(dir: String, source_res_path: String, dest_name: String) -> void:
	_write_fixture_bytes(dir, dest_name, FileAccess.get_file_as_bytes(source_res_path))


func _fixture_items_text() -> String:
	return FileAccess.get_file_as_bytes(
			"res://../fixtures/def/items.def").get_string_from_ascii()


func _item_db_from_text(dir: String, text: String) -> ItemDatabase:
	TestFs.write_text(self, dir.path_join("items.def"), text)
	var db := ItemDatabase.new()
	assert_eq(db.load(dir.path_join("items.def")), OK)
	return db


# Rename one 16-byte USRP name field in raw .3di bytes (whole-name match) —
# the same byte-patch technique the KZ husk test uses — so a committed model
# can stand in for any retail seat prefix without another binary fixture.
func _bytes_with_renamed_user_point(bytes: PackedByteArray, from_name: String,
		to_name: String) -> PackedByteArray:
	var needle := from_name.to_ascii_buffer()
	var offset := -1
	for i in range(bytes.size() - needle.size()):
		var matches := true
		for j in range(needle.size()):
			if bytes[i + j] != needle[j]:
				matches = false
				break
		if matches and bytes[i + needle.size()] == 0:
			offset = i
			break
	assert_gte(offset, 0, "user point %s present in the model bytes" % from_name)
	if offset < 0:
		return bytes
	for j in range(16):
		bytes[offset + j] = 0
	var replacement := to_name.to_ascii_buffer()
	for j in range(replacement.size()):
		bytes[offset + j] = replacement[j]
	return bytes


const BINOC_REL := "bad/BINOC.bad"


# The shipped BINOC.bad from the reference fixture set; "" (after pending)
# without OPENNOVA_JO_ASSETS. Every rig test starts by checking it.
func _seat_fixture_spawn(model_path: String, point_name: String,
		position: Vector3, rotation: Vector3 = Vector3.ZERO) -> Vector3:
	var model := ObjectData.new()
	assert_eq(model.open_file(model_path), OK)
	for i in range(model.get_user_point_count()):
		var point := model.get_user_point_info(i)
		if String(point.name).to_lower() == point_name.to_lower():
			var posed := MissionObjectPlacer.entity_transform(position, rotation) * point.position
			return Vector3(posed.x, -posed.z, posed.y)
	fail_test("missing authored seat point " + point_name)
	return position


func _install_native_seat_table(sim: Simulation, dir: String,
		item_db: ItemDatabase, type_ids: PackedInt32Array) -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	sim.set_asset_root(root)
	# Boarding requires the same definition metadata as the authored seat table.
	sim.resolve_item_traits(item_db)
	assert_true(sim.install_seat_specs_for_type_ids(item_db, type_ids),
			"native seat-spec install over the composed fixture root")
	return root


func _native_asset_root(sim: Simulation, dir: String) -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	sim.set_asset_root(root)
	return root


# Retail runs the entity update only while its world holds a human: on the
# authority, Game_ProcessMainFrame skips Entity_UpdateAllEntities once the WAC
# clock has started and Server_BuildEntitySlotLists counted no visible player,
# and Entity_UpdateAllEntities itself returns while there is no local player
# entity. A single-player world always has its player, so a fixture built
# without one stands its local player in the world: far from the entities
# under test unless the test places it.
const FIXTURE_HUMAN_POSITION := Vector3(0, 0, 2000)


func _spawn_fixture_human(sim: Simulation,
		position: Vector3 = FIXTURE_HUMAN_POSITION, team: int = 1) -> void:
	assert_true(sim.spawn_local_player(position, 0.0, team),
			"the fixture's local player is the human that keeps the world running")


func _fast_rope_item_db() -> ItemDatabase:
	var path := ProjectSettings.globalize_path(
			"res://.godot/ctrl_fast_rope_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	# The part-anim channels live in an AI brain, which a placed item owns when
	# its row carries AIData and a brain-class ai_function (the attrib test in
	# Entity_SpawnFromBMSRecord, then the class init).
	file.store_string("""begin "Fast Rope Control Fixture"
  id 105006
  type vehicle
  graphic StaticCrate1
  sid fastropectrl
  ai_function cveh
  render_function cveh
  move_function cveh
  hp 50
  attrib: AIData FastRope
end
""")
	file.close()
	var result := ItemDatabase.new()
	assert_eq(result.load(path), OK)
	assert_eq(int(result.get_attrib(105006)) & 0x1000, 0x1000,
			"fixture carries retail's FastRope item attribute")
	return result


func _vehicle_ctrl_item_db() -> ItemDatabase:
	var path := ProjectSettings.globalize_path(
			"res://.godot/ctrl_vehicle_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string("""begin "CTRL Vehicle Fixture"
  id 105007
  type vehicle
  graphic tank
  sid ctrlvehicle
  ai_function cveh
  render_function cveh
  move_function cveh
  attrib: AIData neutral PlayerControl
  hp 3000
  turn_rate 65
  turn_rate2 41
  acceleration 15
  deceleration 70
  player_speed 94
  physics 1
  torque 3
end
""")
	file.close()
	var result := ItemDatabase.new()
	assert_eq(result.load(path), OK)
	assert_eq(int(result.get_vehicle_physics(105007)[0]), 1)
	return result


func _physicsless_air_item_db() -> ItemDatabase:
	var path := ProjectSettings.globalize_path(
			"res://.godot/physicsless_air_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string("""begin "CHel without ground selector A"
  id 105008
  type vehicle
  graphic StaticCrate1
  sid chel_without_physics_a
  ai_function chel
  render_function chel
  move_function chel
  attrib: AIData neutral PlayerControl
  hp 3000
  climb_speed 25
  turn_roll 30
  speed_pitch 20
end

begin "CHel without ground selector B"
  id 105009
  type vehicle
  graphic StaticCrate1
  sid chel_without_physics_b
  ai_function CHel
  render_function CHel
  move_function CHelScout
  attrib: AIData neutral PlayerControl
  hp 3000
  climb_speed 20
end

begin "Ground control without selector"
  id 105010
  type vehicle
  graphic StaticCrate1
  sid cveh_without_physics
  ai_function cveh
  render_function cveh
  move_function cveh
  attrib: AIData neutral PlayerControl
  hp 3000
end

begin "cpln without ground selector"
  id 105011
  type vehicle
  graphic StaticCrate1
  sid cpln_without_physics
  ai_function cpln
  render_function cpln
  move_function cpln
  attrib: AIData neutral PlayerControl
  hp 3000
  climb_speed 30
  turn_roll 25
  speed_pitch 15
end
""")
	file.close()
	var result := ItemDatabase.new()
	assert_eq(result.load(path), OK)
	assert_eq(int(result.get_vehicle_physics(105008)[0]), 0,
			"the parsed chel witness really omits the ground physics selector")
	assert_eq(int(result.get_vehicle_physics(105009)[0]), 0)
	assert_eq(int(result.get_vehicle_physics(105010)[0]), 0)
	assert_eq(int(result.get_vehicle_physics(105011)[0]), 0)
	return result


func test_host_projectile_options_roundtrip() -> void:
	var sim := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.fat_bullets = true
	host_options.one_shot_kill = true
	sim.configure_host_session(host_options)
	var options := sim.get_host_session_config()
	assert_true(options.fat_bullets)
	assert_true(options.one_shot_kill)


func test_host_spectator_options_and_live_f3_transition() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.max_players = 4
	host_options.spectator_slots = -1
	host_options.spectator_password = "watch"
	sim.configure_host_session(host_options)
	var options := sim.get_host_session_config()
	assert_eq(options.spectator_slots, -1)
	assert_eq(options.spectator_password, "watch")
	assert_true(sim.enable_host_listen(0))
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.has_local_player(),
			"the listen host owns the player F3 will transition")

	var start_tick := sim.get_logic_tick()
	var body_position := sim.get_local_player_position()
	assert_true(sim.set_local_spectator(true))
	assert_true(sim.is_local_spectator())
	sim.set_player_input(true, false, false, false, false, false, false)
	for _i in range(3):
		assert_true(sim.step())
	assert_eq(sim.get_logic_tick(), start_tick + 3,
			"spectator free flight does not pause the authoritative game")
	assert_eq(sim.get_local_player_position(), body_position,
			"spectator input is detached from the hidden player body")

	assert_true(sim.set_local_spectator(false))
	assert_false(sim.is_local_spectator())
	assert_true(sim.has_local_player(),
			"leaving spectator mode respawns the same playable slot")


func test_host_class_allow_mask_roundtrips_to_the_ui_seam() -> void:
	var sim := Simulation.new()
	assert_eq(sim.get_class_allow_mask(), 0x03FF,
			"a fresh host exposes retail's all-ten-classes default")
	var host_options := HostSessionOptions.new()
	host_options.class_allow_mask = 0x0155
	sim.configure_host_session(host_options)
	var options := sim.get_host_session_config()
	assert_eq(options.class_allow_mask, 0x0155,
			"the configured writer source survives the Godot session adapter")
	assert_eq(sim.get_class_allow_mask(), 0x0155,
			"the armory-facing seam exposes the same configured host mask")


func test_all_mode_rule_options_roundtrip_to_the_host() -> void:
	var sim := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.game_type = NetProtocol.GAME_TYPE_FLAGBALL
	host_options.max_score = 9
	host_options.koth_delta = 7
	host_options.flag_return_ticks = 333
	host_options.flag_reset_seconds = 444
	host_options.armory_reuse_time = 45
	host_options.capture_duration_seconds = 27
	host_options.capture_speed_setting = 2
	host_options.spawn_wave_time_base = 4
	host_options.spawn_wave_time_zone = 12
	host_options.default_spawn_requires_no_team_zone = 1
	host_options.num_teams = 4
	sim.configure_host_session(host_options)
	var options := sim.get_host_session_config()
	assert_eq(options.game_type, NetProtocol.GAME_TYPE_FLAGBALL)
	assert_eq(options.max_score, 9)
	assert_eq(options.koth_delta, 7)
	assert_eq(options.flag_return_ticks, 333)
	assert_eq(options.flag_reset_seconds, 444,
			"the flag carry limit rides the host record into the live config")
	assert_eq(options.armory_reuse_time, 45,
			"the armory-reuse cooldown rides the host record into the live config")
	assert_eq(options.capture_duration_seconds, 27)
	assert_eq(options.capture_speed_setting, 2)
	assert_eq(options.spawn_wave_time_base, 4)
	assert_eq(options.spawn_wave_time_zone, 12)
	assert_eq(options.default_spawn_requires_no_team_zone, 1)
	assert_eq(options.num_teams, 4)


func test_joiner_full_bms_load_admits_against_the_joined_session_player_cap() -> void:
	# A record flagged "no less than N players" (bmsi attribute bit 0x10) admits
	# only once the session's player limit reaches N. The joiner's own full-BMS
	# load (tools and direct joins) must gate on the cap the HOST published in
	# its pre-load burst (the S2C 0x64 session block), not on the joiner's
	# untouched host-screen default of 1, or the two peers promote different
	# worlds from the same document.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	mission.add_entity(3, 0x14BF, Vector3(0, 0, 0), Vector3.ZERO) # KIND_ORGANIC, Generic Soldier
	mission.add_entity(3, 0x14BF, Vector3(10, 0, 0), Vector3.ZERO)
	mission.add_entity(3, 0x14BF, Vector3(20, 0, 0), Vector3.ZERO)
	assert_true(mission.set_entity_property_int(3, 2, "ai_flags", 0x10))
	assert_true(mission.set_entity_property_int(3, 2, "no_less_than", 3))

	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	assert_eq(host.get_spawned_count(), 3,
			"the host's cap of 4 admits the three-player-minimum organic")

	var joiner := Simulation.new()
	assert_true(joiner.enable_join("127.0.0.1", host.get_host_listen_port(), "CapJoiner"))
	joiner.set_join_world_ready(false)
	# Retail authenticates and drains the pre-load burst (the 0x60/0x64
	# transfers, the player list, the terminal 0x11) before it builds a world.
	var preloaded := false
	for _i in range(600):
		joiner.poll_join_preload()
		host.step()
		if joiner.is_join_preload_ready():
			preloaded = true
			break
		OS.delay_msec(2)
	assert_true(preloaded, "the joiner drained the pre-load burst on the live socket")
	assert_true(joiner.load_from_mission_data(mission))
	assert_eq(joiner.get_spawned_count(), 3,
			"the joiner admits the same organic against the host's published cap of 4")


func test_host_integrity_profile_is_explicit_and_roundtrips() -> void:
	var sim := Simulation.new()
	assert_eq(sim.get_host_session_config().integrity_profile, "",
			"a host never infers integrity bytes from its expansion name")
	var host_options := HostSessionOptions.new()
	host_options.integrity_profile = " retail-revx02-024f56f2-2d087374 "
	sim.configure_host_session(host_options)
	assert_eq(sim.get_host_session_config().integrity_profile,
			"retail-revx02-024f56f2-2d087374",
			"the independently witnessed corpus is an explicit host-session input")


func test_hud_minimap_snapshot_and_controls_have_a_stable_contract() -> void:
	var sim: Simulation = autofree(Simulation.new())
	var snapshot: PackedInt32Array = sim.get_hud_minimap_snapshot()
	assert_eq(snapshot.size(), int(Simulation.HUD_MINIMAP_HEADER_SIZE),
			"A fresh Simulation publishes an empty minimap snapshot.")
	assert_eq(snapshot[0], int(Simulation.HUD_MINIMAP_SNAPSHOT_VERSION),
			"Snapshot version leads the header.")
	assert_eq(int(Simulation.HUD_MINIMAP_SNAPSHOT_VERSION), 4,
			"Row layout v4: the draw-policy tail plus the local-team medic bit ride each row.")
	assert_eq(snapshot[1], int(Simulation.HUD_MINIMAP_STRIDE))
	assert_eq(snapshot[2], 0, "No retained rows without a mission.")
	assert_eq(int(Simulation.HUD_MINIMAP_HEADER_SIZE), 3)
	assert_eq(int(Simulation.HUD_MINIMAP_STRIDE), 17,
			"v4 appends the medic bit the map marker walk turns into the cross.")
	assert_eq(sim.get_local_player_heading_bam(), 0,
			"No local player -> heading zero.")
	assert_eq(sim.get_hud_radar_zoom_q16(), 65536,
			"The radar zoom boots at the retail Q16 default.")
	# Zoom OUT grows the world-extent value x1.15; IN shrinks x0.85.
	# [orig: Input_HandleActionBinding cases 361/360 @0x49beaf/@0x49bcb0]
	assert_eq(sim.request_hud_radar_zoom(1), 75366,
			"radarout applies the retail x1.15 truncation.")
	assert_eq(sim.request_hud_radar_zoom(0), 65536,
			"Direction zero restores the spawn value.")
	assert_eq(sim.request_hud_radar_zoom(-1), 55705,
			"radarin applies the retail x0.85 truncation.")
	assert_false(sim.get_hud_map_flip_180(),
			"No mission -> no RotateMap180 attribute.")
	sim.request_hud_radar_zoom(-1)
	sim.build_demo_mission()
	assert_eq(sim.get_hud_radar_zoom_q16(), 65536,
			"A world reset restores the spawn zoom like Player_InitPlayer.")
	# The M cycle and the mode-gated zoom routing (witnessed in the engine's
	# HudMapControl: HUD_CycleMapMode 0->2->3->0; the radar keys step the
	# big-map zoom while a mode is up).
	assert_eq(sim.get_hud_map_mode(), 0, "The map boots off.")
	assert_eq(sim.request_hud_map_cycle(), 2, "M cycles 0 -> 2.")
	assert_eq(sim.request_hud_radar_zoom(1), 602931,
			"While a mode is up the radar keys step the big-map zoom.")
	assert_eq(sim.get_hud_radar_zoom_q16(), 65536,
			"The corner zoom is untouched by big-map stepping.")
	assert_eq(sim.request_hud_map_cycle(), 3, "M cycles 2 -> 3.")
	assert_eq(sim.request_hud_map_cycle(), 0, "M cycles 3 -> 0.")
	assert_eq(sim.get_hud_big_zoom_q16(), 602931,
			"The big-map zoom persists across the cycle until a respawn.")


func test_hud_minimap_snapshot_restores_the_local_deploy_marker() -> void:
	var sim: Simulation = autofree(Simulation.new())
	sim.build_demo_mission()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	var snapshot: PackedInt32Array = sim.get_hud_minimap_snapshot()
	var stride := int(Simulation.HUD_MINIMAP_STRIDE)
	var header := int(Simulation.HUD_MINIMAP_HEADER_SIZE)
	assert_eq(snapshot.size(), header + int(snapshot[2]) * stride)
	var local_handle := sim.get_local_player_wire_handle()
	var matches := 0
	for row in range(int(snapshot[2])):
		var base := header + row * stride
		if int(snapshot[base + 1]) != local_handle:
			continue
		matches += 1
		assert_eq(int(snapshot[base + 0]), 0,
				"the local deploy row occupies retail's regular persistent bank")
		assert_eq(int(snapshot[base + 6]), 3,
				"a live Person resolves to TSDicon cell 3")
		assert_eq(int(snapshot[base + 7]) & 0xFFFFFFFF, 0xFF304080,
				"team 1 uses the raw retail blue before MODULATE2X")
		assert_eq(int(snapshot[base + 8]), 0x10)
		assert_eq(int(snapshot[base + 10]), 0,
				"regular local rows remain drawable at zero lifetime")
		assert_eq(int(snapshot[base + 11]), 1)
		assert_eq(int(snapshot[base + 12]) & 1, 1,
				"the live-player glyph rotates with its heading")
		assert_eq(int(snapshot[base + 13]), 0x20000)
		assert_eq(int(snapshot[base + 14]), 0x20000)
		assert_eq(int(snapshot[base + 15]), 6,
				"the 2-world-unit Person class still floors at six pixels")
	assert_eq(matches, 1,
			"the local deploy contributes exactly one ordinary player marker")


func test_demo_mission_promotes() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_true(sim.is_loaded(), "demo mission promoted")
	assert_eq(sim.get_entity_count(), 2, "two organics got AI brains")
	assert_eq(sim.get_brain_count(), 2, "two AI brains attached")
	assert_eq(sim.get_spawned_count(), 6, "one building + three markers + two organics spawned into pools")
	assert_eq(sim.get_entity_state(0), 0, "an organic leaves the unused vehicle brain in state zero")


func test_runtime_profiling_is_opt_in_reset_stable_and_behavior_neutral() -> void:
	var sim := Simulation.new()
	assert_false(sim.is_runtime_profiling_enabled(),
			"retail/default play does not own the profiling clocks")
	sim.build_demo_mission()
	# The entity update's phases are sampled only while it runs.
	_spawn_fixture_human(sim)
	sim.occlusion_init_mission()
	assert_true(sim.step())
	sim.run_occlusion_frame(
			Transform3D.IDENTITY, 90.0, 1.0, 640.0, 500.0, -100.0, false)
	var unprofiled_snapshot: PackedFloat32Array = sim.get_present_snapshot()
	# The full verdict set through the delta forms: a baseline reset re-arms
	# the complete emission (the same walk the occlusion frame consumes).
	sim.reset_occlusion_apply_baseline()
	var unprofiled_buildings: PackedInt64Array = sim.get_building_visibility_changes()
	var unprofiled_culled: PackedInt32Array = sim.get_render_culled_changes()
	var unprofiled_positions: Array[Vector3] = []
	for i in range(sim.get_entity_count()):
		unprofiled_positions.append(sim.get_entity_position(i))
	var counters: Dictionary = sim.get_runtime_perf_counters()
	_assert_native_runtime_timings_zero(counters)
	assert_false(bool(counters.get("runtime_profiling_enabled", true)))
	assert_false(bool(counters.get("trace_profiling_enabled", true)))

	sim.set_runtime_profiling_enabled(true)
	assert_true(sim.is_runtime_profiling_enabled())
	sim.run_occlusion_frame(
			Transform3D.IDENTITY, 90.0, 1.0, 640.0, 500.0, -100.0, false)
	sim.reset_occlusion_apply_baseline()
	assert_eq(sim.get_building_visibility_changes(), unprofiled_buildings,
			"profiling does not change building submission")
	assert_eq(sim.get_render_culled_changes(), unprofiled_culled,
			"profiling does not change entity render gates")
	assert_eq(sim.get_present_snapshot(), unprofiled_snapshot,
			"profiling does not change the client-view snapshot")
	for i in range(unprofiled_positions.size()):
		assert_eq(sim.get_entity_position(i), unprofiled_positions[i],
				"turning profiling on does not mutate simulation state")
	assert_true(sim.step())
	counters = sim.get_runtime_perf_counters()
	assert_true(bool(counters.get("runtime_profiling_enabled", false)))
	assert_true(bool(counters.get("trace_profiling_enabled", false)))
	var sampled_us := 0
	for key in NATIVE_RUNTIME_TIMING_KEYS:
		sampled_us += int(counters.get(key, 0))
	assert_gt(sampled_us, 0,
			"the enabled gate records at least one native runtime span")
	# The tick profile folds onto the frame-stats board once per session
	# frame: every world-update row the direct tick ran is sampled, the
	# host-only rows are not.
	var stats := FrameStats.new()
	sim.set_frame_stats(stats)
	stats.set_capture_active(true)
	var frame_input := MissionFrameInput.new()
	frame_input.delta_seconds = Simulation.tick_dt()
	var frame_outcome := sim.step_session_frame(frame_input)
	assert_not_null(frame_outcome)
	var window := stats.drain()
	var samples: PackedInt32Array = window.get_sample_frames()
	var sums: PackedInt64Array = window.get_sums()
	var world_phase_us := 0
	for slot in WORLD_PHASE_SLOTS:
		assert_gt(samples[slot], 0,
				"the direct tick samples the '%s' F3 phase" % FrameStats.slot_name(slot))
		world_phase_us += sums[slot]
	for slot in HOST_ONLY_SLOTS:
		assert_eq(samples[slot], 0,
				"a direct tick never samples the host-only '%s' row" % FrameStats.slot_name(slot))
	assert_gt(world_phase_us, 0,
			"an enabled direct tick attributes work below the world-update box")

	# Mission reload rebuilds CollisionWorld, so this pins reapplication of the
	# one profiling request as well as the cleared timing snapshot.
	sim.build_demo_mission()
	assert_true(sim.is_runtime_profiling_enabled(),
			"a mission reset preserves the active consumer request")
	counters = sim.get_runtime_perf_counters()
	_assert_native_runtime_timings_zero(counters)
	assert_true(bool(counters.get("trace_profiling_enabled", false)),
			"the rebuilt CollisionWorld inherits the unified gate")

	sim.set_runtime_profiling_enabled(false)
	assert_false(sim.is_runtime_profiling_enabled())
	_assert_native_runtime_timings_zero(sim.get_runtime_perf_counters())
	sim.occlusion_init_mission()
	sim.run_occlusion_frame(
			Transform3D.IDENTITY, 90.0, 1.0, 640.0, 500.0, -100.0, false)
	assert_true(sim.step())
	sim.get_present_snapshot()
	counters = sim.get_runtime_perf_counters()
	_assert_native_runtime_timings_zero(counters)
	assert_false(bool(counters.get("trace_profiling_enabled", true)))
	stats.set_capture_active(true)
	frame_outcome = sim.step_session_frame(frame_input)
	assert_not_null(frame_outcome)
	assert_eq(frame_outcome.get_ticks_run(), 1,
			"the tick accounting is exported with profiling closed")
	window = stats.drain()
	samples = window.get_sample_frames()
	for slot in WORLD_PHASE_SLOTS:
		assert_eq(samples[slot], 0,
				"'%s' is not sampled while F3/native profiling is closed" % FrameStats.slot_name(slot))


func _assert_native_runtime_timings_zero(counters: Dictionary) -> void:
	for key in NATIVE_RUNTIME_TIMING_KEYS:
		assert_eq(int(counters.get(key, -1)), 0,
				"%s stays zero while native profiling is closed" % key)


func test_binocular_and_nvg_requests_drive_effective_view_state() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	assert_true(sim.request_local_player_binoculars_toggle())
	var view := sim.get_local_player_view()
	assert_true(view.binoculars_requested)
	assert_true(view.binoculars_raised)
	assert_true(view.binoculars_view_active)
	assert_almost_eq(view.fov_h_deg, 20.0, 0.001)
	assert_eq(Vector2(view.binocular_yaw_offset_deg, view.binocular_pitch_offset_deg),
			Vector2.ZERO, "observing the view never latches the sway")
	# The rendered frame owns the latch [orig: Render_ProcessMainSceneFrame
	#  @0x5CA3E1..0x5CA3F3 -> Binoculars_RandomizeSwayOffsets].
	view = sim.present_local_player_view()
	var jitter := Vector2(
			view.binocular_yaw_offset_deg,
			view.binocular_pitch_offset_deg)
	assert_almost_eq(jitter.length(), 2.8125, 0.0001,
			"the rendered frame seeds the fixed 0x02000000-BAM displacement")
	assert_eq(sim.get_local_player_view().binocular_yaw_offset_deg,
			view.binocular_yaw_offset_deg, "later observations read the latched sway")

	sim.set_player_input(true, false, false, false, false, false, false)
	view = sim.get_local_player_view()
	assert_true(view.binoculars_requested,
			"movement suppresses rather than destroys raw intent")
	assert_false(view.binoculars_raised)
	assert_false(view.binoculars_view_active)
	sim.set_player_input(false, false, false, false, false, false, false)
	sim.set_local_player_debug_third_person(true)
	view = sim.get_local_player_view()
	assert_true(view.binoculars_raised,
			"third person retains the remote-visible body pose")
	assert_false(view.binoculars_view_active)
	sim.set_local_player_debug_third_person(false)
	sim.request_local_player_binoculars_toggle()
	assert_false(sim.get_local_player_view().binoculars_requested)

	assert_eq(sim.request_local_player_nvg_gain(99), 4)
	assert_eq(sim.get_local_player_view().nvg_gain, 4,
			"gain is adjustable while NVG is inactive")
	assert_true(sim.request_local_player_nvg_toggle())
	assert_true(sim.get_local_player_view().nvg_visible)
	sim.set_local_player_debug_third_person(true)
	view = sim.get_local_player_view()
	assert_true(view.nvg_active)
	assert_false(view.nvg_visible,
			"third person suppresses treatment without clearing NVG")


func test_start_with_nvg_reseeds_on_player_init() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_true(mission.set_header_flag(
			MissionData.ATTRIB_START_WITH_NVG_ON, true))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var view := sim.get_local_player_view()
	assert_true(view.nvg_active)
	assert_eq(view.nvg_gain, 0)

	assert_false(sim.request_local_player_nvg_toggle())
	assert_eq(sim.request_local_player_nvg_gain(3), 3)
	sim.respawn_local_player_loadout()
	view = sim.get_local_player_view()
	assert_true(view.nvg_active,
			"Player_InitPlayer reseeds the mission's StartWithNVGOn bit")
	assert_eq(view.nvg_gain, 0)


func test_nvg_inset_scope_drop_refusal_and_restore_latch() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var weapon := WeaponDef.new()
	weapon.name = "WPN_INSET_NVG_TEST"
	weapon.set_actions([WeaponActionRow.make("idle", 0, 0)])
	weapon.flags = 0x1
	weapon.flags2 = 0x200
	weapon.clipsize = 30
	weapon.startrounds = 60
	sim.set_local_player_weapon(weapon, {})
	sim.step()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(7):
		sim.step()
	assert_true(sim.get_local_player_view().scope_engaged)

	assert_true(sim.request_local_player_nvg_toggle())
	assert_false(sim.get_local_player_view().scope_engaged,
			"enabling NVG drops a settled Inset scope")
	for _i in range(7):
		sim.step()
	assert_false(sim.request_local_player_scope_toggle(),
			"Inset scope-up is refused while NVG remains active")
	sim.rebake_local_player_weapon(weapon, {})

	assert_false(sim.request_local_player_nvg_toggle())
	assert_true(sim.get_local_player_view().scope_engaged,
			"a render-only same-weapon rebake preserves the scope restore latch")
	# NVG-on only drops a promoted scope (Player_IsEquippedWeaponScoped
	# @0x4DCC80). Let the restored raise settle before creating another latch;
	# a direct mount retains a running interpolation and its engaged byte.
	for _i in range(7):
		sim.step()
	assert_almost_eq(sim.get_local_player_view().scope_fraction, 1.0, 0.001)

	assert_true(sim.request_local_player_nvg_toggle())
	assert_false(sim.get_local_player_view().scope_engaged,
			"the second NVG-on request drops the settled scope before the mount")
	sim.set_local_player_weapon(weapon, {})
	assert_false(sim.request_local_player_nvg_toggle())
	assert_false(sim.get_local_player_view().scope_engaged,
			"a real weapon mount invalidates the stale scope restore latch")


# The HUD waypoint track: the demo mission's BLUE route becomes the player track;
# a spawned local player latches waypoint 0 on the first tick and walking into the
# radius advances. [orig chain: NetPacket_WriteWorldStateLoad0x0F @0x502e41 list ->
# Player_UpdatePerFrame @0x4de5f7 advance; docs/interface/hud-re.md §Waypoint HUD]
func test_waypoint_hud_view_tracks_the_demo_route() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	var wp := sim.get_waypoint_hud_view()
	assert_true(wp.show, "waypoints visible by default")
	assert_eq(wp.count, 3, "the blue route's three markers")
	assert_eq(wp.current, -1, "no selection before a player tick")

	# Spawn the local player far from marker 0 (demo marker 0 = mission (100,0,0)
	# = Godot (100, 0, 0); radius 25). The first tick latches entry 0.
	assert_true(sim.spawn_local_player(Vector3(0, 0, 0), 0.0, 1))
	sim.step()
	wp = sim.get_waypoint_hud_view()
	assert_eq(wp.current, 0, "first tick latches waypoint 0")
	assert_eq(wp.number, 1, "1-based display number")
	assert_eq(wp.name_id, 1, "marker 0's authored name id")
	var pos := wp.position
	assert_almost_eq(pos.x, 100.0, 0.01, "marker 0 world X")

	# Teleport inside the 25 u radius (mission space; the player is AI index 2,
	# after the demo's two organics): the next tick advances to waypoint 1.
	sim.debug_set_entity_position(2, Vector3(95, 0, 0))
	sim.step()
	wp = sim.get_waypoint_hud_view()
	assert_eq(wp.current, 1, "proximity advance onto waypoint 1")

const ANIM_FIXTURES := "res://../fixtures/anim"


# (The duck-typed placer stubs are gone with S3b, and the placer argument
# with the boot-contract cleanup: every pose/collision source is the sim's
# own asset root, composed per test from the committed fixtures.)

func _anim_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path(ANIM_FIXTURES))
	return root


func _minimal_weapon(name: String, animadm: String) -> WeaponDef:
	var def := WeaponDef.new()
	def.name = name
	def.animadm = animadm
	return def


func _weapon_arm_pitch_deg(sim: Simulation) -> float:
	var overlay := sim.get_local_player_aim_overlay()
	var angles := overlay.segment_angles if overlay != null else PackedVector3Array()
	return float(angles[4].x) if angles.size() > 4 else 0.0


func _aim_verdict_sim(flags: int) -> Simulation:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def_1 := WeaponDef.new()
	def_1.name = "WPN_AIM_VERDICT"
	def_1.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("fire", 0, 0),
		WeaponActionRow.make("recoil", 0, 0),
		WeaponActionRow.make("reload", 8, 8),
		WeaponActionRow.make("scopeup", 0, 0),
		WeaponActionRow.make("scopedown", 0, 0),
	])
	def_1.flags = flags
	def_1.clipsize = 30
	def_1.startrounds = 60
	sim.set_local_player_weapon(def_1, {})
	return sim


func _aimed_shot_available(sim: Simulation) -> bool:
	return sim.get_local_player_weapon_state().aimed_shot_available


func _present_field_for_origin(sim: Simulation, kind: int, index: int,
		field: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_KIND]) == kind \
				and int(snapshot[base + Simulation.PF_INDEX]) == index:
			return int(snapshot[base + field])
	return -1


func _present_phase_for_origin(sim: Simulation, kind: int, index: int,
		channel: int) -> int:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_KIND]) == kind \
				and int(snapshot[base + Simulation.PF_INDEX]) == index:
			return Simulation.decode_present_part_anim_phase(
					snapshot, base, channel)
	return 0


func test_authoritative_cveh_snapshot_publishes_vehicle_motion_controls() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ITEM, 105007,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(placed)
	assert_true(md.set_entity_property_int(
			MissionData.KIND_ITEM, placed.index, "team", 2))
	var item_db := _vehicle_ctrl_item_db()
	assert_not_null(item_db)
	if item_db == null:
		return

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# The fixture def's tank graphic carries the one authored ctrlx25 point,
	# so the native extraction installs the controller seat the drive needs.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/tank.3di", "tank.3di")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([5007]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	sim.step()
	var index := placed.index
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_VEHICLE_MOTION_VALID), 1,
			"a resolved authority cveh row owns both registers at rest")
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_VEHICLE_STEERING), 0)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_VEHICLE_SPEED), 0)
	# The part-animation words ride the same valid bit and rest at zero
	# (Entity_CacheVehicleHUDStats publishes the rotor/wheel accumulators'
	# high words; an unoccupied, unmoved vehicle has none).
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_VEHICLE_ROTOR), 0)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_VEHICLE_WHEELS), 0)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_TEX_TEAM_VALID), 1,
			"a pool-1 sector-model row executes the retail TEX_TEAM writer")
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_TEX_TEAM), 2)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_ZONE_CTRL_VALID), 0,
			"a non-zone sector model does not synthesize the generic-zone writers")

	assert_true(sim.local_player_toggle_mount())
	sim.set_player_input(true, false, true, false, false, false, false)
	for _tick in range(40):
		sim.step()
	var steering := _present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_VEHICLE_STEERING)
	var speed := _present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_VEHICLE_SPEED)
	assert_ne(steering, 0,
			"the snapshot carries the zero-extended live steer high word")
	assert_gt(speed, 0,
			"the snapshot carries the live currentSpeed magnitude")
	assert_lte(speed, 0x10000,
			"the retail speed publisher saturates at its fixed-point endpoint")
	# Driving turns the wheels: the wheel phase integrates the speed word every
	# tick [orig: the +0x2B8 accumulate @0x48c4c5..0x48c4d0], and the snapshot
	# carries its high word for VEHICLE_WHEELS.
	assert_ne(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, index,
			Simulation.PF_VEHICLE_WHEELS), 0,
			"the snapshot carries the live wheel-phase high word once driven")


func test_selector_zero_installs_direct_air_and_simple_ground_traits() -> void:
	var item_db := _physicsless_air_item_db()
	assert_not_null(item_db)
	if item_db == null:
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var air_a := md.add_entity(
			MissionData.KIND_ITEM, 105008,
			Vector3(0, 0, 4), Vector3.ZERO)
	var air_b := md.add_entity(
			MissionData.KIND_ITEM, 105009,
			Vector3(5, 0, 4), Vector3.ZERO)
	var ground := md.add_entity(
			MissionData.KIND_ITEM, 105010,
			Vector3(10, 0, 0), Vector3.ZERO)
	var plane := md.add_entity(
			MissionData.KIND_ITEM, 105011,
			Vector3(15, 0, 4), Vector3.ZERO)
	assert_not_null(air_a)
	assert_not_null(air_b)
	assert_not_null(ground)
	assert_not_null(plane)

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	var air_a_card: EntityCard = sim.entity_card_by_net_id(air_a.bms_id)
	var air_b_card: EntityCard = sim.entity_card_by_net_id(air_b.bms_id)
	var ground_card: EntityCard = sim.entity_card_by_net_id(ground.bms_id)
	var plane_card: EntityCard = sim.entity_card_by_net_id(plane.bms_id)
	assert_eq(air_a_card.get_item_id(), 5008,
			"the public pool-1 probe resolves the first parsed definition")
	assert_eq(air_b_card.get_item_id(), 5009,
			"the public pool-1 probe resolves the second parsed definition")
	assert_eq(ground_card.get_item_id(), 5010,
			"the public pool-1 probe resolves the ground control")
	assert_eq(plane_card.get_item_id(), 5011,
			"the public pool-1 probe resolves the parsed cpln definition")
	assert_eq(air_a_card.get_vehicle_family(), 2,
			"plain chel installs the Helicopter prediction family without physics")
	assert_eq(air_b_card.get_vehicle_family(), 2,
			"case-folded fourcc chel installs the same air prediction family")
	assert_eq(ground_card.get_vehicle_family(), 0,
			"zero selects the simpler motor for the ground cveh family")
	assert_eq(plane_card.get_vehicle_family(), 3,
			"physicsless cpln installs the Plane prediction family")


func _mounted_npc_right_hand_verdict(seat_type: int) -> int:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var mount := md.add_entity(
			MissionData.KIND_ITEM, 101294,
			Vector3(0, 8, 0), Vector3.ZERO)
	var npc := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			_seat_fixture_spawn("res://../fixtures/threedi/synth/mount.3di", "Usegun", Vector3(0, 8, 0)), Vector3.ZERO)
	assert_not_null(mount)
	assert_not_null(npc)
	assert_true(md.set_entity_property_int(
			MissionData.KIND_ORGANIC, npc.index,
			"waypoint_id", 125))
	assert_true(md.set_entity_property_int(
			MissionData.KIND_ORGANIC, npc.index,
			"wp_number", mount.bms_id))
	# One-seat carrier per retail prefix: mount's authored Usegun row (bone 6),
	# byte-renamed for the sitex/ctrlx/drvrx variants.
	var renames := {1: "sitex00", 2: "ctrlx00", 5: "drvrx00"}
	var dir := _native_fixture_dir()
	var model_bytes := FileAccess.get_file_as_bytes(
			"res://../fixtures/threedi/synth/mount.3di")
	if renames.has(seat_type):
		model_bytes = _bytes_with_renamed_user_point(
				model_bytes, "Usegun", String(renames[seat_type]))
	_write_fixture_bytes(dir, "seatgun.3di", model_bytes)
	var item_db := _item_db_from_text(dir, """begin "Seat Verdict Gun"
  id 101294
  attrib: EWeap PlayerControl
  type object
  graphic seatgun
  phrase_set 6
end
""")
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries instead of a single step.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	var card: EntityCard = sim.entity_card_by_net_id(npc.bms_id)
	assert_true(card.is_mounted(), "seat predicate fixture must attach its occupant")
	assert_eq(card.get_mount_type(), seat_type, "fixture attaches the requested seat type")
	var verdict := _present_field_for_origin(
			sim, MissionData.KIND_ORGANIC, npc.index,
			Simulation.PF_RIGHT_HAND_COLLAPSED)
	return verdict


func test_mounted_npc_right_hand_predicate_excludes_passengers() -> void:
	# Retail parentSlot codes: sitex=1 keeps the hand/weapon, while ctrlx=2,
	# UseGun=3, and drvrx=5 zero BN17 for non-player organics.
	for row in [
		{"seat": 1, "collapsed": 0, "name": "Passenger"},
		{"seat": 2, "collapsed": 1, "name": "Controller"},
		{"seat": 3, "collapsed": 1, "name": "Gunner"},
		{"seat": 5, "collapsed": 1, "name": "Driver"},
	]:
		assert_eq(_mounted_npc_right_hand_verdict(int(row["seat"])),
				int(row["collapsed"]), "%s seat predicate" % row["name"])


func test_aim_overlay_exports_the_retail_authored_pitch_sign() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
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


func test_aimed_shot_verdict_uses_both_promoted_optic_predicates() -> void:
	# Player_IsOpticalViewVisible calls both helpers: Scoped is Flags bit 0, while the
	# misleadingly named second helper is simply Sighted bit 1 outside SWITCHFROM.
	# Both read the same post-ease promoted active bit. [orig: @0x4dcc80/@0x4dcd30]
	for case in [
		{"flags": 0x1, "expected": true, "name": "Scoped"},
		{"flags": 0x2, "expected": true, "name": "Sighted-only"},
	]:
		var sim := _aim_verdict_sim(int(case["flags"]))
		sim.step()
		assert_true(sim.request_local_player_scope_toggle())
		for _i in range(15):
			sim.step()
			assert_false(_aimed_shot_available(sim),
					"%s never promotes during the ease" % case["name"])
		# The current host order ticks the view after the body; the next body tick
		# observes the now-promoted endpoint. It may be one tick late, never early.
		sim.step()
		assert_eq(_aimed_shot_available(sim), bool(case["expected"]),
				"%s uses the correct aimed-shot predicate" % case["name"])
		if int(case["flags"]) == 0x2:
			sim.set_water_z(1.0)
			sim.set_player_input(true, false, false, false, false, false, false)
			sim.step()
			assert_true(_aimed_shot_available(sim),
					"promoted Sighted bypasses the movement/water checks")


func test_ordinary_aimed_shot_rejects_water_and_movement() -> void:
	var sim := _aim_verdict_sim(0x1)
	sim.step()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(16):
		sim.step()
	assert_true(_aimed_shot_available(sim), "settled stationary Scoped view is aimed")

	sim.set_water_z(1.0)
	sim.step()
	assert_false(_aimed_shot_available(sim),
			"Drowning/raw fixed source height rejects ordinary aimed fire")
	sim.set_water_z(0.0)
	sim.step()
	assert_true(_aimed_shot_available(sim), "leaving water restores aimed fire")


	# The normal Scoped move path also requests an unscope, but its public result
	# pins Player_IsOpticalViewVisible's MoveOrder&8 rejection end-to-end.
	sim = _aim_verdict_sim(0x1)
	sim.step()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(16):
		sim.step()
	assert_true(_aimed_shot_available(sim))
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_false(_aimed_shot_available(sim),
			"MoveOrder moving rejects an ordinary aimed shot")


func test_forcescoped_overrides_ordinary_gates_but_not_card_switch_reload() -> void:
	# ForceScoped overwrites the ordinary scope/movement/air/water verdict in
	# first person. The reload test sits earlier in Player_IsOpticalViewVisible and is
	# therefore still terminal. [orig: @0x5cf7c7 and @0x5cf845..0x5cf874]
	var sim := _aim_verdict_sim(0x20000001)
	sim.step()
	assert_true(_aimed_shot_available(sim),
			"ForceScoped is aimed even without an ordinary promoted scope")
	sim.set_water_z(1.0)
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_true(_aimed_shot_available(sim),
			"ForceScoped preserves its movement/water override")
	sim.set_water_z(0.0)
	sim.set_player_input(false, false, false, false, false, false, false)

	# Spend one round so the public reload input gate can enter action 4.
	sim.set_local_player_weapon_input(false, true, false)
	var spent_round := false
	for _i in range(12):
		sim.step()
		var state := sim.get_local_player_weapon_state()
		if state.clip == 29 and state.current_action == 0:
			spent_round = true
			break
	assert_true(spent_round, "fixture reached idle with a partial magazine")
	sim.set_local_player_weapon_input(false, false, true)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var reload_seen := false
	for _i in range(12):
		sim.step()
		if sim.get_local_player_weapon_state().current_action == 4:
			reload_seen = true
			sim.step() # aimed verdict samples the already-current reload action
			break
	assert_true(reload_seen, "fixture entered the card-switch reload action")
	assert_false(_aimed_shot_available(sim),
			"ForceScoped cannot bypass the earlier card-switch reload rejection")


func test_decoded_round_stance_uses_retail_animation_flags() -> void:
	# The decoded-round bridge consumes these 0x100/0x200 bits directly. Pin the
	# transition rows that the former hand-maintained list missed, plus
	# idle_mortar, which it incorrectly called crouched. Live IDA table reads:
	# 46=0x008, 169..171=0x18D, 172=0x28D.
	# [orig: g_AnimStateFlagsTable @0x8139E8]
	for row in [
		{"anim": 46, "flags": 0x008, "category": 2},
		{"anim": 169, "flags": 0x18D, "category": 1},
		{"anim": 170, "flags": 0x18D, "category": 1},
		{"anim": 171, "flags": 0x18D, "category": 1},
		{"anim": 172, "flags": 0x28D, "category": 0},
	]:
		var flags := int(Simulation.infantry_anim_flags(int(row["anim"])))
		assert_eq(flags, int(row["flags"]), "retail flags for anim %d" % row["anim"])
		var category := 0 if (flags & 0x200) != 0 else (1 if (flags & 0x100) != 0 else 2)
		assert_eq(category, int(row["category"]),
				"decoded recoil category for anim %d" % row["anim"])


func test_weapon_channel_keeps_own_phase_and_switch_identity_per_entity() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	sim.set_local_player_weapon(_minimal_weapon("WPN_A", "shared.adm"), {})
	sim.step()
	var state := sim.get_local_player_weapon_state()
	assert_eq(state.body_anim_key, "anim_idle",
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

func test_weapon_clip_variant_ring_rotates_bake_reads_and_plays() -> void:
	# Multi-clip .adm variant rings, end to end through the public binding: clip
	# lengths arrive as per-key VARIANT arrays; the bake consumes ONE ring entry per
	# 'auto' delay field (serve-then-advance), and every play consumes + latches the
	# served variant into the state dict. A both-auto reload over a 3-ring therefore
	# eats entries 2 and 1 at bake (a ring serves its last token first) — the FIRST
	# reload PLAY serves variant 0, the next serves 2 (the REVVY M4 "m4_1r" "m4_1r"
	# "m4_1r2" shape).
	# [orig: Anim_InitActions reads @0x5421c5/@0x5421d8 via Anim_GetDurationTicks
	#  @0x53ee10; AnimMap_PlayAnimBySlot @0x40bda0 latches at animState+68]
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := WeaponDef.new()
	def.name = "WPN_RING"
	def.animadm = "ring.adm"
	def.set_actions([
		WeaponActionRow.make("idle", 0, 0, "anim_wpn_idle"),
		WeaponActionRow.make("fire", 0, 0, "anim_wpn_fire"),
		WeaponActionRow.make("reload", -1, -1, "anim_wpn_reload"),
	])
	def.flags = 0
	def.clipsize = 30
	def.startrounds = 60
	sim.set_local_player_weapon(def, {
		"anim_wpn_idle": PackedFloat32Array([0.2]),
		"anim_wpn_fire": PackedFloat32Array([0.05]),
		"anim_wpn_reload": PackedFloat32Array([0.5, 1.0, 0.25]),
	})
	sim.step()
	var state := sim.get_local_player_weapon_state()
	assert_eq(state.anim_key, "anim_wpn_idle", "fresh slot idles")
	assert_eq(state.anim_variant, 0, "single-entry rings always serve 0")

	# Spend a round (letting the fire+recoil chain settle back to idle — the reload
	# dispatch gate refuses the edge mid-FIRE), then reload: the bake's two 'auto'
	# reads served entries 2 then 1, so the FIRST reload serves variant 0.
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(6):
		sim.step()
	assert_eq(sim.get_local_player_weapon_state().clip, 29, "one round spent")
	sim.set_local_player_weapon_input(false, false, true)
	var reload_variant := -1
	var first_reload_serial := -1
	for _i in range(90):
		sim.step()
		state = sim.get_local_player_weapon_state()
		if state.anim_key == "anim_wpn_reload":
			reload_variant = state.anim_variant
			first_reload_serial = state.play_serial
			break
	assert_eq(reload_variant, 0,
		"the first reload serves variant 0 — the both-auto bake consumed entries 2 and 1")

	# Let the reload finish (ds 17 + de 47 ticks and the transitions), spend another
	# round, reload again: the ring wrapped, so the play serves variant 2.
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
		if state.anim_key == "anim_wpn_reload" 				and state.play_serial != first_reload_serial:
			reload_variant = state.anim_variant
			break
	assert_eq(reload_variant, 2, "the second reload wraps the ring back to variant 2")


func test_weapon_event_batch_preserves_three_undrained_ticks() -> void:
	# Game_MainLoop catch-up presents once after N fixed ticks. The sim must retain
	# each tick's clip/begin/end payload in order, including its age at the drain.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := WeaponDef.new()
	def.name = "WPN_EVENT_BATCH"
	# The clip lengths below stand in for this ANIMADM's rings; a def with no
	# ANIMADM loads no anim table and plays no clip (net-re §5.62).
	def.animadm = "batch.adm"
	def.set_actions([
		WeaponActionRow.make("idle", 0, 0, "anim_wpn_idle"),
		WeaponActionRow.make("fire", 0, 0, "anim_wpn_fire", "FIRE_BEGIN", "FIRE_END"),
		WeaponActionRow.make("recoil", 0, 0, "anim_wpn_recoil", "RECOIL_BEGIN", "", "", "Effect_TestCas", "bcasing"),
	])
	def.flags = 0x100
	def.clipsize = 30
	def.startrounds = 60
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
			(events[0] as PlayerWeaponEvent).anim_key,
			(events[1] as PlayerWeaponEvent).anim_key,
			(events[2] as PlayerWeaponEvent).anim_key,
		], ["anim_wpn_fire", "anim_wpn_recoil", "anim_wpn_fire"])
		assert_eq([
			(events[0] as PlayerWeaponEvent).action_started,
			(events[1] as PlayerWeaponEvent).action_started,
			(events[2] as PlayerWeaponEvent).action_started,
		], [2, 3, 2])
		assert_eq([
			(events[0] as PlayerWeaponEvent).action_finished,
			(events[1] as PlayerWeaponEvent).action_finished,
			(events[2] as PlayerWeaponEvent).action_finished,
		], [2, -1, 2])
		assert_eq([
			(events[0] as PlayerWeaponEvent).age_ticks,
			(events[1] as PlayerWeaponEvent).age_ticks,
			(events[2] as PlayerWeaponEvent).age_ticks,
		], [2, 1, 0])
		assert_eq([
			(events[0] as PlayerWeaponEvent).action_soundset,
			(events[1] as PlayerWeaponEvent).action_soundset,
			(events[2] as PlayerWeaponEvent).action_soundset,
		], ["FIRE_BEGIN", "RECOIL_BEGIN", "FIRE_BEGIN"],
			"payloads are copied before a later tick or remount can overwrite them")
		assert_eq([
			(events[0] as PlayerWeaponEvent).action_end_soundset,
			(events[1] as PlayerWeaponEvent).action_end_soundset,
			(events[2] as PlayerWeaponEvent).action_end_soundset,
		], ["FIRE_END", "", "FIRE_END"])
		assert_eq((events[1] as PlayerWeaponEvent).action_effect, 3,
				"the recoil arbiter event survives the catch-up batch")
		assert_eq((events[1] as PlayerWeaponEvent).effect_particle,
				"Effect_TestCas")
		assert_eq((events[1] as PlayerWeaponEvent).effect_particle_userpoint,
				"bcasing")
	assert_true(sim.drain_local_player_weapon_events().is_empty(), "the drain is destructive")


func test_weapon_event_batch_snapshots_the_scope_settle_tick() -> void:
	# Retail promotes the view before weapon actions. Across one catch-up batch,
	# an auto-fire event before 15/15 remains unsuppressed while the later event
	# at 15/15 carries the settled gate.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := WeaponDef.new()
	def.name = "WPN_SCOPE_BATCH"
	def.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("fire", 0, 0, "", "", "", "", "Effect_TestMF", "muzzle1"),
		WeaponActionRow.make("recoil", 0, 0),
	])
	def.flags = 0x102 # Auto + Sighted
	def.clipsize = 30
	def.startrounds = 60
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
			func(event: PlayerWeaponEvent) -> bool: return event.action_started == 2)
	assert_eq(fire_events.size(), 2)
	if fire_events.size() == 2:
		assert_false((fire_events[0] as PlayerWeaponEvent).scope_settled,
				"the earlier catch-up tick still shows its muzzle")
		assert_true((fire_events[1] as PlayerWeaponEvent).scope_settled,
				"the 15/15 tick alone suppresses its muzzle")


func test_weapon_cycle_steps_the_scope_zoom_while_the_optical_view_is_up() -> void:
	# The weapon-cycle actions are dual-purpose: while the optical view is up
	# on a def whose scope_min_mag (the record default 2) differs from its
	# scope_max_mag, cycleweaponP (+1) and cycleweaponN (-1) step the slot
	# zoom by +2 / -2 in place of a cycle, clamped into [scope_min_mag,
	# scope_max_mag]; at the hip they cycle (engine:
	# runtime/world/local_player_view.h local_player_weapon_cycle_route,
	# Input_HandleActionBinding_0 cases 212/214 -> Player_AdjustWeaponElevation).
	# A Sighted def projects the slot zoom straight into the view fov (80 / zoom),
	# and a fresh slot starts at its seed, the floor for a one-value
	# scope_max_mag (runtime/world/weapon_inventory.h weapon_slot_initial_zoom).
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := WeaponDef.new()
	def.name = "WPN_SCOPE_STEP"
	def.set_actions([WeaponActionRow.make("idle", 0, 0)])
	def.flags = 0x01000902 # the M4 EOTech vector: Scoped + Sighted (the optical view)
	def.scope_max_mag = 10.0
	def.scope_min_mag = 2 # the record default a parsed row carries
	def.clipsize = 30
	def.startrounds = 60
	sim.set_local_player_weapon(def, {})
	sim.step()
	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(16):
		sim.step()
	assert_almost_eq(sim.get_local_player_view().scope_fraction, 1.0, 0.001,
			"the ADS ease settled before the zoom steps")
	assert_almost_eq(sim.get_local_player_view().fov_h_deg, 40.0, 0.001,
			"the fresh slot starts at its seed 2 (80 / 2), not the lazy maximum")
	# Four cycleweaponP (+1) presses walk the zoom up to scope_max_mag: +2 per
	# press from the seed, capped at the max (2 -> 4 -> 6 -> 8 -> 10).
	for _i in range(4):
		sim.request_local_player_weapon_cycle(1)
		sim.step()
	assert_almost_eq(sim.get_local_player_view().fov_h_deg, 8.0, 0.001,
			"the zoom sits at scope_max_mag 10 (80 / 10)")
	sim.request_local_player_weapon_cycle(-1)
	sim.step()
	assert_almost_eq(sim.get_local_player_view().fov_h_deg, 10.0, 0.001,
			"cycleweaponN (-1) steps the zoom down by 2 (80 / 8)")
	sim.request_local_player_weapon_cycle(1)
	sim.step()
	assert_almost_eq(sim.get_local_player_view().fov_h_deg, 8.0, 0.001,
			"cycleweaponP (+1) steps it back up by 2, capped at scope_max_mag")
	assert_almost_eq(sim.get_local_player_view().scope_fraction, 1.0, 0.001,
			"no zoom step ever cycled the weapon (the optical view stayed up)")


func test_nocardswitch_controls_settled_sights_card_for_sighted_weapon() -> void:
	# Retail-derived flag vectors: M4 EOTech is Sighted and has no NoCardSwitch;
	# Remington is Sighted plus NoCardSwitch. The original's suppression predicate
	# exempts ForceScoped. Parser-to-HUD coverage lives in hud_overlay_test.gd.
	for case in [
		{"name": "WPN_M4AUTO_EOTECH", "flags": 0x01000902, "expected_card": true},
		{"name": "WPN_RemmingtonSG", "flags": 0x02000002, "expected_card": false},
		{"name": "WPN_SCOPED_CONTROL", "flags": 0x1, "expected_card": true},
		{"name": "WPN_FORCE_SCOPED_OVERRIDE", "flags": 0x22000002, "expected_card": true},
	]:
		var md := MissionData.new()
		assert_eq(md.create_default(), OK)
		var sim := Simulation.new()
		assert_true(sim.load_from_mission_data(md))
		assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
		var def_4 := WeaponDef.new()
		def_4.name = String(case["name"])
		def_4.set_actions([WeaponActionRow.make("idle", 0, 0)])
		def_4.flags = int(case["flags"])
		def_4.clipsize = 30
		def_4.startrounds = 60
		sim.set_local_player_weapon(def_4, {})
		sim.step()
		# ForceScoped is promoted by the mount itself @0x4DFB31; the toggle
		# refuses its promoted scope @0x4DF12D instead of raising it again.
		var forced := (def_4.flags & 0x20000000) != 0
		assert_eq(sim.request_local_player_scope_toggle(), not forced,
				"only an ordinary optic needs a scope-up request after mounting")
		for _i in range(15):
			sim.step()
		var view := sim.get_local_player_view()
		assert_almost_eq(view.scope_fraction, 1.0, 0.001,
			"the ADS ease settled before checking the card switch")
		assert_eq(view.scope_card_active, bool(case["expected_card"]),
			"Scoped/Sighted and NoCardSwitch select the card for %s" % case["name"])


func test_reload_during_scope_raise_does_not_stash_an_unpromoted_scope() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def_5 := WeaponDef.new()
	def_5.name = "WPN_SCOPE_RELOAD"
	def_5.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("fire", 0, 0),
		WeaponActionRow.make("recoil", 0, 0),
		WeaponActionRow.make("reload", 1, 1),
	])
	def_5.flags = 0x2
	def_5.clipsize = 30
	def_5.startrounds = 60
	sim.set_local_player_weapon(def_5, {})
	sim.step()
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(6):
		sim.step()
	assert_eq(sim.get_local_player_weapon_state().clip, 29)
	assert_true(sim.request_local_player_scope_toggle())
	sim.step()
	assert_lt(sim.get_local_player_view().scope_fraction, 1.0)
	var before := sim.get_local_player_weapon_state().unscope_serial
	sim.set_local_player_weapon_input(false, false, true)
	for _i in range(3):
		sim.step()
	assert_eq(sim.get_local_player_weapon_state().unscope_serial, before)
	assert_true(sim.get_local_player_view().scope_engaged)


func test_weapon_event_batch_does_not_cross_lifecycle_boundaries() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := WeaponDef.new()
	def.name = "WPN_EVENT_LIFECYCLE"
	def.set_actions([WeaponActionRow.make("idle", 0, 0, "anim_wpn_idle")])
	var clips := {"anim_wpn_idle": 0.1}

	sim.set_local_player_weapon(def, clips)
	sim.step()
	var before_rebake := sim.get_local_player_weapon_state()
	# The FP model resolve explicitly rebakes the SAME weapon once its viewmodel
	# loads. Queued presentation and the live action slot survive that late bind.
	sim.rebake_local_player_weapon(def, clips)
	var after_rebake := sim.get_local_player_weapon_state()
	assert_eq(after_rebake.current_action, before_rebake.current_action)
	assert_eq(after_rebake.next_action, before_rebake.next_action)
	assert_eq(after_rebake.play_serial,
		before_rebake.play_serial)
	assert_false(sim.drain_local_player_weapon_events().is_empty(),
		"an explicit same-weapon rebake preserves queued presentation")
	sim.step()
	# A real mount is a new epoch even when the def name is unchanged.
	sim.set_local_player_weapon(def, clips)
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"a same-name real mount discards the previous epoch's presentation")
	var remounted := sim.get_local_player_weapon_state()
	assert_eq(remounted.current_action, 0)
	assert_eq(remounted.next_action, 0)
	assert_eq(remounted.play_serial, 0)
	sim.step()
	# A different-weapon mount has the same epoch boundary.
	var def_b := def.copy()
	def_b.name = "WPN_EVENT_LIFECYCLE_B"
	sim.set_local_player_weapon(def_b, clips)
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"a new-weapon mount discards the previous weapon's queued presentation")
	sim.step()
	sim.clear_local_player_weapon()
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"clear discards the unmounted weapon's queued presentation")
	sim.set_local_player_weapon(def, clips)
	sim.step()
	sim.reset_session()
	assert_true(sim.drain_local_player_weapon_events().is_empty(),
		"restart cannot age a pre-rewind event across the logic-tick reset")


func test_restart_clears_powerthrow_charge_and_input_latches() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var def := WeaponDef.new()
	def.name = "WPN_RESTART_POWERTHROW"
	def.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("fire", 0, 0),
		WeaponActionRow.make("recoil", 0, 0),
	])
	def.flags = 1 << 31
	def.clipsize = 1
	def.startrounds = 0
	sim.set_local_player_weapon(def, {})
	sim.step() # advance off tick zero so the idle sentinel cannot mask the windup
	sim.set_local_player_weapon_input(true, true, false)
	for _tick in range(5):
		sim.step()
	var wound := sim.get_local_player_weapon_state()
	assert_true(wound.windup_active)
	assert_gt(wound.windup_held_ticks, 0)

	sim.reset_session()
	var rewound := sim.get_local_player_weapon_state()
	assert_false(rewound.windup_active)
	assert_eq(rewound.windup_held_ticks, 0)
	var fired_before := rewound.fired_serial
	var rounds_before := rewound.round_ring_count
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.step() # stale held input must not recreate the windup
	assert_false(sim.get_local_player_weapon_state().windup_active)
	sim.set_local_player_weapon_input(false, false, false)
	for _tick in range(6):
		sim.step()
	var settled := sim.get_local_player_weapon_state()
	assert_eq(settled.fired_serial, fired_before)
	assert_eq(settled.round_ring_count, rounds_before)


func test_entities_walk_their_route() -> void:
	# Soldiers are anim-driven [orig: Entity_UpdateInfantryAI @0x4b9910]: their motion
	# comes from .bad root-motion clips resolved through a model's .adm. Without a clip
	# set they hold and stand; with one they walk the route.
	var sim := Simulation.new()
	sim.build_demo_mission()
	_spawn_fixture_human(sim)
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

func test_infantry_anim_map_failure_paths() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_eq(int(sim.set_infantry_anim_map(null, "soldier.adm")), 0, "null root -> 0 clips")
	assert_eq(int(sim.set_infantry_anim_map(_anim_root(), "missing.adm")), 0, "absent .adm -> 0 clips")
	assert_eq(sim.get_infantry_clip_count(), 0, "failed load leaves no stale clip set")
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	assert_eq(int(sim.set_infantry_anim_map(_anim_root(), "missing.adm")), 0)
	assert_eq(sim.get_infantry_clip_count(), 0,
			"rebuilding without a per-model resolver clears the previous default")


func test_infantry_anim_map_without_default_keeps_model_specific_clips() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_infantry_adm_ids(_anim_root(), item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var player_ai_index := -1
	var local_handle := sim.get_local_player_wire_handle()
	for ai_index in range(sim.get_entity_count()):
		if sim.get_entity_wire_handle(ai_index) == local_handle:
			player_ai_index = ai_index
			break
	assert_gte(player_ai_index, 0)
	assert_eq(sim.entity_card_by_ai_index(0).get_adm_name(), "soldier.adm",
			"the demo soldier uses the configured default before rebuilding")
	assert_eq(int(sim.set_infantry_anim_map(_anim_root(), "missing.adm")), 0,
			"the return value reports clips from the requested default only")
	assert_eq(sim.entity_card_by_ai_index(player_ai_index).get_adm_name(), "US01.adm",
			"the player's own map is resolved again after rebuilding")
	assert_eq(sim.entity_card_by_ai_index(0).get_adm_name(), "",
			"the demo soldier cannot inherit the player's map as a fallback")
	assert_eq(sim.get_infantry_clip_count(), 4,
			"the rebuilt registry starts with US01's four clips, not the stale default")
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_idle",
			"US01 resolves the requested gait through its own idle clip")


func test_restart_rebinds_baseline_player_to_own_adm() -> void:
	# The listen host's player is captured in AiSystem's baseline before the
	# MissionRoot per-entity ADM sweep. Stop/Restart replaces the live AI rows
	# with that baseline at the same count, so a count-only late-spawn resolver
	# must explicitly repopulate the restored rows.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.has_local_player(), "listen host player exists in the restart baseline")
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	sim.resolve_infantry_adm_ids(_anim_root(), item_db)
	var player_ai_index := -1
	var local_handle := sim.get_local_player_wire_handle()
	for ai_index in range(sim.get_entity_count()):
		if sim.get_entity_wire_handle(ai_index) == local_handle:
			player_ai_index = ai_index
			break
	assert_gte(player_ai_index, 0)
	assert_eq(sim.entity_card_by_ai_index(player_ai_index).get_adm_name(),
			"US01.adm", "the live host player owns its graphic ADM")
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_idle",
			"US01 lacks the requested gait and resolves through its own idle clip")

	sim.reset_session()
	assert_eq(sim.entity_card_by_ai_index(player_ai_index).get_adm_name(),
			"US01.adm", "restart immediately repopulates the restored baseline row")
	sim.set_player_input(true, false, false, false, false, false, false)
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_idle",
			"restart repopulates US01 instead of silently retaining default soldier.adm")


func test_load_from_editor_mission_data() -> void:
	# The editor-integration path: promote a live MissionData (what the mission editor holds),
	# not a file or the synthetic demo. KIND_ORGANIC = 3.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK, "empty in-memory mission created")
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md), "promoted the editor's live mission")
	assert_eq(sim.get_brain_count(), 2, "both organics got AI brains")
	assert_eq(sim.get_entity_kind(0), 3, "entity 0 maps back to KIND_ORGANIC")

# The mounted view hands an authored items.def ID to the HUD's SID lookup.
# A runtime type (1294) cannot find the definition (101294); the live retail
# join used to draw no panel even though the mount itself succeeded.
func test_vehicle_panel_resolves_authored_item_id_after_seat_selection() -> void:
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/carrier.3di", "carrier.3di")
	var item_db := _item_db_from_text(dir, """begin "Panel carrier"
  id 101294
  sid carrier_panel
  type vehicle
  graphic carrier
  hp 3000
  attrib: PlayerControl
end
""")
	TestFs.write_text(self, dir.path_join("hudpos.def"), """VEHICLE_HUD
  sid carrier_panel
  interface carrier_panel.tga
  driver 16,196
  seats 4,38,196,60,196,82,196,104,196
VEHICLE_END
""")
	var layout := HudPos.new()
	assert_eq(layout.load(dir.path_join("hudpos.def")), OK)
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(MissionData.KIND_ITEM, 101294, Vector3.ZERO, Vector3.ZERO))
	var sim := Simulation.new()
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	# Two units off the carrier, facing it: its seats sit inside the scan cone.
	assert_true(sim.spawn_local_player(Vector3(-2, 0, 0), 90.0, 1))
	MountLook.face(self, sim, Vector3.ZERO)
	assert_true(sim.local_player_toggle_mount())
	# Pick a different seat first so key 1 exercises the driver's request.
	sim.local_player_select_seat(1)
	assert_true(sim.local_player_select_seat(0))
	var view := sim.get_vehicle_panel_view()
	assert_true(view.shown)
	assert_eq(view.item_id, 101294, "the HUD receives the definition ID, not wire type 1294")
	var block := layout.get_vehicle_hud(item_db.get_sid(view.item_id))
	assert_not_null(block, "a confirmed driver resolves the authored vehicle panel")


func test_item_seat_specs_mount_command_125_spawn() -> void:
	# carrier authors one ctrlx13 point plus four sitexNN points: the native
	# extraction supplies the seat table the Dictionary seam used to fake.
	var fixture_dir := _native_fixture_dir()
	_copy_fixture(fixture_dir, "res://../fixtures/threedi/synth/carrier.3di", "carrier.3di")
	var item_db := _item_db_from_text(fixture_dir, """begin "Drivable Transport Truck"
  id 101294
  type vehicle
  graphic carrier
  attrib: PlayerControl
  physics 1
  player_speed 60
  acceleration 10
  deceleration 20
  turn_rate 45
  turn_rate2 30
  sound_profile SP_Transport
end

begin "Driver"
  id 102072
  type person
end
""")
	var sndprof_path := fixture_dir.path_join("SndProf.def")
	TestFs.write_text(self, fixture_dir.path_join("SndProf.def"), """begin "SP_Transport"
  soundloop_1 V_TRUCK_ILP .8 1.2
end
""")
	var suv := ObjectData.new()
	assert_eq(suv.open_file(fixture_dir.path_join("carrier.3di")), OK)
	var ctrl_point := Vector3.INF
	var passenger_point := Vector3.INF
	for point_index in range(suv.get_user_point_count()):
		var info := suv.get_user_point_info(point_index)
		match info.name:
			"ctrlx13":
				ctrl_point = info.position
			"sitex00d":
				passenger_point = info.position
	assert_true(ctrl_point.is_finite(), "carrier authors its ctrlx13 point")
	assert_true(passenger_point.is_finite(), "carrier authors its sitex00d point")

	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(MissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(MissionData.KIND_ORGANIC, 102072, Vector3(11, 0, 0), Vector3.ZERO)
	# A second command-125 rider: with the controller seat claimed it takes the
	# first passenger row (sitex00d), whose authored direction faces backward —
	# the yaw-offset carry witness.
	var rider_point := MissionObjectPlacer.entity_transform(
			Vector3(10, 0, 0), Vector3.ZERO) * passenger_point
	var rider := md.add_entity(MissionData.KIND_ORGANIC, 102072,
			Vector3(rider_point.x, -rider_point.z, rider_point.y), Vector3.ZERO)
	assert_not_null(vehicle)
	assert_not_null(soldier)
	assert_not_null(rider)
	for organic in [soldier, rider]:
		assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, organic.index, "waypoint_id", 125))
		assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, organic.index, "wp_number", vehicle.bms_id))

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	_install_native_seat_table(sim, fixture_dir, item_db, PackedInt32Array([1294]))
	sim.set_sound_profiles(FileAccess.get_file_as_bytes(sndprof_path))
	assert_true(sim.load_from_mission_data(md), "loaded command-125 mission with seat specs")
	sim.resolve_item_traits(item_db)
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries instead of a single step.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	var started: MissionEffect = null
	for effect_v in sim.drain_effects():
		var effect: MissionEffect = effect_v
		if effect.kind == "vehicle_control_started":
			started = effect
			break
	assert_not_null(started,
			"command-125 controller mount emits the occupied-item lifecycle edge")
	var expected_vehicle_handle := -1
	var carrier_position := Vector3.INF
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for base in range(0, snapshot.size(), stride):
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == 1294:
			expected_vehicle_handle = int(
					snapshot[base + Simulation.PF_WIRE_HANDLE])
			carrier_position = Vector3(snapshot[base + Simulation.PF_POS_X],
					snapshot[base + Simulation.PF_POS_Y], snapshot[base + Simulation.PF_POS_Z])
			break
	assert_gte(expected_vehicle_handle, 0)
	var idle: SoundEmitterRow = null
	for emitter_v in sim.drain_sound_emitters():
		var emitter: SoundEmitterRow = emitter_v
		if emitter.lane == 0 and emitter.soundset == "V_TRUCK_ILP":
			idle = emitter
			break
	assert_not_null(idle,
			"an NPC control-seat occupant keeps the truck idle emitter alive without a local player")
	if idle == null:
		return
	assert_gt(idle.source_spawn_id, 0,
			"the emitter key carries the registry-lifetime identity")
	assert_eq(idle.handle, expected_vehicle_handle)
	assert_eq(idle.source_bms_id, vehicle.bms_id)
	assert_eq(idle.lane, 0)
	assert_eq(idle.lifetime, 30)
	assert_eq(idle.emitted_tick, int(sim.get_logic_tick()),
			"catch-up transport retains the producing world tick")
	assert_eq(idle.pitch_q16, 0x10000)
	assert_eq(idle.volume_q8_8, 0xFFFF)
	assert_false(idle.source_only)
	assert_eq(idle.slot, 0)
	assert_eq(idle.soundset, "V_TRUCK_ILP")
	assert_lt(idle.pos.distance_to(carrier_position), 0.001,
			"the emitter tracks the carrier across the boarding ticks")
	assert_eq(started.wire_handle, expected_vehicle_handle,
			"binding names the packed identity used by dynamic presentation")
	assert_eq(started.d, expected_vehicle_handle,
			"the generic effect payload retains the same packed identity")
	# Both command-125 boarders walk in and claim their seat on the staggered
	# infantry think, so which of the two reaches the controller seat first is
	# decided by the witnessed stagger rather than by authored order (the
	# promote-time mount shortcut that fixed the order is gone). Identify the
	# crew by the seat each holds; every seat expectation below is unchanged.
	# [orig: tickCounter = current_tick + 36 * entity[31]]
	var soldier_idx := _organic_ai_index_with_mount_type(sim, 2)
	assert_true(soldier_idx >= 0, "found the soldier's AI row")
	var pos := sim.get_entity_position(soldier_idx)
	# The mounted origin is the model's authored ctrlx13 point through the
	# placer's entity transform — the same identity the render side applies.
	var expected_ctrl := MissionObjectPlacer.entity_transform(
			Vector3(carrier_position.x, -carrier_position.z, carrier_position.y),
			Vector3.ZERO) * ctrl_point
	assert_lt(pos.distance_to(expected_ctrl), 0.001,
		"command-125 soldier uses the IDA-priority ctrlx seat, converted to Godot axes")
	assert_almost_eq(sim.get_entity_yaw_deg(soldier_idx), 0.0, 0.01,
		"the forward-facing ctrlx13 point carries a zero yaw offset")
	# The mounted anim state (89 = anim_sit_13) is asserted via the debug card below; the present
	# snapshot is the listen-server ClientState now (covered by listen_server_test).
	var card: EntityCard = sim.entity_card_by_ai_index(soldier_idx)
	assert_true(card.is_mounted(), "debug card marks mounted occupants")
	assert_eq(card.get_mount_target_net_id(), vehicle.bms_id)
	assert_eq(card.get_mount_seat(), 0, "ctrlx seat was selected by original priority")
	assert_eq(card.get_mount_type(), 2, "seat type is ctrlx/controller")
	assert_eq(card.get_mount_seat_bone(), 1, "the 1-based USRP row of ctrlx13")
	assert_eq(card.get_mount_seat_pose_index(), 13)
	assert_eq(card.get_mount_seat_source_name(), "ctrlx13")
	assert_true(card.get_mount_seat_local().is_equal_approx(
			Vector3(-ctrl_point.x, ctrl_point.z, ctrl_point.y)),
			"seat local is the authored point in the mission seat frame")
	assert_eq(card.get_mount_seat_yaw_offset(), 0)
	var target_seats: Array = card.get_mount_target_seats()
	assert_eq(target_seats.size(), 5, "debug card carries every target seat candidate")
	assert_eq((target_seats[0] as EntityCardSeat).get_source_name(), "ctrlx13")
	assert_eq((target_seats[0] as EntityCardSeat).get_type(), 2)
	assert_eq((target_seats[0] as EntityCardSeat).get_pose_index(), 13)
	assert_eq((target_seats[0] as EntityCardSeat).get_retail_slot(), 8)
	assert_eq((target_seats[1] as EntityCardSeat).get_source_name(), "sitex00d")
	assert_eq((target_seats[1] as EntityCardSeat).get_type(), 1)
	assert_eq((target_seats[1] as EntityCardSeat).get_retail_slot(), 0)
	assert_eq(card.get_anim_state(), 89)
	assert_eq(card.get_anim_key(), "anim_sit_13")

	# The second rider found the controller claimed and took the first
	# passenger row; sitex00d faces backward, so the seat's yaw offset
	# drives the carried BODY. The NPC's live gaze chases independently.
	var rider_idx := -1
	for ai_index in range(sim.get_entity_count()):
		if ai_index == soldier_idx:
			continue
		var row: EntityCard = sim.entity_card_by_ai_index(ai_index)
		if row == null:
			break
		if row.get_pool() == 0:
			rider_idx = ai_index
			break
	assert_gte(rider_idx, 0, "found the rider's AI row")
	var rider_card: EntityCard = sim.entity_card_by_ai_index(rider_idx)
	assert_true(rider_card.is_mounted())
	assert_eq(rider_card.get_mount_type(), 1, "the rider fell back to sitex00d")
	assert_eq(rider_card.get_mount_seat_source_name(), "sitex00d")
	assert_eq(absi(rider_card.get_mount_seat_yaw_offset()), 180,
			"the backward-facing passenger point extracts a half-turn offset")
	var mounted_rows := sim.get_present_snapshot()
	var rider_body_yaw := NAN
	for base in range(0, mounted_rows.size(), Simulation.PF_STRIDE):
		if int(mounted_rows[base + Simulation.PF_WIRE_HANDLE]) == sim.get_entity_wire_handle(rider_idx):
			rider_body_yaw = mounted_rows[base + Simulation.PF_AIM_BODY_YAW_DEG]
			break
	assert_false(is_nan(rider_body_yaw), "the passenger publishes its carried body frame")
	assert_almost_eq(absf(wrapf(rider_body_yaw, -180.0, 180.0)),
			180.0, 0.01, "the backward seat carries the body yaw offset")
	assert_lte(absf(wrapf(sim.get_entity_yaw_deg(rider_idx) - rider_body_yaw, -180.0, 180.0)),
			90.01, "independent passenger gaze stays within the mounted body arc")
	var expected_rider := MissionObjectPlacer.entity_transform(
			Vector3(carrier_position.x, -carrier_position.z, carrier_position.y),
			Vector3.ZERO) * passenger_point
	assert_lt(sim.get_entity_position(rider_idx).distance_to(expected_rider), 0.001)
	assert_eq(rider_card.get_anim_state(), 76)
	assert_eq(rider_card.get_anim_key(), "anim_sit")



# The floating attach labels [orig: HUD_DrawVehicleSeatAndArmoryLabels @0x5a3290
# selection half]: free seats in the 4.0 u radius label with exactly one nearest
# highlight; the unarmed local player sees every candidate; the armory zone flag is
# absent here so seat mode applies and no armory labels appear.
func test_attach_labels_seats() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(MissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	assert_not_null(vehicle)
	var sim := Simulation.new()
	# mount byte-renamed: heat -> a sitex00 passenger beside the authored
	# Usegun, BCasing -> an armory1 anchor behind the def's Armory attrib.
	var dir := _native_fixture_dir()
	var model_bytes := FileAccess.get_file_as_bytes(
			"res://../fixtures/threedi/synth/mount.3di")
	model_bytes = _bytes_with_renamed_user_point(model_bytes, "heat", "sitex00")
	model_bytes = _bytes_with_renamed_user_point(model_bytes, "BCasing", "armory1")
	_write_fixture_bytes(dir, "labelgun.3di", model_bytes)
	var item_db := _item_db_from_text(dir, """begin "Labels Gun"
  id 101294
  type object
  attrib: Armory
  graphic labelgun
  primary_weapon WPN_EMPLCD50
end
""")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md), "loaded the labels mission")
	assert_true(sim.spawn_local_player(Vector3(12, 0, 0), 0.0, 1), "spawned the local player")
	var hud := _attach_overlay(sim)
	assert_eq(hud.get_attach_label_count(), 2, "both free seats label inside 4.0 u (armory points stay out of seat mode)")
	assert_gte(hud.get_attach_label_selected(), 0, "exactly the scan winner is highlighted")
	var texts := _attach_label_texts(sim)
	assert_false(texts.has("!UseArmory"), "no armory labels out of the zone")
	# WPN_EMPLCD50 is not in a loaded weapon table here -> the key stays absent
	# and the HUD falls to the STROVER_USEGUN default; sit + UseGun seats both report.
	assert_true(texts.has("!sit") and texts.has("!UseGun"), "sit + UseGun seats both reported")


func test_attach_labels_share_complete_can_fire_verdict() -> void:
	# The label pass consumes the same live Player_IsOpticalViewVisible verdict as the
	# body/HUD spread row: owning an equipped slot alone is not sufficient.
	# [orig: Player_IsOpticalViewVisible @0x5cf780; label branch @0x5a32df]
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(10, 0, 0), Vector3.ZERO))
	assert_not_null(md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(14, 0, 0), Vector3.ZERO))
	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	var model_bytes := FileAccess.get_file_as_bytes(
			"res://../fixtures/threedi/synth/mount.3di")
	_write_fixture_bytes(dir, "labelgun.3di", model_bytes)
	var item_db := _item_db_from_text(dir, """begin "Labels Gun"
  id 101294
  type object
  graphic labelgun
  primary_weapon WPN_EMPLCD50
end
""")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3(12, 0, 0), 0.0, 1))
	# Facing the +x gun: the nearest scan wants one candidate inside its cone;
	# the label list itself has no cone, only its 4.0 u radius.
	MountLook.face(self, sim, Vector3(14, 0, 0))
	var def_6 := WeaponDef.new()
	def_6.name = "WPN_LABEL_SCOPE"
	def_6.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("scopeup", 0, 0),
		WeaponActionRow.make("scopedown", 0, 0),
	])
	def_6.flags = 0x1
	def_6.clipsize = 30
	def_6.startrounds = 60
	sim.set_local_player_weapon(def_6, {})
	sim.step()
	assert_eq(_attach_label_count(sim), 2,
			"an unraised Scoped weapon cannot fire, so both candidates label")

	assert_true(sim.request_local_player_scope_toggle())
	for _i in range(16):
		sim.step()
	assert_true(_aimed_shot_available(sim))
	assert_eq(_attach_label_count(sim), 1,
			"settled first-person aim restricts labels to the nearest candidate")

	sim.set_local_player_debug_third_person(true)
	assert_eq(_attach_label_count(sim), 2,
			"the live camera gate applies before another simulation tick")
	sim.set_local_player_debug_third_person(false)
	sim.set_water_z(1.0)
	sim.step()
	assert_false(_aimed_shot_available(sim))
	assert_eq(_attach_label_count(sim), 2,
			"an underwater ordinary scope exposes every attach candidate")


func test_attach_labels_hide_occupied_and_out_of_range() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(MissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(MissionData.KIND_ORGANIC, 102072, _seat_fixture_spawn("res://../fixtures/threedi/synth/mount.3di", "Usegun", Vector3(10, 0, 0)), Vector3.ZERO)
	assert_not_null(vehicle)
	assert_not_null(soldier)
	# Command-125 boards the soldier into the best seat; that seat must not label.
	assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, soldier.index, "waypoint_id", 125))
	assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, soldier.index, "wp_number", vehicle.bms_id))
	# Same team as the local player: a live ENEMY occupant would reject the whole
	# vehicle instead [orig: Vehicle_HasEnemyOccupant @0x4359f0].
	assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, soldier.index, "team", 1))
	var sim := Simulation.new()
	# Two-seat carrier: mount renamed to one ctrlx00 (the command-125 target)
	# plus one sitex00 passenger.
	var dir := _native_fixture_dir()
	var model_bytes := FileAccess.get_file_as_bytes(
			"res://../fixtures/threedi/synth/mount.3di")
	model_bytes = _bytes_with_renamed_user_point(model_bytes, "Usegun", "ctrlx00")
	model_bytes = _bytes_with_renamed_user_point(model_bytes, "heat", "sitex00")
	_write_fixture_bytes(dir, "ctrlgun.3di", model_bytes)
	var item_db := _item_db_from_text(dir, """begin "Occupied Labels Gun"
  id 101294
  attrib: EWeap PlayerControl
  type object
  graphic ctrlgun
end
""")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	# The local player stands beside the carrier from the start: it is also
	# the human the entity update needs to run the boarding think.
	_spawn_fixture_human(sim, Vector3(12, 0, 0))
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries before reading the mounted state.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	assert_true(sim.entity_card_by_net_id(soldier.bms_id).is_mounted(),
			"label fixture must occupy its controller seat")
	var texts := _attach_label_texts(sim)
	assert_eq(texts.size(), 1, "the AI-occupied ctrlx seat never labels [orig: @0x5a348f]")
	assert_eq(texts[0] if texts.size() == 1 else "", "!sit", "the free sitex remains")


func test_attach_labels_empty_out_of_range() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(MissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	assert_not_null(vehicle)
	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_write_fixture_bytes(dir, "sitgun.3di", _bytes_with_renamed_user_point(
			FileAccess.get_file_as_bytes("res://../fixtures/threedi/synth/mount.3di"),
			"Usegun", "sitex00"))
	var item_db := _item_db_from_text(dir, """begin "One Seat Gun"
  id 101294
  type object
  graphic sitgun
end
""")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3(40, 0, 0), 0.0, 1), "spawned far away")
	assert_eq(_attach_label_count(sim), 0,
		"outside the 4.0 u gate the nearest scan fails and no labels emit [orig: @0x5a32e2]")


# The projected attach labels the overlay fills from the sim (HudOverlay's
# read seams): a camera at the local player looking down +X keeps every
# candidate in front of the near plane.
func _attach_label_count(sim: Simulation) -> int:
	return _attach_overlay(sim).get_attach_label_count()


func _attach_label_texts(sim: Simulation) -> Array[String]:
	var hud := _attach_overlay(sim)
	var texts: Array[String] = []
	for i in range(hud.get_attach_label_count()):
		texts.append(hud.get_attach_label_text(i))
	return texts


func _attach_overlay(sim: Simulation) -> HudOverlay:
	var hud := HudOverlay.new()
	autofree(hud)
	var camera := Transform3D(Basis.looking_at(Vector3.RIGHT), Vector3(-2.0, 0.0, 0.0))
	hud.set_attach_labels(camera, Projection.create_perspective(70.0, 1.0, 0.05, 4000.0),
			null, sim)
	return hud


# Drivable items (control-seat specs) attach AI brains at promote since the vehicle
# pass, so organics no longer sit at AI index 0 — resolve the first pool-0 row.
func _organic_ai_index_with_mount_type(sim: Simulation, mount_type: int) -> int:
	for i in 64:
		var d: EntityCard = sim.entity_card_by_ai_index(i)
		if d == null:
			break
		if d.get_pool() == 0 and d.get_mount_type() == mount_type:
			return i
	return -1

func _first_organic_ai_index(sim: Simulation) -> int:
	for i in 64:
		var d: EntityCard = sim.entity_card_by_ai_index(i)
		if d == null:
			break
		if d.get_pool() == 0:
			return i
	return -1

func test_mounted_seat_local_matches_rotated_vehicle_userpoint() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(MissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3(0, -90, 0))
	var soldier := md.add_entity(MissionData.KIND_ORGANIC, 102072, _seat_fixture_spawn("res://../fixtures/threedi/synth/carrier.3di", "ctrlx13", Vector3(10, 0, 0), Vector3(0, -90, 0)), Vector3.ZERO)
	assert_not_null(vehicle)
	assert_not_null(soldier)
	assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, soldier.index, "waypoint_id", 125))
	assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, soldier.index, "wp_number", vehicle.bms_id))

	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/carrier.3di", "carrier.3di")
	var item_db := _item_db_from_text(dir, """begin "Rotated SUV"
  id 101294
  attrib: PlayerControl
  type vehicle
  graphic carrier
end
""")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md), "loaded rotated command-125 mount")
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries before reading the mounted state.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	var soldier_idx := _first_organic_ai_index(sim)
	assert_true(soldier_idx >= 0, "found the soldier's AI row")
	var pos := sim.get_entity_position(soldier_idx)
	# Command 125 selects carrier's authored ctrlx13; the mounted origin is that
	# model point through the same rotated entity transform the placer renders.
	var suv := ObjectData.new()
	assert_eq(suv.open_file(dir.path_join("carrier.3di")), OK)
	var ctrl_point := Vector3.INF
	for point_index in range(suv.get_user_point_count()):
		var info := suv.get_user_point_info(point_index)
		if info.name == "ctrlx13":
			ctrl_point = info.position
	assert_true(ctrl_point.is_finite())
	var expected := MissionObjectPlacer.entity_transform(
			Vector3(10, 0, 0), Vector3(0, -90, 0)) * ctrl_point
	assert_lt(pos.distance_to(expected), 0.001,
		"mounted seat local follows the same rotated side as the selected model userpoint")


# The USE-ITEM toggle's weapon-busy gate at the sim binding [orig: @0x436958-0x436977]:
# a fire in flight swallows the toggle; back at idle the same toggle mounts.
func test_local_player_toggle_mount_weapon_busy_gate() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var vehicle := md.add_entity(MissionData.KIND_ITEM, 101294, Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(vehicle)
	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_write_fixture_bytes(dir, "sitgun.3di", _bytes_with_renamed_user_point(
			FileAccess.get_file_as_bytes("res://../fixtures/threedi/synth/mount.3di"),
			"Usegun", "sitex00"))
	var item_db := _item_db_from_text(dir, """begin "One Seat Truck"
  id 101294
  type object
  graphic sitgun
end
""")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md), "loaded the one-truck mission")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(self, sim, Vector3(2, 0, 0))
	var def_7 := WeaponDef.new()
	def_7.name = "WPN_GATE"
	def_7.animadm = "gate.adm"
	def_7.set_actions([
		WeaponActionRow.make("idle", 0, 0, "anim_wpn_idle"),
		WeaponActionRow.make("fire", 0, 0, "anim_wpn_fire"),
	])
	def_7.flags = 0
	def_7.clipsize = 30
	def_7.startrounds = 60
	sim.set_local_player_weapon(def_7, {
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
	var card: EntityCard = sim.entity_card_by_net_id(vehicle.bms_id)
	var seats: Array = card.get_seats()
	assert_true(seats.size() == 1 and (seats[0] as EntityCardSeat).is_occupied(),
		"the scan took the truck's one sitex seat")


# Local UseGun follows Player_MountWeaponSlot rather than the nonlocal direct slot
# assignment: holster the personal slot, commit the parent's embedded MountSlot, and
# restore the preserved personal slot through the same switch path on detach.
# [orig: Entity_AttachToUseGunSlot @0x546c25..0x546c3d;
# Player_MountWeaponSlot @0x4dfa40; switch commits @0x543475/@0x543539;
# Entity_DetachFromVehicle restore @0x43565f]
func test_unarmed_offline_local_usegun_toggle_is_rejected() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO))
	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, _fixture_items_text().replace(
			"id 101294", "id 101294\n  graphic mount"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(self, sim, Vector3(2, 0, 0))
	sim.clear_local_player_weapon()
	assert_false(sim.local_player_toggle_mount(),
			"retail rejects offline player UseGun attach without EquippedSlot")


func test_unarmed_offline_local_ordinary_seat_toggle_is_allowed() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO))
	var sim := Simulation.new()
	# A passenger-only carrier (no primary weapon): mount's Usegun row renamed
	# to sitex00 in a minimal authored def.
	var dir := _native_fixture_dir()
	_write_fixture_bytes(dir, "sitgun.3di", _bytes_with_renamed_user_point(
			FileAccess.get_file_as_bytes("res://../fixtures/threedi/synth/mount.3di"),
			"Usegun", "sitex00"))
	var item_db := _item_db_from_text(dir, """begin "Unarmed Seat Carrier"
  id 101294
  type object
  graphic sitgun
end
""")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(self, sim, Vector3(2, 0, 0))
	sim.clear_local_player_weapon()
	assert_true(sim.local_player_toggle_mount(),
			"the null EquippedSlot gate is UseGun-only, not a generic seat gate")


func test_command_125_usegun_mount_renders_emplaced_pose() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(MissionData.KIND_ITEM, 101294, Vector3(10, 0, 0), Vector3.ZERO)
	var soldier := md.add_entity(MissionData.KIND_ORGANIC, 102072, _seat_fixture_spawn("res://../fixtures/threedi/synth/mount.3di", "Usegun", Vector3(10, 0, 0)), Vector3.ZERO)
	assert_not_null(gun)
	assert_not_null(soldier)
	assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, soldier.index, "waypoint_id", 125))
	assert_true(md.set_entity_property_int(MissionData.KIND_ORGANIC, soldier.index, "wp_number", gun.bms_id))

	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, """begin "Config3 UseGun"
  id 101294
  attrib: EWeap
  type object
  graphic mount
  phrase_set 3
end
""")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md), "loaded command-125 UseGun mount")
	sim.resolve_item_traits(item_db)
	_spawn_fixture_human(sim)
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries before reading the mounted state.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	# The mounted anim state (67 = anim_emplaced) is asserted via the debug card below; the present
	# snapshot is the listen-server ClientState now (covered by listen_server_test).
	var card: EntityCard = sim.entity_card_by_ai_index(0)
	assert_true(card.is_mounted(), "debug card marks UseGun occupant mounted")
	assert_eq(card.get_mount_type(), 3, "seat type is UseGun/gunner")
	assert_true(card.is_mount_config_valid())
	assert_eq(card.get_mount_config(), 3)
	assert_true(card.is_mount_target_config_valid())
	assert_eq(card.get_mount_target_config(), 3)
	assert_eq(card.get_anim_state(), 67)
	assert_eq(card.get_anim_key(), "anim_emplaced")


# (P7: the 3 no-net AI-pool present-snapshot tests were deleted — the present is now the listen-
#  server ClientState, covered by listen_server_test; the editor no-net preview is retired.)



# A camera 12 units south of the fixture spawn (24, 0, -12), looking at it
# down -Z; the default IDENTITY camera stands off to its side.
const _ANCHOR_CAMERA := Transform3D(Basis(), Vector3(24.0, 2.0, 0.0))


func _occlusion_frame(sim: Simulation, camera: Transform3D = Transform3D.IDENTITY) -> void:
	sim.run_occlusion_frame(camera, 90.0, 1.0, 640.0, 500.0, -100.0, false)


func test_foliage_mask_anchors_track_local_player_stance() -> void:
	# The hide-in-grass selection: only infantry with a stance bit set
	# ((net_stance_bits & 0x3) != 0) and no groundEntity anchor the distant
	# MODEL/depth-mask foliage tier [orig: Terrain_RenderSectorEntitiesBySide
	# @ 0x5c7dc2/0x5c7ded (MoveOrder & 0x300), groundEntity gate
	# @ 0x5c7dd5..0x5c7df7]. The local player's SELECT latches are the stance
	# writer [orig: Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd]. The
	# occlusion frame selects the anchors.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	var spawn := Vector3(24.0, 0.0, -12.0)
	assert_true(sim.spawn_local_player(spawn, 0.0, 1))
	# The player's visual item (105310, graphic US01) over the synthetic
	# person model gives the body its entity+0 bound radius, which the
	# collector's person leg projects.
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/person.3di", "US01.3di")
	_native_asset_root(sim, dir)
	assert_gt(sim.resolve_collision_instances(item_db), 0)
	sim.occlusion_init_mission()

	sim.step()
	_occlusion_frame(sim, _ANCHOR_CAMERA)
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"a STANDING infantry entity never anchors the silhouette tier")

	assert_true(sim.request_local_player_stance(1))  # crouch (SELECT 169)
	sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"the anchors are the last occlusion frame's, not the tick's")
	# The collector gates the local player like any other person: turned
	# away, the crouched body is not collected and anchors nothing
	# [orig: Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8c60, no
	# local-player exception].
	_occlusion_frame(sim, Transform3D(Basis(Vector3.UP, PI), _ANCHOR_CAMERA.origin))
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"a crouched local player outside the view is not collected")
	_occlusion_frame(sim, _ANCHOR_CAMERA)
	var crouched: PackedVector3Array = sim.get_foliage_mask_anchor_positions()
	assert_eq(crouched.size(), 1, "the crouched local player anchors the silhouette tier")
	if crouched.size() == 1:
		var player := sim.get_local_player_position()
		assert_lt(Vector2(crouched[0].x, crouched[0].z).distance_to(Vector2(player.x, player.z)), 0.1,
			"the anchor is the entity's own Godot-space ground position")

	assert_true(sim.request_local_player_stance(2))  # prone (SELECT 170)
	sim.step()
	_occlusion_frame(sim, _ANCHOR_CAMERA)
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 1,
		"prone anchors too - both MoveOrder stance bits gate the tier")

	assert_true(sim.request_local_player_stance(0))  # stand (SELECT 172)
	sim.step()
	_occlusion_frame(sim, _ANCHOR_CAMERA)
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"standing back up empties the anchor list")


func test_foliage_mask_anchors_ignore_standing_npcs() -> void:
	# Routed organics keep net_stance_bits 0 (the mirror only writes the LOCAL
	# player's SELECT latches; NPC stance never reaches the wire bits here), so
	# a demo mission full of standing walkers produces no anchors - matching
	# retail, where placed objects and standing soldiers leave MoveOrder's
	# stance bits clear [orig: the 0x5c7dc2 (flags & 0x300) reject].
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_eq(sim.get_entity_count(), 2, "the demo mission has AI infantry to reject")
	sim.occlusion_init_mission()
	for _i in range(4):
		sim.step()
		_occlusion_frame(sim)
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"standing NPCs never anchor the hide-in-grass tier")


func test_transport_uses_session_state() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_playing(), "starts paused")
	assert_true(sim.resume_session())
	assert_true(sim.is_playing(), "resume enters the Running session state")

# Standalone fixtures have no host roster publishing humans. Keep the actual
# WAC/BMS clock gate open through an authored write while testing live events.
func _admit_standalone_script_ticks(sim: Simulation) -> void:
	assert_true(sim.compile_and_set_wac(PackedStringArray(["set(ticks,-1)"])))


func test_empty_script_startup_closes_admission_without_humans() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 6, 0, 77))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	for _tick in range(32):
		sim.step()
	assert_false(sim.has_event_fired(0), "empty startup advanced the shared clock to one")
	assert_true(sim.drain_effects().is_empty(), "no humans: normal BMS events remain gated")
	_admit_standalone_script_ticks(sim)
	for _tick in range(80):
		sim.step()
	assert_true(sim.has_event_fired(0), "the authored clock write admits both schedulers")
	assert_eq(sim.drain_effects().size(), 1)


func test_bms_event_fires_through_binding() -> void:
	# The capability consolidation adds: a BMS event evaluates through the SAME binding that
	# runs the AI (the editor preview used to walk AI but never fire events). Build an
	# unconditional OutputText(77) event, tick once, and confirm the host-presentation effect
	# drains out of the shared World EffectLog.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 6, 0, 77))

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md), "loaded the scripted mission")
	_admit_standalone_script_ticks(sim)
	assert_eq(sim.get_event_count(), 1, "one BMS event registered in the runtime")

	# A normal event's first processing pass is the 16th tick (the faithful quarter-list
	# round-robin cadence; see tests/mission/event_runtime_test.cpp).
	for _i in range(16):
		sim.step()
	var effects := sim.drain_effects()
	assert_eq(effects.size(), 1, "one presentation effect drained")
	assert_eq((effects[0] as MissionEffect).kind, "text", "OutputText -> text effect")
	assert_eq((effects[0] as MissionEffect).a, 77, "carries the string id")
	assert_true(sim.has_event_fired(0), "the event is marked fired")
	assert_true(sim.drain_effects().is_empty(), "drain cleared the log")


# --- Read-only introspection (C8: the debug overlay's data feeds) ---

func test_logic_tick_advances_per_step_and_rewinds_on_restart() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	# The pre-mission pass already ran one tick at load, so pin DELTAS, never
	# absolutes.
	var t0: int = sim.get_logic_tick()
	sim.step()
	assert_eq(sim.get_logic_tick(), t0 + 1, "one step advances the logic tick by one")
	sim.step()
	sim.step()
	assert_eq(sim.get_logic_tick(), t0 + 3)
	sim.reset_session()
	assert_eq(sim.get_logic_tick(), t0, "Stop rewinds the clock to the play-start baseline")


func test_variable_snapshots_are_bank_sized_and_track_writes() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	var mission: PackedInt32Array = sim.get_mission_variables_snapshot()
	var globals: PackedInt32Array = sim.get_global_variables_snapshot()
	var music: PackedInt32Array = sim.get_music_variables_snapshot()
	assert_eq(mission.size(), 512, "V0..V511")
	assert_eq(globals.size(), 256, "G0..G255")
	assert_eq(music.size(), 0, "no active music context was bound at compilation")

	sim.set_mission_variable(5, 42)
	sim.set_global_variable(3, -7)
	assert_eq(sim.get_mission_variables_snapshot()[5], 42, "snapshot reflects V writes")
	assert_eq(sim.get_global_variables_snapshot()[3], -7, "snapshot reflects G writes")
	assert_eq(sim.get_global_variable(3), -7, "the scalar G getter agrees")


func test_fired_events_snapshot_matches_scalar() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 6, 0, 77))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_admit_standalone_script_ticks(sim)

	var before: PackedByteArray = sim.get_fired_events_snapshot()
	assert_eq(before.size(), sim.get_event_count(), "one flag per event")
	assert_eq(int(before[0]), 0, "nothing fired before the first quarter pass")

	for _i in range(16):
		sim.step()
	var after: PackedByteArray = sim.get_fired_events_snapshot()
	assert_eq(int(after[0]), 1, "the fired flag sets")
	assert_eq(int(after[0]) == 1, sim.has_event_fired(0), "bulk and scalar reads agree")


func test_entity_debug_card_carries_named_scalars() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	var card: EntityCard = sim.entity_card_by_ai_index(0)
	assert_not_null(card, "a live entity has a card")
	assert_true(card.has_ai() and card.has_world(), "both card halves resolve")
	assert_eq(card.get_state(), 0, "an organic does not enter a vehicle brain state")
	assert_eq(card.get_state_name(), "?", "state zero has no named vehicle behavior")
	assert_eq(card.get_position(), sim.get_entity_position(0),
			"position matches the scalar getter")
	assert_almost_eq(float(card.get_yaw_deg()), sim.get_entity_yaw_deg(0), 0.01)
	assert_eq(card.get_net_id(), sim.get_entity_net_id(0))
	assert_eq(card.get_kind(), sim.get_entity_kind(0))
	assert_true(card.is_alive())
	assert_eq(card.get_health(), card.get_ai_health(),
		"both health mirrors ride the card (they diverge under damage)")
	assert_true(card.is_infantry(), "demo organics route through the infantry motor")

	assert_null(sim.entity_card_by_ai_index(-1), "invalid index reads no card")
	assert_null(sim.entity_card_by_ai_index(999))


func test_debug_entity_mutations_report_missing_invalid_and_success() -> void:
	var sim := Simulation.new()
	assert_eq(int(sim.debug_set_entity_health(0, 37)), ERR_INVALID_PARAMETER,
			"health reports that entity zero is absent from the empty AI pool")
	assert_eq(int(sim.debug_set_entity_position(0, Vector3.ONE)),
			ERR_INVALID_PARAMETER,
			"position reports that entity zero is absent from the empty AI pool")
	assert_eq(int(sim.debug_teleport_local_player(Vector3.ONE, 15.0, -5.0)),
			ERR_UNAVAILABLE, "teleport reports that the local player is absent")

	sim.build_demo_mission()
	var missing_index := sim.get_entity_count()
	assert_eq(int(sim.debug_set_entity_health(-1, 37)), ERR_INVALID_PARAMETER)
	assert_eq(int(sim.debug_set_entity_health(missing_index, 37)), ERR_INVALID_PARAMETER)
	assert_eq(int(sim.debug_set_entity_position(-1, Vector3.ONE)),
			ERR_INVALID_PARAMETER)
	assert_eq(int(sim.debug_set_entity_position(missing_index, Vector3.ONE)),
			ERR_INVALID_PARAMETER)
	assert_eq(int(sim.debug_teleport_local_player(Vector3.ONE, 15.0, -5.0)),
			ERR_UNAVAILABLE, "a live world without a local player is still unavailable")

	assert_eq(int(sim.debug_set_entity_health(0, 37)), OK)
	var card: EntityCard = sim.entity_card_by_ai_index(0)
	assert_eq(card.get_health(), 37)
	assert_eq(card.get_ai_health(), 37)

	# The per-entity items.def attrib override by wire handle (the F3 checkbox
	# and game_debug seam): range checks first, then both words land on the
	# registry row and read back through the card and its inspect JSON.
	var handle := card.get_wire_handle()
	assert_eq(int(sim.debug_set_entity_item_attrib(0xFFFF, 0, 0)), ERR_INVALID_PARAMETER,
			"the invalid handle sentinel is refused")
	assert_eq(int(sim.debug_set_entity_item_attrib(handle, 0x100000000, 0)),
			ERR_INVALID_PARAMETER, "a word above 32 bits is refused")
	assert_eq(int(sim.debug_set_entity_item_attrib(handle, 0, -1)), ERR_INVALID_PARAMETER,
			"a negative word is refused")
	assert_eq(int(sim.debug_set_entity_item_attrib(0x0FFE, 0, 0)), ERR_DOES_NOT_EXIST,
			"an empty slot resolves no row")
	assert_eq(int(sim.debug_set_entity_item_attrib(handle, 0x800100, 0x2000)), OK)
	var overridden: EntityCard = sim.entity_card(handle)
	assert_eq(int(overridden.get_item_attrib()), 0x800100, "the attrib word landed")
	assert_eq(int(overridden.get_item_attrib2()), 0x2000, "the attrib2 word landed")
	var json: Dictionary = overridden.to_json_value()
	assert_eq(int(json.get("item_attrib", -1)), 0x800100,
			"the AI card's inspect JSON carries the registry row's attrib word")
	assert_true(json.has("item_attrib2") and json.has("item_name") and json.has("health_max"),
			"...and the other item facts")
	var rows: Array = sim.entity_directory()
	assert_gt(rows.size(), 0)
	var row := rows[0] as EntityRow
	assert_eq(row.get_world_position(), row.get_mission_position().x * Vector3.RIGHT
			+ row.get_mission_position().z * Vector3.UP - row.get_mission_position().y * Vector3.BACK,
			"the row's world position is the one engine axis map of its mission position")
	assert_eq(typeof(row.get_item_name()), TYPE_STRING, "the row carries the item name")

	var entity_mission_position := Vector3(6.0, 5.0, 7.0)
	assert_eq(int(sim.debug_set_entity_position(0, entity_mission_position)), OK)
	assert_lt(sim.get_entity_position(0).distance_to(Vector3(6.0, 7.0, -5.0)),
			0.001, "the successful move mutates the AI position mirror")
	var world_card: EntityCard = sim.entity_card_by_net_id(card.get_net_id())
	var world_mission_position: Vector3 = world_card.get_mission_position()
	assert_lt(world_mission_position.distance_to(entity_mission_position), 0.001,
			"the successful move mutates the registry position mirror")

	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var player_mission_position := Vector3(11.0, 3.0, -4.0)
	assert_eq(int(sim.debug_teleport_local_player(
			player_mission_position, 123.0, -17.0)), OK)
	assert_lt(sim.get_local_player_position().distance_to(Vector3(11.0, -4.0, -3.0)),
			0.001, "the successful teleport mutates the authoritative player position")
	assert_almost_eq(sim.get_local_player_yaw_deg(), 123.0, 0.01)
	assert_almost_eq(sim.get_local_player_pitch_deg(), -17.0, 0.01)


func test_effect_state_lookup_uses_the_live_registry_not_the_ai_pool() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_BUILDING, 0, Vector3(3, 4, 5), Vector3(10, 20, 30))
	assert_not_null(placed)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 0,
			"a building has a registry slot but no AI-pool row")
	var state: PackedVector3Array = sim.get_entity_effect_state_for_ssn(ssn)
	assert_eq(state.size(), Simulation.EFFECT_STATE_COUNT,
			"the SSN query reaches non-AI registry entities")
	if state.size() == Simulation.EFFECT_STATE_COUNT:
		assert_eq(state[Simulation.EFFECT_STATE_POSITION], Vector3(3, 5, -4),
				"effect position uses the canonical mission-to-Godot frame")
		assert_eq(state[Simulation.EFFECT_STATE_ROTATION_DEG], Vector3(10, 20, 30),
				"effect orientation remains mission Euler degrees for the host adapter")
	assert_true(sim.get_entity_effect_state_for_ssn(0).is_empty(), "SSN zero is invalid")
	assert_true(sim.get_entity_effect_state_for_ssn(65536).is_empty(),
			"out-of-range SSNs must not wrap onto a different registry entity")


func test_collision_backed_building_without_oobj_keeps_batch_visibility() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		MissionData.KIND_BUILDING, 102001, Vector3(0, 20, 0), Vector3.ZERO)
	assert_not_null(placed)

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)
	var data := ObjectData.new()
	assert_eq(data.open_file(
		ProjectSettings.globalize_path("res://../fixtures/threedi/synth/house.3di")), OK)
	assert_true(data.has_collision())
	assert_false(data.has_occlusion(), "fixture must exercise collision without OOBJ")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/house.3di", "GuardTwr1.3di")
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	sim.occlusion_init_mission()
	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 640.0, 500.0, -100.0, false)
	var visibility: PackedInt64Array = sim.get_building_visibility_changes()
	assert_eq(visibility.size(), 3, "collision-backed no-OOBJ building stays in the host batch")
	if visibility.size() == 3:
		assert_eq(int(visibility[0]), placed.bms_id)
		var packed := int(visibility[1])
		# Outdoors a windowless batch member draws its exterior section only
		# (retail Terrain_BuildSectorVisibilityMasks @0x5c87d7..0x5c8830: the
		# +0x2CD flag clear -> mask 1), the same word every batched building
		# carries; the fixture item authors no forced sections.
		assert_eq(Simulation.building_visibility_mask(packed), 1,
			"the raw retail mask reaches the no-OOBJ building too")
		assert_eq(int(visibility[2]), 0, "no forced sections")
		assert_true(Simulation.building_visibility_visible(packed), "the in-frustum building is visible")


func test_building_feed_carries_the_def_forced_sections() -> void:
	# The part draw ORs the def's forced sections over the raw verdict: items.def
	# first_door N stores N - 1 at itemDef +0x891, and every section at or above
	# it draws whatever the raw mask says (retail ItemDef_ParseProperty
	# @0x49F7BA..0x49F7DE; Terrain_RenderSectorModels @0x5c5d7c..0x5c5da8).
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		MissionData.KIND_BUILDING, 102001, Vector3(0, 20, 0), Vector3.ZERO)
	assert_not_null(placed)
	var dir := _native_fixture_dir()
	var item_db := _item_db_from_text(dir, """begin "Guard Tower door"
  id 102001
  type building
  graphic GuardTwr1
  num_doors 1
  First_Door 3
end
""" + _fixture_items_text())
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_copy_fixture(dir, "res://../fixtures/threedi/synth/house.3di", "GuardTwr1.3di")
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	sim.occlusion_init_mission()
	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 640.0, 500.0, -100.0, false)
	var visibility: PackedInt64Array = sim.get_building_visibility_changes()
	assert_eq(visibility.size(), 3)
	if visibility.size() == 3:
		assert_eq(int(visibility[0]), placed.bms_id)
		assert_eq(Simulation.building_visibility_mask(int(visibility[1])), 1,
				"the raw word stays the outdoor exterior bit")
		assert_eq(int(visibility[2]), 0xFFFFFFFC,
				"first_door 3 forces sections 2 and up (-1 << 2)")


func test_occlusion_delta_calls_emit_changes_only() -> void:
	# The diff-based apply contract behind GameWorld's occlusion frame: the
	# first delta call after a frame emits the full verdict state, an unchanged
	# frame emits nothing, and reset_occlusion_apply_baseline() re-arms the
	# full emission (the A/B seam and host cache resets rely on it).
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		MissionData.KIND_BUILDING, 102001, Vector3(0, 20, 0), Vector3.ZERO)
	assert_not_null(placed)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/house.3di", "GuardTwr1.3di")
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	sim.occlusion_init_mission()
	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 640.0, 500.0, -100.0, false)

	var first: PackedInt64Array = sim.get_building_visibility_changes()
	assert_eq(first.size(), 3, "the first delta call emits the building's state")
	assert_eq(sim.get_render_culled_changes(), PackedInt32Array([0, 0]),
			"no entities to cull in this mission")

	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 640.0, 500.0, -100.0, false)
	assert_eq(sim.get_building_visibility_changes().size(), 0,
			"an unchanged frame emits no building deltas")
	assert_eq(sim.get_render_culled_changes(), PackedInt32Array([0, 0]),
			"an unchanged frame emits no culled deltas")

	sim.reset_occlusion_apply_baseline()
	assert_eq(sim.get_building_visibility_changes(), first,
			"a baseline reset re-arms the full emission")
	assert_true(bool(sim.entity_present_visible(placed.bms_id)),
			"a live placed building reads as present-visible")
	assert_true(bool(sim.entity_present_visible(424242)),
			"an unknown bms id defaults visible (never blocks a show)")


func test_face_only_cfac_model_attaches_for_projectile_raycast() -> void:
	# Retail collision construction and the projectile face walker do not
	# require BVOL. The synthetic bird is the face-only witness (18 CFAC over
	# nine bone sections, 0 BVOL);
	# rejecting it here silently degrades authored bullet geometry to a sphere.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		MissionData.KIND_BUILDING, 102001, Vector3(0, 200, 0), Vector3.ZERO)
	assert_not_null(placed)

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path("res://../fixtures/def/items.def")), OK)

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/bird.3di", "GuardTwr1.3di")
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1,
		"face-only CFAC remains a real collision model")
	var entities: Array = sim.get_hitbox_debug().entities
	assert_eq(entities.size(), 1)
	if entities.size() == 1:
		assert_eq((entities[0] as HitboxDebugEntity).face_total, 18,
			"all authored bird faces reach the projectile walker")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true(sim.get_hitbox_debug().entities.is_empty(),
		"the heavier non-organic mesh view retains its local 80-unit range")


func test_panm_liveness_is_scoped_to_the_active_transform_family() -> void:
	# Ten shed variants, each with its LOD0 rows replaced by one row whose flags
	# and track controls are the case (fixtures/README.md):
	# every track control 0, the case's live tracks 0x10.
	var cases := [
		["panm_live_01_spinner", true, "spinner uses raw coefficients"],
		["panm_live_02_view3", true, "view rotation type 3 is evaluated"],
		["panm_live_03_view4", true, "view rotation type 4 is evaluated"],
		["panm_live_04_rotz", true, "rotation family samples rotation tracks"],
		["panm_inert_05_rot_scalex", false,
				"rotation ignores an unrelated live scale track"],
		["panm_inert_06_uniform_scaley", false,
				"uniform scale type samples only scale_x"],
		["panm_live_07_uniform_scalex", true, "uniform scale type samples scale_x"],
		["panm_live_08_axis_scaley", true, "axis scale samples all three scale tracks"],
		["panm_live_09_translation", true, "translation flag samples the translation track"],
		["panm_inert_10_rotrev", false,
				"rotation-reversed without a rotation family is inert"],
	]
	for case in cases:
		var data := ObjectData.new()
		assert_eq(data.open_file(ProjectSettings.globalize_path(
				SYN_PANM_LIVENESS_DIR + String(case[0]) + ".3di")), OK)
		assert_eq(data.get_part_anim_count(0), 1,
				"%s carries exactly one LOD0 row" % case[0])
		assert_eq(data.has_live_panm_for_lod(0), bool(case[1]), String(case[2]))


func test_collision_uses_effective_lod0_and_never_first_live_lod() -> void:
	# The pump jack with an inert local LOD0 row and one live sine rotation on
	# LOD1 (fixtures/README.md).
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
		SYN_PMP_LOD0_INERT_LOD1_LIVE)), OK)
	var lod_count := data.get_lod_count()
	assert_gt(lod_count, 1, "fixture needs a second visual LOD")
	if lod_count <= 1:
		return

	# A nonempty local block wins even when inert; model-level live rows must
	# not leak through it for canonical LOD0.
	assert_eq(data.get_part_anim_count(0), 1)
	assert_false(data.has_live_panm_for_lod(0))
	assert_eq(Array(data.get_effective_panm_targets(0)), [0],
		"inert local row suppresses the model-level fallback")

	# Only LOD1 is live. Visual de-batching may see it, but retail Generic
	# collision always uses canonical LOD0 COBJ ordinals.
	assert_eq(data.get_part_anim_count(1), 1)
	assert_true(data.has_live_panm_for_lod(1))
	assert_eq(data.get_live_panm_lod(), 1)

	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
		MissionData.KIND_BUILDING, 102001,
		Vector3.ZERO, Vector3.ZERO))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		"res://../fixtures/def/items.def")), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	var dir := _native_fixture_dir()
	_copy_fixture(dir, SYN_PMP_LOD0_INERT_LOD1_LIVE, "GuardTwr1.3di")
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	sim.debug_set_panm_time_ms(0)
	var before: PackedVector3Array = (sim.get_hitbox_debug().entities[0] as HitboxDebugEntity).tris
	sim.debug_set_panm_time_ms(640)
	var after: PackedVector3Array = (sim.get_hitbox_debug().entities[0] as HitboxDebugEntity).tris
	assert_eq(after, before, "LOD1 PANM never transforms model-level COBJ")


# A placed item owns an AI brain when its row carries the AIData attrib and a
# brain-class ai_function (Entity_SpawnFromBMSRecord's attrib test, then the
# class init); the part-anim channels live in that brain.
const PART_ANIM_CARRIER_ROW := """
begin "Part Anim Carrier"
  id 105012
  type vehicle
  graphic StaticCrate1
  sid partanimcarrier
  ai_function cveh
  render_function cveh
  move_function cveh
  attrib: AIData neutral
  hp 3000
end
"""


func test_listen_snapshot_exports_authoritative_part_anim_channels() -> void:
	# PLAYPARTANIM stores only each channel's direction and rate in the item's
	# brain (Entity_ApplyCommand's case 34 passes over an item without an AI
	# slot), and no retail code integrates the phase: the sweep integrator
	# Entity_UpdateSuspensionBounce has no caller, so the phase keeps the brain
	# allocator's zero fill and HUD_CacheEntityDisplayInfo publishes that held
	# value on both channels. The static crate carries no brain condition and
	# publishes neither channel.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ITEM, 105012, Vector3.ZERO, Vector3.ZERO)
	var crate := md.add_entity(
			MissionData.KIND_ITEM, 105004, Vector3(8, 0, 0), Vector3.ZERO)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 21, 34, ssn, 1, 1, 65536))
	assert_true(md.add_event_action(0, 21, 34, crate.bms_id, 1, 1, 65536))
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# Both graphics are the committed armory model with its Armory point
	# byte-renamed to ctrlx00 (the fixture defs' graphic).
	var dir := _native_fixture_dir()
	_write_fixture_bytes(dir, "StaticCrate1.3di", _bytes_with_renamed_user_point(
			FileAccess.get_file_as_bytes("res://../fixtures/threedi/synth/armory.3di"),
			"Armory", "ctrlx00"))
	var item_db := _item_db_from_text(dir, _fixture_items_text() + PART_ANIM_CARRIER_ROW)
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([5004, 5012]))
	assert_true(sim.load_from_mission_data(md))
	for _tick in range(80):
		sim.step()
	assert_true(sim.has_event_fired(0), "the PLAYPARTANIM event ran")
	for channel in [1, 2]:
		var active_field: int = Simulation.PF_ACTIVE1 + (channel - 1) * 2
		assert_eq(_present_field_for_origin(
				sim, MissionData.KIND_ITEM, placed.index, active_field), 1,
				"SP/host presentation publishes the brain's channel %d (high word zero)" % channel)
		assert_eq(_present_phase_for_origin(
				sim, MissionData.KIND_ITEM, placed.index, channel), 0,
				"channel %d holds the brain's zero fill after PLAYPARTANIM" % channel)
		assert_eq(_present_field_for_origin(
				sim, MissionData.KIND_ITEM, crate.index, active_field), 0,
				"the brainless crate publishes no channel %d" % channel)


func test_present_part_anim_phase_transport_preserves_every_dword_bit() -> void:
	# PackedFloat32 cannot carry every signed dword numerically. The snapshot
	# schema transports two exact 16-bit integers, with high16+1 reserving zero
	# for "unpublished", so wrapping PLAYPARTANIM states survive unchanged.
	var snapshot := PackedFloat32Array()
	snapshot.resize(Simulation.PF_ACTIVE2 + 1)
	var phases := [
		0,
		65536,
		0x041893ab, # not exactly representable as one numeric float32
		2147483647,
		-1,
		-2147482600,
		-2147483648,
	]
	for channel in [1, 2]:
		var phase_field: int = Simulation.PF_PHASE1 + (channel - 1) * 2
		var active_field: int = Simulation.PF_ACTIVE1 + (channel - 1) * 2
		for phase in phases:
			snapshot[phase_field] = float(int(phase) & 0xffff)
			snapshot[active_field] = float(((int(phase) >> 16) & 0xffff) + 1)
			assert_eq(
				Simulation.decode_present_part_anim_phase(
					snapshot, 0, channel),
				int(phase),
				"channel %d preserves signed dword %d" % [channel, phase])
		snapshot[active_field] = 0.0
		assert_eq(
			Simulation.decode_present_part_anim_phase(
				snapshot, 0, channel),
			0,
			"zero high-word code remains the unpublished sentinel")


func test_fast_rope_suppresses_only_special1_publication() -> void:
	# HUD_CacheEntityDisplayInfo publishes the brain's two part-anim phases;
	# the FastRope attrib withholds VEHICLE_SPECIAL1 alone. PLAYPARTANIM
	# stores only the channels' direction and rate, and nothing integrates the
	# phase (Entity_UpdateSuspensionBounce has no caller), so the published
	# SPECIAL2 value is the brain's zero fill.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ITEM, 105006, Vector3.ZERO, Vector3.ZERO)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)
	assert_gte(md.add_event(0, 0, 0), 0)
	for channel in [1, 2]:
		assert_true(md.add_event_action(0, 21, 34, ssn, channel, 1, 65536))

	var item_db := _fast_rope_item_db()
	assert_not_null(item_db)
	if item_db == null:
		return
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	var dir := _native_fixture_dir()
	_write_fixture_bytes(dir, "StaticCrate1.3di", _bytes_with_renamed_user_point(
			FileAccess.get_file_as_bytes("res://../fixtures/threedi/synth/armory.3di"),
			"Armory", "ctrlx00"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([5006]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	for _tick in range(80):
		sim.step()
	assert_true(sim.has_event_fired(0), "the PLAYPARTANIM event ran")
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, placed.index,
			Simulation.PF_ACTIVE1), 0,
			"FastRope releases VEHICLE_SPECIAL1 ownership")
	assert_gt(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, placed.index,
			Simulation.PF_ACTIVE2), 0,
			"VEHICLE_SPECIAL2 remains unconditionally published")
	assert_eq(_present_phase_for_origin(
			sim, MissionData.KIND_ITEM, placed.index, 2), 0,
			"the published SPECIAL2 phase holds the brain's zero fill")
	assert_false(sim.get_entity_part_anim_active(0, 1))
	assert_true(sim.get_entity_part_anim_active(0, 2))


func test_listen_snapshot_attachment_holds_its_resting_userpoint() -> void:
	# The tank with a VEHICLE_SPECIAL1 track on its real turret part (the part
	# owning the ewep01 user point); hang the synthetic ewep from that user
	# point and run PLAYPARTANIM on the channel. PLAYPARTANIM stores only the
	# channel's direction and rate in the carrier's brain, and no retail code
	# integrates the phase (the sweep integrator Entity_UpdateSuspensionBounce
	# has no caller), so the track holds the brain's zero fill: the
	# authoritative mounted pose keeps the child on the resting userpoint.
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			SYN_TANK_SPECIAL1_SLIDE_EWEP01)), OK)
	assert_eq(String((data.get_control_registers()[0] as Dictionary).get("name", "")),
			"VEHICLE_SPECIAL1", "the fixture authors the semantic local CTRL name")
	var anchor_index := -1
	var anchor_info: ModelUserPoint = null
	for index in range(data.get_user_point_count()):
		var candidate := data.get_user_point_info(index)
		if candidate.name.to_lower() == "ewep01":
			anchor_index = index
			anchor_info = candidate
			break
	assert_gte(anchor_index, 0, "tank fixture has its authored ewep01 attachment point")
	if anchor_index < 0:
		return
	var anchor_part := anchor_info.subobject
	assert_gte(anchor_part, 0, "ewep01 is bound to a model part")
	if anchor_part < 0:
		return
	assert_eq(data.get_part_anim_count(0), 1)
	assert_eq(anchor_part, 1,
			"the fixture slides exactly the part that owns ewep01 (minimal_3di_gen "
			+ "pins tank_special1_slide_ewep01's slide on part 1)")

	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 21, 34, ssn, 1, 1, 65536))
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_trigger(1, 4, 1, 7, 1))
	assert_true(md.add_event_action(1, 20, 0, ssn))

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# This fixture isolates PANM attachment motion on a stationary carrier.
	# The carrier def authors the addeweap row; the fixture model resolves the
	# ewep01 anchor natively (tank also authors the ctrlx25 controller seat the
	# old dict spec faked). This attachment lifecycle needs the real
	# carrier/child rows (health, class, and NoNetworkCallback), so the def is
	# the fixture superset, not an isolated FastRope fixture.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, SYN_TANK_SPECIAL1_SLIDE_EWEP01, "tank.3di")
	var item_db := _item_db_from_text(dir, _fixture_items_text()
			.replace("graphic Dbuggy1", "graphic tank")
			.replace("move_function cveh", "move_function null")
			.replace("id 101291", "id 101291\n  addeweap ewep01 101419"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1291]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)

	# The first fold materializes the synthetic row; the later frames must keep
	# it where it started.
	sim.step()
	var stride := sim.get_present_stride()
	var snapshot := sim.get_present_snapshot()
	var initial_position := Vector3.INF
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == 1419:
			initial_position = Vector3(
					snapshot[base + Simulation.PF_POS_X],
					snapshot[base + Simulation.PF_POS_Y],
					snapshot[base + Simulation.PF_POS_Z])
			break
	assert_true(initial_position.is_finite(), "synthetic ewep reached the listen client")
	for _tick in range(79):
		sim.step()
	assert_true(sim.has_event_fired(0), "the PLAYPARTANIM event ran")
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, placed.index, Simulation.PF_ACTIVE1), 1,
			"the carrier's brain publishes its VEHICLE_SPECIAL1 channel")
	assert_eq(_present_phase_for_origin(
			sim, MissionData.KIND_ITEM, placed.index, 1), 0,
			"PLAYPARTANIM leaves the carrier's phase at the brain's zero fill")
	snapshot = sim.get_present_snapshot()
	var final_position := Vector3.INF
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == 1419:
			final_position = Vector3(
					snapshot[base + Simulation.PF_POS_X],
					snapshot[base + Simulation.PF_POS_Y],
					snapshot[base + Simulation.PF_POS_Z])
			break
	assert_true(final_position.is_finite(), "the attachment remains presented")
	if initial_position.is_finite() and final_position.is_finite():
		assert_lt(final_position.distance_to(initial_position), 0.002,
				"the presented attachment holds still: its carrier part never sweeps")
		assert_lt(final_position.distance_to(anchor_info.position), 0.002,
				"host snapshot uses the authoritative mounted child pose on the resting userpoint")

	# A scripted kill zeroes the carrier's health and its brain's death transition
	# later sets its dead bit. The attachment is never retired: it stays presented,
	# and its hide follows the carrier's dead bit, not its health (the ewep class
	# update's dead-hull exit reads Flags & 2), so it is hidden exactly while the
	# carrier row is flagged dead. Stop restores the authoritative baseline; its
	# fresh decoded view must replay the load stream because this child has no
	# live compact of its own, and the living carrier shows its attachment.
	sim.set_mission_variable(7, 1)
	var kept := true
	var hide_follows_carrier := true
	for _tick in range(80):
		sim.step()
		snapshot = sim.get_present_snapshot()
		var child_seen := false
		var child_hidden := false
		var carrier_dead := false
		for record in range(snapshot.size() / stride):
			var base := record * stride
			var type_id := int(snapshot[base + Simulation.PF_TYPE_ID])
			if type_id == 1419:
				child_seen = true
				child_hidden = snapshot[base + Simulation.PF_HIDDEN] > 0.5
			elif type_id == 1291:
				carrier_dead = snapshot[base + Simulation.PF_ALIVE] < 0.5
		kept = kept and child_seen
		hide_follows_carrier = hide_follows_carrier and child_hidden == carrier_dead
	assert_true(sim.has_event_fired(1), "the scripted kill ran")
	assert_true(kept, "a killed carrier never retires its attachment")
	assert_true(hide_follows_carrier,
			"the attachment is hidden exactly while its carrier is flagged dead")

	sim.reset_session()
	snapshot = sim.get_present_snapshot()
	var restored := false
	var restored_hidden := true
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_TYPE_ID]) == 1419:
			restored = true
			restored_hidden = snapshot[base + Simulation.PF_HIDDEN] > 0.5
			break
	assert_true(restored,
			"restart immediately replays restored NoNetworkCallback attachments")
	assert_false(restored_hidden, "the living carrier shows its attachment again")

	# The first live 0x0A after replay must use the re-applied authoritative
	# items.def classes. If restore had reverted the carrier callback width, this
	# fold would desynchronize and lose/scatter the following child row.
	sim.step()
	snapshot = sim.get_present_snapshot()
	var live_carrier := false
	var live_child := false
	for record in range(snapshot.size() / stride):
		var type_id := int(snapshot[record * stride + Simulation.PF_TYPE_ID])
		live_carrier = live_carrier or type_id == 1291
		live_child = live_child or type_id == 1419
	assert_true(live_carrier and live_child,
			"post-restart 0x0A keeps carrier and attachment class widths aligned")


func _fast_rope_collision_moved_vertices(fixture_res_path: String, channel: int) -> int:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ITEM, 105006, Vector3.ZERO, Vector3.ZERO)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 21, 34, ssn, channel, 1, 65536))

	var item_db := _fast_rope_item_db()
	assert_not_null(item_db)
	if item_db == null:
		return 0
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(fixture_res_path)), OK)
	assert_eq(data.get_part_anim_count(0), 1,
			"the fixture carries one register-driven slide of part 1 (the row is "
			+ "pinned by the minimal_3di_gen ctest)")

	var sim := Simulation.new()
	# One fixture carries both the register-driven PANM row (CTRL 0 named
	# VEHICLE_SPECIAL1 or VEHICLE_SPECIAL2) and, after the byte rename below,
	# the ctrlx00 controller seat (Armory renamed post-export).
	var dir := _native_fixture_dir()
	_write_fixture_bytes(dir, "StaticCrate1.3di", _bytes_with_renamed_user_point(
			FileAccess.get_file_as_bytes(fixture_res_path), "Armory", "ctrlx00"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([5006]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	# The human stands inside the F3 hitbox view's range of the item.
	_spawn_fixture_human(sim, Vector3(0, 0, 20))
	var before_rows: Array = sim.get_hitbox_debug().entities
	assert_eq(before_rows.size(), 1)
	if before_rows.size() != 1:
		return 0
	var before: PackedVector3Array = (before_rows[0] as HitboxDebugEntity).tris
	for _tick in range(80):
		sim.step()
	assert_true(sim.has_event_fired(0), "the PLAYPARTANIM event ran")
	var after_rows: Array = sim.get_hitbox_debug().entities
	assert_eq(after_rows.size(), 1)
	if after_rows.size() != 1:
		return 0
	var after: PackedVector3Array = (after_rows[0] as HitboxDebugEntity).tris
	assert_eq(after.size(), before.size())
	var moved := 0
	for i in before.size():
		if before[i].distance_to(after[i]) > 3.99:
			moved += 1
	return moved


func test_fast_rope_collision_holds_both_special_slides_at_rest() -> void:
	# The collision CTRL dictionary takes the brain's two part-anim phases, with
	# FastRope withholding SPECIAL1 alone, and PLAYPARTANIM never moves either:
	# it stores only the channel's direction and rate, and no retail code
	# integrates the phase (Entity_UpdateSuspensionBounce has no caller).
	assert_eq(_fast_rope_collision_moved_vertices(SYN_ARMRY_SPECIAL1_SLIDE, 1), 0,
			"FastRope suppresses SPECIAL1 in authoritative collision evaluation")
	assert_eq(_fast_rope_collision_moved_vertices(SYN_ARMRY_SPECIAL2_SLIDE, 2), 0,
			"the published SPECIAL2 holds the brain's zero fill, so its slide stays at rest")


func test_register_driven_collision_holds_its_rest_pose_headlessly() -> void:
	# The armory's COBJ parents are all 0; face counts are [12, 8, 2, 2]
	# (tests/fixtures/minimal_3di_gen.cpp), and its one register-driven slide
	# rides VEHICLE_SPECIAL1, the brain's first part-anim phase. PLAYPARTANIM
	# stores only the channel's direction and rate, and no retail code
	# integrates the phase (the sweep integrator Entity_UpdateSuspensionBounce
	# has no caller), so the brain keeps its zero fill and the authoritative
	# collision holds every section at rest.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		MissionData.KIND_ITEM, 105012, Vector3.ZERO, Vector3.ZERO)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)
	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 21, 34, ssn, 1, 1, 65536))

	# armory with CTRL 0 named VEHICLE_SPECIAL1 and one register-driven slide
	# of ordinal 1 (fixtures/README.md).
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(SYN_ARMRY_SPECIAL1_SLIDE)), OK)
	assert_eq(String((data.get_control_registers()[0] as Dictionary).get("name", "")),
			"VEHICLE_SPECIAL1", "the fixture authors the semantic local CTRL name")
	assert_true(data.has_collision())
	assert_eq(data.get_part_anim_count(0), 1,
			"the fixture slides ordinal 1 (the row is pinned by the minimal_3di_gen ctest)")

	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_write_fixture_bytes(dir, "StaticCrate1.3di", _bytes_with_renamed_user_point(
			FileAccess.get_file_as_bytes(SYN_ARMRY_SPECIAL1_SLIDE), "Armory", "ctrlx00"))
	var item_db := _item_db_from_text(dir, _fixture_items_text() + PART_ANIM_CARRIER_ROW)
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([5012]))
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 1, "the carrier owns the AI brain")
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	# The human stands inside the F3 hitbox view's range of the carrier.
	_spawn_fixture_human(sim, Vector3(0, 0, 20))
	var before_debug: Array = sim.get_hitbox_debug().entities
	assert_eq(before_debug.size(), 1)
	assert_eq((before_debug[0] as HitboxDebugEntity).face_total, 24)
	var before: PackedVector3Array = (before_debug[0] as HitboxDebugEntity).tris
	assert_eq(before.size(), 24 * 3)

	# No present pass/render node: collision reads authoritative AI state.
	for _tick in range(80):
		sim.step()
	assert_true(sim.has_event_fired(0), "the PLAYPARTANIM event ran")
	assert_eq(sim.get_entity_part_anim_phase(0, 1), 0,
			"PLAYPARTANIM leaves the brain's phase at its zero fill")
	var after_debug: Array = sim.get_hitbox_debug().entities
	assert_eq(after_debug.size(), 1)
	var after: PackedVector3Array = (after_debug[0] as HitboxDebugEntity).tris
	assert_eq(after.size(), before.size())
	var stayed := 0
	for i in before.size():
		if before[i].distance_to(after[i]) < 0.002:
			stayed += 1
	assert_eq(stayed, 24 * 3, "every section, ordinal 1's slide included, holds its rest pose")


func test_f3_hides_unresolved_local_player_fallback() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().organics as Array).is_empty(),
			"an unresolved local avatar never leaks through the fallback path")


func test_f3_organic_fallbacks_match_live_filtering_bounds() -> void:
	# F3 must describe the same unresolved pool-0 actors RoundSim can hit:
	# engine-flag filtering only (dead bodies remain solid), bounded to the local
	# 80-unit view and shared 96-entity debug budget, with the local avatar hidden.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	for i in range(101):
		var pos := Vector3(200, 0, 0) if i == 0 else Vector3(i % 10, 0, i % 7)
		assert_not_null(md.add_entity(
				MissionData.KIND_ORGANIC, 105311, pos,
				Vector3.ZERO))

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.debug_set_entity_health(1, 0)

	var rows: Array = sim.get_hitbox_debug().organics
	assert_eq(rows.size(), 96, "fallback entities obey the F3 target cap")
	var handles := {}
	for value in rows:
		var row: HitboxDebugOrganic = value
		assert_true(row.fallback)
		handles[row.entity_handle] = true
	assert_false(handles.has(0), "the 200-unit actor is outside the local F3 range")
	assert_true(handles.has(1), "a zero-health corpse retains its bullet fallback")


func test_time_driven_collision_advances_without_an_ai_brain() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
		MissionData.KIND_BUILDING, 102001,
		Vector3.ZERO, Vector3.ZERO))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		'res://../fixtures/def/items.def')), OK)
	var data := ObjectData.new()
	const PUMP_MODEL := 'res://../fixtures/threedi/synth/pump.3di'
	assert_eq(data.open_file(ProjectSettings.globalize_path(PUMP_MODEL)), OK)
	assert_gt(data.get_part_anim_count(0), 0)
	assert_true(data.has_live_panm_for_lod(0),
		'the immutable pump-jack PANM is live for LOD0')
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

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 0, 'static has no AiEntity controls')
	var dir := _native_fixture_dir()
	_copy_fixture(dir, PUMP_MODEL, "GuardTwr1.3di")
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	var fallback_tick := sim.get_logic_tick()
	var fallback_before: PackedVector3Array = (
		(sim.get_hitbox_debug().entities[0] as HitboxDebugEntity).tris)
	for _tick in range(40):
		sim.step()
	assert_eq(sim.get_logic_tick() - fallback_tick, 40)
	var fallback_after: PackedVector3Array = (
		(sim.get_hitbox_debug().entities[0] as HitboxDebugEntity).tris)
	var fallback_moved := 0
	for i in fallback_before.size():
		if fallback_before[i].distance_to(fallback_after[i]) > 0.002:
			fallback_moved += 1
	assert_gt(fallback_moved, 0,
		'direct/headless simulation uses deterministic logic_tick * 16')

	sim.debug_set_panm_time_ms(0)
	var before_debug: Array = sim.get_hitbox_debug().entities
	assert_eq(before_debug.size(), 1)
	var before: PackedVector3Array = (before_debug[0] as HitboxDebugEntity).tris
	assert_gt(before.size(), 0)
	sim.debug_set_panm_time_ms(640)
	var after_debug: Array = sim.get_hitbox_debug().entities
	assert_eq(after_debug.size(), 1)
	var after: PackedVector3Array = (after_debug[0] as HitboxDebugEntity).tris
	assert_eq(after.size(), before.size())
	var moved := 0
	for i in before.size():
		if before[i].distance_to(after[i]) > 0.002:
			moved += 1
	assert_gt(moved, 0,
		'free-running PANM uses retail milliseconds with zero controls')


func test_scripted_remove_frees_the_ai_entity_with_its_registry_slot() -> void:
	# VaporizeSingle (action 22) destroys the registry row, and the brain goes
	# with it (Entity_Destroy @0x43e810 frees the AI component, memset @0x43e995),
	# so no card is reachable by the old AI index and the attached effects detach.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	var probe := Simulation.new()
	assert_true(probe.load_from_mission_data(md))
	var ssn := probe.get_entity_net_id(0)

	assert_gte(md.add_event(0, 0, 0), 0)
	assert_true(md.add_event_action(0, 22, 0, ssn))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_admit_standalone_script_ticks(sim)
	assert_eq(sim.get_entity_effect_state_for_ssn(ssn).size(), Simulation.EFFECT_STATE_COUNT,
			"the effect lookup sees the live registry slot before VaporizeSingle")
	for _i in range(16):
		sim.step()

	var card: EntityCard = sim.entity_card_by_ai_index(0)
	assert_null(card, "the AI entity is freed with its registry slot")
	assert_eq(sim.get_entity_net_id(0), 0,
			"no live registry row answers for the vaporized entity")
	assert_true(sim.get_entity_effect_state_for_ssn(ssn).is_empty(),
			"attached effects detach as soon as VaporizeSingle removes the registry slot")


func test_ai_state_name_static_lookup() -> void:
	assert_eq(Simulation.ai_state_name(16), "GROUND_FOLLOWWP")
	assert_eq(Simulation.ai_state_name(13), "?", "id gaps read as unknowns")
	assert_eq(Simulation.ai_state_name(23), "GROUND_DEAD")


# The USE key's vehicle-loadout arm gates (the engine's
# local_player_in_vehicle_loadout_zone / local_player_vehicle_zone_team_matches,
# the useitem arm's parentSlot + Flags & 0x800 + groundEntity team reads): no
# local player reads false on both; a spawned player off any type-11 volume is
# not in a bay, and free-standing (no ground entity) reads team 0 = open. The
# positive bay case is the local_player_view ctest.
func test_vehicle_loadout_zone_gates_off_a_bay() -> void:
	var sim := Simulation.new()
	assert_false(sim.local_player_in_vehicle_loadout_zone(), "no world: no bay")
	assert_false(sim.local_player_vehicle_zone_team_matches(), "no player: no team read")
	sim.build_demo_mission()
	assert_false(sim.local_player_in_vehicle_loadout_zone(), "no local player: no bay")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	sim.step()
	assert_false(sim.local_player_in_vehicle_loadout_zone(),
			"the demo mission authors no vehicle-loadout volume")
	assert_true(sim.local_player_vehicle_zone_team_matches(),
			"a free-standing player reads ground team 0 = open")


# The view actions' producers of the BMS input-action word (the cat-7 player
# triggers' LIVE word): viewchase (402) sets 0x8000000, view1st (400)
# 0x4000000, viewwithgun (401) 0x10000000, the PlayerCockpitView bit. The
# bits accumulate (only the BMS clear action and the trigger commit rewrite
# the word); an action without a binding row writes nothing.
func test_view_actions_set_the_input_action_bits() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_eq(sim.debug_input_action_bits(), 0, "a fresh world carries no input bits")
	sim.apply_local_player_view_action(Simulation.VIEW_ACTION_CHASE)
	assert_eq(sim.debug_input_action_bits(), 0x8000000, "viewchase sets 0x8000000")
	sim.apply_local_player_view_action(Simulation.VIEW_ACTION_FIRST_PERSON)
	assert_eq(sim.debug_input_action_bits(), 0x8000000 | 0x4000000,
			"view1st sets 0x4000000; the earlier bit stays set")
	sim.apply_local_player_view_action(Simulation.VIEW_ACTION_WITH_GUN)
	assert_eq(sim.debug_input_action_bits(), 0x8000000 | 0x4000000 | 0x10000000,
			"viewwithgun sets 0x10000000")
	sim.apply_local_player_view_action(412)
	assert_eq(sim.debug_input_action_bits(), 0x8000000 | 0x4000000 | 0x10000000,
			"an unbound action writes nothing")
