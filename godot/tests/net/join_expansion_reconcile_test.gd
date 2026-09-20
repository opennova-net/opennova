extends GutTest

# D-NET-178, end to end: a GameWorld joiner drives its real preload against a live
# in-process listen host over loopback UDP, mounting a REAL packed install (base
# resource.pff + expansion/jox01/jox01.pff). The host advertises its expansion in S2C 0x7B,
# and the joiner must be running the host's data set before it resolves anything of the
# host's — the ADM weapon index space is expansion-scoped, so mounting the local persisted
# expansion instead silently renames every wire ADM index at and after the first diverging
# weapon.def row.
#
# The observable that makes each case falsifiable: the host's referenced TRN/ENV
# assets exist ONLY inside the expansion archive. HOSTMAP.BMS is deliberately
# invalid, but a retail-shaped join must never open it:
#   assets found (expansion mounted) -> the wire-header world loads
#   assets absent (base mounted)     -> referenced terrain-not-found failure
# Each case starts from a persisted local expansion that is the WRONG one, and the
# pre-assertions pin what that local-only mount would have resolved.
#
# Half the cases inject the root the way the shipping game does — main_game's menu-entry leg hands
# GameWorld the shell's LIVE menu mount, so the product never joins on a root GameWorld mounted
# for itself. Those cases assert on the injected object itself: retail switches THE ONE global
# mount in place [orig: UI_JoinSelectedSession @ 0x5699d0 -> Expansion_SwitchTo @ 0x5688c0].

const HOST_MAP := "HOSTMAP.BMS"
const NOT_A_MISSION := "this is not a bms"
const HOST_TERRAIN := "HOSTTRN"
const HOST_ENVIRONMENT := "HOSTENV"
const ASSET_MISSING_FAILURE := "HOSTTRN.trn"
const HOST_TRN := """terrain_name "Expansion join fixture"
polytrn_colormap mnml_dm.tga
polytrn_detailmap mnml_dm.tga
polytrn_polydata mnml.cpt
polytrn_sectorcount 1
polytrn_sectors 1
"""
# The in-process host needs a handful of steps to reach its S2C 0x11 admission marker; the
# joiner's own admission observer is tick-polled, so both are pumped from one loop.
const PRELOAD_PUMP_FRAMES := 900

var _saved_expansion := ""
var _dirs: Array[String] = []


func before_each() -> void:
	_saved_expansion = ResourceDirSettings.get_expansion()


func after_each() -> void:
	ResourceDirSettings.set_expansion(_saved_expansion)
	for dir in _dirs:
		_remove_install(dir)
	_dirs.clear()


func test_expansion_host_remounts_a_base_mounted_joiner() -> void:
	var dir := _make_install()
	ResourceDirSettings.set_expansion("")  # the local (wrong) choice: base game

	# BEFORE: mounted exactly as the joiner would have mounted it on its own, the host's
	# mission is unreachable — this is the state the fix has to move off.
	var local := ResourceRoot.new()
	assert_eq(local.mount_runtime(dir, ""), OK)
	assert_eq(local.get_expansion(), "", "the local-only mount is base game")
	assert_false(local.has_file(HOST_TERRAIN + ".trn"),
		"the host's terrain lives only in the expansion")
	local.clear()

	var world := _make_world()
	var reason := await _join_against_host("jox01", dir, world, _watch_failures(world))
	# AFTER: the wire-header world resolved through the expansion-only assets.
	assert_eq(reason, "", "the host's expansion assets loaded without reopening HOSTMAP.BMS")
	assert_true(world.is_loaded(),
		"the joiner remounted onto the host's expansion before resolving host assets")
	assert_eq(world.get_resource_root().get_expansion(), "jox01")
	world.unload()


func test_base_host_remounts_an_expansion_mounted_joiner() -> void:
	var dir := _make_install()
	ResourceDirSettings.set_expansion("jox01")  # the local (wrong) choice: an expansion

	# BEFORE: the local-only mount DOES resolve the host's mission, so a joiner that never
	# reconciled would sail past the lookup on expansion data while the host runs base JO.
	var local := ResourceRoot.new()
	assert_eq(local.mount_runtime(dir, "jox01"), OK)
	assert_eq(local.get_expansion(), "jox01", "the local-only mount is the expansion")
	assert_true(local.has_file(HOST_TERRAIN + ".trn"))
	local.clear()

	var reason := await _join_against_host("", dir)
	assert_string_contains(reason, ASSET_MISSING_FAILURE,
		"the joiner remounted down to the base game the host actually runs")
	assert_string_contains(reason, "not found")


func test_uninstalled_host_expansion_aborts_the_join() -> void:
	var dir := _make_install()
	ResourceDirSettings.set_expansion("jox01")

	var world := _make_world()
	var failures := _watch_failures(world)
	var reason := await _join_against_host("revx02", dir, world, failures)
	assert_string_contains(reason, "revx02", "the reason names the expansion the host runs")
	assert_string_contains(reason, "not installed")
	assert_string_contains(reason, "jox01", "the reason names what IS installed")
	assert_false(world.is_loaded(), "no world loads on a data-set mismatch")
	assert_null(world.get_sim(), "and the session is torn down, not left half-joined")


func test_matching_expansion_leaves_the_mount_alone() -> void:
	var dir := _make_install()
	ResourceDirSettings.set_expansion("jox01")

	var local := ResourceRoot.new()
	assert_eq(local.mount_runtime(dir, "jox01"), OK)
	assert_true(local.has_file(HOST_TERRAIN + ".trn"))
	local.clear()

	var world := _make_world()
	var reason := await _join_against_host("jox01", dir, world, _watch_failures(world))
	assert_eq(reason, "", "an agreeing host loads through the already-correct mount")
	assert_true(world.is_loaded())
	assert_eq(world.get_resource_root().get_expansion(), "jox01")
	world.unload()


func test_injected_shell_root_is_switched_in_place() -> void:
	var dir := _make_install()
	ResourceDirSettings.set_expansion("")  # the local (wrong) choice: base game

	# The mount the shipping game actually joins on: the shell's live runtime root, handed to
	# GameWorld at menu entry and sitting on the local choice.
	var shell_root := ResourceRoot.new()
	assert_eq(shell_root.mount_runtime(dir, ""), OK)
	assert_eq(shell_root.get_expansion(), "", "the shell mounted base game")
	assert_false(shell_root.has_file(HOST_TERRAIN + ".trn"),
		"the host's terrain lives only in the expansion")

	var world := _make_world()
	world.set_resource_root(shell_root)
	var reason := await _join_against_host("jox01", dir, world, _watch_failures(world))
	assert_eq(reason, "", "the host's assets resolved without reopening HOSTMAP.BMS")
	assert_true(world.is_loaded(), "the join ran on the host's expansion")
	assert_eq(shell_root.get_expansion(), "jox01",
		"and it is the SHELL's own root that moved — the switch is in place, not a swap")
	assert_true(shell_root.has_file(HOST_TERRAIN + ".trn"))
	assert_eq(ResourceDirSettings.get_expansion(), "",
		"the host owns this session's data set, not the persisted menu choice")
	world.unload()
	shell_root.clear()  # release the archive handles before after_each deletes the install


func test_uninstalled_host_expansion_aborts_an_injected_shell_root_too() -> void:
	var dir := _make_install()
	ResourceDirSettings.set_expansion("jox01")

	var shell_root := ResourceRoot.new()
	assert_eq(shell_root.mount_runtime(dir, "jox01"), OK)
	assert_eq(shell_root.get_expansion(), "jox01")

	var world := _make_world()
	world.set_resource_root(shell_root)
	var reason := await _join_against_host("revx02", dir, world, _watch_failures(world))
	assert_string_contains(reason, "revx02", "the reason names the expansion the host runs")
	assert_string_contains(reason, "not installed")
	assert_false(world.is_loaded(),
		"an injected mount is no license to enter the host's world on our own data set")
	assert_null(world.get_sim(), "and the session is torn down, not left half-joined")
	assert_eq(shell_root.get_expansion(), "jox01",
		"the aborted join leaves the shell's mount exactly as it found it")
	shell_root.clear()


func test_loose_root_stands_down_rather_than_aborting_an_uninstalled_expansion() -> void:
	# The regression that took the shell's own join test red: a loose authoring mount reports an
	# EMPTY installed set by construction (it layers no archives), so an expansion-running host
	# always looks "not installed" from it. Policing an install we do not own would abort every
	# fixture or standalone debug join against such a host. The stand-down must therefore
	# precede the abort — the abort belongs to runtime mounts, which are what the shipping game
	# always joins on.
	var dir := _make_install()
	ResourceDirSettings.set_expansion("")

	var loose_root := ResourceRoot.new()
	assert_eq(loose_root.set_root_dir(dir), OK)
	assert_false(loose_root.is_runtime_mount())
	assert_false(loose_root.list_expansions(dir).has("revx02"),
		"the host's expansion is genuinely absent here, so this is the abort-shaped case")

	var world := _make_world()
	world.set_resource_root(loose_root)
	var reason := await _join_against_host("revx02", dir, world, _watch_failures(world))
	assert_false(reason.contains("which is not installed"),
		"the join must not be refused over an install the authoring mount never claimed to have")
	assert_string_contains(reason, ASSET_MISSING_FAILURE,
		"it fails later on a host-referenced asset — not on expansion policing")
	assert_false(loose_root.is_runtime_mount(), "and the authoring mount is left untouched")
	loose_root.clear()


func test_injected_loose_root_stands_down_instead_of_switching() -> void:
	var dir := _make_install()
	ResourceDirSettings.set_expansion("")

	# Injected-fixture shape: a loose authoring mount layers no expansion
	# archives, so there is nothing to switch and the authored data set stands.
	var loose_root := ResourceRoot.new()
	assert_eq(loose_root.set_root_dir(dir), OK)
	assert_false(loose_root.is_runtime_mount())
	assert_true(loose_root.list_expansions(dir).has("jox01"),
		"the host's expansion IS installed here, so this is the stand-down leg, not the abort")

	var world := _make_world()
	world.set_resource_root(loose_root)
	var reason := await _join_against_host("jox01", dir, world, _watch_failures(world))
	assert_string_contains(reason, ASSET_MISSING_FAILURE,
		"the authoring mount never gained the expansion archive containing host assets")
	assert_false(loose_root.is_runtime_mount(), "and it is still the authoring mount")
	assert_eq(loose_root.get_expansion(), "")
	loose_root.clear()


# --- harness -----------------------------------------------------------------------------

# Run a real GameWorld joiner preload against a listen host advertising `host_expansion`.
# Return its load-failure reason, or an empty string once the wire-header world loads.
# HOSTMAP.BMS remains deliberately invalid: success proves the client never opened it.
func _join_against_host(host_expansion: String, dir: String,
		world = null, failures: Array = []) -> String:
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Expansion Host"
	host_options.mission_name = "Expansion Probe"
	host_options.mission_file = HOST_MAP
	host_options.expansion = host_expansion
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0), "the in-process listen host bound a loopback port")
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_true(mission.set_header_string("mission_name", "Expansion Probe"))
	assert_true(mission.set_header_string("terrain", HOST_TERRAIN))
	assert_true(mission.set_header_string("environment", HOST_ENVIRONMENT))
	assert_true(host.load_from_mission_data(mission))

	if world == null:
		world = _make_world()
		failures = _watch_failures(world)
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = host.get_host_listen_port()
	target.dir = dir
	target.player_name = "ExpansionJoiner"
	assert_eq(world.load_mission_as_joiner(target), OK)

	for _i in range(PRELOAD_PUMP_FRAMES):
		if not failures.is_empty() or world.is_loaded():
			break
		host.step()
		await get_tree().process_frame
	assert_true(not failures.is_empty() or world.is_loaded(),
		"the preload reached a decision within %d frames" % PRELOAD_PUMP_FRAMES)
	return String(failures[0]) if not failures.is_empty() else ""


func _make_world() -> GameWorld:
	var world := GameWorld.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	# The environment child makes the code-built world mission-loadable: the
	# typed placement path stamps _env.light_state onto every placed batch
	# (the game_world_test harness precedent).
	var env := MissionEnvironment.new()
	env.name = "MissionEnvironment"
	world.add_child(env)
	add_child_autofree(world)
	world.set_playable(false)
	return world


func _watch_failures(world: GameWorld) -> Array:
	var failures: Array = []
	world.load_failed.connect(func(reason): failures.append(String(reason)))
	return failures


# A real packed install: a base resource.pff (so the runtime mount's boot table opens) plus
# one expansion whose archive is the ONLY place the host's mission exists.
func _make_install() -> String:
	var dir := OS.get_cache_dir().path_join("opennova_join_expansion").path_join(
		"install_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir.path_join("expansion/jox01")), OK)
	WorldFixture.write_pff(self, dir.path_join("resource.pff"), [{"name": "basetag.txt", "bytes": "BASE"}])
	# The expansion must contain a loadable terrain, not just a resolvable name:
	# load_trn enforces retail's required map names and sector dimensions.
	var heightmap := FileAccess.get_file_as_bytes(RuntimeFixture.file("mnml.cpt"))
	var texture := FileAccess.get_file_as_bytes(RuntimeFixture.file("mnml_dm.tga"))
	assert_false(heightmap.is_empty(), "the synthetic heightmap fixture exists")
	assert_false(texture.is_empty(), "the synthetic terrain texture exists")
	WorldFixture.write_pff(self, dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "exptag.txt", "bytes": "EXP"},
		{"name": HOST_MAP, "bytes": NOT_A_MISSION},
		{"name": HOST_TERRAIN + ".trn", "bytes": HOST_TRN},
		{"name": "mnml.cpt", "bytes": heightmap},
		{"name": "mnml_dm.tga", "bytes": texture},
		{"name": HOST_ENVIRONMENT + ".env", "bytes": "not an env"},
	])
	_dirs.append(dir)
	return dir


func _remove_install(dir: String) -> void:
	for sub in ["resource.pff", "expansion/jox01/jox01.pff", "expansion/jox01", "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)
