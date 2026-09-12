extends GutTest

const NATIVE_RUNTIME_TIMING_KEYS := [
	"sim_tick_us",
	"net_tick_us",
	"present_snapshot_us",
	"occlusion_build_us",
	"occlusion_probe_us",
]

# The world-update rows every role's logic tick lands on the frame-stats
# board through the kernel's one tick profile (ADR 0043 d5): the direct
# (no-net) tick fills exactly these; the host/joiner-only rows stay unsampled.
const WORLD_PHASE_SLOTS := [
	FrameStats.SIM_SERVER_WORLD, FrameStats.SIM_WORLD_SETUP,
	FrameStats.SIM_WORLD_SCRIPTS, FrameStats.SIM_WORLD_AI,
	FrameStats.SIM_WORLD_ATTACHMENTS, FrameStats.SIM_WORLD_THROWABLES,
	FrameStats.SIM_WORLD_WEAPONS, FrameStats.SIM_WORLD_PROJECTILES,
	FrameStats.SIM_WORLD_DESTRUCTION, FrameStats.SIM_WORLD_HOUSEKEEPING,
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
		for file_name in DirAccess.get_files_at(dir):
			DirAccess.remove_absolute(dir.path_join(file_name))
		DirAccess.remove_absolute(dir)
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


func _write_fixture_text(dir: String, name: String, text: String) -> void:
	var file := FileAccess.open(dir.path_join(name), FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return
	file.store_string(text)
	file.close()


func _copy_fixture(dir: String, source_res_path: String, dest_name: String) -> void:
	_write_fixture_bytes(dir, dest_name, FileAccess.get_file_as_bytes(source_res_path))


func _fixture_items_text() -> String:
	return FileAccess.get_file_as_bytes(
			"res://../fixtures/def/items.def").get_string_from_ascii()


func _item_db_from_text(dir: String, text: String) -> ItemDatabase:
	_write_fixture_text(dir, "items.def", text)
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
func _binoc_path() -> String:
	var path := RetailData.fixture(BINOC_REL)
	if path.is_empty():
		pending(RetailData.fixture_pending_text(BINOC_REL))
	return path


# The 19-bone person + BINOC rig as a named organic graphic: <graphic>.3di,
# <graphic>.adm (the anim map the native pose provider resolves through the
# item's anim_def), and the shared BINOC.bad clip (the caller has checked
# _binoc_path()).
func _write_char_rig(dir: String, graphic: String) -> void:
	_copy_fixture(dir, "res://../fixtures/threedi/synth/person.3di",
			graphic + ".3di")
	_write_fixture_bytes(dir, "BINOC.bad", FileAccess.get_file_as_bytes(RetailData.fixture(BINOC_REL)))
	var quote := String.chr(34)
	_write_fixture_text(dir, graphic + ".adm",
			"anim_reset %sBINOC.bad%s\n" % [quote, quote]
			+ "anim_idle %sBINOC.bad%s\n" % [quote, quote]
			+ "anim_idle_2 %sBINOC.bad%s\n" % [quote, quote]
			+ "anim_emplaced %sBINOC.bad%s\n" % [quote, quote])


# These pose fixtures have no walking clips. Spawn their boarder at the
# authored entry/seat point so they exercise attachment within the real radius.
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


func _fast_rope_item_db() -> ItemDatabase:
	var path := ProjectSettings.globalize_path(
			"res://.godot/ctrl_fast_rope_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string("""begin "Fast Rope Control Fixture"
  id 105006
  type object
  graphic StaticCrate1
  sid fastropectrl
  hp 50
  attrib: FastRope
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
	assert_eq(options.capture_duration_seconds, 27)
	assert_eq(options.capture_speed_setting, 2)
	assert_eq(options.spawn_wave_time_base, 4)
	assert_eq(options.spawn_wave_time_zone, 12)
	assert_eq(options.default_spawn_requires_no_team_zone, 1)
	assert_eq(options.num_teams, 4)


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
	sim.occlusion_init_mission()
	assert_true(sim.step())
	sim.run_occlusion_frame(
			Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
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
			Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
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
			Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
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
	var jitter := Vector2(
			view.binocular_yaw_offset_deg,
			view.binocular_pitch_offset_deg)
	assert_almost_eq(jitter.length(), 2.8125, 0.0001,
			"the toggle seeds the fixed 0x02000000-BAM displacement")

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

	assert_true(sim.request_local_player_nvg_toggle())
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


func test_hud_spread_row_tracks_stance_and_settled_aim_state() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# HUD ERROR is selected from two stance triplets. Air/water/mount overrides
	# live in the body; this public seam pins the ordinary stance order and the
	# settled-first-person +3 verdict. [orig: HUD_DrawCrosshair
	# @0x592b35..0x592b87; Player_CanFireWeapon @0x5cf780]
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	# A compact deterministic FSM is enough for the view toggle; the entity's
	# equipped ADM index still resolves the exact M4 ERROR table loaded above.
	var def_2 := WeaponDef.new()
	def_2.name = "WPN_M4AUTO"
	def_2.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("scopeup", 0, 0),
		WeaponActionRow.make("scopedown", 0, 0),
	])
	def_2.flags = 0x1
	def_2.clipsize = 30
	def_2.startrounds = 300
	sim.set_local_player_weapon(def_2, {})
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 2,
			"standing selects row 2")

	assert_true(sim.request_local_player_stance(2))
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 0,
			"prone selects row 0")

	sim.set_water_z(1.0)
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 2,
			"below-water source height forces the standing row")
	sim.set_water_z(0.0)
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 0,
			"leaving water restores the authored prone row")

	assert_true(sim.request_local_player_scope_toggle())
	var aimed_row_seen := false
	for _i in range(15):
		sim.step()
		assert_false(_aimed_shot_available(sim),
				"Scoped ADS never promotes before the ease endpoint")
	for _i in range(9):
		sim.step()
		if sim.get_local_player_weapon_state().hud_spread_row == 3:
			aimed_row_seen = true
			break
	assert_true(aimed_row_seen,
			"settled first-person aim adds the second-triplet offset")

	sim.set_local_player_debug_third_person(true)
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().hud_spread_row, 0,
			"third person clears aimed-shot availability without changing stance")


func test_aimed_shot_verdict_uses_both_promoted_optic_predicates() -> void:
	# Player_CanFireWeapon calls both helpers: Scoped is Flags bit 0, while the
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
	# pins Player_CanFireWeapon's MoveOrder&8 rejection end-to-end.
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
	# first person. The reload test sits earlier in Player_CanFireWeapon and is
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
	# [orig: g_animStateFlagsTable @0x8139E8]
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


func test_local_fire_exports_recoil_camera_and_hud_spread() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# Keep the world's built-in player ItemDef traits intact: the compact items.def
	# fixture intentionally lacks retail's player template 105305, while recoil's
	# source gate requires a person with an ItemDef. [orig: RoundData_SpawnRound
	# @0x4ec0d0; recoil add @0x4ec8a3]
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	var def_3 := WeaponDef.new()
	def_3.name = "WPN_M4AUTO"
	def_3.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("fire", 0, 0),
		WeaponActionRow.make("recoil", 0, 0),
	])
	def_3.flags = 0
	def_3.clipsize = 30
	def_3.startrounds = 300
	sim.set_local_player_weapon(def_3, {})
	sim.step()
	sim.set_local_player_weapon_input(false, true, false)
	for _i in range(4):
		sim.step()
		if sim.get_local_player_weapon_state().fired_serial > 0:
			break

	var weapon_state := sim.get_local_player_weapon_state()
	var recoil_pitch := weapon_state.recoil_pitch_bam
	var weight_spread := weapon_state.weapon_weight_spread_bam
	assert_gt(recoil_pitch, 0,
			"the successful M4 round stamps the standing ammo recoil impulse")
	assert_eq(weapon_state.hud_spread_row, 2,
			"standing hip fire selects the first triplet's standing row")
	assert_eq(weapon_state.hud_spread_fp16,
			0x4000 + (recoil_pitch >> 7) + (weight_spread >> 7),
			"HUD spread preserves exact ERROR plus both live SAR terms")
	var recoil_view := sim.get_local_player_view()
	assert_almost_eq(recoil_view.fp_pitch_recoil_deg,
			float(recoil_pitch) * 2.0 * 360.0 / 4294967296.0, 0.0001,
			"the bridge exports retail's wrapped 2*recoil camera pitch")


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
	# eats entries 0 and 1 at bake — the FIRST reload PLAY serves variant 2, the
	# next serves 0 (the REVVY M4 "m4_1r" "m4_1r" "m4_1r2" shape).
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
	# dispatch gate refuses the edge mid-FIRE), then reload: the bake left the reload
	# ring's head at 2 (two 'auto' reads), so the FIRST reload serves variant 2.
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
		if state.anim_key == "anim_wpn_reload" 				and state.play_serial != first_reload_serial:
			reload_variant = state.anim_variant
			break
	assert_eq(reload_variant, 0, "the second reload wraps the ring back to variant 0")


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
		assert_true(sim.request_local_player_scope_toggle())
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


func test_local_fire_spawns_the_authoritative_round_and_impact() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The listen-server loopback handler skips C2S 0x06 because retail local fire
	# already appends/spawns synchronously. Pin that local action seam end-to-end:
	# FSM fired -> RoundSim -> the organic effects_table row.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	# Spawn yaw 0 = mission yaw 0 = engine heading 90 BAM-deg, which faces
	# mission +y (bearing 90). The target sits 8 m along +y so the shot connects
	# only when the round bearing rides the heading frame directly — the old
	# (90 - heading) flip flew the shot along +x and only an east-side target
	# could pass (the compensating-error pair the fp_impact probe pinned;
	# ledger D-WPN-18).
	# Use the fixture's Generic Soldier (wire id 5311 -> items.def id 105311),
	# then resolve traits through the same production seam as MissionRoot. Retail
	# returns before projectile damage when the struck entity has no ItemDef.
	assert_not_null(md.add_entity(MissionData.KIND_ORGANIC, 5311,
			Vector3(0, 8, 0), Vector3.ZERO))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)
	sim.resolve_item_traits(item_db)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO")
	var fire_def := WeaponDef.new()
	fire_def.name = "WPN_M4AUTO"
	fire_def.set_actions([
		WeaponActionRow.make("idle", 0, 0),
		WeaponActionRow.make("fire", 0, 0),
		WeaponActionRow.make("recoil", 0, 0),
	])
	fire_def.flags = 0
	fire_def.clipsize = 30
	fire_def.startrounds = 300
	sim.set_local_player_weapon(fire_def, {})
	sim.step()
	sim.drain_local_player_weapon_events()
	sim.set_local_player_weapon_input(false, true, false)
	var impacts: Array = []
	for _i in range(4):
		sim.step()
		impacts.append_array(sim.drain_round_impacts())

	var weapon_state := sim.get_local_player_weapon_state()
	assert_eq(weapon_state.round_ring_count, 1,
			"local FIRE appends exactly one tag-2 fan-out record")
	# The fixture M4's first shot samples the pre-consume 30-round magazine, so
	# ((30 & 3) << 4) | 2 produces 0x22, not a hard-coded 0x02 and not the
	# unrelated category/rank weapon-slot combo.
	# [orig: WeaponAction_Fire @ 0x542c11; net-re §5.9.1 capture cross-witness]
	assert_eq(weapon_state.last_round_flags, 0x22)
	assert_eq(weapon_state.last_round_subtype, 12,
			"ordinary on-foot hip fire carries the retail default zoom subtype")
	assert_eq(weapon_state.last_round_slot_byte, 0,
			"the sole modeled local weapon slot has retail slot id zero")
	assert_eq(weapon_state.last_round_seq, 1)
	assert_eq(impacts.size(), 1, "one local shot reaches the target and emits one impact")
	if impacts.size() == 1:
		# The victim is a NON-LOCAL person, so the flesh row (23), not the local
		# player's row (2). Real small-arms ammo authors Effect_AmHitBody on both,
		# so only the SOUND distinguishes them.
		# [orig: Projectile_HandleTerrainImpact_0 @ 0x4e98f0 — local-player compare
		#  @0x4e9a55, push 2 @0x4e9aa1, push 17h @0x4e9ad7]
		assert_eq((impacts[0] as RoundImpactRow).effect, "Effect_AmHitBody")
		assert_eq((impacts[0] as RoundImpactRow).sound, "IMP_BULLET_FLESH")
		# The drained position is Godot-space (x, z_up, -y): the +y_m flight lands
		# near (0, ~eye, -8). Pins the local fire bearing = the engine heading
		# frame (D-WPN-18; RoundSim's wire-validated (cos, sin) mapping).
		var impact_pos := (impacts[0] as RoundImpactRow).position
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
	assert_eq(weapon_state.round_ring_count, 2)
	assert_eq(weapon_state.last_round_seq, 2,
			"weapon remount does not reset the shooter-lifetime sequence")


func test_local_round_damages_enemy_mounted_on_rotated_emplaced_gun() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	if _binoc_path().is_empty():
		return
	# Exact player report: the target rendered in a rotated UseGun seat must keep
	# its authored COBJ sections under the carried body basis while its look yaw
	# remains independent, so a local-owned round through a visible section can
	# damage it.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var reference_gun := md.add_entity(
			MissionData.KIND_ITEM, 101294,
			Vector3(-20, 8, 0), Vector3.ZERO)
	var reference_enemy := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			_seat_fixture_spawn("res://../fixtures/threedi/synth/mount.3di", "Usegun", Vector3(-20, 8, 0)), Vector3.ZERO)
	var rotated_gun := md.add_entity(
			MissionData.KIND_ITEM, 101294,
			Vector3(0, 8, 0), Vector3(0, -90, 0))
	var rotated_enemy := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			_seat_fixture_spawn("res://../fixtures/threedi/synth/mount.3di", "Usegun", Vector3(0, 8, 0), Vector3(0, -90, 0)), Vector3.ZERO)
	for pair in [
		[reference_enemy, reference_gun],
		[rotated_enemy, rotated_gun],
	]:
		assert_not_null(pair[0] as MissionEntityRecord)
		assert_not_null(pair[1] as MissionEntityRecord)
		assert_true(md.set_entity_property_int(
				MissionData.KIND_ORGANIC,
				(pair[0] as MissionEntityRecord).index,
				"waypoint_id", 125))
		assert_true(md.set_entity_property_int(
				MissionData.KIND_ORGANIC,
				(pair[0] as MissionEntityRecord).index,
				"wp_number", (pair[1] as MissionEntityRecord).bms_id))

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# Native root: the guns pose their UseGun seat from mount's authored
	# Usegun point; both enemies pose their COBJ sections from the US02
	# person + BINOC rig resolved through their anim_def.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	_write_char_rig(dir, "us02")
	var item_db := _item_db_from_text(dir, _fixture_items_text().replace(
			"id 101294", "id 101294\n  graphic mount"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_gte(sim.resolve_collision_instances(item_db), 2,
			"precondition: both enemies use authored posed COBJ collision")
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries instead of a single step.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	var reference_card: EntityCard = sim.entity_card_by_ai_index(0)
	var rotated_card: EntityCard = sim.entity_card_by_ai_index(1)
	assert_true(reference_card.is_mounted())
	assert_true(rotated_card.is_mounted())
	assert_eq(rotated_card.get_mount_type(), 3)
	var health_before := rotated_card.get_health()
	assert_gt(health_before, 0)

	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_ammo_table(root, "ammo.def"), OK)
	# Advance one authoritative frame so collision and the decoded presentation
	# snapshot expose the same mounted body pose.
	sim.step()
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	var reference_row_base := -1
	var rotated_row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_KIND]) != MissionData.KIND_ORGANIC:
			continue
		var mission_index := int(snapshot[base + Simulation.PF_INDEX])
		if mission_index == reference_enemy.index:
			reference_row_base = base
		elif mission_index == rotated_enemy.index:
			rotated_row_base = base
	assert_gte(reference_row_base, 0,
			"the reference gunner reached the decoded presentation")
	assert_gte(rotated_row_base, 0,
			"the rotated gunner reached the decoded presentation")
	if reference_row_base < 0 or rotated_row_base < 0:
		return
	assert_eq(int(snapshot[reference_row_base +
			Simulation.PF_AIM_OVERLAY_VALID]), 1)
	assert_eq(int(snapshot[rotated_row_base +
			Simulation.PF_AIM_OVERLAY_VALID]), 1)
	var sections: Array = sim.get_hitbox_debug().organics
	var reference_by_section := {}
	var rotated_by_section := {}
	for value in sections:
		var row: HitboxDebugOrganic = value
		var handle := row.entity_handle
		var section := row.section
		if handle == 0:
			reference_by_section[section] = row
		elif handle == 1:
			rotated_by_section[section] = row
	assert_eq(reference_by_section.size(), 19)
	assert_eq(rotated_by_section.size(), 19)

	var reference_pos: Vector3 = reference_card.get_position()
	var rotated_pos: Vector3 = rotated_card.get_position()
	# PF_YAW_DEG / debug yaw is the gunner's independent look. The final body
	# field is the basis EntityPresenter.aim_apply actually applies to the rendered model
	# and the same body class build_section_matrices uses for posed collision.
	var reference_body_yaw := snapshot[reference_row_base +
			Simulation.PF_AIM_BODY_YAW_DEG]
	var rotated_body_yaw := snapshot[rotated_row_base +
			Simulation.PF_AIM_BODY_YAW_DEG]
	var relative_basis := MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0, rotated_body_yaw, 0)) * MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0, reference_body_yaw, 0)).inverse()
	var selected_section := -1
	var selected_score := -1.0
	# Hips, thighs, calves, and feet are the body/leg overlay classes. Their
	# mounted yaw is carried by the seat even when the two gunners independently
	# aim their upper bodies after the authoritative step.
	for section_value in [0, 7, 8, 11, 12, 17, 18]:
		var section := int(section_value)
		if not reference_by_section.has(section):
			continue
		var row: HitboxDebugOrganic = reference_by_section[section]
		var offset: Vector3 = row.pos - reference_pos
		var radius := maxf(row.radius, 0.001)
		var score := Vector2(offset.x, offset.z).length() / radius
		if rotated_by_section.has(section) and score > selected_score:
			selected_score = score
			selected_section = section
	assert_gte(selected_section, 0,
			"a shared off-axis authored section exists")
	var reference_center: Vector3 = (
			reference_by_section[selected_section] as HitboxDebugOrganic).pos
	var expected_center := rotated_pos + relative_basis * (
			reference_center - reference_pos)
	var collision_center: Vector3 = (
			rotated_by_section[selected_section] as HitboxDebugOrganic).pos
	assert_lt(collision_center.distance_to(expected_center), 0.01,
			"posed collision follows the mounted entity's rendered body yaw")

	var radial := expected_center - rotated_pos
	radial.y = 0.0
	assert_gt(radial.length(), 0.01)
	var tangent := Vector3(-radial.z, 0.0, radial.x).normalized()
	assert_gte(sim.debug_spawn_round(
			expected_center - tangent,
			tangent, "AMMO_CAR15_556MM"), 0,
			"a local-owned live round starts through the rendered section")
	for _i in range(2):
		sim.step()
	var rotated_after: EntityCard = sim.entity_card_by_ai_index(1)
	assert_lt(rotated_after.get_health(), health_before,
			"the local round damages the rotated mounted enemy organic")


func test_mounted_rendered_head_matrix_matches_collision_and_authoritative_shot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	if _binoc_path().is_empty():
		return
	# Config 6 is the decisive per-config witness: the mounted body/neck stay on
	# the carrier frame while the head alone consumes aim. Build an actual
	# ObjectModel from the same person + BINOC rig as collision, feed it
	# the packed presentation result, and compare its final head deformation to
	# COBJ section 14 before shooting through that rendered point.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(
			MissionData.KIND_ITEM, 101294,
			Vector3(0, 8, 0), Vector3(45, 0, 0))
	var enemy := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3(0, 8, 0), Vector3.ZERO)
	assert_not_null(gun)
	assert_not_null(enemy)
	assert_true(md.set_entity_property_int(
			MissionData.KIND_ORGANIC, enemy.index,
			"waypoint_id", 125))
	assert_true(md.set_entity_property_int(
			MissionData.KIND_ORGANIC, enemy.index,
			"wp_number", gun.bms_id))

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# Native sim sources: mount's authored Usegun seat (bone 6) with the def's
	# phrase_set 6, plus the US02 person + BINOC rig for the mounted pose.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	_write_char_rig(dir, "us02")
	var item_db := _item_db_from_text(dir, _fixture_items_text().replace(
			"id 101294", "id 101294\n  graphic mount\n  phrase_set 6"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	# The render-side witness model keeps its own rig objects; the sim no
	# longer reads them (native-only pose sources).
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/person.3di")), OK)
	var bad_root := ResourceRoot.new()
	assert_eq(bad_root.set_root_dir(RetailData.fixture(BINOC_REL).get_base_dir()), OK)
	var skeletal := SkeletalAnim.new()
	assert_true(skeletal.load_from_bad_files(
			bad_root, "BINOC.bad", {"anim_emplaced": "BINOC.bad"},
			data.get_bone_origins(), data.get_bone_parents()),
			"mounted person rig loads: %s" % skeletal.get_last_error())
	assert_gte(sim.resolve_collision_instances(item_db), 1,
			"the mounted enemy owns authored posed COBJ collision")
	var ammo_root := ResourceRoot.new()
	assert_eq(ammo_root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_ammo_table(ammo_root, "ammo.def"), OK)

	# Advance one authoritative frame so the mounted seat frame, collision pose,
	# and listen-server client snapshot all describe the same clip phase.
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries instead of a single step.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	var enemy_idx := _first_organic_ai_index(sim)
	assert_gte(enemy_idx, 0)
	var card: EntityCard = sim.entity_card_by_ai_index(enemy_idx)
	assert_true(card.is_mounted())
	assert_true(card.is_mount_config_valid())
	assert_eq(card.get_mount_config(), 6)
	assert_eq(card.get_anim_key(), "anim_emplaced")
	var enemy_handle := sim.get_entity_wire_handle(enemy_idx)
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	var row_base := -1
	for record in range(snapshot.size() / stride):
		var base := record * stride
		if int(snapshot[base + Simulation.PF_KIND]) == MissionData.KIND_ORGANIC \
				and int(snapshot[base + Simulation.PF_INDEX]) == enemy.index:
			row_base = base
			break
	assert_gte(row_base, 0, "the mounted placed enemy reached the decoded present")
	if row_base < 0:
		return
	assert_eq(int(snapshot[row_base + Simulation.PF_AIM_OVERLAY_VALID]), 1)
	assert_eq(int(snapshot[row_base + Simulation.PF_RIGHT_HAND_COLLAPSED]), 1,
			"the mounted NPC exports retail's BN17 final-row verdict")
	var packed_body := Vector3(
			snapshot[row_base + Simulation.PF_AIM_BODY_PITCH_DEG],
			snapshot[row_base + Simulation.PF_AIM_BODY_YAW_DEG],
			snapshot[row_base + Simulation.PF_AIM_BODY_ROLL_DEG])
	var head_offset: int = (row_base + Simulation.PF_AIM_ANGLES
			+ 8 * Simulation.PF_AIM_CLASS_STRIDE)
	var packed_head := Vector3(
			snapshot[head_offset], snapshot[head_offset + 1],
			snapshot[head_offset + 2])
	assert_gt(packed_head.distance_to(packed_body), 10.0,
			"the config-6 witness really drives head aim away from the mounted body")

	var model = ObjectModel.new()
	add_child_autofree(model)
	model.set_skeletal_anim(skeletal)
	model.set_object_data(data)
	assert_true(model.has_skeleton())
	var presented_position := Vector3(
			snapshot[row_base + Simulation.PF_POS_X],
			snapshot[row_base + Simulation.PF_POS_Y],
			snapshot[row_base + Simulation.PF_POS_Z])
	assert_lt(presented_position.distance_to(card.get_position()), 0.5,
			"the selected row is the mounted placed enemy")
	model.global_position = presented_position
	model.play_body_clip_at(
			"anim_emplaced",
			int(snapshot[row_base + Simulation.PF_ANIM_PHASE_TICKS]))
	EntityPresenter.aim_apply(model, snapshot, row_base)
	await get_tree().process_frame
	await get_tree().process_frame

	var skeleton: Skeleton3D = model.get_skeleton()
	# Inspect the same composed pose once without the clip verdict to capture the
	# animated joint, then restore the snapshot's mounted verdict.
	model.set_right_hand_collapsed(false)
	await get_tree().process_frame
	await get_tree().process_frame
	skeleton.force_update_all_bone_transforms()
	var authored_hand_joint := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16).origin)
	model.set_right_hand_collapsed(true)
	await get_tree().process_frame
	await get_tree().process_frame
	skeleton.force_update_all_bone_transforms()
	# The synthetic person's COBJ 14/head authors center=(1/16, 0, 13/16)
	# in signed 16.16 collision space (tests/fixtures/minimal_3di_gen.cpp). The
	# retail fixed-to-render sandwich maps local collision (x,y,z) to the
	# skeleton's node frame as (y,z,x) before the live bone deformation.
	var head_center_model := Vector3(0.0, 0.8125, 0.0625)
	var rendered_head_matrix := (skeleton.global_transform
			* skeleton.get_bone_global_pose(14)
			* skeleton.get_bone_global_rest(14).affine_inverse())
	var rendered_head_center: Vector3 = rendered_head_matrix * head_center_model
	var rendered_hand_matrix := (skeleton.global_transform
			* skeleton.get_bone_global_pose(16)
			* skeleton.get_bone_global_rest(16).affine_inverse())
	assert_true(rendered_hand_matrix.basis.x.is_zero_approx()
			and rendered_hand_matrix.basis.y.is_zero_approx()
			and rendered_hand_matrix.basis.z.is_zero_approx(),
			"render skinning receives retail's zero-scale BN17 basis")
	assert_true(rendered_hand_matrix.origin.is_equal_approx(authored_hand_joint),
			"render skinning collapses BN17 at the animated joint, never world origin")

	var collision_head := Vector3.INF
	var collision_hand := Vector3.INF
	for value in sim.get_hitbox_debug().organics:
		var section: HitboxDebugOrganic = value
		if section.entity_handle == enemy_handle \
				and section.section == 14:
			collision_head = section.pos
		if section.entity_handle == enemy_handle \
				and section.section == 16:
			collision_hand = section.pos
	assert_ne(collision_head, Vector3.INF,
			"authoritative collision exposes mounted head section 14")
	if collision_head != Vector3.INF:
		assert_lt(collision_head.distance_to(rendered_head_center), 0.01,
				"rendered final head bone matrix and collision section 14 are identical")
	assert_ne(collision_hand, Vector3.INF,
			"authoritative collision exposes mounted right-hand section 16")
	if collision_hand != Vector3.INF:
		assert_true(collision_hand.is_zero_approx(),
				"authoritative COBJ 16 retains retail's separate all-zero final row")

	var health_before := card.get_health()
	var incoming := rendered_head_matrix.basis.x.normalized()
	# The round's segment as a plain geometric trace first (the picker walks
	# the same posed person bone spheres the round will): it resolves the
	# rendered mounted target at its posed head section, never a stand-in
	# sphere — read before the shot lands, since the kill spawns the corpse
	# twin on the same spot.
	var pick := sim.debug_pick_entity(rendered_head_center - incoming * 2.0, incoming, 4.0)
	assert_true(pick.hit,
			"the authoritative shot resolves against the rendered mounted target")
	if pick.hit:
		assert_eq(pick.entity_handle, enemy_handle)
		assert_eq(pick.hit_class, "person")
		assert_eq(pick.section, 14,
				"the primary posed-hit section remains authoritative")
	assert_gte(sim.debug_spawn_round(
			rendered_head_center - incoming * 2.0,
			incoming, "AMMO_CAR15_556MM"), 0,
			"a local-owned round starts through the rendered mounted head")
	for _tick in range(2):
		sim.step()
	var impacts := sim.drain_round_impacts()
	assert_eq(impacts.size(), 1)
	if impacts.size() == 1:
		var impact: RoundImpactRow = impacts[0]
		assert_lt(impact.direction.distance_to(incoming), 0.001,
				"the incoming shot direction remains authoritative for reactions")
	assert_lt(sim.entity_card_by_ai_index(enemy_idx).get_health(),
			health_before, "the posed head shot damages the mounted enemy")


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


func test_armory_reads_and_clears_authoritative_local_loadout() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)

	assert_eq(sim.get_local_player_class(), 8, "spawned player exposes its rifleman class")
	assert_eq(sim.get_local_player_team(), 2, "spawned player exposes the assigned red team")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"weapon-table load exposes the entity's stamped default instead of the FP fallback")
	assert_false(sim.has_explicit_spawn_loadout(),
		"the engine's WPN_M4AUTO fallback is not an authored spawn kit")
	var fallback_loadout := sim.get_local_player_loadout()
	assert_eq(fallback_loadout.size(), 1,
		"the canonical transport exposes the effective default kit")
	assert_eq((fallback_loadout[0] as WeaponKitEntry).name, "WPN_M4AUTO")
	assert_true(sim.set_local_player_class(7),
		"profile class can commit without replacing the current weapon kit")
	assert_eq(sim.get_local_player_class(), 7)
	sim.set_spawn_loadout([WeaponKitEntry.make("WPN_M4AUTO")], true)
	assert_true(sim.has_explicit_spawn_loadout(),
		"a supplied mission/profile kit is distinguishable from the fallback")

	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 6))
	assert_eq(sim.get_local_player_class(), 6, "accepted class is authoritative on reopen")
	var accepted_loadout := sim.get_local_player_loadout()
	assert_eq(accepted_loadout.size(), 1)
	assert_eq((accepted_loadout[0] as WeaponKitEntry).ammo_primary, -1,
		"unspecified canonical ammo remains the retail fallback sentinel")
	var inv := sim.get_local_player_inventory()
	assert_true(inv.valid, "the ACCEPT rebuilt the slot pool")
	assert_eq(inv.equipped_name, "WPN_M4AUTO",
		"the ACCEPT re-selected the accepted weapon")
	assert_true(sim.apply_local_player_loadout([], 9), "the all-NONE kit is a valid apply")
	assert_eq(sim.get_local_player_class(), 9, "NONE still commits the selected class")
	assert_eq(sim.get_local_player_weapon_name(), "", "NONE clears the equipped AdmDef")
	assert_true(sim.get_local_player_loadout().is_empty(),
		"an explicit all-NONE kit remains empty instead of falling back to M4")


func test_same_name_armory_accept_refills_the_live_weapon_slot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load_from_resource_root(root, "weapon.def"), OK)
	var m4_index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(m4_index, 0)
	var m4: WeaponDef = weapons.get_weapon(m4_index)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	sim.set_local_player_weapon(m4, {})
	sim.step()
	var full_clip := sim.get_local_player_weapon_state().clip
	sim.set_local_player_weapon_input(false, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var spent_clip := sim.get_local_player_weapon_state().clip
	assert_lt(spent_clip, full_clip, "the live M4 spent a round before reopening armory")

	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 8))
	var inventory := sim.get_local_player_inventory()
	var equipped_combo := inventory.equipped_combo
	var rebuilt_clip := -1
	for value in inventory.slots:
		var slot: PlayerInventorySlot = value
		if slot.combo == equipped_combo:
			rebuilt_clip = slot.clip
			break
	assert_gt(rebuilt_clip, spent_clip, "ACCEPT rebuilt the same named slot at full clip")
	sim.set_local_player_weapon(m4, {})
	var mounted := sim.get_local_player_weapon_state()
	assert_eq(mounted.clip, rebuilt_clip,
		"same-name real mount reads the rebuilt authoritative inventory")
	assert_eq(mounted.current_action, 0)
	assert_eq(mounted.next_action, 0)
	assert_false(mounted.windup_active)
	assert_false(sim.get_local_player_view().scope_engaged)


func test_loadout_weapon_category_switch_changes_equipped_weapon() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)

	assert_true(sim.apply_local_player_loadout([
		WeaponKitEntry.make("WPN_M4AUTO"),
		WeaponKitEntry.make("WPN_colt45"),
	], 8))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var primary_index := weapons.find_weapon("WPN_M4AUTO")
	assert_gte(primary_index, 0)
	sim.set_local_player_weapon(weapons.get_weapon(primary_index), {})
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"the primary is equipped before switching")
	sim.drain_local_player_weapon_events()

	# The retail '2' binding requests category 2 (secondary). Let the outgoing
	# weapon's SWITCHFROM action complete, then observe the committed slot.
	sim.request_local_player_weapon_category(2)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_colt45":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_colt45",
		"switching to a secondary in the accepted loadout changes the equipped weapon")


func test_weapon_switch_requested_during_draw_commits_without_a_second_press() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([
		WeaponKitEntry.make("WPN_M4AUTO"),
		WeaponKitEntry.make("WPN_colt45"),
	], 8))

	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var primary_index := weapons.find_weapon("WPN_M4AUTO")
	var secondary_index := weapons.find_weapon("WPN_colt45")
	assert_gte(primary_index, 0)
	assert_gte(secondary_index, 0)
	sim.set_local_player_weapon(weapons.get_weapon(primary_index), {})

	# First switch normally, then mirror the LocalPlayerPresenter installing the new
	# weapon definition. Its first tick enters SWITCHTO (the draw animation).
	sim.request_local_player_weapon_category(2)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_colt45":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_colt45")
	sim.set_local_player_weapon(weapons.get_weapon(secondary_index), {})
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().current_action, 6,
		"the newly equipped secondary is drawing through SWITCHTO")

	# One press during that draw must be remembered and committed after it settles.
	sim.request_local_player_weapon_category(3)
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_name() == "WPN_M4AUTO":
			break
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
		"a switch requested during draw does not require a second press")


func test_entities_walk_their_route() -> void:
	# Soldiers are anim-driven [orig: Entity_UpdateInfantryAI @0x4b9910]: their motion
	# comes from .bad root-motion clips resolved through a model's .adm. Without a clip
	# set they hold and stand; with one they walk the route.
	var sim := Simulation.new()
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


func test_late_spawn_player_resolves_own_adm_before_configured_usegun_pose() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# MissionRoot resolves per-entity ADMs once after the host player spawn. A
	# joiner's local L and host-admitted remote players spawn later; they must still
	# receive US01 rather than retaining the default E_STAND/soldier map. Otherwise
	# B50 phrase_set 4 cannot select anim_emplaced_5 and silently uses the generic
	# emplaced pose, visibly placing the body beside the gun.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO))
	var sim := Simulation.new()
	# The fixture def row (101419) authors graphic mount, phrase_set 4, and
	# primary_weapon WPN_EMPLCD50NA — the native install reads all three.
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1419]))
	assert_true(sim.load_from_mission_data(md))
	assert_gt(sim.set_infantry_anim_map(_anim_root(), "soldier.adm"), 0)
	# Reproduce the production ordering bug: the one-time sweep happens before
	# this player exists.
	sim.resolve_infantry_adm_ids(_anim_root(), item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(sim, Vector3(2, 0, 0))

	var weapon_root := ResourceRoot.new()
	assert_eq(weapon_root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(weapon_root, "weapon.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	sim.set_local_player_weapon(
			weapons.get_weapon(weapons.find_weapon("WPN_M4AUTO")), {})
	for _tick in range(120):
		if sim.get_local_player_weapon_state().current_action < 2:
			break
		sim.step()
	assert_true(sim.local_player_toggle_mount())
	sim.step()
	assert_eq(sim.get_local_player_anim_key(), "anim_emplaced_5",
			"a late-spawn player receives US01 before phrase_set 4 selects its pose")


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
	_write_fixture_text(dir, "hudpos.def", """VEHICLE_HUD
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
	MountLook.face(sim, Vector3.ZERO)
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
	_write_fixture_text(fixture_dir, "SndProf.def", """begin "SP_Transport"
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



# The floating attach labels [orig: draw_vehicle_seat_and_armory_labels @0x5a3290
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
	# The label pass consumes the same live Player_CanFireWeapon verdict as the
	# body/HUD spread row: owning an equipped slot alone is not sufficient.
	# [orig: Player_CanFireWeapon @0x5cf780; label branch @0x5a32df]
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
	MountLook.face(sim, Vector3(14, 0, 0))
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
	# Command-125 boarders spawn ON FOOT and attach through the infantry
	# think's board leg; the think gate is (logic_tick + 36*net_id) & 15,
	# so drive a few 16-tick boundaries before reading the mounted state.
	# Co-located with the carrier, the first think boards.
	for _board_tick in range(48):
		sim.step()
	assert_true(sim.entity_card_by_net_id(soldier.bms_id).is_mounted(),
			"label fixture must occupy its controller seat")
	assert_true(sim.spawn_local_player(Vector3(12, 0, 0), 0.0, 1))
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
	MountLook.face(sim, Vector3(2, 0, 0))
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
func test_local_first_person_usegun_parent_cull_follows_live_mount_slot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# Retail suppresses the parent model only after THIS parent's MountSlot is
	# EquippedSlot in first person. Pre-commit attach, third person, and detach
	# render it normally. [orig: Entity_RenderVehicleModel @0x4407d0;
	# predicate @0x4407f6..0x44084c; Render_SubmitEntity @0x440918]
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(gun)
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# This cull witness pairs the emplacement with WPN_EMPLCD50 (a def with an
	# authored gfx1), so the superset def overrides the fixture's AVENGER row.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, _fixture_items_text()
			.replace("id 101294", "id 101294\n  graphic mount")
			.replace("primary_weapon WPN_AVENGER", "primary_weapon WPN_EMPLCD50"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(sim, Vector3(2, 0, 0))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	sim.step() # seed the decoded listen-client present rows
	var gun_index := gun.index
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"an unattached emplacement renders in the world pass")

	assert_true(sim.local_player_toggle_mount())
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the parent stays visible until its embedded slot commits")
	sim.step()
	var mount_event_found := false
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50":
			mount_event_found = true
	assert_true(mount_event_found)
	sim.set_local_player_weapon(mounted, {}, true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"authored gfx1 alone cannot cull when its first-person model failed to resolve")
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the live parent slot with a resolved FP model suppresses the duplicate world gun")

	sim.set_local_player_debug_third_person(true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"third person restores the parent model")
	sim.set_local_player_debug_third_person(false)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1)
	for _tick in range(120):
		if sim.get_local_player_weapon_state().current_action < 2:
			break
		sim.step()
	assert_true(sim.local_player_toggle_mount())
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"detach restores the world model immediately, before the holster commit")


func test_world_model_heat_glow_samples_parent_slot_and_caps_below_fp() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The emplacement carries a deterministic HEAT_GLOW collision track: the
	# mount fixture with its LOD0 rows replaced by one register-driven slide of
	# part 1 (0..4 wu on CTRL 0 = HEAT_GLOW). The scoped parent visual/collision
	# frame must sample its embedded MountSlot only while a live UseGun child is
	# attached; the local FP state is the comparison witness for the
	# intentionally different endpoint.
	# [orig: attachment caller @ 0x546518;
	#  HUD_CacheWeaponSlotInfo stores @ 0x440969 / @ 0x440991]
	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(
			SYN_MOUNT_HEAT_GLOW_SLIDE)), OK)
	assert_eq(String((object_data.get_control_registers()[0] as Dictionary).get("name", "")),
			"HEAT_GLOW")
	assert_eq(object_data.get_part_anim_count(0), 1)
	# (The row itself — a register-driven translation slide of part 1 on
	# CTRL 0 — is pinned by the minimal_3di_gen ctest: mount_heat_glow_slide_part1.)

	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(MissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(gun)
	var gun_index := gun.index

	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# The authored HEAT_GLOW track rides the fixture bytes; the sim's own parse
	# of mount.3di is the only seat/collision source.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, SYN_MOUNT_HEAT_GLOW_SLIDE, "mount.3di")
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1419]))
	assert_eq(sim.get_mounted_graphic_source_count(), 1,
			"the fixture model resolves as the one mounted-pose source")
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(sim, Vector3(2, 0, 0))
	# Listen-host player creation rebuilds the authoritative registry. Bind the
	# collision instance to that final registry identity, as GameWorld does.
	assert_eq(sim.resolve_collision_instances(item_db), 1)

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50NA"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	sim.step()

	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW_VALID), 0,
			"an unoccupied carrier is outside retail's attachment writer scope")
	var before_rows: Array = sim.get_hitbox_debug().entities
	assert_eq(before_rows.size(), 1)
	if before_rows.size() != 1:
		return
	var before: PackedVector3Array = (before_rows[0] as HitboxDebugEntity).tris

	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var mounted_switch := false
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50NA":
			mounted_switch = true
	assert_true(mounted_switch)
	sim.set_local_player_weapon(mounted, {}, true)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW_VALID), 1)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW), 0,
			"a live UseGun carrier owns the cold zero branch")

	sim.set_local_player_weapon_input(true, true, false)
	var fp_heat_glow := 0
	for _tick in range(3000):
		sim.step()
		fp_heat_glow = sim.get_local_player_weapon_state().heat_glow
		if fp_heat_glow >= 0x10000:
			break
	sim.set_local_player_weapon_input(false, false, false)
	assert_eq(fp_heat_glow, 0x10000,
			"the mounted first-person consumer reaches its distinct endpoint")
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW_VALID), 1)
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, gun_index,
			Simulation.PF_WORLD_HEAT_GLOW), 0xFFFF,
			"the same inline slot saturates the world model at 0xFFFF")

	var after_rows: Array = sim.get_hitbox_debug().entities
	assert_eq(after_rows.size(), 1)
	if after_rows.size() != 1:
		return
	var after: PackedVector3Array = (after_rows[0] as HitboxDebugEntity).tris
	assert_eq(after.size(), before.size())
	var moved := 0
	for index in before.size():
		if before[index].distance_to(after[index]) > 3.9:
			moved += 1
	assert_gt(moved, 0,
			"headless collision consumes the same authoritative HEAT_GLOW frame")


func test_local_usegun_aim_articulates_emplaced_weapon_model() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# mount's authored PANM binds its turret and barrel to the semantic
	# EWEAP_GUNYAW/EWEAP_GUNPITCH registers. A mounted local player's live look
	# must pose those parts in authoritative model space, not only turn the camera.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ITEM, 101419,
			Vector3(2, 0, 0), Vector3.ZERO))
	var object_data := ObjectData.new()
	assert_eq(object_data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/mount.3di")), OK)
	var usegun_info: ModelUserPoint = null
	for point_index in range(object_data.get_user_point_count()):
		var info := object_data.get_user_point_info(point_index)
		if info.name.nocasecmp_to("Usegun") == 0:
			usegun_info = info
	assert_not_null(usegun_info, "mount exposes its authored Usegun seat")
	var expected_usegun_world := MissionObjectPlacer.entity_transform(
			Vector3(2, 0, 0), Vector3.ZERO) * Vector3(
					usegun_info.position)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var sim := Simulation.new()
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1419]))
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	# Start well off the gun's authored zero yaw. Retail's attach request snaps the
	# requester's look/body heading to the UseGun heading before establishing the
	# relationship; leaving this stale produces the visible torso twist at the grips.
	const PRE_ATTACH_YAW_DEG := 160.0
	assert_true(sim.spawn_local_player(Vector3.ZERO, PRE_ATTACH_YAW_DEG, 1))
	assert_gt(absf(wrapf(sim.get_local_player_yaw_deg(), -180.0, 180.0)),
			90.0, 'fixture starts far from the gun yaw')
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([WeaponKitEntry.make("WPN_M4AUTO")], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var mounted: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50NA"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	assert_true(sim.local_player_toggle_mount())
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "mounted player origin coincides with the authored Usegun point")
	assert_lt(absf(wrapf(sim.get_local_player_yaw_deg(), -180.0, 180.0)),
			0.01, 'UseGun attach pre-snaps a mismatched local look to the gun yaw')
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50NA":
			sim.set_local_player_weapon(mounted, {}, true)
	sim.debug_set_panm_time_ms(0)
	var initial_weapon_state := sim.get_local_player_weapon_state()
	assert_true(initial_weapon_state.emplaced_controls_valid)
	var initial_yaw_control := initial_weapon_state.emplaced_gun_yaw
	assert_eq(initial_yaw_control, 0,
			'the attach snap starts EWEAP_GUNYAW at its neutral phase')
	var initial_overlay := sim.get_local_player_aim_overlay()
	assert_not_null(initial_overlay)
	var initial_body := initial_overlay.body_angles
	var initial_angles := initial_overlay.segment_angles
	assert_gt(initial_angles.size(), 0)
	assert_lt(absf(wrapf(initial_body.y, -180.0, 180.0)), 0.01,
			'the mounted body neutral overlay follows the snapped gun yaw')
	var max_initial_twist_deg := 0.0
	for angle in initial_angles:
		max_initial_twist_deg = maxf(max_initial_twist_deg,
				absf(wrapf(angle.y - initial_body.y, -180.0, 180.0)))
	assert_lt(max_initial_twist_deg, 0.01,
			'no segment retains the pre-attach look as a torso twist')
	var initial_pitch_control := initial_weapon_state.emplaced_gun_pitch

	var before_entities: Array = sim.get_hitbox_debug().entities
	assert_eq(before_entities.size(), 1)
	var before: PackedVector3Array = (before_entities[0] as HitboxDebugEntity).tris
	assert_gt(before.size(), 0)

	sim.set_local_player_mouse(511, false)
	var yaw_before := sim.get_local_player_yaw_deg()
	sim.add_local_player_look(100.0, 0.0)
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "look yaw cannot move the player off the Usegun point")
	assert_gt(absf(sim.get_local_player_yaw_deg() - yaw_before), 0.1,
			"the mounted local player's authoritative yaw changed")
	assert_ne(sim.get_local_player_weapon_state().emplaced_gun_yaw, initial_yaw_control,
			"look yaw reaches the retail EWEAP_GUNYAW phase")
	var yaw_entities: Array = sim.get_hitbox_debug().entities
	var after_yaw: PackedVector3Array = (yaw_entities[0] as HitboxDebugEntity).tris
	var yaw_moved := 0
	for index in before.size():
		if before[index].distance_to(after_yaw[index]) > 0.001:
			yaw_moved += 1
	assert_gt(yaw_moved, 0,
			"EWEAP_GUNYAW moves the authored mount turret with local look")

	var pitch_before := sim.get_local_player_pitch_deg()
	sim.add_local_player_look(0.0, 100.0)
	sim.step()
	assert_lt(sim.get_local_player_position().distance_to(expected_usegun_world),
			0.001, "look pitch cannot move the player off the Usegun point")
	assert_gt(absf(sim.get_local_player_pitch_deg() - pitch_before), 0.1,
			"the mounted local player's authoritative pitch changed")
	assert_ne(sim.get_local_player_weapon_state().emplaced_gun_pitch,
			initial_pitch_control,
			"look pitch reaches the retail EWEAP_GUNPITCH phase")
	var pitch_entities: Array = sim.get_hitbox_debug().entities
	var after_pitch: PackedVector3Array = (pitch_entities[0] as HitboxDebugEntity).tris
	var pitch_moved := 0
	for index in after_yaw.size():
		if after_yaw[index].distance_to(after_pitch[index]) > 0.001:
			pitch_moved += 1
	assert_gt(pitch_moved, 0,
			"EWEAP_GUNPITCH moves the authored mount barrel with local look")


func test_local_usegun_switches_viewmodel_and_borrows_parent_weapon_slot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var gun := md.add_entity(
			MissionData.KIND_ITEM, 101294, Vector3(2, 0, 0), Vector3.ZERO)
	assert_not_null(gun)
	var sim := Simulation.new()
	# The fixture def row already authors primary_weapon WPN_AVENGER (a finite
	# clip makes parent-slot persistence observable across remounts); the
	# superset adds the mount graphic for the authored Usegun seat.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, _fixture_items_text().replace(
			"id 101294", "id 101294\n  graphic mount"))
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([1294]))
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(sim, Vector3(2, 0, 0))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	assert_true(sim.apply_local_player_loadout([
		WeaponKitEntry.make("WPN_M4AUTO"),
		WeaponKitEntry.make("WPN_M4"),
	], 1))
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal_idx := weapons.find_weapon("WPN_M4AUTO")
	var mounted_idx := weapons.find_weapon("WPN_AVENGER")
	assert_gte(personal_idx, 0)
	assert_gte(mounted_idx, 0)
	var personal_def: WeaponDef = weapons.get_weapon(personal_idx)
	var mounted_def: WeaponDef = weapons.get_weapon(mounted_idx)
	# Exercise the merge seam with a PowerThrow-shaped personal def while the
	# authoritative inventory still selects WPN_M4AUTO. A UseGun handoff must
	# cancel its windup before borrowing the parent's persistent slot.
	var powerthrow_personal := personal_def.copy()
	powerthrow_personal.flags = personal_def.flags | (1 << 31)
	sim.set_local_player_weapon(powerthrow_personal, {})
	sim.drain_local_player_weapon_events()
	sim.step() # move beyond tick zero, the windup's idle sentinel
	sim.set_local_player_weapon_input(true, true, false)
	for _tick in range(5):
		sim.step()
	var wound_before_mount := sim.get_local_player_weapon_state()
	assert_true(wound_before_mount.windup_active)
	var personal_fired_before := wound_before_mount.fired_serial
	var personal_rounds_before := wound_before_mount.round_ring_count
	var personal_clip := sim.get_local_player_weapon_state().clip
	var inventory_clip := int(
			sim.get_local_player_inventory().slots[0].clip)

	assert_true(sim.local_player_toggle_mount())
	assert_eq(sim.get_local_player_weapon_name(), "WPN_M4AUTO",
			"local UseGun stages the parent slot instead of assigning it immediately")
	# The still-held PowerThrow plus a simultaneous trigger edge cannot overwrite
	# the pending mount action.
	sim.set_local_player_weapon_input(true, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var mount_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		var event: PlayerWeaponEvent = raw
		if event.switch_to_weapon == "WPN_AVENGER":
			mount_event = event
	assert_false(mount_event == null,
			"an Emplaced pending def takes retail's same-pump -901 commit")
	assert_true(mount_event.preserve_slot_state,
			"the host must not reset the emplacement's persistent slot")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER")
	var handed_off := sim.get_local_player_weapon_state()
	assert_false(handed_off.windup_active,
			"UseGun input suppression cancels the outgoing PowerThrow windup")
	assert_eq(handed_off.fired_serial, personal_fired_before)
	assert_eq(handed_off.round_ring_count, personal_rounds_before,
			"releasing through the handoff cannot leak a charged personal round")
	sim.set_local_player_weapon(mounted_def, {}, true)
	var mounted_before := sim.get_local_player_weapon_state()
	assert_eq(mounted_before.clip,
			mounted_def.clipsize)
	var mounted_slot_before_rebake := {
		"current_action": mounted_before.current_action,
		"next_action": mounted_before.next_action,
		"phase": mounted_before.phase,
		"clip": mounted_before.clip,
	}
	sim.rebake_local_player_weapon(mounted_def, {
		"anim_wpn_idle": PackedFloat32Array([0.2]),
	}, true)
	var mounted_after_rebake := sim.get_local_player_weapon_state()
	for field in mounted_slot_before_rebake:
		assert_eq(int(mounted_after_rebake.get(field)),
				int(mounted_slot_before_rebake[field]),
				"late mounted-def rebake preserves parent slot %s" % field)

	sim.set_local_player_weapon_input(true, true, false)
	var fired_before := mounted_before.fired_serial
	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_state().fired_serial > fired_before:
			break
	sim.set_local_player_weapon_input(false, false, false)
	var mounted_clip_after := int(
			sim.get_local_player_weapon_state().clip)
	assert_eq(mounted_clip_after, mounted_before.clip - 1,
			"local LMB consumes the parent's embedded weapon slot")
	assert_eq(sim.get_local_player_inventory().slots[0].clip, inventory_clip,
			"mounted fire cannot consume the saved personal magazine")

	for _tick in range(180):
		sim.step()
		if sim.get_local_player_weapon_state().current_action < 2:
			break
	assert_true(sim.local_player_toggle_mount())
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER",
			"detach also waits for the mounted slot's holster commit")
	# The entity is already detached, but its borrowed slot still owns the pending
	# UseGun SWITCHFROM. Manual requests in this frame must not replace that action.
	sim.request_local_player_weapon_category(3)
	sim.request_local_player_weapon_cycle(1)
	sim.set_local_player_weapon_input(true, true, false)
	sim.step()
	sim.set_local_player_weapon_input(false, false, false)
	var detach_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		var event: PlayerWeaponEvent = raw
		if event.switch_to_weapon == "WPN_M4AUTO":
			detach_event = event
	assert_false(detach_event == null,
			"outgoing Emplaced detach also commits on the next pump")
	assert_true(detach_event.preserve_slot_state)
	sim.set_local_player_weapon(personal_def, {}, true)
	assert_eq(sim.get_local_player_weapon_state().clip,
			personal_clip, "the personal slot resumes with its original magazine")

	for _tick in range(120):
		sim.step()
		if sim.get_local_player_weapon_state().current_action < 2:
			break
	# A dismount leaves the player standing on the seat point (retail never
	# displaces the rider); step a unit back and look at the gun to remount.
	MountLook.face(sim, Vector3(2, 0, 0), Vector3(1, 0, 0))
	assert_true(sim.local_player_toggle_mount())
	var remount_event: PlayerWeaponEvent = null
	for _tick in range(120):
		sim.step()
		for raw in sim.drain_local_player_weapon_events():
			var event: PlayerWeaponEvent = raw
			if event.switch_to_weapon == "WPN_AVENGER":
				remount_event = event
		if not remount_event == null:
			break
	assert_false(remount_event == null)
	sim.set_local_player_weapon(mounted_def, {}, true)
	assert_eq(sim.get_local_player_weapon_state().clip,
			mounted_clip_after,
			"the emplacement keeps its own clip state while nobody is attached")

	sim.reset_session()
	var restart_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		var event: PlayerWeaponEvent = raw
		if event.switch_to_weapon == "WPN_M4AUTO":
			restart_event = event
	assert_false(restart_event == null,
			"restart explicitly restores the saved personal presentation")
	assert_false(restart_event.preserve_slot_state,
			"restart installs a fresh personal slot epoch")
	assert_false(sim.get_local_player_weapon_state().active,
			"the mounted definition cannot pump the restored personal slot")


func test_local_usegun_direct_swap_targets_latest_parent_without_switchto() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	# Three guns in a row along their +y facing, 3.5 u apart: a seated USE
	# swaps only onto a seat inside the rider's 5 deg cone, which the gunner
	# reaches by looking down the row at the next seat point (3.5 u out, a
	# 10 deg depression from the eye); the 4.0 u reach keeps the third gun
	# out of the first mount.
	var first_gun := md.add_entity(MissionData.KIND_ITEM, 101294,
			Vector3(2, 0, 0), Vector3.ZERO)
	var second_gun := md.add_entity(MissionData.KIND_ITEM, 101295,
			Vector3(2, 3.5, 0), Vector3.ZERO)
	var third_gun := md.add_entity(MissionData.KIND_ITEM, 101296,
			Vector3(2, 7, 0), Vector3.ZERO)
	assert_not_null(first_gun)
	assert_not_null(second_gun)
	assert_not_null(third_gun)
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# All three fixture emplacement rows keep their authored primaries
	# (AVENGER / EMPLCD50 / EMPLCD50); the shared mount graphic supplies the
	# one authored Usegun seat per gun.
	var dir := _native_fixture_dir()
	_copy_fixture(dir, "res://../fixtures/threedi/synth/mount.3di", "mount.3di")
	var item_db := _item_db_from_text(dir, _fixture_items_text()
			.replace("id 101294", "id 101294\n  graphic mount")
			.replace("id 101295", "id 101295\n  graphic mount")
			.replace("id 101296", "id 101296\n  graphic mount"))
	_install_native_seat_table(sim, dir, item_db,
			PackedInt32Array([1294, 1295, 1296]))
	assert_true(sim.load_from_mission_data(md))
	sim.resolve_item_traits(item_db)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	MountLook.face(sim, Vector3(2, 0, 0))
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var personal: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	var first_mount: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_AVENGER"))
	var second_mount: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_EMPLCD50"))
	sim.set_local_player_weapon(personal, {})
	sim.drain_local_player_weapon_events()
	# Seed the listen-client rows and let the personal slot reach an attachable
	# state. The cull verdict is produced against this decoded presentation view.
	for _tick in range(80):
		sim.step()
		if sim.get_local_player_weapon_state().current_action < 2:
			break
	var first_gun_index := first_gun.index
	var second_gun_index := second_gun.index
	var third_gun_index := third_gun.index

	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var first_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_AVENGER":
			first_event = raw
	assert_false(first_event == null)
	sim.set_local_player_weapon(first_mount, {}, true)
	# Even a host-side resolution report cannot manufacture fpModel on a Def that
	# has none, and a different target Def must not inherit that model identity.
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			first_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"a live AVENGER MountSlot without gfx1 keeps its world model")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"mounting AVENGER cannot suppress the unrelated 50-cal parent")
	for _tick in range(80):
		sim.step()
		if sim.get_local_player_weapon_state().current_action < 2:
			break

	# Look down the row at the second gun's seat point (the same seat offset,
	# 3.5 u further along +y from this seat).
	MountLook.face(sim, MountLook.local_player_mission_position(sim) + Vector3(0, 3.5, 0))
	assert_true(sim.local_player_toggle_mount(),
			"a second gun inside the seated 5 deg cone is a direct mounted-seat swap")
	assert_eq(sim.get_local_player_weapon_name(), "WPN_AVENGER",
			"the old parent remains equipped until the rank commit")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			first_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"pre-commit swap restores the old parent world model")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"pre-commit target is not culled before its MountSlot is equipped")
	sim.step()
	var swap_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50":
			swap_event = raw
	assert_false(swap_event == null,
			"latest pending parent commits directly with no personal interlude")
	sim.set_local_player_weapon(second_mount, {}, true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			first_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the committed swap leaves the old AVENGER parent visible")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the new authored model does not cull before its FP graphic resolves")
	sim.set_local_player_first_person_model_available(true)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the resolved EMPLCD50 replacement culls only its own world model")
	sim.step()
	assert_eq(sim.get_local_player_weapon_state().current_action, 0,
			"SWITCHRANK does not queue SWITCHTO onto the target parent slot")
	assert_true(sim.get_local_player_weapon_state().borrowed_usegun_slot)

	# The scan walks the rider's proximity slice, which refreshes every 16
	# ticks; it was last built at the first gun's seat, 8 u from the third gun,
	# so let it refresh before looking down the row.
	for _tick in range(17):
		sim.step()
	MountLook.face(sim, MountLook.local_player_mission_position(sim) + Vector3(0, 3.5, 0))
	assert_true(sim.local_player_toggle_mount(),
			"a third gun down the row drives a second direct mounted-seat swap")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the outgoing parent restores before the same-Def swap commits")
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			third_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0,
			"the target parent stays visible before its slot is live")
	sim.step()
	var same_def_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_EMPLCD50":
			same_def_event = raw
	assert_false(same_def_event == null)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			second_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 0)
	assert_eq(_present_field_for_origin(sim, MissionData.KIND_ITEM,
			third_gun_index, Simulation.PF_LOCAL_VIEW_SUPPRESSED), 1,
			"the same resolved Def model immediately suppresses the newly live parent")


func test_death_during_usegun_draw_restores_personal_weapon() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
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
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			RetailData.def_root()), OK)
	assert_eq(sim.load_weapon_table(root, "weapon.def"), OK)
	var weapons := WeaponDatabase.new()
	assert_eq(weapons.load(RetailData.fixture("def/weapon.def")), OK)
	var mounted_def: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_AVENGER"))
	var personal_def: WeaponDef = weapons.get_weapon(
			weapons.find_weapon("WPN_M4AUTO"))
	sim.set_local_player_weapon(personal_def, {})
	sim.drain_local_player_weapon_events()

	MountLook.face(sim, Vector3(2, 0, 0))
	assert_true(sim.local_player_toggle_mount())
	sim.step()
	var mount_event: PlayerWeaponEvent = null
	for raw in sim.drain_local_player_weapon_events():
		if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_AVENGER":
			mount_event = raw
	assert_false(mount_event == null,
			"the emplacement commits before its SWITCHTO draw")
	sim.set_local_player_weapon(mounted_def, {}, true)
	var fired_before := sim.get_local_player_weapon_state().fired_serial
	sim.set_local_player_weapon_input(true, true, false)
	sim.debug_set_entity_health(0, 0)
	var restore_event: PlayerWeaponEvent = null
	for _tick in range(160):
		sim.step()
		for raw in sim.drain_local_player_weapon_events():
			if (raw as PlayerWeaponEvent).switch_to_weapon == "WPN_M4AUTO":
				restore_event = raw
		if not restore_event == null:
			break
	assert_false(restore_event == null,
			"forced detach during SWITCHTO eventually restores the personal slot")
	assert_eq(sim.get_local_player_weapon_state().fired_serial,
			fired_before, "a dead local gunner cannot fire the emplacement")
	sim.set_local_player_weapon(personal_def, {}, true)
	assert_false(sim.get_local_player_weapon_state().borrowed_usegun_slot)


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
	MountLook.face(sim, Vector3(2, 0, 0))
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
	MountLook.face(sim, Vector3(2, 0, 0))
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



func test_foliage_mask_anchors_track_local_player_stance() -> void:
	# The hide-in-grass selection: only infantry with a stance bit set
	# ((net_stance_bits & 0x3) != 0) and no groundEntity anchor the distant
	# MODEL/depth-mask foliage tier [orig: Terrain_RenderSectorEntitiesBySide
	# @ 0x5c7dc2/0x5c7ded (MoveOrder & 0x300), groundEntity gate
	# @ 0x5c7dd5..0x5c7df7]. The local player's SELECT latches are the stance
	# writer [orig: Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd].
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
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


func test_foliage_mask_anchors_ignore_standing_npcs() -> void:
	# Routed organics keep net_stance_bits 0 (the mirror only writes the LOCAL
	# player's SELECT latches; NPC stance never reaches the wire bits here), so
	# a demo mission full of standing walkers produces no anchors - matching
	# retail, where placed objects and standing soldiers leave MoveOrder's
	# stance bits clear [orig: the 0x5c7dc2 (flags & 0x300) reject].
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_eq(sim.get_entity_count(), 2, "the demo mission has AI infantry to reject")
	for _i in range(4):
		sim.step()
	assert_eq(sim.get_foliage_mask_anchor_positions().size(), 0,
		"standing NPCs never anchor the hide-in-grass tier")


func test_transport_uses_session_state() -> void:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_playing(), "starts paused")
	assert_true(sim.resume_session())
	assert_true(sim.is_playing(), "resume enters the Running session state")

func test_bms_event_fires_through_binding() -> void:
	# The capability consolidation adds: a BMS event evaluates through the SAME binding that
	# runs the AI (the editor preview used to walk AI but never fire events). Build an
	# unconditional OutputText(77) event, tick once, and confirm the host-presentation effect
	# drains out of the shared World EffectLog.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(0, MissionEventAction.make(6, 0, 77)))

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md), "loaded the scripted mission")
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
	assert_eq(music.size(), 16, "M0..M15")

	sim.set_mission_variable(5, 42)
	sim.set_global_variable(3, -7)
	assert_eq(sim.get_mission_variables_snapshot()[5], 42, "snapshot reflects V writes")
	assert_eq(sim.get_global_variables_snapshot()[3], -7, "snapshot reflects G writes")
	assert_eq(sim.get_global_variable(3), -7, "the scalar G getter agrees")


func test_fired_events_snapshot_matches_scalar() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(0, MissionEventAction.make(6, 0, 77)))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))

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
	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
	var visibility: PackedInt64Array = sim.get_building_visibility_changes()
	assert_eq(visibility.size(), 2, "collision-backed no-OOBJ building stays in the host batch")
	if visibility.size() == 2:
		assert_eq(int(visibility[0]), placed.bms_id)
		var packed := int(visibility[1])
		assert_eq(Simulation.building_visibility_mask(packed), 0xFFFFFFFF,
			"without a section map the host preserves every de-batched render part")
		assert_true(Simulation.building_visibility_visible(packed), "the in-frustum building is visible")


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
	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)

	var first: PackedInt64Array = sim.get_building_visibility_changes()
	assert_eq(first.size(), 2, "the first delta call emits the building's state")
	assert_eq(sim.get_render_culled_changes(), PackedInt32Array([0, 0]),
			"no entities to cull in this mission")

	sim.run_occlusion_frame(Transform3D.IDENTITY, 90.0, 1.0, 0.05, 500.0, -100.0, false)
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


func test_listen_snapshot_exports_authoritative_part_anim_channels() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ITEM, 105004, Vector3.ZERO, Vector3.ZERO)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(0, MissionEventAction.make(21, 34, ssn, 1, 1, 65536)))
	var sim := Simulation.new()
	sim.enable_listen_server(true)
	# The crate's controller seat comes from the committed armory model with
	# its Armory point byte-renamed to ctrlx00 (the fixture def's graphic).
	var dir := _native_fixture_dir()
	_write_fixture_bytes(dir, "StaticCrate1.3di", _bytes_with_renamed_user_point(
			FileAccess.get_file_as_bytes("res://../fixtures/threedi/synth/armory.3di"),
			"Armory", "ctrlx00"))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([5004]))
	assert_true(sim.load_from_mission_data(md))
	for _tick in range(80):
		sim.step()
	assert_gt(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, placed.index,
			Simulation.PF_ACTIVE1), 0)
	assert_eq(_present_phase_for_origin(
			sim, MissionData.KIND_ITEM, placed.index, 1), 65536,
			"SP/host presentation receives the authoritative PLAYPARTANIM pose")


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
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ITEM, 105006, Vector3.ZERO, Vector3.ZERO)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)
	assert_not_null(md.add_event(0, 0, 0))
	for channel in [1, 2]:
		assert_not_null(md.add_event_action(0, MissionEventAction.make(21, 34, ssn, channel, 1, 65536)))

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
	assert_eq(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, placed.index,
			Simulation.PF_ACTIVE1), 0,
			"FastRope releases VEHICLE_SPECIAL1 ownership")
	assert_gt(_present_field_for_origin(
			sim, MissionData.KIND_ITEM, placed.index,
			Simulation.PF_ACTIVE2), 0,
			"VEHICLE_SPECIAL2 remains unconditionally published")
	assert_eq(_present_phase_for_origin(
			sim, MissionData.KIND_ITEM, placed.index, 2), 65536)
	assert_false(sim.get_entity_part_anim_active(0, 1))
	assert_true(sim.get_entity_part_anim_active(0, 2))


func test_listen_snapshot_attachment_follows_animated_userpoint() -> void:
	# The tank with a deterministic VEHICLE_SPECIAL1 track on its real turret
	# part (the part owning the ewep01 user point); hang the synthetic ewep from
	# that user point. A rigid parent-local reconstruction stays at the authored
	# point; the authoritative mounted pose carries it four metres with the live
	# part.
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
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(0, MissionEventAction.make(21, 34, ssn, 1, 1, 65536)))
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_trigger(1, MissionEventTrigger.make(4, 1, 7, 1)))
	assert_not_null(md.add_event_action(1, MissionEventAction.make(20, 0, ssn)))

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

	# First fold materializes the synthetic row before the scripted animation has
	# reached its endpoint. Retain that baseline so the assertion cannot pass on
	# a root-only follow implementation.
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
	assert_eq(_present_phase_for_origin(
			sim, MissionData.KIND_ITEM, placed.index, 1), 65536,
			"carrier reached the scripted live PANM endpoint")
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
	assert_true(final_position.is_finite(), "animated attachment remains presented")
	if initial_position.is_finite() and final_position.is_finite():
		assert_gt(final_position.distance_to(initial_position), 3.9,
				"presented attachment follows its animated userpoint, not only the parent root")
		var expected: Vector3 = anchor_info.position + Vector3(4, 0, 0)
		assert_lt(final_position.distance_to(expected), 0.002,
				"host snapshot uses the authoritative mounted child pose")

	# A zero-health vehicle compact legitimately retires the decoded attachment
	# subtree. Stop restores the authoritative baseline; its fresh decoded view
	# must replay the load stream because this child has no live compact of its own.
	sim.set_mission_variable(7, 1)
	var retired := false
	for _tick in range(80):
		sim.step()
		snapshot = sim.get_present_snapshot()
		retired = true
		for record in range(snapshot.size() / stride):
			if int(snapshot[record * stride + Simulation.PF_TYPE_ID]) == 1419:
				retired = false
				break
		if retired:
			break
	assert_true(retired,
			"decoded zero-health carrier retires the synthetic child subtree")

	sim.reset_session()
	snapshot = sim.get_present_snapshot()
	var restored := false
	for record in range(snapshot.size() / stride):
		if int(snapshot[record * stride + Simulation.PF_TYPE_ID]) == 1419:
			restored = true
			break
	assert_true(restored,
			"restart immediately replays restored NoNetworkCallback attachments")

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
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(0, MissionEventAction.make(21, 34, ssn, channel, 1, 65536)))

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
	var before_rows: Array = sim.get_hitbox_debug().entities
	assert_eq(before_rows.size(), 1)
	if before_rows.size() != 1:
		return 0
	var before: PackedVector3Array = (before_rows[0] as HitboxDebugEntity).tris
	for _tick in range(80):
		sim.step()
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


func test_fast_rope_collision_publishes_special2_but_not_special1() -> void:
	assert_eq(_fast_rope_collision_moved_vertices(SYN_ARMRY_SPECIAL1_SLIDE, 1), 0,
			"FastRope suppresses SPECIAL1 in authoritative collision evaluation")
	assert_eq(_fast_rope_collision_moved_vertices(SYN_ARMRY_SPECIAL2_SLIDE, 2), 24,
			"SPECIAL2 remains published through the same collision CTRL dictionary")


func test_animated_collision_uses_retail_section_ordinal_headlessly() -> void:
	# The armory's COBJ parents are all 0; face counts are [12, 8, 2, 2]
	# (tests/fixtures/minimal_3di_gen.cpp).
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
		MissionData.KIND_ITEM, 105004, Vector3.ZERO, Vector3.ZERO)
	var ssn := placed.bms_id
	assert_gt(ssn, 0)
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(0, MissionEventAction.make(21, 34, ssn, 1, 1, 65536)))

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
		'res://../fixtures/def/items.def')), OK)
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
	_install_native_seat_table(sim, dir, item_db, PackedInt32Array([5004]))
	assert_true(sim.load_from_mission_data(md))
	assert_eq(sim.get_entity_count(), 1)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	var before_debug: Array = sim.get_hitbox_debug().entities
	assert_eq(before_debug.size(), 1)
	assert_eq((before_debug[0] as HitboxDebugEntity).face_total, 24)
	var before: PackedVector3Array = (before_debug[0] as HitboxDebugEntity).tris
	assert_eq(before.size(), 24 * 3)

	# No present pass/render node: collision reads authoritative AI state.
	for _tick in range(80):
		sim.step()
	assert_eq(sim.get_entity_part_anim_phase(0, 1), 65536)
	var after_debug: Array = sim.get_hitbox_debug().entities
	assert_eq(after_debug.size(), 1)
	var after: PackedVector3Array = (after_debug[0] as HitboxDebugEntity).tris
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
	assert_eq(moved, 24, "only ordinal 1 moves")
	assert_eq(stayed, 48)
	assert_eq(partial, 0)


func test_organic_collision_samples_current_skeletal_pose_headlessly() -> void:
	if _binoc_path().is_empty():
		return
	# BINOC is the shipped 19-bone, three-frame BAD. Pair it with person's
	# canonical 19-row model table/COBJ block so the real SkeletalAnim ->
	# Simulation -> CollisionWorld path can be tested without retail assets.
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us02")
	# BINOC is a static three-frame clip. Turn BN15/head frame 1 into an
	# identity quaternion at its parser-pinned rotation offset (1324 + 16)
	# to make a deterministic moving-bone fixture while retaining its real
	# 19-bone hierarchy and every other shipped byte.
	var moving_bad := FileAccess.open(
			dir.path_join("BINOC.bad"), FileAccess.READ_WRITE)
	assert_not_null(moving_bad)
	if moving_bad == null:
		return
	moving_bad.seek(1340)
	moving_bad.store_float(0.0)
	moving_bad.store_float(0.0)
	moving_bad.store_float(0.0)
	moving_bad.store_float(1.0)
	moving_bad.close()

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth/person.3di")), OK)
	assert_true(data.has_collision())
	var skeletal := SkeletalAnim.new()
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

	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3(10, 0, 0), Vector3.ZERO))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_gt(sim.set_infantry_anim_map(root, "us02.adm"), 0)
	sim.set_asset_root(root)
	assert_eq(sim.resolve_collision_instances(item_db), 1)

	var before: Array = sim.get_hitbox_debug().organics
	assert_eq(before.size(), 19, "one posed sphere per person COBJ/bone")
	var before_by_section := {}
	for value in before:
		var row: HitboxDebugOrganic = value
		assert_false(row.fallback)
		var pos := row.pos as Vector3
		assert_lt(pos.distance_to(Vector3(10, 0, 0)), 3.0,
				"bind pose applies entity translation exactly once")
		before_by_section[row.section] = pos
	assert_true(before_by_section.has(14), "head COBJ/bone is present")

	# No presentation node or Skeleton3D is involved: advancing authoritative
	# clip_phase must move the CollisionWorld/F3 matrices directly.
	for _tick in 2:
		sim.step()
	var after: Array = sim.get_hitbox_debug().organics
	assert_eq(after.size(), 19)
	var moved_sections := 0
	for value in after:
		var row: HitboxDebugOrganic = value
		var section := row.section
		if before_by_section.has(section) and (
				before_by_section[section] as Vector3).distance_to(
						row.pos as Vector3) > 0.0001:
			moved_sections += 1
	assert_gt(moved_sections, 0,
			"current BAD pose, not bind/entity-only matrices, drives collision")


func test_late_spawned_player_resolves_posed_collision_on_demand() -> void:
	if _binoc_path().is_empty():
		return
	# Mission collision is resolved before deploy in production. A player added
	# afterward must demand the same authored COBJ + ADM source instead of
	# becoming the one-sphere fallback until the next explicit sweep.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	# The player's own graphic (US01) + rig live in the sim's asset root; the
	# demand resolve must find them there after the sweep already ran.
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us01")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 0,
			"the initial pre-deploy sweep has no player to attach")
	assert_true(sim.spawn_local_player(Vector3(10, 0, 0), 0.0, 1))

	# F3 still exercises CollisionWorld's demand provider for late targets, but
	# presentation filters the local avatar before any posed/fallback row escapes.
	assert_true((sim.get_hitbox_debug().organics as Array).is_empty(),
			"the local avatar never renders posed or fallback hitboxes")
	var local_bms_id := sim.entity_card_by_ai_index(
			sim.get_entity_count() - 1).get_bms_id()
	assert_true(sim.has_collision_instance(local_bms_id),
			"the hidden local avatar was nevertheless attached on demand")


func test_f3_hides_local_player_and_omits_distant_posed_organic() -> void:
	if _binoc_path().is_empty():
		return
	# The F3 person view is a nearby diagnostic. It must neither wrap the local
	# avatar in debug spheres nor spend its pose/debug budget on a target more
	# than 80 mission units away.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3(200, 0, 0), Vector3.ZERO))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us02")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))

	var rows: Array = sim.get_hitbox_debug().organics
	assert_true(rows.is_empty(),
			"F3 omits the 200-unit target while still excluding local handle 1")


func test_f3_hides_unresolved_local_player_fallback() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().organics as Array).is_empty(),
			"an unresolved local avatar never leaks through the fallback path")


func test_reused_player_slot_invalidates_old_collision_attempt_identity() -> void:
	if _binoc_path().is_empty():
		return
	# US02 intentionally cannot resolve through this provider, so the mission
	# soldier leaves a negative collision attempt on pool-0 slot 0. After WAC
	# removes it, the local US01 player reuses that exact packed handle. The new
	# registry spawn identity must invalidate the negative cache and resolve all
	# authored person sections on demand.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	assert_not_null(md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3.ZERO, Vector3.ZERO))
	var probe := Simulation.new()
	assert_true(probe.load_from_mission_data(md))
	var old_ssn := probe.get_entity_net_id(0)
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(
			0, MissionEventAction.make(22, 0, old_ssn)))

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	# A US01-only root: the placed US02 soldier deliberately cannot resolve,
	# while the replacement local player's graphic can.
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us01")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 0,
			"US02 records one unresolved attempt on slot 0")
	for _tick in 16:
		sim.step()
	assert_true(sim.get_entity_effect_state_for_ssn(old_ssn).is_empty(),
			"the original slot occupant was removed")
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1),
			"US01 reuses the freed pool-0 slot")

	assert_true((sim.get_hitbox_debug().organics as Array).is_empty(),
			"the newly resolved local avatar remains hidden from F3")
	var local_bms_id := sim.entity_card_by_ai_index(
			sim.get_entity_count() - 1).get_bms_id()
	assert_true(sim.has_collision_instance(local_bms_id),
			"the old negative attempt cannot suppress the new slot identity")


func test_restart_re_resolves_the_restored_collision_identity() -> void:
	if _binoc_path().is_empty():
		return
	# Collision caches live outside World::Snapshot. Reusing slot 0 during play
	# must not leave the restored baseline actor unbound after Stop/Restart.
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var placed := md.add_entity(
			MissionData.KIND_ORGANIC, 105311,
			Vector3.ZERO, Vector3.ZERO)
	assert_not_null(placed)
	var bms_id := placed.bms_id
	var probe := Simulation.new()
	assert_true(probe.load_from_mission_data(md))
	var old_ssn := probe.get_entity_net_id(0)
	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(
			0, MissionEventAction.make(22, 0, old_ssn)))

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")), OK)
	# Both the placed US02 soldier and the replacement US01 local player
	# resolve from the same root.
	var dir := _native_fixture_dir()
	_write_char_rig(dir, "us02")
	_write_char_rig(dir, "us01")

	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
	_native_asset_root(sim, dir)
	assert_eq(sim.resolve_collision_instances(item_db), 1)
	assert_true(sim.has_collision_instance(bms_id))
	for _tick in 16:
		sim.step()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	assert_true((sim.get_hitbox_debug().organics as Array).is_empty())
	var local_bms_id := sim.entity_card_by_ai_index(
			sim.get_entity_count() - 1).get_bms_id()
	assert_true(sim.has_collision_instance(local_bms_id),
			"the replacement local occupant receives the cached graphic")

	sim.reset_session()
	assert_true(sim.has_collision_instance(bms_id),
			"restart rebinds the baseline before any F3 or round demand query")
	var restored_rows: Array = sim.get_hitbox_debug().organics
	assert_eq(restored_rows.size(), 19,
			"the restored non-local actor exposes every authored section")
	for value in restored_rows:
		var row: HitboxDebugOrganic = value
		assert_eq(row.entity_handle, 0)
		assert_false(row.fallback)


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

	assert_not_null(md.add_event(0, 0, 0))
	assert_not_null(md.add_event_action(0, MissionEventAction.make(22, 0, ssn)))
	var sim := Simulation.new()
	assert_true(sim.load_from_mission_data(md))
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
