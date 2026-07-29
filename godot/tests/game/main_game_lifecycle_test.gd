extends GutTest

# Full runtime-shell regression: retail-shaped packed boot resources, public menu intents, and
# the real blocking GameWorld load. It never relies on the test process having /d.

const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
const FIXTURE_DIR := "res://../fixtures/minimal/resources"
const BAKED_TERRAIN_DIR := "res://../fixtures/godot/dvxi5"
const MAIN_GAME_SCENE := preload("res://game/main_game.tscn")
# Witnessed retail placement (fixtures/minimal/README.md): strings plus the
# mission .bin/.pcx/.lwf family live in language; menus/defs/.bms/.dbf in
# localres; environment, terrain, and terrain art in resource.
const LANGUAGE_FILES := [
	"gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
	"menutxt.bin", "mnml.bin", "mnml.pcx", "mnml.lwf",
]
const LOCALRES_FILES := [
	"items.def", "weapon.def", "ammo.def", "main.mnu", "mp.mnu",
	"mnml.bms", "menu_style.mns", "newarow1.tga", "mnml.dbf",
]
const RESOURCE_FILES := [
	"mnml.env", "mnml.trn", "mnml_c.tga", "mnml_dm.tga",
	"mnml_dc1.tga", "mnml_t.tga", "mnml_m.pcx", "mnml_f.pcx",
]
const BAKED_TERRAIN_FILES := [
	"Dvxi5.cpt", "Dvxi5_c.tga", "Dvxi5_d1.tga", "Dvxi5_dc1.tga",
	"Dvxi5_dc2.tga", "Dvxi5_dc3.tga", "Dvxi5_dm.tga", "Dvxi5_dm2.tga",
	"Dvxi5_dmd.tga", "Dvxi5_f.pcx", "Dvxi5_m.pcx", "TRNTILE10.TGA",
]
# This lifecycle-only armory deliberately resolves the engine fallback as well
# as the selected profile weapon. The regression must fail if GameWorld mistakes
# a nonempty WPN_M4AUTO fallback inventory for a mission-authored kit.
const LIFECYCLE_WEAPON_DEF := """
ammoclass_max_carry CLASS_556MM 1000

weapon "WPN_AK47AUTO"
	category 1
	rank 0
	statid 102
	ammo AM_556MM
	clip 30
	maxclips 7
	gfx1 AK_TEST_FIRST
end

weapon "WPN_M4AUTO"
	category 1
	rank 0
	statid 101
	ammo AM_556MM
	clip 30
	maxclips 7
	gfx1 M4AUTO_TEST_FIRST
end

weapon "WPN_M4"
	category 1
	rank 0
	statid 100
	ammo AM_556MM
	clip 30
	maxclips 7
	gfx1 M4_TEST_FIRST
end
"""
const ISOLATED_ENV := [
	"NW_REPLAY", "NW_SP_MISSION", "NW_LAN_HOST", "NW_LAN_JOIN",
]

var _saved_config := PackedByteArray()
var _had_config := false
var _saved_env := {}
var _temp_dir := ""
var _shell: Node = null


func before_each() -> void:
	_had_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) \
			if _had_config else PackedByteArray()
	_saved_env.clear()
	for variable in ISOLATED_ENV:
		_saved_env[variable] = OS.get_environment(variable)
		OS.set_environment(variable, "")
	# The persisted expansion is process-wide state an earlier suite file can leave set, and
	# these cases join a fixture host that has no expansion archives at all. A stale name makes
	# the host advertise an expansion this install cannot mount, which the joiner's preload
	# correctly refuses (D-NET-178) — a failure about suite order, not about what is under test.
	# after_each restores the whole config file, so pinning it here leaks nothing.
	NovaResourceDirSettings.set_expansion("")


func after_each() -> void:
	# Explicitly release the mounted archive handles before deleting the fixture.
	if is_instance_valid(_shell):
		var world = _shell.get_node_or_null("World")
		if world != null:
			world.unload()
			var world_root = world.get_resource_root()
			if world_root != null:
				world_root.clear()
		var menu_host = _shell.get_node_or_null("MenuLayer/MenuHost")
		if menu_host != null and menu_host.get_menu() != null:
			var menu_root = menu_host.get_menu().get_resource_root()
			if menu_root != null:
				menu_root.clear()
		_shell.queue_free()
		_shell = null
	await get_tree().process_frame
	NovaMusicService.stop_context()
	if not _temp_dir.is_empty():
		_remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	for variable in ISOLATED_ENV:
		OS.set_environment(variable, String(_saved_env.get(variable, "")))
	if _had_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


func test_mission_return_restores_menu_frame_and_supports_another_load() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("NovaTerrain")
	var menu_host = _shell.get_node("MenuLayer/MenuHost")
	var boot_clear: Color = world.get_current_frame_clear_color()
	_assert_clean_menu(world, terrain, menu_host, boot_clear)
	# Once the shell owns a mounted resource session, later persistence changes
	# cannot redirect one consumer into a separately remounted VFS.
	var detached_resource_dir := _temp_dir.path_join("detached")
	assert_eq(DirAccess.make_dir_recursive_absolute(detached_resource_dir), OK)
	NovaResourceDirSettings.set_resource_dir(detached_resource_dir)
	assert_eq(NovaResourceDirSettings.get_resource_dir(), detached_resource_dir,
			"the persisted directory now points away from the mounted fixture")

	# This is the same public intent emitted by the mission-list ACCEPT command.
	menu_host.start_requested.emit("mnml.bms")
	assert_true(_shell.is_world_loading(),
			"the loading handoff is pending before the blocking load starts")
	assert_true(_shell.has_loading_background(),
			"the deferred handoff decodes archive-only mnml.pcx from language.pff")
	assert_false(world.is_loaded(),
			"the world cannot finish in the callback that mounts the loading UI")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_host)
	assert_false(world.get_current_frame_clear_color().is_equal_approx(boot_clear),
			"the loaded mission exercised a distinct frame clear")

	# The pause menu's ABORT command emits this public intent.
	menu_host.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_host, boot_clear)

	# Hiding retained terrain/environment state must not break the next load.
	menu_host.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_host)
	menu_host.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_host, boot_clear)

	# Failed deferred loads obey the same rollback contract.
	menu_host.start_requested.emit("missing-mission.bms")
	assert_true(_shell.is_world_loading(), "a failed load enters the deferred handoff")
	await _wait_for_load_to_settle()
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_host, boot_clear)


func test_join_loading_stays_raised_until_authoritative_admission() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")

	# The host's world contents are irrelevant to this shell boundary; its
	# authoritative session record names the packed mnml.bms installed in the
	# lifecycle fixture, which is what the joiner must load locally.
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var host := NovaSimulation.new()
	host.configure_host_session({
		"server_name": "Loading Hold Host",
		"mission_name": "Minimal",
		"mission_file": "mnml.bms",
		"gametype": 0x30020,
		"max_players": 4,
		# The lifecycle fixture ships base archives only. Say so on the wire: an unset field
		# leaves the host advertising whatever expansion this machine last persisted, and the
		# joiner's preload then correctly refuses a data set this install cannot mount
		# (D-NET-178) — a failure about machine state, not about the loading-screen hold.
		"expansion": "",
	})
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))

	var observed := {"local_load": false, "held": false}
	world.world_loaded.connect(func() -> void:
		observed["local_load"] = true
	, CONNECT_ONE_SHOT)
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = host.get_host_listen_port()
	target.server_name = "browse-time hint"
	_shell.join_lan_server(target)

	for _frame in range(1200):
		host.step()
		await get_tree().process_frame
		if bool(observed["local_load"]) and not bool(observed["held"]):
			# All handlers for world_loaded have now returned. The old behavior
			# dismissed the loading screen in MainGame's handler here.
			observed["held"] = _shell.is_world_loading() and not world.visible
		if bool(observed["local_load"]) and not _shell.is_world_loading():
			break

	assert_true(bool(observed["local_load"]), "the joiner completed its local mission load")
	assert_true(bool(observed["held"]),
			"local world_loaded cannot reveal the joiner before host admission")
	assert_false(_shell.is_world_loading(),
			"the loading screen releases after the authoritative join edge")
	assert_true(world.visible, "the admitted world is revealed")
	assert_true(world.get_sim() != null and world.get_sim().is_joined_in_match())
	host.free()


# An in-match session loss must tear the world down to the menu through the SAME
# abort presentation a join failure or a load abort uses -- retail exits the mission
# with a mapped exit reason and shows no in-world dialog. The signal is GameWorld's
# public surface, so this drives the shell leg without reaching into shell state; the
# mission it happens to be in is irrelevant to the shell boundary under test.
# [orig: the cs_dir0.timeout_ms = 120000 reap CNapiNetwork_Init @ 0x4ca4a0 ->
#  CNapiNetwork_OnDisconnectedFromServer @ 0x4c63d0 -> g_mission_exit_reason]
func test_in_match_session_loss_returns_to_the_menu() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("NovaTerrain")
	var menu_host = _shell.get_node("MenuLayer/MenuHost")
	var boot_clear: Color = world.get_current_frame_clear_color()
	assert_true(world.has_signal("session_lost"),
			"GameWorld publishes the in-match session-loss edge")

	menu_host.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_host)

	world.session_lost.emit("lost connection to the host (no traffic for 120 seconds)")
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_host, boot_clear)

	# The shell is usable again straight afterwards: a loss is an abort, not a wedge.
	menu_host.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_host)
	menu_host.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_host, boot_clear)


func test_debug_overlay_suspends_input_without_stopping_the_world() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("NovaTerrain")
	var menu_host = _shell.get_node("MenuLayer/MenuHost")
	menu_host.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	var runtime = world.get_runtime()
	assert_not_null(runtime)
	if runtime == null:
		return

	assert_true(_shell.is_gameplay_input_active())
	var tick_before := int(runtime.get_sim().get_logic_tick())
	_shell.toggle_debug_overlay()
	assert_false(_shell.is_gameplay_input_active(),
			"the public input gate closes on the same F3 edge")
	for _frame in range(4):
		await get_tree().process_frame
	assert_true(_shell.is_debug_overlay_open())
	assert_false(_shell.is_gameplay_input_active(),
			"F3 submits neutral player input while its controls own the cursor")
	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE,
			"F3 leaves the dump button clickable")
	assert_gt(int(runtime.get_sim().get_logic_tick()), tick_before,
			"the live world keeps ticking under the inspector")

	var overlay = _shell.find_child("DebugOverlay", true, false)
	assert_not_null(overlay)
	var pose_path := _temp_dir.path_join("f3-player-pose.json")
	var dumped_path: String = overlay.dump_debug_snapshot(pose_path)
	assert_eq(dumped_path, pose_path)
	var pose_file := FileAccess.open(dumped_path, FileAccess.READ)
	assert_not_null(pose_file)
	var payload_variant: Variant = JSON.parse_string(pose_file.get_as_text()) \
			if pose_file != null else null
	if pose_file != null:
		pose_file.close()
	var payload: Dictionary = payload_variant if payload_variant is Dictionary else {}
	var view: Dictionary = payload.get("view", {})
	var camera_snapshot: Dictionary = view.get("camera", {})
	assert_false(camera_snapshot.is_empty(),
			"the game host supplies its actual foliage-dispatch camera")
	assert_eq(String(camera_snapshot.get("mode", "")), "first_person")
	var actual_camera := _shell.get_node("Camera3D") as Camera3D
	var dumped_camera: Dictionary = camera_snapshot.get("position_godot", {})
	assert_almost_eq(float(dumped_camera.get("x", 0.0)),
			actual_camera.global_position.x, 0.0001)
	assert_almost_eq(float(dumped_camera.get("y", 0.0)),
			actual_camera.global_position.y, 0.0001)
	assert_almost_eq(float(dumped_camera.get("z", 0.0)),
			actual_camera.global_position.z, 0.0001)

	# The option registry drives the real world end to end: one programmatic
	# flip rides debug_option_changed through the shell's generic handler and
	# builds the world's skeleton view; the counter-flip frees it.
	overlay.set_option(&"show_skeletons", true)
	assert_not_null(world.get_node_or_null("SkeletonDebug"),
			"the registry flip built the world's skeleton debug view")
	overlay.set_option(&"show_skeletons", false)
	await get_tree().process_frame
	assert_null(world.get_node_or_null("SkeletonDebug"),
			"...and the counter-flip freed it")

	# The pick stack, end to end: the world load installed the highlight view
	# for the shell's list, F3-open flipped the click catcher on, and the
	# sim-authoritative ray is terrain-occluded exactly like a bullet.
	assert_not_null(world.get_node_or_null("PickDebug"),
			"the world renders the shell's pick list")
	assert_not_null(world.get_node_or_null("PickClickCatcher"),
			"overlay open: world clicks ray-pick")
	var pick_sim = runtime.get_sim()
	var player_pos: Vector3 = pick_sim.get_local_player_position()
	var down: Dictionary = pick_sim.debug_pick_entity(
			player_pos + Vector3(0, 20, 0), Vector3.DOWN, 100.0)
	assert_false(bool(down.get("hit", true)))
	assert_eq(String(down.get("blocked", "")), "terrain",
			"terrain blocks the pick exactly like a bullet")
	_shell.pick_at_crosshair()
	assert_not_null(_shell.find_child("PickToast", true, false),
			"the crosshair pick confirms every attempt with a toast")

	_shell.toggle_debug_overlay()
	await get_tree().process_frame
	assert_false(_shell.is_debug_overlay_open())
	assert_true(_shell.is_gameplay_input_active(),
			"closing F3 restores the gameplay-input policy")
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"overlay closed: clicks are gameplay again")


func test_player_info_loadout_is_equipped_on_initial_spawn() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var menu_host = _shell.get_node("MenuLayer/MenuHost")
	_shell.set_local_player_profile({
		"player_class": 5,
		"primary": "WPN_M4",
		"primary_clips": -1,
		"secondary": "",
		"secondary_clips": -1,
		"accessory": "",
		"accessory_clips": -1,
	})

	menu_host.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await get_tree().process_frame

	assert_eq(world.local_player_weapon_name(), "WPN_M4",
		"the first viewmodel uses the primary selected in PLAYER_INFO")
	var viewmodel_def: PlayerViewmodelDef = world.local_player_viewmodel_def()
	assert_not_null(viewmodel_def)
	assert_eq(viewmodel_def.weapon_name, "WPN_M4")
	assert_eq(viewmodel_def.gfx1, "M4_TEST_FIRST",
		"the selected weapon's first-person model replaces the AK fallback")
	var inventory: Dictionary = world.get_sim().get_local_player_inventory()
	assert_eq(String(inventory.get("equipped_name", "")), "WPN_M4",
		"the spawned simulation equips the same selected primary")


func _make_shell():
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	assert_eq(LANGUAGE_FILES.size() + LOCALRES_FILES.size() + RESOURCE_FILES.size(), 25,
			"the retail-shaped archives contain every minimal fixture resource")
	var language_entries := _fixture_entries(LANGUAGE_FILES)
	var localres_entries := _fixture_entries(LOCALRES_FILES)
	var resource_entries := _fixture_entries(RESOURCE_FILES)
	# The minimal TRN is intentionally CPT-less and cannot create render RIDs.
	# Pack the substituted TRN's baked payload + textures so this regression
	# reaches the exact raw-patch visibility leak.
	for filename in BAKED_TERRAIN_FILES:
		var bytes := FileAccess.get_file_as_bytes(BAKED_TERRAIN_DIR.path_join(filename))
		assert_false(bytes.is_empty(), "%s is available in the baked fixture" % filename)
		resource_entries.append({"name": filename, "bytes": bytes})
	_write_pff(_temp_dir.path_join("language.pff"), language_entries)
	_write_pff(_temp_dir.path_join("localres.pff"), localres_entries)
	_write_pff(_temp_dir.path_join("resource.pff"), resource_entries)

	NovaResourceDirSettings.set_resource_dir(_temp_dir)
	NovaResourceDirSettings.set_expansion("")
	NovaResourceDirSettings.set_game("jo")
	var shell = MAIN_GAME_SCENE.instantiate()
	assert_not_null(shell)
	if shell == null:
		return null
	add_child(shell)
	await get_tree().process_frame
	var menu_host = shell.get_node("MenuLayer/MenuHost")
	assert_eq(menu_host.get_current_menu_file().to_lower(),
			"main.mnu", "the packed fixture boots through the real menu host")
	return shell


func _fixture_entries(filenames: Array) -> Array:
	var entries: Array = []
	for filename in filenames:
		var source := FIXTURE_DIR.path_join(filename)
		# mnml.bms names mnml.trn. Substitute a committed render-capable TRN
		# while retaining that logical archive name.
		if filename == "mnml.trn":
			source = BAKED_TERRAIN_DIR.path_join("Dvxi5.trn")
		var bytes := FileAccess.get_file_as_bytes(source)
		if filename == "weapon.def":
			bytes = LIFECYCLE_WEAPON_DEF.to_utf8_buffer()
		assert_false(bytes.is_empty(), "%s is available in the committed fixture" % filename)
		entries.append({"name": filename, "bytes": bytes})
	return entries


func _wait_for_world_load(world, frame_limit := 240) -> void:
	for _frame in range(frame_limit):
		if world.is_loaded() and not _shell.is_world_loading():
			return
		await get_tree().process_frame


func _wait_for_visible_terrain(terrain, frame_limit := 60) -> void:
	for _frame in range(frame_limit):
		if terrain.get_visible_patch_count() > 0:
			return
		await get_tree().process_frame


func _wait_for_load_to_settle(frame_limit := 240) -> void:
	for _frame in range(frame_limit):
		if not _shell.is_world_loading():
			return
		await get_tree().process_frame


func _assert_loaded(world, terrain, menu_host) -> void:
	assert_false(_shell.is_world_loading(), "the loading gate closes after world_loaded")
	assert_true(world.is_loaded(), "the minimal mission loaded through the full shell")
	assert_same(world.get_resource_root(), menu_host.get_menu().get_resource_root(),
			"menu, loading screen, and GameWorld share one mounted resource session")
	assert_true(world.visible, "the loaded world is presented")
	assert_false(menu_host.visible, "the main menu stays hidden during play")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms")
	assert_not_null(world.get_node_or_null("MissionObjects"))
	assert_gt(terrain.get_visible_patch_count(), 0,
			"the loaded mission made raw RenderingServer terrain patches visible")


func _assert_clean_menu(world, terrain, menu_host, boot_clear: Color) -> void:
	assert_false(_shell.is_world_loading(), "no loading operation leaks into the menu")
	assert_false(world.is_loaded(), "the returned-to-menu world is unloaded")
	assert_false(world.visible, "mission presentation is hidden behind the menu")
	assert_eq(world.get_loaded_mission_file(), "", "the active mission filename is cleared")
	assert_true(menu_host.visible, "the main menu is visible")
	assert_eq(menu_host.get_current_menu_file().to_lower(), "main.mnu")
	assert_eq(terrain.get_visible_patch_count(), 0,
			"raw terrain RIDs obey the hidden GameWorld ancestor")
	assert_true(world.get_current_frame_clear_color().is_equal_approx(boot_clear),
			"the mission sky clear is restored to the boot/menu frame clear")
	assert_null(world.get_node_or_null("MissionObjects"),
			"no mission presentation subtree remains")


# PFF3: 20-byte header, 36-byte entries with 16-byte names, then payloads.
func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "the packed runtime fixture is writable")
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_offset := header_size + entries.size() * entry_size
	file.store_32(header_size)
	file.store_32(0x33464650)
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)
	for entry in entries:
		var bytes: PackedByteArray = entry.bytes
		var name_bytes := String(entry.name).to_utf8_buffer()
		assert_true(name_bytes.size() <= 16, "%s fits the PFF name field" % entry.name)
		file.store_32(0)
		file.store_32(next_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		for index in range(16):
			file.store_8(name_bytes[index] if index < name_bytes.size() else 0)
		file.store_32(0)
		next_offset += bytes.size()
	for entry in entries:
		file.store_buffer(entry.bytes)
	file.close()


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
