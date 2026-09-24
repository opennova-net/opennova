extends GutTest

# Full runtime-shell regression: retail-shaped packed boot resources, public menu intents, and
# the real blocking GameWorld load. It never relies on the test process having /d.

const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH
static var FIXTURE_DIR := RuntimeFixture.directory()
const MAIN_GAME_SCENE := preload("res://game/main_game.tscn")
# The packed shell recipe (the retail-shaped archive layout, the baked Tmap
# terrain, the lifecycle-only weapon.def) lives on WorldFixture.boot_shell.


# The shell the entity-discovery adapter reads: a GameShell answering one real
# MissionRoot (rule 11's sanctioned fake: public verbs only).
class EntityRuntimeShell:
	extends GameShell

	var runtime: MissionRoot = null

	func get_runtime() -> MissionRoot:
		return runtime


# A real MissionRoot over an in-memory mission: two authored organics
# plus the auto-spawned host player supply the AI/registry rows the discovery
# pages walk (the sim-double era ended when discovery became the engine's typed
# Simulation.entity_directory()).
func _entity_runtime(parent: Node) -> MissionRoot:
	var mission := MissionData.new()
	assert(mission.create_default() == OK)
	mission.add_entity(3, 0, Vector3(10, 0, -30), Vector3.ZERO)
	mission.add_entity(3, 0, Vector3(20, 0, -40), Vector3.ZERO)
	var container := Node3D.new()
	parent.add_child(container)
	var runtime := MissionRoot.new()
	parent.add_child(runtime)
	runtime.setup(mission, container)
	return runtime


var _saved_config := PackedByteArray()
var _had_config := false
var _temp_dir := ""
var _shell: Node = null


func test_public_audio_debug_knobs_validate_and_mutate_the_process_mixer() -> void:
	# The audio rows of the shell's debug-control table bind the AudioServer
	# in C++ (ADR 0043 d12): their argument schemas refuse before the mixer.
	var shell: Node = autofree(MAIN_GAME_SCENE.instantiate())
	var debug_adapter: GameDebugAdapter = autofree(shell.get_game_debug_adapter())
	var controls: DebugControlTable = debug_adapter.get_debug_controls()
	assert_eq(int(controls.invoke(&"set_audio_bus_mute", ["__missing_bus__", true]).error),
			ERR_INVALID_PARAMETER)
	assert_eq(int(controls.invoke(&"set_audio_bus_volume", ["Master", INF]).error),
			ERR_INVALID_PARAMETER)
	assert_eq(int(controls.invoke(&"set_audio_bus_volume",
			["Master", DebugControlTable.AUDIO_BUS_VOLUME_MAX_DB + 0.5]).error),
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
	assert_eq(int(controls.invoke(&"set_audio_bus_volume", ["SFX", -14.5]).error), OK)
	assert_eq(int(controls.invoke(&"set_audio_bus_mute", ["SFX", true]).error), OK)
	assert_eq(int(controls.invoke(&"set_audio_bus_solo", ["SFX", true]).error), OK)
	assert_eq(int(controls.invoke(&"set_audio_bus_bypass", ["SFX", true]).error), OK)
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
	# The null shell (GameShell's base) answers "menu"-less unavailability for
	# every supplier; the catalog contract only needs the arms to exist.
	adapter.configure(autofree(GameShell.new()))
	for action in GameMcpCatalog.PUBLIC_GAME_CONTROL_ACTIONS:
		assert_ne(adapter.mcp_game_control(action), ERR_INVALID_PARAMETER,
				"the adapter recognizes cataloged action '%s'" % action)
	assert_eq(adapter.mcp_game_control("warp"), ERR_INVALID_PARAMETER,
			"an uncataloged action is rejected")
	await get_tree().process_frame


func test_mcp_entity_discovery_uses_client_present_order_and_ai_mapping() -> void:
	# The runtime + container mount under the test; the adapter reads them
	# through a GameShell fake, no MainGame instance involved.
	var shell: EntityRuntimeShell = autofree(EntityRuntimeShell.new())
	shell.runtime = _entity_runtime(self)
	var debug_adapter: GameDebugAdapter = autofree(GameDebugAdapter.new())
	debug_adapter.configure(shell)

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
	# The vegetation asset caches are the world's foliage dispatcher's own
	# state (they die with the world); the shell's exit leg empties them
	# explicitly, observed here through the idempotent leg EXIT_TREE runs.
	var world := _shell.get_node("World") as GameWorld
	var dispatcher: FoliageDispatcher = world.get_foliage_dispatcher()
	dispatcher.list_graphics(resource_root, true)
	var weak_texture: WeakRef = weakref(texture)
	texture = null
	assert_not_null(weak_texture.get_ref(),
			"the runtime root retains the decoded texture")
	assert_gt(dispatcher.asset_cache_entry_count(), 0,
			"the world's vegetation asset caches are populated before exit")

	_shell.finish_runtime_shutdown()
	assert_eq(dispatcher.asset_cache_entry_count(), 0,
			"MainGame exit clears the world's vegetation asset caches")
	_shell.queue_free()
	_shell = null
	await get_tree().process_frame

	assert_true(resource_root.get_root_dir().is_empty(),
			"MainGame exit clears the mounted root before extension deinitialization")
	assert_null(weak_texture.get_ref(),
			"the cached ImageTexture dies while RenderingServer is still alive")
	assert_null(weak_cursor.get_ref(),
			"the process-global menu cursor dies before RenderingServer shutdown")
	assert_null(weak_water_color.get_ref(),
			"the water color ImageTexture dies before RenderingServer shutdown")
	assert_null(weak_water_normal.get_ref(),
			"the water normal ImageTexture dies before RenderingServer shutdown")


func test_explicit_spectator_choice_ignores_team_password() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = 9
	target.server_flags = (JoinTarget.FLAG_ALLOW_SPECTATORS
			| JoinTarget.FLAG_BLUE_PASSWORD | JoinTarget.FLAG_RED_PASSWORD)
	target.join_role = JoinTarget.ROLE_SPECTATOR
	target.role_explicit = true
	_shell.join_lan_server(target)
	assert_true(_shell.is_world_loading(),
			"an explicit spectator starts loading without supplying an irrelevant team password")
	var operation: WorldLoadOperation = _shell.begin_runtime_shutdown()
	if operation != null and not operation.is_settled():
		await operation.settled
	_shell.finish_runtime_shutdown()


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
	# This fixture exercises shutdown during an already-decided player join;
	# unknown direct targets now perform spectator discovery before loading.
	target.role_explicit = true
	var weak_target: WeakRef = weakref(target)
	_shell.join_lan_server(target)
	assert_true(_shell.is_world_loading(),
			"the bound JoinTarget enters the two-frame loading-screen barrier")
	target = null

	assert_same(_shell.get_game_debug_adapter().get_shell(), _shell,
			"the configured debug adapter holds MainGame before shutdown")
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
	assert_null(_shell.get_game_debug_adapter().get_shell(),
			"shutdown releases the shell from the debug adapter")


func test_mount_boot_root_falls_back_to_the_loose_authoring_mount() -> void:
	# The explicit loose-root contract: with --loose-root, a directory
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
	# The packed variant satisfies the boot manifest the same way boot_shell's
	# fixture does — a partial runtime install would report missing boot
	# resources as engine errors and fail this test about mounting.
	WorldFixture.stage_shell_archives(self, packed_dir, false)
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
	ConfigStore.write(STATE_CONFIG_PATH, "resources", "resource_dir", detached_resource_dir)
	assert_eq(LaunchFlags.resource_dir(), _temp_dir,
			"a stale saved directory cannot redirect the launch root")

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

	# The pause menu's CONFIRM_YES command (ABORT's "Are you sure?" panel)
	# emits this public intent.
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
	# The ESC overlay and the armory are the shipped game.mnu / weapon.mnu.
	for rel in ["mnu/jo_game.mnu", "mnu/jo_weapon.mnu"]:
		if RetailData.fixture(rel).is_empty():
			pending(RetailData.fixture_pending_text(rel))
			return
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
	assert_eq(String(state["session"]["role"]), "single_player",
			"game_state carries the inmatch session role")
	assert_eq(String(state["session"]["state"]), "paused",
			"the SP shell pause runs through inmatch::State::Paused")
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
	assert_eq(String(state["session"]["state"]), "running",
			"resume leaves the inmatch session running")
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

	# F3's Resume is the debug-control table's runtime_transport row: it takes
	# the same shell resume leg, so a pause overlay left up closes too.
	assert_eq(adapter.mcp_game_control("open_ingame_menu"), OK)
	var resumed := adapter.get_debug_controls().invoke(&"runtime_transport", ["resume"], true)
	assert_eq(int(resumed.error), OK, "the transport row resumes over the pause overlay")
	state = adapter.get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "world",
			"the transport row's resume hands play back through the shell")
	assert_false(menu_shell.visible, "the transport row's resume hides the overlay")

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
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Loading Hold Host"
	host_options.mission_name = "Minimal"
	host_options.mission_file = advertised_file
	host_options.game_type = 0x30020
	host_options.max_players = 4
	# The lifecycle fixture ships base archives only. Say so on the wire: an unset field
	# leaves the host advertising whatever expansion this machine last persisted, and the
	# joiner's preload then correctly refuses a data set this install cannot mount
	# (D-NET-178) — a failure about machine state, not about the loading-screen hold.
	host_options.expansion = ""
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	var streamed_til := TilFixture.bytes_for_cell(4)
	host.set_terrain_til_data(streamed_til)
	assert_true(host.load_from_mission_data(mission))

	var observed := {"local_load": false, "held": false, "held_progress": -1}
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
			observed["held_progress"] = _shell.loading_progress_percent()
		if bool(observed["local_load"]) and not _shell.is_world_loading():
			break

	assert_true(bool(observed["local_load"]), "the joiner completed its wire-header world load")
	assert_true(bool(observed["held"]),
			"local world_loaded cannot reveal the joiner before host admission")
	assert_eq(int(observed["held_progress"]), MissionData.LOAD_PROGRESS_WORLD_READY,
			"the held joiner reports local-world readiness, not false completion")
	assert_false(_shell.is_world_loading(),
			"the loading screen releases after the authoritative join edge")
	assert_eq(_shell.loading_progress_percent(), -1,
			"the completed loading presentation no longer exposes a stale value")
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
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Truncated TIL Host"
	host_options.mission_name = "Minimal"
	host_options.mission_file = advertised_file
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host_options.expansion = ""
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	# Advertise two records but provide one. The host emits the canonical first
	# page [0,1), then has no second page; admission must see Receiving, never
	# reinterpret it as the valid no-0x45 Absent case.
	var truncated_til := TilFixture.bytes_for_cell(4)
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


func test_f11_withdraws_the_tools_platform_windows_before_the_switch() -> void:
	# ImGui multi-viewport must be off at the NewFrame that first sees the
	# fullscreen size (the window_fullscreen probe pins the black frame on the
	# live process); the shell's F11 handler withdraws it before the mode change.
	_shell = await _make_shell()
	if _shell == null:
		return
	var dev_tools: DevTools = _shell.get_dev_tools()
	assert_true(dev_tools.are_platform_windows_allowed(),
			"windowed: undocked tool windows may become OS windows")
	var f11 := InputEventKey.new()
	f11.keycode = WindowState.TOGGLE_KEY
	f11.physical_keycode = WindowState.TOGGLE_KEY
	f11.pressed = true
	Input.parse_input_event(f11)
	await get_tree().process_frame
	assert_false(dev_tools.are_platform_windows_allowed(),
			"entering fullscreen withdraws them ahead of the mode switch")
	dev_tools.set_platform_windows_allowed(true)
	assert_true(dev_tools.are_platform_windows_allowed(),
			"the return to windowed re-allows them")


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
	var mode_edges: Array[bool] = []
	dev_tools.game_input_mode_changed.connect(
			func(playing: bool) -> void: mode_edges.append(playing))
	assert_true(_shell.is_gameplay_input_active())
	var tick_before := int(runtime.get_sim().get_logic_tick())
	dev_tools.toggle()
	assert_true(_shell.is_dev_tools_open())
	assert_false(_shell.is_gameplay_input_active(),
			"the public input gate closes on the same F3 edge")
	assert_true(dev_tools.is_game_play_available(),
			"a live normal gameplay state enables the Game window's Play action")
	assert_false(dev_tools.is_game_playing(), "F3 always opens in Interact")
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

	# The pick stack, end to end: F3-open flipped the click catcher on, and the
	# sim-authoritative ray is terrain-occluded exactly like a bullet.
	assert_not_null(world.get_node_or_null("PickClickCatcher"),
			"dev tools open: world clicks ray-pick")
	var pick_sim = runtime.get_sim()
	var player_pos: Vector3 = pick_sim.get_local_player_position()
	var down: DebugPickCard = pick_sim.debug_pick_entity(
			player_pos + Vector3(0, 20, 0), Vector3.DOWN, 100.0)
	assert_false(down.hit)
	assert_eq(down.blocked, "terrain",
			"terrain blocks the pick exactly like a bullet")
	_shell.pick_at_crosshair()
	assert_not_null(_shell.find_child("PickToast", true, false),
			"the crosshair pick confirms every attempt with a toast")

	dev_tools.set_game_playing(true)
	await get_tree().process_frame
	assert_true(dev_tools.is_game_playing(), "Play latches through the DevTools seam")
	assert_true(_shell.is_gameplay_input_active(),
			"Play reopens the normal gameplay-input gate under the workspace")
	if DisplayServer.get_name() != "headless":
		assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_CAPTURED,
				"Play captures through the existing player-input path")
	else:
		assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE,
				"headless accepts the capture request but has no cursor to capture")
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"Play disables Interact's world-click picker")

	var escape := InputEventKey.new()
	escape.keycode = KEY_ESCAPE
	escape.physical_keycode = KEY_ESCAPE
	escape.pressed = true
	get_viewport().push_input(escape)
	await get_tree().process_frame
	assert_true(_shell.is_dev_tools_open(), "the first Play Escape keeps F3 open")
	assert_false(dev_tools.is_game_playing(), "the first Play Escape returns to Interact")
	assert_false(_shell.is_gameplay_input_active(), "Interact suspends gameplay again")
	assert_eq(String(_shell.get_game_debug_adapter().get_mcp_game_state()["shell"]["state"]),
			"world", "the consumed Play Escape never opens the pause menu")
	assert_not_null(world.get_node_or_null("PickClickCatcher"),
			"Interact restores debug picking")

	get_viewport().push_input(escape)
	await get_tree().process_frame
	assert_false(_shell.is_dev_tools_open())
	assert_true(_shell.is_gameplay_input_active(),
			"the second Escape closes F3 and restores ordinary gameplay")
	assert_false(board.is_capture_active(),
			"closing the tools releases the board's capture")
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"closing removes the click picker")
	assert_eq(mode_edges, [true, false], "Play and Interact emit one ordered mode edge each")

	dev_tools.toggle()
	await get_tree().process_frame
	assert_not_null(world.get_node_or_null("PickClickCatcher"))
	dev_tools.set_game_playing(true)
	assert_true(dev_tools.is_game_playing())
	var f3 := InputEventKey.new()
	f3.keycode = KEY_F3
	f3.physical_keycode = KEY_F3
	f3.pressed = true
	get_viewport().push_input(f3)
	await get_tree().process_frame
	assert_false(_shell.is_dev_tools_open())
	assert_false(dev_tools.is_game_playing(), "F3 closes directly from Play and resets mode")
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"F3 again removes the click picker through the same open edge")

	dev_tools.toggle()
	dev_tools.set_game_playing(true)
	assert_true(dev_tools.is_game_playing())

	# A selection never crosses worlds: packed handles name slots, not entities.
	dev_tools.select_entity(0x3001)
	assert_eq(dev_tools.selected_entity_handle(), 0x3001, "a pick selects while the world lives")
	menu_shell.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_true(_shell.is_dev_tools_open(), "the workspace may remain open in the menu")
	assert_false(dev_tools.is_game_play_available(), "menu state disables Play")
	assert_eq(dev_tools.selected_entity_handle(), -1,
			"the world unload clears the Entities selection")
	assert_false(dev_tools.is_game_playing(),
			"world teardown cannot leave gameplay capture or mode latched")
	# The tools stay open across the next load: the click picker returns once
	# the load's presentation completes (Play availability waits on that edge).
	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	assert_true(_shell.is_dev_tools_open(), "the workspace survives into the next mission")
	assert_true(dev_tools.is_game_play_available(), "the loaded mission re-enables Play")
	assert_not_null(world.get_node_or_null("PickClickCatcher"),
			"a load under open tools re-installs the click picker when the load completes")
	dev_tools.set_open(false)
	await get_tree().process_frame
	assert_false(_shell.is_dev_tools_open(), "closing the tools in the next mission works")
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"closing removes the click picker again")


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
	var inventory: PlayerInventory = world.get_sim().get_local_player_inventory()
	assert_eq(inventory.equipped_name, "WPN_M4",
		"the spawned simulation equips the same selected primary")


# Every DebugControlTable row resolves its live owner over a loaded world (ADR
# 0043 d12). The no-world harness (debug_controls_test.gd) can only read the
# world rows as unavailable; here each value row is available and writable for
# a confirmed authority caller, writes its own value back through its owner
# and reads it back, and each action reaches its engine or device verdict
# instead of "no owner" (ERR_UNAVAILABLE) or "no row" (ERR_DOES_NOT_EXIST). A
# row whose owner binding stops resolving fails here, on the real shell.
func test_debug_controls_rows_resolve_over_a_loaded_world() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("Terrain")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	var boot_clear: Color = world.get_current_frame_clear_color()
	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_shell)
	await get_tree().process_frame

	var adapter: GameDebugAdapter = _shell.get_game_debug_adapter()
	var controls: DebugControlTable = adapter.get_debug_controls()
	assert_true(adapter.has_debug_authority(), "the SP shell owns debug authority")
	assert_same(_shell.get_dev_tools().get_debug_control_table(), controls,
			"F3 drives the same table MCP does")
	var ids := controls.row_ids()
	assert_gt(ids.size(), 0, "the table carries its rows")
	var engine_rows := 0
	for id in ids:
		var row := controls.control(id)
		var state := controls.get_state(id, true)
		assert_true(state.available,
				"'%s' resolves its owner over the loaded world (%s)" % [id, state.reason])
		if not state.available:
			continue
		if row.owner == DebugControlRow.OWNER_ENGINE:
			engine_rows += 1
		assert_true(state.writable,
				"'%s' is writable for a confirmed authority caller (%s)" % [id, state.reason])
		if row.kind == DebugControlRow.ACTION:
			continue
		assert_ne(state.value, null, "'%s' reads a live value" % id)
		var normalized: Variant = DebugControlTable.normalize_value(row, state.value)
		assert_ne(normalized, null, "'%s' reads a value inside its own domain" % id)
		assert_eq(controls.set_value(id, state.value, true), OK,
				"'%s' writes its own value back through its owner" % id)
		var read_back: Variant = controls.get_state(id, true).value
		if row.kind == DebugControlRow.SLIDER:
			assert_almost_eq(float(read_back), float(normalized), maxf(row.step, 0.001),
					"'%s' reads back the value it wrote" % id)
		else:
			assert_eq(read_back, normalized, "'%s' reads back the value it wrote" % id)
	assert_gt(engine_rows, 0, "the engine rows answer over the loaded world")

	# The actions, each with in-domain arguments, against the engine's or the
	# device's own verdict. The audio rows write the mixer, so they restore it;
	# the weather rows write the world that leaves below.
	var master := AudioServer.get_bus_index("Master")
	var master_volume := AudioServer.get_bus_volume_db(master)
	var master_mute := AudioServer.is_bus_mute(master)
	var must_succeed: Array = [
		[&"teleport_local_player", [Vector3(1.0, 1.0, 1.0), 0.0, 0.0]],
		[&"cycle_map_mode", []],
		[&"set_audio_bus_volume", ["Master", master_volume]],
		[&"set_audio_bus_mute", ["Master", master_mute]],
		[&"set_audio_bus_solo", ["Master", false]],
		[&"set_audio_bus_bypass", ["Master", false]],
		[&"runtime_transport", ["pause"]],
		[&"runtime_transport", ["resume"]],
		[&"set_mission_variable", [0, 0]],
		[&"environment_lightning_short", []],
		[&"environment_lightning_long", []],
		[&"environment_sky_height", [65536000]],
		[&"environment_time_of_day_minutes", [720]],
		[&"environment_sun_fade", [0, 0]],
		[&"environment_color_fade", [0]],
		[&"environment_wind_scale", [256]],
		[&"environment_block_color", [0, 0xFFFFFF]],
		[&"environment_lightning_color", [0xFFFFFF]],
		[&"environment_weather_snapshot", []],
		[&"deploy_pick", [0]],
		[&"set_viewmodel_weapon", ["WPN_M4"]],
		[&"clear_viewmodel_weapon", []],
		[&"kill_group", [1]],
		[&"local_player_look", [1.0, 0.0]],
	]
	for case in must_succeed:
		var outcome := controls.invoke(case[0], case[1], true)
		assert_eq(int(outcome.error), OK,
				"action '%s' reaches its owner's verdict" % case[0])
	# In-domain arguments that name entities this minimal world may not carry:
	# the engine answers with its own refusal, never with a missing owner.
	var may_refuse: Array = [
		[&"set_entity_health", [0, 100]],
		[&"set_entity_position", [0, Vector3(1.0, 1.0, 1.0)]],
		[&"crew_vehicle", [1, 2]],
		[&"crew_local_player", [1]],
	]
	for case in may_refuse:
		var outcome := controls.invoke(case[0], case[1], true)
		var error := int(outcome.error)
		assert_true(error == OK or error == ERR_INVALID_PARAMETER,
				"action '%s' reaches the engine (error %d)" % [case[0], error])
	AudioServer.set_bus_volume_db(master, master_volume)
	AudioServer.set_bus_mute(master, master_mute)

	# The one action that ends the world runs last, and leaves a clean menu.
	var leave := controls.invoke(&"runtime_return_to_menu", [], true)
	assert_eq(int(leave.error), OK, "runtime_return_to_menu reaches the shell")
	await get_tree().process_frame
	await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)
	assert_false(controls.get_state(&"hide_foliage").available,
			"the world rows read unavailable again once the world is gone")


# The packed shell (WorldFixture.boot_shell): the staged archive dir is this
# test's _temp_dir so after_each removes it once the shell released its roots.
func _make_shell() -> MainGame:
	var shell: MainGame = await WorldFixture.boot_shell(self)
	_temp_dir = WorldFixture.last_shell_dir()
	return shell


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
	assert_not_null(world.get_node_or_null("MissionRoot/MissionObjects"),
			"the placed container is the mission root's child")
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
	assert_null(world.get_node_or_null("MissionRoot"),
			"no mission presentation subtree remains")


# The SP lose flow's SHELL half (world-wac-ai-re §20): the WAC Lose banner
# effect and the "round_end" host effect (the Server_ProcessRoundEnd tail)
# reach the shell over the world's mission_effects signal, the MISSION FAILED
# screen mounts after the lead-in beat with the banner line, and ESC through
# the real input path leaves to the menu [orig: ESC -> g_mission_exit_reason=1
# -> the "Post Menu" push @0x526867]. The sim half (kill tally -> WAC lose ->
# round end, winner 2) is ctest lose_flow_04tr on the retail mission.
func test_round_end_effect_mounts_the_failed_screen_and_esc_returns_to_the_menu() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("Terrain")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	var boot_clear: Color = world.get_current_frame_clear_color()
	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	_assert_loaded(world, terrain, menu_shell)
	assert_null(_shell.find_child("MissionEndScreen", true, false),
			"no end screen before the round ends")
	assert_true(_shell.is_gameplay_input_active(), "gameplay input is live in the world")

	var banner_key := "STRMISC_KILLEDGREEN"
	world.mission_effects.emit([
		MissionEffect.make("lose", 0, 0, 0, banner_key),
		MissionEffect.make("round_end", 2),
	])
	assert_false(_shell.is_gameplay_input_active(),
			"the round-end latch stops gameplay input while the world keeps ticking")
	# The lead-in beat is 3 s of shell time (the cine stand-in); run it fast.
	var saved_scale := Engine.time_scale
	Engine.time_scale = 20.0
	var screen: Node = null
	for _i in range(300):
		await get_tree().process_frame
		screen = _shell.find_child("MissionEndScreen", true, false)
		if screen != null:
			break
	Engine.time_scale = saved_scale
	assert_not_null(screen, "the MISSION FAILED screen mounts after the lead-in beat")
	if screen == null:
		return
	assert_true(world.is_loaded(), "the world stays loaded under the end screen")
	var gametext: RtxtStringFile = Strings.get_table("gametext")
	if gametext != null and gametext.has_string_in_section("Misc", banner_key):
		var banner: String = Strings.lookup_display("gametext", "Misc", banner_key)
		var banner_visible := false
		for node in screen.find_children("*", "Label", true, false):
			if (node as Label).text == banner:
				banner_visible = true
				break
		assert_true(banner_visible, "the WAC Lose banner line is on the failed screen")

	# ESC through the real input path.
	for pressed in [true, false]:
		var key := InputEventKey.new()
		key.keycode = KEY_ESCAPE
		key.physical_keycode = KEY_ESCAPE
		key.pressed = pressed
		Input.parse_input_event(key)
	for _i in range(4):
		await get_tree().process_frame
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)
	assert_null(_shell.find_child("MissionEndScreen", true, false),
			"the end screen goes with the world")
	assert_true(_shell.is_gameplay_input_active() == false,
			"nothing is live in the menu")
