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

const MAIN_GAME_SCENE := preload("res://game/main_game.tscn")
static var FIXTURE_DIR := RuntimeFixture.directory()
const TMP_DIR := "res://.godot/host_punt_surfacing_test"
const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH
# What JoinerConnection composes for the captured punt: the DPC/DC codes the client's own
# exit-reason switch keys on, plus the sender's tag and formatted mismatch type.
const PUNT_REASON := "the host closed the session (GDC033; reason 33, class 2): LogPuntEvent t35"
# The boot files a menu-only shell needs: the fatal-set string tables plus the front end.
const LANGUAGE_FILES := ["gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
		"menutxt.bin"]
const LOCALRES_FILES := ["main.mnu", "menu_style.mns", "items.def"]

const AI_TYPE := 0x14BF        # Generic Soldier (items.def id 105311)
const SPAWN_ZONE_TYPE := 1359  # pool-1 fixture; ItemDef supplies SpawnPoint
const ZONE_ITEMS_FILE := "host_punt_spawn_zone_items.def"

# DeployMenuFixture authors the death-screen controls and the string tables
# the punt surfacing resolves.
const STAGED_FIXTURES: Array[String] = ["death.mnu", "menutxt.BIN", "gametext.bin"]

var _config: TestFs.Snapshot
var _temp_dir := ""
var _shell: Node = null


func before_each() -> void:
	Strings.clear()
	_config = TestFs.snapshot(STATE_CONFIG_PATH)
	# A shell booted here must see no launch flags: the GUT process carries none,
	# and the override guards against a sibling test leaving one behind.
	LaunchFlags.set_args_override(PackedStringArray([]))
	DeployMenuFixture.stage(self, TMP_DIR, STAGED_FIXTURES)


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
	MusicService.stop_context()
	Strings.clear()
	if not _temp_dir.is_empty():
		TestFs.remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	LaunchFlags.clear_args_override()
	_config.restore()


func after_all() -> void:
	PresenterFixture.unstage(TMP_DIR, STAGED_FIXTURES)
	ItemDbFixture.release(ZONE_ITEMS_FILE)


func _spawn_zone_item_db() -> ItemDatabase:
	return ItemDbFixture.with_rows(self, ZONE_ITEMS_FILE, ItemDbFixture.SPAWN_ZONE_ROW)


# A REAL loopback join held at the DEATH deploy pick (the
# deploy_screen_presenter_test recipe): the initial join deploys with no pick —
# retail's initial join sends no C2S 0x0E — then the authority kills the joiner
# and the death edge re-arms the pick. Returns {host, joiner}; both autofreed
# Nodes.
func _join_pair_with_pending_pick() -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	mission.add_entity(3, AI_TYPE, Vector3(0, 0, 0), Vector3.ZERO)
	var zone := mission.add_entity(
			MissionData.KIND_ITEM, SPAWN_ZONE_TYPE, Vector3(40, 0, 0), Vector3.ZERO)
	assert_not_null(zone)
	if zone != null:
		assert_true(mission.set_entity_property_int(
				MissionData.KIND_ITEM, zone.index, "team", 1))
	var item_db := _spawn_zone_item_db()
	assert_not_null(item_db)

	var host := Simulation.new()
	autofree(host)
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	assert_true(host.spawn_local_player(Vector3(5, 0, 5), 0.0, 1))
	var host_anim := PresenterFixture.anim_root(self)
	assert_gt(host.set_infantry_anim_map(host_anim, "soldier.adm"), 0)
	host.resolve_item_traits(item_db)
	host.resolve_infantry_adm_ids(host_anim, item_db)

	var joiner := Simulation.new()
	autofree(joiner)
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "PuntJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	var joiner_anim := PresenterFixture.anim_root(self)
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
	assert_false(joiner.is_join_deploy_pick_pending(),
			"the initial join deploys with no forced C2S 0x0E")
	# The kill targets the joiner's wire handle: wait for the 0x0C name-match to
	# bind it, then the authority's real death transaction re-arms the pick (the
	# DEATH screen).
	var self_bound := false
	for _i in range(400):
		host.step()
		joiner.step()
		if joiner.get_joiner_self_handle() > 0:
			self_bound = true
			break
		OS.delay_msec(2)
	assert_true(self_bound, "the joiner bound its wire handle before the kill")
	assert_eq(host.debug_kill_player_entity(joiner.get_joiner_self_handle()), OK,
			"the host queued the joiner's death")
	var pick_pending := false
	for _i in range(240):
		host.step()
		joiner.step()
		if joiner.is_join_deploy_pick_pending():
			pick_pending = true
			break
		OS.delay_msec(2)
	assert_true(pick_pending,
			"the death edge holds the deploy pick (begin_redeployment)")
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
	var view := PresenterFixture.FakeWorldView.new()
	view.root = PresenterFixture.root_over(self, TMP_DIR)
	view.sim_value = pair.joiner
	deploy_host.setup(view, _shell.get_node("HUD"))
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
	WorldFixture.write_pff(self, _temp_dir.path_join("language.pff"), _fixture_entries(LANGUAGE_FILES))
	WorldFixture.write_pff(self, _temp_dir.path_join("localres.pff"), _fixture_entries(LOCALRES_FILES))
	LaunchFlags.set_args_override(PackedStringArray(["--resource-dir", _temp_dir]))
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
