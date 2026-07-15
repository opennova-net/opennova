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
	var dumped_path: String = overlay.dump_local_player_pose(pose_path)
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

	_shell.toggle_debug_overlay()
	await get_tree().process_frame
	assert_false(_shell.is_debug_overlay_open())
	assert_true(_shell.is_gameplay_input_active(),
			"closing F3 restores the gameplay-input policy")


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
