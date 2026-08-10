extends GutTest

# THE HOST'S PUNT — what the PLAYER gets. A retail host closes a session on its own
# terms by sending the connection-description record (the settings flag + tag 0x103)
# carrying DS/DC/DP1/DP2/DSTR/DPC/DDSTR. Captured live against a stock 1.7.5.7 co-op
# host as DPC 33 / DC 2 / DSTR "t35" / DDSTR "LogPuntEvent": the six-minute
# deploy-screen idle kick [orig: Server_TickUpdate's AFK arm (cmp eax, 57E40h @0x51e109
# -> push 23h @0x51e13a) -> Server_LogCRCMismatchPunt @0x517ed0 ->
# CNapiNPConnection_SendChatMessage @0x4c7ef0 -> NapiNPDataTransfer_SendDescription
# @0x628c80]. Retail's presentation is the ordinary mission exit, not a dialog — the
# disconnect handler routes DPC 33 to Input_QueueEvent(3) @0x4c67a4 and reason 1 takes
# the abort-leg teardown + nav-push "MainMenu" [orig: @0x568460 / @0x5684a8 -> @0x568654].
#
# Typed surfaces (ADR 0034): the deploy screen is opened over a REAL loopback join
# (the deploy_screen_presenter_test recipe) and the decoded loss reason is driven
# through GameWorld's session_lost signal, exactly where JoinerConnection lands it.
#
# The sim-side punt SEMANTICS — the record decode, the terminal close clearing
# deployment-pending and in-match, the once-per-session loss latch, and the world
# observer raising ONE session_lost from the deploy wait — are native and pinned by
# tests/npruntime/host_punt_test plus the NetSessionPolicy edge tests
# (net_session_policy_test.gd); the deployment-RELEASE close is pinned end to end in
# deploy_screen_presenter_test. Driving the punt legs shell-side needs a bound
# host-side punt trigger (none is exposed), so the former sim-double tests of the
# observer/teardown edges retired with the doubles.

const DeployHost := preload("res://game/world/deploy_screen_presenter.gd")
const MAIN_GAME_SCENE := preload("res://game/main_game.tscn")
const FIXTURE_DIR := "res://../fixtures/minimal/resources"
const TMP_DIR := "res://.godot/host_punt_surfacing_test"
const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
# What JoinerConnection composes for the captured punt: the DPC/DC codes the client's own
# exit-reason switch keys on, plus the sender's tag and formatted mismatch type.
const PUNT_REASON := "the host closed the session (reason 33, class 2): LogPuntEvent t35"
# The boot files a menu-only shell needs: the fatal-set string tables plus the front end.
const LANGUAGE_FILES := ["gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
		"menutxt.bin"]
const LOCALRES_FILES := ["main.mnu", "menu_style.mns", "items.def"]
const ISOLATED_ENV := ["NW_REPLAY", "NW_SP_MISSION", "NW_LAN_HOST", "NW_LAN_JOIN"]

const AI_TYPE := 0x14BF        # Generic Soldier (items.def id 105311)
const SPAWN_ZONE_TYPE := 1359  # pool-1 fixture; ItemDef supplies SpawnPoint

var _saved_config := PackedByteArray()
var _had_config := false
var _saved_env := {}
var _temp_dir := ""
var _shell: Node = null


class DeployWorldHarness:
	extends GameWorld
	var root: ResourceRoot
	var sim: Simulation

	func get_sim() -> Simulation:
		return sim

	func get_resource_root() -> ResourceRoot:
		return root


func before_each() -> void:
	Strings.clear()
	_had_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_config = FileAccess.get_file_as_bytes(STATE_CONFIG_PATH) \
			if _had_config else PackedByteArray()
	_saved_env.clear()
	for variable in ISOLATED_ENV:
		_saved_env[variable] = OS.get_environment(variable)
		OS.set_environment(variable, "")
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	if not DirAccess.dir_exists_absolute(dir):
		assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_copy_fixture("res://../fixtures/mnu/jo_death.mnu", dir.path_join("death.mnu"))
	_copy_fixture("res://../fixtures/rtxt/menutxt.bin", dir.path_join("menutxt.BIN"))
	_copy_fixture("res://../fixtures/rtxt/gametext.bin", dir.path_join("gametext.bin"))


func after_each() -> void:
	if is_instance_valid(_shell):
		var world = _shell.get_node_or_null("World")
		if world != null:
			world.unload()
			var world_root = world.get_resource_root()
			if world_root != null:
				world_root.clear()
		var menu_shell = _shell.get_node_or_null("MenuLayer/MenuShell")
		if menu_shell != null and menu_shell.get_resource_root() != null:
			var menu_root = menu_shell.get_resource_root()
			if menu_root != null:
				menu_root.clear()
		_shell.queue_free()
		_shell = null
	await get_tree().process_frame
	NovaMusicService.stop_context()
	Strings.clear()
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


func after_all() -> void:
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	for name in ["death.mnu", "menutxt.BIN", "gametext.bin"]:
		var path := dir.path_join(name)
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(dir)
	var zone_def := ProjectSettings.globalize_path(
			"res://.godot/host_punt_spawn_zone_items.def")
	if FileAccess.file_exists(zone_def):
		DirAccess.remove_absolute(zone_def)


func _copy_fixture(source: String, target: String) -> void:
	var output := FileAccess.open(target, FileAccess.WRITE)
	assert_not_null(output, "temporary deploy-screen fixture opens for write")
	if output != null:
		output.store_buffer(FileAccess.get_file_as_bytes(source))
		output.close()


func _make_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(TMP_DIR)), OK)
	return root


func _anim_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/anim")), OK)
	return root


func _spawn_zone_item_db() -> ItemDatabase:
	var base_path := ProjectSettings.globalize_path(
			"res://../fixtures/def/items.def")
	var base_file := FileAccess.open(base_path, FileAccess.READ)
	assert_not_null(base_file)
	if base_file == null:
		return null
	var base_items := base_file.get_as_text().replace("\r\n", "\n")
	base_file.close()
	var path := ProjectSettings.globalize_path(
			"res://.godot/host_punt_spawn_zone_items.def")
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file == null:
		return null
	file.store_string(base_items)
	if not base_items.ends_with("\n"):
		file.store_string("\n")
	file.store_string("""begin "Punt Spawn Zone Fixture"
  id 101359
  type object
  graphic MrkAlpha
  sid punt_spawn_zone
  hp 100
  attrib: SpawnPoint
end
""")
	file.close()
	var result := ItemDatabase.new()
	assert_eq(result.load(path), OK)
	return result


# A REAL loopback join held at the deploy pick (the deploy_screen_presenter_test
# recipe). Returns {host, joiner}; both autofreed Nodes.
func _join_pair_with_pending_pick() -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	mission.add_entity(3, AI_TYPE, Vector3(0, 0, 0), Vector3.ZERO)
	var zone := mission.add_entity(
			MissionData.KIND_ITEM, SPAWN_ZONE_TYPE, Vector3(40, 0, 0), Vector3.ZERO)
	assert_false(zone.is_empty())
	if not zone.is_empty():
		assert_true(mission.set_entity_property_int(
				MissionData.KIND_ITEM, int(zone.get("index", -1)), "team", 1))
	var item_db := _spawn_zone_item_db()
	assert_not_null(item_db)

	var host := Simulation.new()
	autofree(host)
	host.configure_host_session({"gametype": 0x30020})
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	assert_true(host.spawn_local_player(Vector3(5, 0, 5), 0.0, 1))
	var host_anim := _anim_root()
	assert_gt(host.set_infantry_anim_map(host_anim, "soldier.adm"), 0)
	host.resolve_item_traits(item_db)
	host.resolve_infantry_adm_ids(host_anim, item_db)

	var joiner := Simulation.new()
	autofree(joiner)
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "PuntJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	var joiner_anim := _anim_root()
	assert_gt(joiner.set_infantry_anim_map(joiner_anim, "soldier.adm"), 0)
	joiner.resolve_item_traits(item_db)
	joiner.resolve_infantry_adm_ids(joiner_anim, item_db)

	var reached := false
	for _i in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match():
			reached = true
			break
		OS.delay_msec(2)
	assert_true(reached, "the joiner reached in-match over real loopback UDP")
	assert_true(joiner.is_join_deploy_pick_pending(),
			"the spawn-zone join holds the player-paced deploy pick")
	return {"host": host, "joiner": joiner}


# The shell leg: an OPEN deploy screen plus a punted session must land the player back in
# the front end, not in State.DEPLOY over a dead world. The screen rides a REAL
# deploy-pending loopback joiner; the decoded punt reason is driven through the
# world's session_lost signal — the exact seam JoinerConnection's decode lands on
# (the decode itself is pinned by tests/npruntime/host_punt_test).
func test_shell_returns_a_punted_deploy_screen_to_the_menu() -> void:
	_shell = await _make_menu_shell()
	if _shell == null:
		return
	var deploy_hosts := _shell.find_children("*", "DeployScreenPresenter", true, false)
	assert_eq(deploy_hosts.size(), 1, "the shell owns exactly one joiner deploy screen")
	if deploy_hosts.is_empty():
		return
	var deploy_host: DeployScreenPresenter = deploy_hosts[0]
	var shell_world = _shell.get_node("World")
	var pair := _join_pair_with_pending_pick()
	var world := DeployWorldHarness.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	world.root = _make_root()
	world.sim = pair.joiner
	add_child_autofree(world)
	deploy_host.setup(world, _shell.get_node("HUD"))
	assert_true(deploy_host.open(), "the shell's deploy screen opens over the join")
	await get_tree().process_frame
	assert_false(_shell.is_gameplay_input_active(),
			"the open screen owns the cursor (the rows are only clickable outside WORLD)")

	shell_world.session_lost.emit(PUNT_REASON)
	await get_tree().process_frame
	await get_tree().process_frame

	assert_false(deploy_host.is_open(), "the deploy screen is gone")
	assert_null(_shell.get_node("HUD").get_node_or_null("DeployScreenMenu"),
			"the shell tore the DEATH screen down rather than leaving it parented")
	assert_false(shell_world.is_loaded(), "the world is unloaded with the session")
	assert_false(_shell.is_gameplay_input_active(),
			"and the cursor is never handed back to a world that cannot be played")
	var menu_shell = _shell.get_node("MenuLayer/MenuShell")
	assert_eq(menu_shell.get_current_menu_file().to_lower(), "main.mnu",
			"the front end is back up and usable")


# A menu-only boot of the real shell: the runtime mount is PFF-only, so pack the front end
# and the fatal-set string tables the same way the lifecycle regression does. No mission is
# loaded — the leg under test is the shell's teardown, not a world.
func _make_menu_shell():
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_host_punt_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	_write_pff(_temp_dir.path_join("language.pff"), _fixture_entries(LANGUAGE_FILES))
	_write_pff(_temp_dir.path_join("localres.pff"), _fixture_entries(LOCALRES_FILES))
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
	assert_eq(menu_shell.get_current_menu_file().to_lower(), "main.mnu",
			"the packed fixture boots through the real menu host")
	return shell


func _fixture_entries(filenames: Array) -> Array:
	var entries: Array = []
	for filename in filenames:
		var bytes := FileAccess.get_file_as_bytes(FIXTURE_DIR.path_join(filename))
		assert_false(bytes.is_empty(), "%s is available in the committed fixture" % filename)
		entries.append({"name": filename, "bytes": bytes})
	return entries


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
