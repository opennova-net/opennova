extends GutTest

# THE HOST'S PUNT. A retail host closes a session on its own terms by sending the
# connection-description record (the settings flag + tag 0x103) carrying DS/DC/DP1/DP2/
# DSTR/DPC/DDSTR. Captured live against a stock 1.7.5.7 co-op host as DPC 33 / DC 2 /
# DSTR "t35" / DDSTR "LogPuntEvent": the six-minute deploy-screen idle kick
# [orig: Server_TickUpdate's AFK arm (cmp eax, 57E40h @0x51e109 -> push 23h @0x51e13a) ->
#  Server_LogCRCMismatchPunt @0x517ed0 -> CNapiNPConnection_SendChatMessage @0x4c7ef0 ->
#  NapiNPDataTransfer_SendDescription @0x628c80].
#
# libs/npruntime decodes the record and raises a session loss from it (the wire and
# runtime halves are pinned by tests/npruntime/host_punt_test). This file covers what the
# PLAYER gets: retail's presentation is the ordinary mission exit, not a dialog — the
# disconnect handler routes DPC 33 to Input_QueueEvent(3) @0x4c67a4, whose action sets
# g_mission_exit_reason = 1 and drops the connection [orig: Input_HandleActionBinding
# case 3 @0x49af2c], and reason 1 takes the same teardown + nav-push "MainMenu" every
# abort leg takes [orig: the mission-exit dispatcher @0x568460 — the reason-1 arm
# @0x5684a8 -> @0x568654]. So the bar is: the deploy screen goes away, the world is torn
# down, and the shell is back in its menu with the reason reported.

const DeployHost := preload("res://engine/world/deploy_screen_presenter.gd")
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

var _saved_config := PackedByteArray()
var _had_config := false
var _saved_env := {}
var _temp_dir := ""
var _shell: Node = null


# The joiner state the deploy screen and the world observer read, in the shapes the real
# NovaSimulation reports them: the host's close is TERMINAL in the connection, so it
# clears the deployment sub-state and in-match with it — a double that only set a reason
# would not model what these surfaces actually see.
class PuntedJoinerSim:
	extends RefCounted
	var loss_reason := ""
	var pick_pending := true
	var in_match := false
	var picks: Array[int] = []
	var zones: Array[Dictionary] = []

	func is_joiner() -> bool:
		return true

	func is_joined_in_match() -> bool:
		return in_match

	func is_join_deploy_pick_pending() -> bool:
		return pick_pending

	func is_session_lost() -> bool:
		return not loss_reason.is_empty()

	func get_session_loss_reason() -> String:
		return loss_reason

	func get_join_assigned_team() -> int:
		return 1

	func get_deploy_spawn_zones() -> Array[Dictionary]:
		return zones

	func send_deployment_pick(param: int) -> bool:
		picks.append(param)
		return true

	func close_from_host(reason: String) -> void:
		loss_reason = reason
		pick_pending = false
		in_match = false

	# The deployment RELEASE: the host spawned this player, so the pick stops being owed
	# while the session stays healthy [orig: the §5.61 hold chain, flags1 bit1 clearing].
	func release_deployment() -> void:
		pick_pending = false
		in_match = true


class PuntedJoinerRuntime:
	extends Node
	var sim: PuntedJoinerSim = null

	func is_playing() -> bool:
		return true

	func tick() -> bool:
		return true

	func get_sim() -> PuntedJoinerSim:
		return sim


class FakeWorld:
	extends Node
	var root: NovaResourceRoot
	var sim: PuntedJoinerSim

	func get_resource_root() -> NovaResourceRoot:
		return root

	func get_sim() -> PuntedJoinerSim:
		return sim


func before_each() -> void:
	NovaStrings.clear()
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
		if menu_shell != null and menu_shell.get_menu() != null:
			var menu_root = menu_shell.get_menu().get_resource_root()
			if menu_root != null:
				menu_root.clear()
		_shell.queue_free()
		_shell = null
	await get_tree().process_frame
	NovaMusicService.stop_context()
	NovaStrings.clear()
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


# The world-side observer: the host's close must reach the shell as ONE session_lost
# carrying the decoded reason verbatim, and it must not be mistaken for the deployment
# release that would re-arm the deploy-screen edge.
func test_world_raises_the_hosts_close_once_from_the_deploy_wait() -> void:
	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	add_child_autofree(world)
	var runtime := PuntedJoinerRuntime.new()
	var sim := PuntedJoinerSim.new()
	runtime.sim = sim
	add_child_autofree(runtime)
	_install_runtime(world, runtime)
	var reasons: Array = []
	var deploy_edges := [0]
	world.session_lost.connect(func(reason: String) -> void: reasons.append(reason))
	world.join_deploy_pick_required.connect(
			func() -> void: deploy_edges[0] = int(deploy_edges[0]) + 1)

	world.tick(Vector3.ZERO)
	assert_eq(int(deploy_edges[0]), 1, "the owed pick opens the deploy screen once")
	assert_eq(reasons.size(), 0, "a healthy deploy wait is not a session loss")

	sim.close_from_host(PUNT_REASON)
	world.tick(Vector3.ZERO)
	assert_eq(reasons, [PUNT_REASON],
			"the host's close reaches the shell carrying the decoded reason verbatim")
	assert_eq(int(deploy_edges[0]), 1,
			"a closed session never re-opens the deploy screen")

	world.tick(Vector3.ZERO)
	world.tick(Vector3.ZERO)
	assert_eq(reasons.size(), 1, "the latched reason surfaces exactly once per session")
	_install_runtime(world, null)


# THE REGRESSION: the spawn screen must not survive the kick taking clicks that go
# nowhere. Closing is not enough either — `closed` is the deployment-release edge that
# hands the shell back to State.WORLD, so a dead session must tear the screen down.
func test_deploy_screen_tears_down_when_the_host_closes_the_session() -> void:
	var sim := PuntedJoinerSim.new()
	sim.zones = [{"param": 1, "letter": "A", "name_key": "STRWPNAME001"}]
	var overlay := Control.new()
	overlay.size = Vector2(800, 600)
	add_child_autofree(overlay)
	var host = _open_host(sim, overlay)
	watch_signals(host)
	assert_true(host.open(), "the player-paced join opens death.mnu")
	var menu := overlay.get_node_or_null("DeployScreenMenu") as NovaMnuMenu
	assert_not_null(menu, "the DEATH screen is mounted over the world")
	if menu == null:
		return
	var list := menu.find_child("SPAWNPOINTS_LIST", true, false) as ItemList
	assert_not_null(list, "the spawn list is populated and clickable")
	if list == null:
		return
	assert_eq(list.item_count, 2, "the default spawn plus the one secured zone")

	sim.close_from_host(PUNT_REASON)
	await get_tree().process_frame

	assert_false(host.is_open(), "the punted screen does not stay up")
	assert_signal_emit_count(host, "closed", 0,
			"a dead session is not a deployment release: the shell must not be handed "
			+ "back to State.WORLD")
	assert_null(overlay.get_node_or_null("DeployScreenMenu"),
			"the screen's menu is torn down, so there are no rows left to click")
	assert_eq(sim.picks, [] as Array[int],
			"no deploy pick was queued on the closed session")


# The control: the same screen on a HEALTHY session. The deployment release still closes
# it through `closed`, which is what returns the shell to play.
func test_deploy_screen_still_closes_through_the_deployment_release() -> void:
	var sim := PuntedJoinerSim.new()
	sim.zones = [{"param": 1, "letter": "A", "name_key": "STRWPNAME001"}]
	var overlay := Control.new()
	overlay.size = Vector2(800, 600)
	add_child_autofree(overlay)
	var host = _open_host(sim, overlay)
	watch_signals(host)
	assert_true(host.open(), "the player-paced join opens death.mnu")

	sim.release_deployment()
	await get_tree().process_frame

	assert_false(host.is_open(), "the release closes the screen")
	assert_signal_emit_count(host, "closed", 1,
			"the release hands gameplay input back to the world")
	assert_not_null(overlay.get_node_or_null("DeployScreenMenu"),
			"a released screen is only hidden — the next death reopens it")


# The other control: an ordinary deploy wait is untouched. The screen stays up across the
# periodic content refresh and nothing reports a loss.
func test_an_ordinary_deploy_wait_is_untouched() -> void:
	var sim := PuntedJoinerSim.new()
	sim.zones = [{"param": 1, "letter": "A", "name_key": "STRWPNAME001"}]
	var overlay := Control.new()
	overlay.size = Vector2(800, 600)
	add_child_autofree(overlay)
	var host = _open_host(sim, overlay)
	watch_signals(host)
	assert_true(host.open(), "the player-paced join opens death.mnu")

	await get_tree().create_timer(DeployHost.REFRESH_INTERVAL_S + 0.05).timeout

	assert_true(host.is_open(), "a healthy deploy wait keeps the screen up")
	assert_signal_emit_count(host, "closed", 0, "nothing closed a healthy screen")
	assert_true(sim.get_session_loss_reason().is_empty(),
			"a healthy session reports no loss reason")
	var menu := overlay.get_node_or_null("DeployScreenMenu") as NovaMnuMenu
	assert_not_null(menu, "the refreshed screen is still mounted")
	if menu == null:
		return
	var list := menu.find_child("SPAWNPOINTS_LIST", true, false) as ItemList
	assert_not_null(list, "the refreshed screen still carries its spawn rows")
	if list != null:
		list.item_selected.emit(0)
		assert_eq(sim.picks, [0] as Array[int],
				"the healthy screen still queues the default-spawn pick")


# The shell leg: an OPEN deploy screen plus a punted session must land the player back in
# the front end, not in State.DEPLOY over a dead world. The screen's world seam is
# duck-typed (see NovaDeployScreenPresenter), so this drives the real shell's teardown with the
# same double the screen tests use.
func test_shell_returns_a_punted_deploy_screen_to_the_menu() -> void:
	_shell = await _make_menu_shell()
	if _shell == null:
		return
	var deploy_hosts := _shell.find_children("*", "NovaDeployScreenPresenter", true, false)
	assert_eq(deploy_hosts.size(), 1, "the shell owns exactly one joiner deploy screen")
	if deploy_hosts.is_empty():
		return
	var deploy_host = deploy_hosts[0]
	var shell_world = _shell.get_node("World")
	var sim := PuntedJoinerSim.new()
	sim.zones = [{"param": 1, "letter": "A", "name_key": "STRWPNAME001"}]
	var world := FakeWorld.new()
	world.root = _make_root()
	world.sim = sim
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


func _install_runtime(world, runtime) -> void:
	# GameWorld's runtime seam is private by design; game_world_test.gd installs doubles
	# the same way rather than standing up a whole mission for an observer test.
	world._runtime = runtime
	world._loaded = runtime != null


func _open_host(sim: PuntedJoinerSim, overlay: Control):
	var world := FakeWorld.new()
	world.root = _make_root()
	world.sim = sim
	add_child_autofree(world)
	var host := DeployHost.new()
	host.setup(world, overlay)
	add_child_autofree(host)
	return host


func _make_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(TMP_DIR)), OK)
	return root


func _copy_fixture(source: String, target: String) -> void:
	var output := FileAccess.open(target, FileAccess.WRITE)
	assert_not_null(output, "temporary deploy-screen fixture opens for write")
	if output != null:
		output.store_buffer(FileAccess.get_file_as_bytes(source))
		output.close()


# A menu-only boot of the real shell: the runtime mount is PFF-only, so pack the front end
# and the fatal-set string tables the same way the lifecycle regression does. No mission is
# loaded — the leg under test is the shell's teardown, not a world.
func _make_menu_shell():
	_temp_dir = OS.get_cache_dir().path_join(
			"opennova_host_punt_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_temp_dir), OK)
	_write_pff(_temp_dir.path_join("language.pff"), _fixture_entries(LANGUAGE_FILES))
	_write_pff(_temp_dir.path_join("localres.pff"), _fixture_entries(LOCALRES_FILES))
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
