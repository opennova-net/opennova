extends GutTest

# Full runtime-shell regression: retail-shaped packed boot resources, public menu intents, and
# the real blocking GameWorld load. It never relies on the test process having /d.

const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH
const FIXTURE_DIR := "res://../assets"
const BAKED_TERRAIN_DIR := "res://../fixtures/godot/dvxi5"
const MAIN_GAME_SCENE := preload("res://game/main_game.tscn")
const MissionPresentation := preload("res://game/world/mission_presentation.gd")
const VegAssetsScript := preload("res://game/terrain/veg_assets.gd")
# Witnessed retail placement (assets/README.md): strings plus the
# mission .bin/.pcx/.lwf family live in language; menus/defs/.bms/.dbf in
# localres; environment, terrain, and terrain art in resource.
const LANGUAGE_FILES := [
	"gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
	"menutxt.bin", "mnml.bin", "mnml.pcx", "mnml.lwf",
]
const LOCALRES_FILES := [
	"items.def", "weapon.def", "ammo.def", "main.mnu", "mp.mnu",
	"game.mnu", "weapon.mnu",
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


class EntityShellHarness:
	extends "res://game/main_game.gd"

	# A real MissionPresentation over an in-memory mission: two authored organics
	# plus the auto-spawned host player supply the AI/present rows the
	# discovery pages walk (the sim-double era ended with the typed
	# DebugEntities.list(sim: Simulation) signature).
	var runtime: MissionPresentation = null

	func ensure_runtime(parent: Node) -> void:
		if runtime != null:
			return
		var mission := MissionData.new()
		assert(mission.create_default() == OK)
		mission.add_entity(3, 0, Vector3(10, 0, -30), Vector3.ZERO)
		mission.add_entity(3, 0, Vector3(20, 0, -40), Vector3.ZERO)
		var container := Node3D.new()
		parent.add_child(container)
		runtime = MissionPresentation.new()
		parent.add_child(runtime)
		runtime.setup(mission, container)

	func _current_runtime():
		return runtime


var _saved_config := PackedByteArray()
var _had_config := false
var _temp_dir := ""
var _shell: Node = null


func test_public_audio_debug_knobs_validate_and_mutate_the_process_mixer() -> void:
	var shell: Node = autofree(MAIN_GAME_SCENE.instantiate())
	var debug_adapter: GameDebugAdapter = autofree(shell.get_game_debug_adapter())
	assert_eq(debug_adapter.debug_set_audio_bus_mute("__missing_bus__", true),
			ERR_INVALID_PARAMETER)
	assert_eq(debug_adapter.debug_set_audio_bus_volume("Master", INF),
			ERR_INVALID_PARAMETER)
	assert_eq(debug_adapter.debug_set_audio_bus_volume(
			"Master", DebugCatalog.AUDIO_BUS_VOLUME_MAX_DB + 0.5),
			ERR_INVALID_PARAMETER)
	var bus := AudioServer.get_bus_index("SFX")
	if bus < 0:
		pass_test("no SFX bus in this layout")
		return
	var previous := {
		"volume": AudioServer.get_bus_volume_db(bus),
		"mute": AudioServer.is_bus_mute(bus),
		"solo": AudioServer.is_bus_solo(bus),
		"bypass": AudioServer.is_bus_bypassing_effects(bus),
	}
	assert_eq(debug_adapter.debug_set_audio_bus_volume("SFX", -14.5), OK)
	assert_eq(debug_adapter.debug_set_audio_bus_mute("SFX", true), OK)
	assert_eq(debug_adapter.debug_set_audio_bus_solo("SFX", true), OK)
	assert_eq(debug_adapter.debug_set_audio_bus_bypass("SFX", true), OK)
	assert_almost_eq(AudioServer.get_bus_volume_db(bus), -14.5, 0.001)
	assert_true(AudioServer.is_bus_mute(bus))
	assert_true(AudioServer.is_bus_solo(bus))
	assert_true(AudioServer.is_bus_bypassing_effects(bus))
	AudioServer.set_bus_volume_db(bus, float(previous["volume"]))
	AudioServer.set_bus_mute(bus, bool(previous["mute"]))
	AudioServer.set_bus_solo(bus, bool(previous["solo"]))
	AudioServer.set_bus_bypass_effects(bus, bool(previous["bypass"]))


func test_game_debug_adapter_handles_every_cataloged_public_control_action() -> void:
	# GameMcpCatalog.PUBLIC_GAME_CONTROL_ACTIONS is the one action list; the
	# adapter's match arms are its implementation. An action added to the catalog
	# without an adapter arm would fall through to ERR_INVALID_PARAMETER here.
	var adapter: GameDebugAdapter = add_child_autofree(GameDebugAdapter.new())
	adapter.configure(
			func(): return null,
			func(): return null,
			func(): return null,
			func(): return "menu",
			func(): return false,
			func(): return false,
			func(): pass,
			func(): pass,
			func(): pass)
	for action in GameMcpCatalog.PUBLIC_GAME_CONTROL_ACTIONS:
		assert_ne(adapter.mcp_game_control(action), ERR_INVALID_PARAMETER,
				"the adapter recognizes cataloged action '%s'" % action)
	assert_eq(adapter.mcp_game_control("warp"), ERR_INVALID_PARAMETER,
			"an uncataloged action is rejected")
	await get_tree().process_frame


func test_mcp_entity_discovery_uses_client_present_order_and_ai_mapping() -> void:
	# The shell stays OFF-tree (its _process expects the packaged scene's
	# children); the runtime + container mount under the test instead.
	var shell: EntityShellHarness = autofree(EntityShellHarness.new())
	shell.ensure_runtime(self)
	var debug_adapter: GameDebugAdapter = autofree(shell.get_game_debug_adapter())

	var page: Dictionary = debug_adapter.get_mcp_game_entities(0, 64)

	# Two authored organics + the auto-spawned host player, in the sim's
	# client-present order with stable indices and live AI identity.
	assert_eq(int(page["total"]), 3)
	var entities: Array = page["entities"]
	assert_eq(entities.size(), 3)
	for i in range(entities.size()):
		assert_eq(int((entities[i] as Dictionary)["index"]), i,
				"page indices follow present order")
	var net_ids := {}
	for raw in entities:
		var row: Dictionary = raw
		assert_true(row.has("net_id"), "every row carries its AI mapping")
		net_ids[int(row["net_id"])] = true
	assert_eq(net_ids.size(), 3, "each present row maps to a distinct entity")
	# Every row in this all-AI world is editable with a live AI mapping and a
	# mission-space position; the single-entity read agrees with its page row.
	for raw in entities:
		var row: Dictionary = raw
		assert_gt(int(row["ai_index"]), -1, "AI rows carry their ai_index")
		assert_true(bool(row["editable"]), "AI rows are editable")
		assert_true(row.has("mission_position"))
	var first: Dictionary = debug_adapter.get_mcp_game_entity(0)
	assert_eq(String(first["name"]), String((entities[0] as Dictionary)["name"]),
			"the single-entity read matches page row 0")
	assert_eq(int(first["ai_index"]), int((entities[0] as Dictionary)["ai_index"]))


func before_each() -> void:
	_had_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) \
			if _had_config else PackedByteArray()
	# A shell booted here sees only the launch flags a case sets through the
	# override (the GUT process carries none; no sibling leftovers).
	LaunchFlags.set_args_override(PackedStringArray([]))
	# The persisted expansion is process-wide state an earlier suite file can leave set, and
	# these cases join a fixture host that has no expansion archives at all. A stale name makes
	# the host advertise an expansion this install cannot mount, which the joiner's preload
	# correctly refuses (D-NET-178) — a failure about suite order, not about what is under test.
	# after_each restores the whole config file, so pinning it here leaks nothing.
	ResourceDirSettings.set_expansion("")


func after_each() -> void:
	# Explicitly release the mounted archive handles before deleting the fixture.
	if is_instance_valid(_shell):
		var world = _shell.get_node_or_null("World")
		if world != null:
			world.unload()
			var world_root = world.get_resource_root()
			if world_root != null:
				world_root.clear()
		var menu_shell = _shell.get_node_or_null("MenuLayer/MenuShell")
		if menu_shell != null:
			var menu_root = menu_shell.get_resource_root()
			if menu_root != null:
				menu_root.clear()
		_shell.queue_free()
		_shell = null
	await get_tree().process_frame
	MusicService.stop_context()
	if not _temp_dir.is_empty():
		TestFs.remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	LaunchFlags.clear_args_override()
	if _had_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_config)
			file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))


func test_boot_gates_flag_mission_when_the_resource_dir_cannot_mount() -> void:
	# A loose-only directory (no packed archives) fails the runtime mount when no
	# --loose-root flag sanctions the loose fallback. The boot continuations
	# (--mission here, --loose-mission in a managed run) must gate on
	# that failure instead of starting a world load with no mounted root.
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	var loose := FileAccess.open(_temp_dir.path_join("Alpha.TRN"), FileAccess.WRITE)
	assert_not_null(loose)
	loose.store_string("loose trn")
	loose.close()
	ResourceDirSettings.set_resource_dir(_temp_dir)
	assert_eq(ResourceDirSettings.get_resource_dir(), _temp_dir,
			"the persisted dir round-trips, so the boot below reads THIS dir")
	ResourceDirSettings.set_game("jo")
	LaunchFlags.set_args_override(PackedStringArray(["--mission", "mnml.bms"]))
	_shell = MAIN_GAME_SCENE.instantiate()
	assert_not_null(_shell)
	add_child(_shell)
	await get_tree().process_frame
	assert_false(_shell.is_world_loading(),
			"no load handoff may start without a mounted root")
	assert_null(_shell.current_resource_root(),
			"the failed mount leaves the shell without a resource session")
	assert_false(_shell.get_node("World").is_loaded())
	var state: Dictionary = _shell.get_game_debug_adapter().get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "menu",
			"the shell stays on the front-end state the picker contract needs")


func test_shell_exit_releases_runtime_texture_caches_before_renderer_shutdown() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var resource_root: ResourceRoot = _shell.current_resource_root()
	assert_not_null(resource_root)
	var texture: Texture2D = resource_root.load_texture("mnml_c.tga")
	assert_not_null(texture, "the packed runtime root owns a decoded ImageTexture")
	# The compiled frame resolves the claim cursor (screen default) on the first
	# pump; one mouse sample installs the retail cursor process-wide via Input.
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	menu_shell.get_driver().process_mouse(Vector2(400, 300), false)
	var cursor_texture: Texture2D = menu_shell.get_frame().get_cursor_texture()
	assert_not_null(cursor_texture,
			"the retail-shaped main menu installs its decoded custom cursor")
	var weak_cursor: WeakRef = weakref(cursor_texture)
	cursor_texture = null
	var water: Water = _shell.get_node("World/Water")
	var water_material: ShaderMaterial = water.get_water_material()
	var water_color_texture: Texture2D = water_material.get_shader_parameter(
			"u_noise_color")
	var water_normal_texture: Texture2D = water_material.get_shader_parameter(
			"u_noise_normal")
	var weak_water_color: WeakRef = weakref(water_color_texture)
	var weak_water_normal: WeakRef = weakref(water_normal_texture)
	water_color_texture = null
	water_normal_texture = null
	assert_not_null(weak_water_color.get_ref(),
			"the retained water graph owns its live color ImageTexture")
	assert_not_null(weak_water_normal.get_ref(),
			"the retained water graph owns its live normal ImageTexture")
	VegAssetsScript.list_graphics(resource_root, true)
	var weak_texture: WeakRef = weakref(texture)
	texture = null
	assert_not_null(weak_texture.get_ref(),
			"the runtime root retains the decoded texture")
	assert_gt(VegAssetsScript.cache_entry_count(), 0,
			"the process-static vegetation registry is populated before exit")

	_shell.queue_free()
	_shell = null
	await get_tree().process_frame

	assert_true(resource_root.get_root_dir().is_empty(),
			"MainGame exit clears the mounted root before extension deinitialization")
	assert_eq(VegAssetsScript.cache_entry_count(), 0,
			"MainGame exit clears the process-static renderer resource cache")
	assert_null(weak_texture.get_ref(),
			"the cached ImageTexture dies while RenderingServer is still alive")
	assert_null(weak_cursor.get_ref(),
			"the process-global menu cursor dies before RenderingServer shutdown")
	assert_null(weak_water_color.get_ref(),
			"the water color ImageTexture dies before RenderingServer shutdown")
	assert_null(weak_water_normal.get_ref(),
			"the water normal ImageTexture dies before RenderingServer shutdown")


func test_shutdown_settlement_releases_join_target_awaited_by_loading_barrier() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	menu_shell.get_driver().process_mouse(Vector2(400, 300), false)
	var cursor_texture: Texture2D = menu_shell.get_frame().get_cursor_texture()
	assert_not_null(cursor_texture)
	var weak_cursor: WeakRef = weakref(cursor_texture)
	cursor_texture = null
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 9
	target.server_name = "shutdown barrier probe"
	var weak_target: WeakRef = weakref(target)
	_shell.join_lan_server(target)
	assert_true(_shell.is_world_loading(),
			"the bound JoinTarget enters the two-frame loading-screen barrier")
	target = null

	var load_operation: WorldLoadOperation = _shell.begin_runtime_shutdown()
	assert_not_null(load_operation)
	if not load_operation.is_settled():
		await load_operation.settled

	assert_null(weak_target.get_ref(),
			"settlement proves the outer load coroutine released its bound JoinTarget")
	_shell.finish_runtime_shutdown()
	assert_null(menu_shell.get_frame().get_cursor_texture(),
			"the released frame retains no cursor texture")
	assert_false(menu_shell.get_frame().is_configured(),
			"finish_runtime_shutdown leaves the frame unconfigured")
	assert_null(weak_cursor.get_ref(),
			"the cooperative shutdown path drops its global custom cursor before exit")


func test_picker_pick_persists_only_for_unmanaged_runs() -> void:
	# The picker's accept leg (apply_picked_resource_dir, the ADR-0018 seam
	# behind _on_dir_selected): a process-local launch must never write its
	# picker escape into the game's resource_dir preference, an unmanaged
	# first-launch pick must, and an unmountable pick changes nothing.
	_shell = await _make_shell()
	if _shell == null:
		return
	var picked_dir := _temp_dir.path_join("picked")
	assert_eq(DirAccess.make_dir_recursive_absolute(picked_dir), OK)
	_write_pff(picked_dir.path_join("language.pff"), _fixture_entries(LANGUAGE_FILES))
	_write_pff(picked_dir.path_join("localres.pff"), _fixture_entries(LOCALRES_FILES))
	_write_pff(picked_dir.path_join("resource.pff"), _fixture_entries(RESOURCE_FILES))
	assert_eq(ResourceDirSettings.get_resource_dir(), _temp_dir)

	assert_true(_shell.apply_picked_resource_dir(picked_dir, true),
			"a process-local pick mounts and enters the menu")
	assert_eq(ResourceDirSettings.get_resource_dir(), _temp_dir,
			"a process-local pick never writes the game's persisted key")
	assert_false(_shell.apply_picked_resource_dir(
			_temp_dir.path_join("does-not-exist"), false),
			"an unmountable pick is refused")
	assert_eq(ResourceDirSettings.get_resource_dir(), _temp_dir,
			"a refused pick changes nothing")
	assert_true(_shell.apply_picked_resource_dir(picked_dir, false),
			"an unmanaged pick mounts")
	assert_eq(ResourceDirSettings.get_resource_dir(), picked_dir,
			"the unmanaged first-launch pick persists")


func test_mount_boot_root_falls_back_to_the_loose_authoring_mount() -> void:
	# The ONED run contract: with --loose-root, a directory
	# holding none of the packed archives mounts as the loose file set being
	# authored; without it, retail's no-archives fatal stands. Parameterized
	# entry so the contract is testable without process arguments (ADR 0018).
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	var loose_dir := _temp_dir.path_join("loose")
	var packed_dir := _temp_dir.path_join("packed")
	assert_eq(DirAccess.make_dir_recursive_absolute(loose_dir), OK)
	assert_eq(DirAccess.make_dir_recursive_absolute(packed_dir), OK)
	var loose := FileAccess.open(loose_dir.path_join("Alpha.TRN"), FileAccess.WRITE)
	assert_not_null(loose)
	loose.store_string("loose trn")
	loose.close()
	# The packed variant satisfies the boot manifest the same way _make_shell's
	# fixture does — a partial runtime install would report missing boot
	# resources as engine errors and fail this test about mounting.
	_write_pff(packed_dir.path_join("language.pff"), _fixture_entries(LANGUAGE_FILES))
	_write_pff(packed_dir.path_join("localres.pff"), _fixture_entries(LOCALRES_FILES))
	_write_pff(packed_dir.path_join("resource.pff"), _fixture_entries(RESOURCE_FILES))
	ResourceDirSettings.set_game("jo")

	assert_null(BootRootMount.mount(loose_dir, false),
			"without the flag a loose-only dir keeps retail's fatal mount error")
	var fallback: ResourceRoot = BootRootMount.mount(loose_dir, true)
	assert_not_null(fallback, "--loose-root plays the loose authoring dir")
	if fallback != null:
		assert_false(fallback.is_runtime_mount(),
				"the fallback is a loose mount, not a packed install")
		assert_eq(fallback.read_file("alpha.trn").get_string_from_utf8(), "loose trn")
		fallback.clear()
	var packed: ResourceRoot = BootRootMount.mount(packed_dir, true)
	assert_not_null(packed)
	if packed != null:
		assert_true(packed.is_runtime_mount(),
				"a dir with archives keeps the normal runtime mount even under the flag")
		packed.clear()


func test_bundled_game_dir_is_the_exe_dir_only_when_it_carries_a_boot_archive() -> void:
	# A shipped opennova.exe sits IN its game dir, retail-style. A dir with no
	# boot-table archive (a dev run: the Godot binary's own dir) is not a game.
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	assert_eq(LaunchFlags.bundled_game_dir(_temp_dir), "",
			"no boot archive -> not a game dir")
	assert_eq(LaunchFlags.bundled_game_dir(""), "",
			"an empty probe dir is never a game dir")
	_write_pff(_temp_dir.path_join("localres.pff"), [])
	assert_eq(LaunchFlags.bundled_game_dir(_temp_dir), _temp_dir,
			"any boot-table archive makes the exe dir the default game dir")


func test_boot_defaults_to_the_bundled_game_dir_and_never_persists_it() -> void:
	# The shipped-zip flow: no flag, nothing configured, the exe's own dir carries
	# the packed game -> that is the boot dir. It is a per-boot default, not a
	# pick: the settings key stays untouched, and a configured dir always wins.
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	var game_dir := _temp_dir.path_join("game")
	var configured_dir := _temp_dir.path_join("configured")
	assert_eq(DirAccess.make_dir_recursive_absolute(game_dir), OK)
	assert_eq(DirAccess.make_dir_recursive_absolute(configured_dir), OK)
	_write_pff(game_dir.path_join("localres.pff"), [])
	var saved_dir := ResourceDirSettings.get_resource_dir()
	var saved_override: String = LaunchFlags.get_bundled_probe_override()
	ResourceDirSettings.set_resource_dir("")

	LaunchFlags.set_bundled_probe_override(game_dir)
	assert_eq(LaunchFlags.boot_resource_dir(ResourceDirSettings.get_resource_dir()), game_dir,
			"unconfigured boot resolves to the bundled game dir")
	assert_eq(ResourceDirSettings.get_resource_dir(), "",
			"the default is never written to the shared settings key")

	ResourceDirSettings.set_resource_dir(configured_dir)
	assert_eq(LaunchFlags.boot_resource_dir(ResourceDirSettings.get_resource_dir()), configured_dir,
			"an explicit configured dir wins over the bundle")
	ResourceDirSettings.set_resource_dir(saved_dir)
	LaunchFlags.set_bundled_probe_override(saved_override)


func test_boot_falls_through_to_bundled_loose_assets_and_blesses_only_them() -> void:
	# The dev-zip flow: no flag, nothing configured, no packed game beside the exe, but
	# an assets/ sibling of loose sources -> that is the boot dir, and it (alone) may
	# take the loose authoring mount without --loose-root. A picked or persisted loose
	# dir keeps retail's no-archives fatal.
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	var exe_dir := _temp_dir.path_join("exe")
	var assets_dir := exe_dir.path_join("assets")
	var picked_dir := _temp_dir.path_join("picked")
	assert_eq(DirAccess.make_dir_recursive_absolute(assets_dir), OK)
	assert_eq(DirAccess.make_dir_recursive_absolute(picked_dir), OK)
	var saved_dir := ResourceDirSettings.get_resource_dir()
	var saved_override: String = LaunchFlags.get_bundled_probe_override()
	ResourceDirSettings.set_resource_dir("")

	LaunchFlags.set_bundled_probe_override(exe_dir)
	assert_eq(LaunchFlags.boot_resource_dir(ResourceDirSettings.get_resource_dir()), assets_dir,
			"with no packed game beside the exe, the loose assets/ sibling is the boot dir")
	assert_true(LaunchFlags.boot_loose_allowed(assets_dir),
			"exactly that dir is blessed for the loose mount")
	assert_false(LaunchFlags.boot_loose_allowed(picked_dir),
			"any other dir keeps retail's no-archives fatal without --loose-root")
	assert_eq(ResourceDirSettings.get_resource_dir(), "", "never persisted")

	# The packed bundle outranks the loose sibling when both are present (tagged zip
	# carrying stray sources still boots the packed game).
	_write_pff(exe_dir.path_join("localres.pff"), [])
	assert_eq(LaunchFlags.boot_resource_dir(ResourceDirSettings.get_resource_dir()), exe_dir,
			"a boot archive beside the exe wins over the assets/ sibling")
	ResourceDirSettings.set_resource_dir(saved_dir)
	LaunchFlags.set_bundled_probe_override(saved_override)


func test_mission_return_restores_menu_frame_and_supports_another_load() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("Terrain")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	var boot_clear: Color = world.get_current_frame_clear_color()
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)
	# Once the shell owns a mounted resource session, later persistence changes
	# cannot redirect one consumer into a separately remounted VFS.
	var detached_resource_dir := _temp_dir.path_join("detached")
	assert_eq(DirAccess.make_dir_recursive_absolute(detached_resource_dir), OK)
	ResourceDirSettings.set_resource_dir(detached_resource_dir)
	assert_eq(ResourceDirSettings.get_resource_dir(), detached_resource_dir,
			"the persisted directory now points away from the mounted fixture")

	# This is the same public intent emitted by the mission-list ACCEPT command.
	menu_shell.start_requested.emit("mnml.bms")
	assert_true(_shell.is_world_loading(),
			"the loading handoff is pending before the blocking load starts")
	assert_true(_shell.has_loading_background(),
			"the deferred handoff decodes archive-only mnml.pcx from language.pff")
	assert_false(world.is_loaded(),
			"the world cannot finish in the callback that mounts the loading UI")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_shell)
	assert_false(world.get_current_frame_clear_color().is_equal_approx(boot_clear),
			"the loaded mission exercised a distinct frame clear")

	# The pause menu's ABORT command emits this public intent.
	menu_shell.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)

	# Hiding retained terrain/environment state must not break the next load.
	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_shell)
	menu_shell.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)

	# Failed deferred loads obey the same rollback contract.
	menu_shell.start_requested.emit("missing-mission.bms")
	assert_true(_shell.is_world_loading(), "a failed load enters the deferred handoff")
	await _wait_for_load_to_settle()
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)


func test_mcp_screen_verbs_reach_pause_and_armory_over_a_loaded_world() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	var adapter: GameDebugAdapter = _shell.get_game_debug_adapter()
	# In the front-end menu neither in-world screen exists.
	assert_eq(adapter.mcp_game_control("open_ingame_menu"), ERR_UNAVAILABLE,
			"the pause overlay needs a loaded world")
	assert_eq(adapter.mcp_game_control("open_armory"), ERR_UNAVAILABLE,
			"the armory needs a loaded world")

	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	assert_true(world.is_loaded(), "the minimal mission loaded through the full shell")

	# The ESC-pause leg through the MCP verb: game.mnu over the kept world.
	assert_eq(adapter.mcp_game_control("open_ingame_menu"), OK)
	var state: Dictionary = adapter.get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "paused",
			"open_ingame_menu takes the ESC pause leg")
	assert_true(menu_shell.visible, "the pause overlay is presented")
	var snapshot: Dictionary = menu_shell.menu_snapshot(false)
	assert_eq(String(snapshot["file"]).to_lower(), "game.mnu",
			"the overlay is the in-game menu file")
	assert_true(bool(snapshot["in_game"]),
			"the shell marks the overlay as the in-game menu")
	assert_eq(adapter.mcp_game_control("open_ingame_menu"), OK,
			"open is idempotent while already paused")
	assert_eq(adapter.mcp_game_control("open_armory"), ERR_UNAVAILABLE,
			"the armory does not stack over the pause overlay")
	assert_eq(adapter.mcp_game_control("resume"), OK)
	state = adapter.get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "world", "resume hands play back")
	assert_false(menu_shell.visible, "the overlay is hidden after resume")

	# The armory over live play (the presenter's direct-open seam; no armory
	# volume is authored in mnml.bms, and the verb deliberately skips the
	# useitem key's zone gate).
	assert_eq(adapter.mcp_game_control("open_armory"), OK)
	state = adapter.get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "armory",
			"open_armory opens weapon.mnu's WEAPON screen over live play")
	assert_eq(adapter.mcp_game_control("open_ingame_menu"), ERR_UNAVAILABLE,
			"ESC in the armory resumes, so the pause verb requires resume first")
	assert_eq(adapter.mcp_game_control("open_armory"), OK,
			"open is idempotent while the armory is up")
	assert_eq(adapter.mcp_game_control("resume"), OK)
	state = adapter.get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "world",
			"resume closes the armory and hands play back")

	menu_shell.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_eq(adapter.mcp_game_control("open_armory"), ERR_UNAVAILABLE,
			"the unloaded world takes the armory verb back off the table")


func test_join_loading_stays_raised_until_authoritative_admission() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")

	# The advertised filename is deliberately absent from the client install.
	# Retail joins from the exact S2C 0x0B header + streamed world/0x45 terrain
	# overlay; reopening an advertised local .bms is the D-NET-194 regression.
	var mission := MissionData.new()
	assert_eq(mission.open_file(ProjectSettings.globalize_path(
			FIXTURE_DIR.path_join("mnml.bms"))), OK)
	var advertised_file := "wire_only_mnml.bms"
	var client_root: ResourceRoot = _shell.current_resource_root()
	assert_not_null(client_root, "the menu boot mounted the client resource session")
	if client_root == null:
		return
	assert_false(client_root.has_file(advertised_file),
			"the join proof cannot accidentally fall back to a local mission body")
	var host_body_records := 0
	for kind in range(4):
		host_body_records += mission.get_entity_count(kind)
	assert_gt(host_body_records, 0,
			"the host fixture includes authored pools that must reach the wire-only join")
	var host := Simulation.new()
	host.configure_host_session({
		"server_name": "Loading Hold Host",
		"mission_name": "Minimal",
		"mission_file": advertised_file,
		"gametype": 0x30020,
		"max_players": 4,
		# The lifecycle fixture ships base archives only. Say so on the wire: an unset field
		# leaves the host advertising whatever expansion this machine last persisted, and the
		# joiner's preload then correctly refuses a data set this install cannot mount
		# (D-NET-178) — a failure about machine state, not about the loading-screen hold.
		"expansion": "",
	})
	assert_true(host.enable_host_listen(0))
	var streamed_til := _til_bytes_for_cell(4)
	host.set_terrain_til_data(streamed_til)
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

	assert_true(bool(observed["local_load"]), "the joiner completed its wire-header world load")
	assert_true(bool(observed["held"]),
			"local world_loaded cannot reveal the joiner before host admission")
	assert_false(_shell.is_world_loading(),
			"the loading screen releases after the authoritative join edge")
	assert_true(world.visible, "the admitted world is revealed")
	assert_true(world.get_sim() != null and world.get_sim().is_joined_in_match())
	assert_eq(world.get_sim().get_join_terrain_til_state(),
			Simulation.JOIN_TERRAIN_TIL_COMPLETE)
	assert_eq(world.get_sim().get_join_terrain_til(), streamed_til,
			"the structurally complete wire image remains byte-exact")
	assert_eq(world.get_loaded_mission_file(), advertised_file)
	var tile_info := world.get_node("Terrain").tile_info_override as TerrainTileInfo
	assert_not_null(tile_info, "the host's paged S2C 0x45 rebuilt the mission TIL")
	if tile_info != null:
		assert_true(tile_info.blocks_foliage(72.0, 8.0, 2.0),
				"the streamed host tile override, not a local sidecar, reached terrain")
	var wire_stats: WirePresentStats = world.get_runtime().get_wire_present_stats()
	assert_gt(wire_stats.live, 0,
			"host-only pools reach native materialization and wire presentation from the load/live stream")
	assert_gt(wire_stats.spawned + wire_stats.unresolved, 0,
			"the renderer attempted every streamed row even when this minimal fixture lacks its .3di")
	host.free()


func test_join_rejects_a_truncated_terrain_stream_before_reveal() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("Terrain")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	var boot_clear: Color = world.get_current_frame_clear_color()

	var mission := MissionData.new()
	assert_eq(mission.open_file(ProjectSettings.globalize_path(
			FIXTURE_DIR.path_join("mnml.bms"))), OK)
	var advertised_file := "wire_truncated_til.bms"
	var host := Simulation.new()
	host.configure_host_session({
		"server_name": "Truncated TIL Host",
		"mission_name": "Minimal",
		"mission_file": advertised_file,
		"gametype": 0x30020,
		"max_players": 4,
		"expansion": "",
	})
	assert_true(host.enable_host_listen(0))
	# Advertise two records but provide one. The host emits the canonical first
	# page [0,1), then has no second page; admission must see Receiving, never
	# reinterpret it as the valid no-0x45 Absent case.
	var truncated_til := _til_bytes_for_cell(4)
	truncated_til.encode_u32(4, 2)
	host.set_terrain_til_data(truncated_til)
	assert_true(host.load_from_mission_data(mission))

	var failures: Array[String] = []
	var revealed := false
	var observed_receiving := false
	world.load_failed.connect(func(reason: String) -> void:
		failures.append(reason)
	)
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = host.get_host_listen_port()
	target.server_name = "browse-time hint"
	_shell.join_lan_server(target)
	for _frame in range(1200):
		host.step()
		await get_tree().process_frame
		revealed = revealed or world.visible
		var join_sim: Simulation = world.get_sim()
		if join_sim != null and join_sim.is_joiner():
			observed_receiving = observed_receiving or \
					join_sim.get_join_terrain_til_state() == \
					Simulation.JOIN_TERRAIN_TIL_RECEIVING
		if not failures.is_empty() and not _shell.is_world_loading():
			break

	assert_eq(failures.size(), 1,
			"one fail-closed edge owns the incomplete terrain stream")
	assert_true(observed_receiving,
			"the admitted join exposed a begun but incomplete 0x45 stream")
	if not failures.is_empty():
		assert_string_contains(failures[0], "streamed mission assets")
	assert_false(revealed,
			"a Receiving terrain stream cannot reveal the admitted world")
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)
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
	var terrain = world.get_node("Terrain")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	var boot_clear: Color = world.get_current_frame_clear_color()
	assert_true(world.has_signal("session_lost"),
			"GameWorld publishes the in-match session-loss edge")

	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_shell)

	world.session_lost.emit("lost connection to the host (no traffic for 120 seconds)")
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)

	# The shell is usable again straight afterwards: a loss is an abort, not a wedge.
	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_shell)
	menu_shell.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)


func test_dev_tools_suspend_input_without_stopping_the_world() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("Terrain")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	var runtime = world.get_runtime()
	assert_not_null(runtime)
	if runtime == null:
		return

	# The open state is the shell-facing contract and works headless (no ImGui
	# context attaches under the headless DisplayServer); only drawing needs one.
	var dev_tools: DevTools = _shell.get_dev_tools()
	assert_not_null(dev_tools, "the shell owns its dev tools from construction")
	assert_false(dev_tools.is_available(), "headless runs never attach an ImGui context")
	assert_true(_shell.is_gameplay_input_active())
	var tick_before := int(runtime.get_sim().get_logic_tick())
	dev_tools.toggle()
	assert_true(_shell.is_dev_tools_open())
	assert_false(_shell.is_gameplay_input_active(),
			"the public input gate closes on the same F3 edge")
	var board: FrameStats = _shell.get_frame_stats()
	assert_true(board.is_capture_active(),
			"the Stats window (open by default) arms the board's capture on the F3 edge")
	for _frame in range(4):
		await get_tree().process_frame
	assert_false(_shell.is_gameplay_input_active(),
			"F3 submits neutral player input while the tools own the cursor")
	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE,
			"F3 frees the mouse for the tool windows")
	assert_gt(int(runtime.get_sim().get_logic_tick()), tick_before,
			"the live world keeps ticking under the tools")

	# The pick stack, end to end: the world load installed the highlight view
	# for the shell's list, F3-open flipped the click catcher on, and the
	# sim-authoritative ray is terrain-occluded exactly like a bullet.
	assert_not_null(world.get_node_or_null("PickDebug"),
			"the world renders the shell's pick list")
	assert_not_null(world.get_node_or_null("PickClickCatcher"),
			"dev tools open: world clicks ray-pick")
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

	dev_tools.set_open(false)
	await get_tree().process_frame
	assert_false(_shell.is_dev_tools_open())
	assert_true(_shell.is_gameplay_input_active(),
			"closing the tools restores the gameplay-input policy")
	assert_false(board.is_capture_active(),
			"closing the tools releases the board's capture")
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"closing removes the click picker")

	dev_tools.toggle()
	await get_tree().process_frame
	assert_not_null(world.get_node_or_null("PickClickCatcher"))
	dev_tools.toggle()
	await get_tree().process_frame
	assert_false(_shell.is_dev_tools_open())
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"F3 again removes the click picker through the same open edge")

	menu_shell.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_false(_shell.is_dev_tools_open(),
			"returning to the menu cannot reopen closed tools")
	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	assert_false(_shell.is_dev_tools_open(),
			"the next mission keeps the tools' own closed lifecycle")


func test_player_info_loadout_is_equipped_on_initial_spawn() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	_shell.set_local_player_profile({
		"player_class": 5,
		"primary": "WPN_M4",
		"primary_clips": -1,
		"secondary": "",
		"secondary_clips": -1,
		"accessory": "",
		"accessory_clips": -1,
	})

	menu_shell.start_requested.emit("mnml.bms")
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
	assert_eq(LANGUAGE_FILES.size() + LOCALRES_FILES.size() + RESOURCE_FILES.size(), 27,
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

	ResourceDirSettings.set_resource_dir(_temp_dir)
	ResourceDirSettings.set_expansion("")
	ResourceDirSettings.set_game("jo")
	var shell = MAIN_GAME_SCENE.instantiate()
	assert_not_null(shell)
	if shell == null:
		return null
	add_child(shell)
	await get_tree().process_frame
	var menu_shell = shell.get_node("MenuLayer/MenuShell")
	assert_eq(menu_shell.get_current_menu_file().to_lower(),
			"main.mnu", "the packed fixture boots through the real menu shell")
	return shell


func _fixture_entries(filenames: Array) -> Array:
	var entries: Array = []
	for filename in filenames:
		var source := FIXTURE_DIR.path_join(filename)
		# mnml.bms names mnml.trn. Substitute a committed render-capable TRN
		# while retaining that logical archive name.
		if filename == "mnml.trn":
			source = BAKED_TERRAIN_DIR.path_join("Dvxi5.trn")
		# The in-world screens (ESC pause overlay + armory) pack the real JO
		# menu fixtures under their retail archive names.
		elif filename == "game.mnu":
			source = "res://../fixtures/mnu/jo_game.mnu"
		elif filename == "weapon.mnu":
			source = "res://../fixtures/mnu/jo_weapon.mnu"
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


func _assert_loaded(world, terrain, menu_shell) -> void:
	assert_false(_shell.is_world_loading(), "the loading gate closes after world_loaded")
	assert_true(world.is_loaded(), "the minimal mission loaded through the full shell")
	assert_same(world.get_resource_root(), menu_shell.get_resource_root(),
			"menu, loading screen, and GameWorld share one mounted resource session")
	assert_true(world.visible, "the loaded world is presented")
	assert_false(menu_shell.visible, "the main menu stays hidden during play")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms")
	assert_not_null(world.get_node_or_null("MissionObjects"))
	assert_gt(terrain.get_visible_patch_count(), 0,
			"the loaded mission made raw RenderingServer terrain patches visible")


func _assert_clean_menu(world, terrain, menu_shell, boot_clear: Color) -> void:
	assert_false(_shell.is_world_loading(), "no loading operation leaks into the menu")
	assert_false(world.is_loaded(), "the returned-to-menu world is unloaded")
	assert_false(world.visible, "mission presentation is hidden behind the menu")
	assert_eq(world.get_loaded_mission_file(), "", "the active mission filename is cleared")
	assert_true(menu_shell.visible, "the main menu is visible")
	assert_eq(menu_shell.get_current_menu_file().to_lower(), "main.mnu")
	assert_eq(terrain.get_visible_patch_count(), 0,
			"raw terrain RIDs obey the hidden GameWorld ancestor")
	assert_true(world.get_current_frame_clear_color().is_equal_approx(boot_clear),
			"the mission sky clear is restored to the boot/menu frame clear")
	assert_null(world.get_node_or_null("MissionObjects"),
			"no mission presentation subtree remains")


func _til_bytes_for_cell(cell_x: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(28)
	bytes.encode_u32(0, 0x74696c30)
	bytes.encode_u32(4, 1)
	bytes.encode_u32(16, cell_x * (16 << 16))
	bytes.encode_u32(20, 0)
	bytes[24] = 1
	return bytes


# The shared PFF3 fixture writer (TestPff.write), asserted here.
func _write_pff(path: String, entries: Array) -> void:
	assert_eq(TestPff.write(path, entries), OK, "PFF fixture should be writable: %s" % path)
