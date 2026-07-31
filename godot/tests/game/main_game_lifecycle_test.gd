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
	"NW_REPLAY_DIR", "NW_REPLAY_LOOSE", "NW_REPLAY_ITEMS",
]


class EntitySimStub:
	extends RefCounted

	var present_snapshot_reads := 0
	var cards := [
		{
			"name": "AI zero",
			"net_id": 111,
			"position": Vector3(10.0, 2.0, -30.0),
			"pool": 0,
			"wire_handle": 1001,
			"alive": true,
			"hidden": false,
			"health": 80,
			"team": 1,
			"state_name": "guard",
		},
		{
			"name": "AI one",
			"net_id": 222,
			"position": Vector3(20.0, 3.0, -40.0),
			"pool": 1,
			"wire_handle": 1002,
			"alive": false,
			"hidden": true,
			"health": 0,
			"team": 2,
			"state_name": "dead",
		},
	]

	func get_entity_count() -> int:
		return cards.size()

	func get_entity_debug(index: int) -> Dictionary:
		return cards[index].duplicate(true) \
				if index >= 0 and index < cards.size() else {}

	func get_present_snapshot() -> PackedFloat32Array:
		present_snapshot_reads += 1
		var snapshot := PackedFloat32Array()
		snapshot.append_array(_present_row(
				222, 1002, 502, Vector3(22.0, 4.0, -44.0)))
		snapshot.append_array(_present_row(
				333, 2001, 703, Vector3(30.0, 5.0, -50.0)))
		snapshot.append_array(_present_row(
				111, 1001, 501, Vector3(11.0, 2.0, -33.0)))
		return snapshot

	func get_present_stride() -> int:
		return NovaSimulation.PF_STRIDE

	func get_world_entity_debug(net_id: int) -> Dictionary:
		if net_id != 333:
			return {}
		return {
			"name": "Client vehicle",
			"state_name": "driving",
			"health": 400,
			"team": 3,
			"alive": true,
		}

	func _present_row(
			net_id: int,
			wire_handle: int,
			type_id: int,
			position: Vector3) -> PackedFloat32Array:
		var row := PackedFloat32Array()
		row.resize(NovaSimulation.PF_STRIDE)
		row[NovaSimulation.PF_TYPE_ID] = type_id
		row[NovaSimulation.PF_NET_ID] = net_id
		row[NovaSimulation.PF_WIRE_HANDLE] = wire_handle
		row[NovaSimulation.PF_KIND] = 1
		row[NovaSimulation.PF_INDEX] = net_id
		row[NovaSimulation.PF_BMS_ID] = 1000 + net_id
		row[NovaSimulation.PF_POS_X] = position.x
		row[NovaSimulation.PF_POS_Y] = position.y
		row[NovaSimulation.PF_POS_Z] = position.z
		row[NovaSimulation.PF_ALIVE] = 1.0
		return row


class EntityRuntimeStub:
	extends RefCounted

	var sim := EntitySimStub.new()

	func get_sim() -> EntitySimStub:
		return sim


class EntityShellHarness:
	extends "res://game/main_game.gd"

	var runtime_stub := EntityRuntimeStub.new()

	func _current_runtime():
		return runtime_stub


var _saved_config := PackedByteArray()
var _had_config := false
var _saved_env := {}
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
			"Master", NovaDebugCatalog.AUDIO_BUS_VOLUME_MAX_DB + 0.5),
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
	var shell = autofree(EntityShellHarness.new())
	var debug_adapter: GameDebugAdapter = autofree(shell.get_game_debug_adapter())

	var page: Dictionary = debug_adapter.get_mcp_game_entities(0, 64)

	assert_eq(page["total"], 3)
	assert_eq(page["entities"][0]["index"], 0)
	assert_eq(page["entities"][0]["net_id"], 222)
	assert_eq(page["entities"][0]["ai_index"], 1)
	assert_true(page["entities"][0]["editable"])
	assert_eq(page["entities"][0]["mission_position"],
			Vector3(22.0, 44.0, 4.0))
	assert_eq(page["entities"][1]["index"], 1)
	assert_eq(page["entities"][1]["net_id"], 333)
	assert_eq(page["entities"][1]["name"], "Client vehicle")
	assert_eq(page["entities"][1]["ai_index"], -1)
	assert_false(page["entities"][1]["editable"],
			"non-AI client entities remain discoverable without becoming editable")
	assert_eq(page["entities"][2]["net_id"], 111)
	assert_eq(page["entities"][2]["ai_index"], 0)
	assert_gt(shell.runtime_stub.sim.present_snapshot_reads, 0)
	assert_eq(debug_adapter.get_mcp_game_entity(0)["name"], "AI one")
	assert_eq(debug_adapter.get_mcp_game_entity(0)["ai_index"], 1)


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
		var menu_shell = _shell.get_node_or_null("MenuLayer/MenuShell")
		if menu_shell != null and menu_shell.get_menu() != null:
			var menu_root = menu_shell.get_menu().get_resource_root()
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


func test_boot_gates_env_mission_when_the_resource_dir_cannot_mount() -> void:
	# A loose-only directory (no packed archives) fails the runtime mount when no
	# --loose-root flag sanctions the editor fallback. The boot continuations
	# (NW_SP_MISSION here, --loose-mission in an editor-managed run) must gate on
	# that failure instead of starting a world load with no mounted root.
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	var loose := FileAccess.open(_temp_dir.path_join("Alpha.TRN"), FileAccess.WRITE)
	assert_not_null(loose)
	loose.store_string("loose trn")
	loose.close()
	NovaResourceDirSettings.set_resource_dir(_temp_dir)
	assert_eq(NovaResourceDirSettings.get_resource_dir(), _temp_dir,
			"the persisted dir round-trips, so the boot below reads THIS dir")
	NovaResourceDirSettings.set_game("jo")
	OS.set_environment("NW_SP_MISSION", "mnml.bms")
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


func test_session_loss_with_no_mounted_root_returns_shell_to_menu_state() -> void:
	# The NW_REPLAY spectate entry runs a world with no mounted root; losing
	# that session must land the shell back on the front-end state (the
	# picker/F9 contract is MENU-only) instead of parking it in WORLD forever.
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_main_game_lifecycle_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	NovaResourceDirSettings.set_resource_dir(_temp_dir)  # empty dir: unmountable
	NovaResourceDirSettings.set_game("jo")
	_shell = MAIN_GAME_SCENE.instantiate()
	assert_not_null(_shell)
	add_child(_shell)
	await get_tree().process_frame
	_shell.enter_net_world()
	var state: Dictionary = _shell.get_game_debug_adapter().get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "world",
			"the spectate entry is in-world with no mounted root")
	_shell.get_node("World").session_lost.emit("test: replay stream ended")
	state = _shell.get_game_debug_adapter().get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "menu",
			"a rootless teardown lands on the front-end state, not WORLD")
	assert_false(_shell.is_world_loading())


func test_rejected_replay_boot_falls_through_to_the_menu_front_end() -> void:
	# A replay boot whose session is rejected before connecting (here: the replay
	# dir cannot mount) must not consume the boot: the shell falls through to the
	# normal front-end — menu visible on main.mnu, world and HUD hidden, no kill
	# feed — instead of ending on a blank visible world with no session.
	OS.set_environment("NW_REPLAY", "127.0.0.1:42000")
	OS.set_environment("NW_REPLAY_DIR", OS.get_cache_dir().path_join(
			"opennova_nonexistent_replay_%d" % Time.get_ticks_usec()))
	_shell = await _make_shell()  # asserts the boot lands on main.mnu
	if _shell == null:
		return
	var state: Dictionary = _shell.get_game_debug_adapter().get_mcp_game_state()
	assert_eq(String(state["shell"]["state"]), "menu",
			"the rejected replay session leaves the shell on the front-end state")
	assert_false(_shell.is_world_loading())
	assert_not_null(_shell.current_resource_root(),
			"the fall-through boot mounted the persisted dir")
	assert_false(_shell.get_node("World").visible,
			"the rejected session's world reveal is rolled back")
	assert_false(_shell.get_node("HUD/FpsLabel").visible,
			"the HUD contents hide with the menu up")
	assert_null(_shell.get_node("HUD").get_node_or_null("NetKillFeed"),
			"no kill feed exists for a session that never started")


func test_picker_pick_persists_only_for_unmanaged_runs() -> void:
	# The picker's accept leg (apply_picked_resource_dir, the ADR-0018 seam
	# behind _on_dir_selected): an editor-managed run must never write its
	# picker escape into the SHARED editor+game resource_dir key, an unmanaged
	# first-launch pick must, and an unmountable pick changes nothing.
	_shell = await _make_shell()
	if _shell == null:
		return
	var picked_dir := _temp_dir.path_join("picked")
	assert_eq(DirAccess.make_dir_recursive_absolute(picked_dir), OK)
	_write_pff(picked_dir.path_join("language.pff"), _fixture_entries(LANGUAGE_FILES))
	_write_pff(picked_dir.path_join("localres.pff"), _fixture_entries(LOCALRES_FILES))
	_write_pff(picked_dir.path_join("resource.pff"), _fixture_entries(RESOURCE_FILES))
	assert_eq(NovaResourceDirSettings.get_resource_dir(), _temp_dir)

	assert_true(_shell.apply_picked_resource_dir(picked_dir, true),
			"an editor-managed pick mounts and enters the menu")
	assert_eq(NovaResourceDirSettings.get_resource_dir(), _temp_dir,
			"an editor-managed pick never writes the shared editor+game key")
	assert_false(_shell.apply_picked_resource_dir(
			_temp_dir.path_join("does-not-exist"), false),
			"an unmountable pick is refused")
	assert_eq(NovaResourceDirSettings.get_resource_dir(), _temp_dir,
			"a refused pick changes nothing")
	assert_true(_shell.apply_picked_resource_dir(picked_dir, false),
			"an unmanaged pick mounts")
	assert_eq(NovaResourceDirSettings.get_resource_dir(), picked_dir,
			"the unmanaged first-launch pick persists")


func test_mount_boot_root_falls_back_to_the_loose_authoring_mount() -> void:
	# The ONED play-test contract (ADR 0025): with --loose-root, a directory
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
	NovaResourceDirSettings.set_game("jo")
	var shell = autofree(preload("res://game/main_game.gd").new())

	assert_null(shell.mount_boot_root(loose_dir, false),
			"without the flag a loose-only dir keeps retail's fatal mount error")
	var fallback: NovaResourceRoot = shell.mount_boot_root(loose_dir, true)
	assert_not_null(fallback, "--loose-root plays the loose authoring dir")
	if fallback != null:
		assert_false(fallback.is_runtime_mount(),
				"the fallback is the editor's loose mount, not a packed install")
		assert_eq(fallback.read_file("alpha.trn").get_string_from_utf8(), "loose trn")
		fallback.clear()
	var packed: NovaResourceRoot = shell.mount_boot_root(packed_dir, true)
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
	var terrain = world.get_node("NovaTerrain")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	var boot_clear: Color = world.get_current_frame_clear_color()
	_assert_clean_menu(world, terrain, menu_shell, boot_clear)
	# Once the shell owns a mounted resource session, later persistence changes
	# cannot redirect one consumer into a separately remounted VFS.
	var detached_resource_dir := _temp_dir.path_join("detached")
	assert_eq(DirAccess.make_dir_recursive_absolute(detached_resource_dir), OK)
	NovaResourceDirSettings.set_resource_dir(detached_resource_dir)
	assert_eq(NovaResourceDirSettings.get_resource_dir(), detached_resource_dir,
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


func test_debug_overlay_suspends_input_without_stopping_the_world() -> void:
	_shell = await _make_shell()
	if _shell == null:
		return
	var world = _shell.get_node("World")
	var terrain = world.get_node("NovaTerrain")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	menu_shell.start_requested.emit("mnml.bms")
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
			"the game shell supplies its actual foliage-dispatch camera")
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
	# flip applies through the shared session's option target and
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

	(overlay.find_child("CloseDebug", true, false) as Button).pressed.emit()
	await get_tree().process_frame
	assert_false(_shell.is_debug_overlay_open())
	assert_true(_shell.is_gameplay_input_active(),
			"the cockpit Close button restores the gameplay-input policy")
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"Close removes the click picker")

	_shell.toggle_debug_overlay()
	await get_tree().process_frame
	assert_not_null(world.get_node_or_null("PickClickCatcher"))
	var escape := InputEventKey.new()
	escape.keycode = KEY_ESCAPE
	escape.pressed = true
	assert_true(overlay.handle_key_input(escape))
	await get_tree().process_frame
	assert_false(_shell.is_debug_overlay_open())
	assert_null(world.get_node_or_null("PickClickCatcher"),
			"Escape removes the click picker through the same visibility edge")
	assert_false(overlay.get_debug_session().is_presented())

	menu_shell.return_to_menu_requested.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_false(_shell.is_debug_overlay_open(),
			"returning to the menu cannot blanket-show a closed F3 layer")
	assert_false(overlay.get_debug_session().is_presented())
	menu_shell.start_requested.emit("mnml.bms")
	await _wait_for_world_load(world)
	await _wait_for_visible_terrain(terrain)
	assert_false(_shell.is_debug_overlay_open(),
			"the next mission keeps the overlay's own closed lifecycle")
	assert_false(overlay.get_debug_session().is_presented())


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
	assert_same(world.get_resource_root(), menu_shell.get_menu().get_resource_root(),
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
