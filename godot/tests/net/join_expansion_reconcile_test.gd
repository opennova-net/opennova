extends GutTest

# D-NET-178, end to end: a GameWorld joiner drives its real preload against a live
# in-process listen host over loopback UDP, mounting a REAL packed install (base
# resource.pff + expansion/jox01/jox01.pff). The host advertises its expansion in S2C 0x7B,
# and the joiner must be running the host's data set before it resolves anything of the
# host's — the ADM weapon index space is expansion-scoped, so mounting the local persisted
# expansion instead silently renames every wire ADM index at and after the first diverging
# weapon.def row.
#
# The observable that makes each case falsifiable: HOSTMAP.BMS exists ONLY inside the
# expansion archive, and its bytes are not a mission. So the join's failure reason says
# which data set was mounted when the lookup ran:
#   found (expansion mounted)  -> "join: failed to parse host mission HOSTMAP.BMS"
#   absent (base mounted)      -> "join: host mission HOSTMAP.BMS is not installed locally"
# Each case starts from a persisted local expansion that is the WRONG one, and the
# pre-assertions pin what that local-only mount would have resolved.
#
# Half the cases inject the root the way the shipping game does — main_game's menu-entry leg hands
# GameWorld the shell's LIVE menu mount, so the product never joins on a root GameWorld mounted
# for itself. Those cases assert on the injected object itself: retail switches THE ONE global
# mount in place [orig: UI_JoinSelectedSession @ 0x5699d0 -> Expansion_SwitchTo @ 0x5688c0].

const HOST_MAP := "HOSTMAP.BMS"
const NOT_A_MISSION := "this is not a bms"
const PARSE_FAILURE := "failed to parse host mission"
const MISSING_FAILURE := "is not installed locally"
# The in-process host needs a handful of steps to reach its S2C 0x11 admission marker; the
# joiner's own driver awaits process_frame, so both are pumped from one loop.
const PRELOAD_PUMP_FRAMES := 900

var _saved_expansion := ""
var _dirs: Array[String] = []


func before_each() -> void:
	_saved_expansion = NovaResourceDirSettings.get_expansion()


func after_each() -> void:
	NovaResourceDirSettings.set_expansion(_saved_expansion)
	for dir in _dirs:
		_remove_install(dir)
	_dirs.clear()


func test_expansion_host_remounts_a_base_mounted_joiner() -> void:
	var dir := _make_install()
	NovaResourceDirSettings.set_expansion("")  # the local (wrong) choice: base game

	# BEFORE: mounted exactly as the joiner would have mounted it on its own, the host's
	# mission is unreachable — this is the state the fix has to move off.
	var local := NovaResourceRoot.new()
	assert_eq(local.mount_runtime(dir, ""), OK)
	assert_eq(local.get_expansion(), "", "the local-only mount is base game")
	assert_false(local.has_file(HOST_MAP, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY),
		"the host's mission lives only in the expansion")
	local.clear()

	var reason := await _join_against_host("jox01", dir)
	# AFTER: the mission resolved, so the root that answered was the expansion one.
	assert_string_contains(reason, PARSE_FAILURE,
		"the joiner remounted onto the host's expansion before resolving the host's mission")


func test_base_host_remounts_an_expansion_mounted_joiner() -> void:
	var dir := _make_install()
	NovaResourceDirSettings.set_expansion("jox01")  # the local (wrong) choice: an expansion

	# BEFORE: the local-only mount DOES resolve the host's mission, so a joiner that never
	# reconciled would sail past the lookup on expansion data while the host runs base JO.
	var local := NovaResourceRoot.new()
	assert_eq(local.mount_runtime(dir, "jox01"), OK)
	assert_eq(local.get_expansion(), "jox01", "the local-only mount is the expansion")
	assert_true(local.has_file(HOST_MAP, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY))
	local.clear()

	var reason := await _join_against_host("", dir)
	assert_string_contains(reason, MISSING_FAILURE,
		"the joiner remounted down to the base game the host actually runs")


func test_uninstalled_host_expansion_aborts_the_join() -> void:
	var dir := _make_install()
	NovaResourceDirSettings.set_expansion("jox01")

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
	NovaResourceDirSettings.set_expansion("jox01")

	var local := NovaResourceRoot.new()
	assert_eq(local.mount_runtime(dir, "jox01"), OK)
	assert_true(local.has_file(HOST_MAP, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY))
	local.clear()

	var reason := await _join_against_host("jox01", dir)
	assert_string_contains(reason, PARSE_FAILURE,
		"an agreeing host changes nothing: the join proceeds on the already-correct mount")


func test_injected_shell_root_is_switched_in_place() -> void:
	var dir := _make_install()
	NovaResourceDirSettings.set_expansion("")  # the local (wrong) choice: base game

	# The mount the shipping game actually joins on: the shell's live runtime root, handed to
	# GameWorld at menu entry and sitting on the local choice.
	var shell_root := NovaResourceRoot.new()
	assert_eq(shell_root.mount_runtime(dir, ""), OK)
	assert_eq(shell_root.get_expansion(), "", "the shell mounted base game")
	assert_false(shell_root.has_file(HOST_MAP, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY),
		"the host's mission lives only in the expansion")

	var world := _make_world()
	world.set_resource_root(shell_root)
	var reason := await _join_against_host("jox01", dir, world, _watch_failures(world))
	assert_string_contains(reason, PARSE_FAILURE,
		"the host's mission resolved, so the join ran on the host's expansion")
	assert_eq(shell_root.get_expansion(), "jox01",
		"and it is the SHELL's own root that moved — the switch is in place, not a swap")
	assert_true(shell_root.has_file(HOST_MAP, NovaResourceRoot.LOOKUP_FORCE_ARCHIVE_ONLY))
	assert_eq(NovaResourceDirSettings.get_expansion(), "",
		"the host owns this session's data set, not the persisted menu choice")
	shell_root.clear()  # release the archive handles before after_each deletes the install


func test_uninstalled_host_expansion_aborts_an_injected_shell_root_too() -> void:
	var dir := _make_install()
	NovaResourceDirSettings.set_expansion("jox01")

	var shell_root := NovaResourceRoot.new()
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
	NovaResourceDirSettings.set_expansion("")

	var loose_root := NovaResourceRoot.new()
	assert_eq(loose_root.set_root_dir(dir), OK)
	assert_false(loose_root.is_runtime_mount())
	assert_false(loose_root.list_expansions(dir).has("revx02"),
		"the host's expansion is genuinely absent here, so this is the abort-shaped case")

	var world := _make_world()
	world.set_resource_root(loose_root)
	var reason := await _join_against_host("revx02", dir, world, _watch_failures(world))
	assert_false(reason.contains("which is not installed"),
		"the join must not be refused over an install the authoring mount never claimed to have")
	assert_string_contains(reason, MISSING_FAILURE,
		"it fails later, on the host's mission being absent from the authored data — not on the mount")
	assert_false(loose_root.is_runtime_mount(), "and the authoring mount is left untouched")
	loose_root.clear()


func test_injected_loose_root_stands_down_instead_of_switching() -> void:
	var dir := _make_install()
	NovaResourceDirSettings.set_expansion("")

	# Injected-fixture shape: a loose authoring mount layers no expansion
	# archives, so there is nothing to switch and the authored data set stands.
	var loose_root := NovaResourceRoot.new()
	assert_eq(loose_root.set_root_dir(dir), OK)
	assert_false(loose_root.is_runtime_mount())
	assert_true(loose_root.list_expansions(dir).has("jox01"),
		"the host's expansion IS installed here, so this is the stand-down leg, not the abort")

	var world := _make_world()
	world.set_resource_root(loose_root)
	var reason := await _join_against_host("jox01", dir, world, _watch_failures(world))
	assert_string_contains(reason, MISSING_FAILURE,
		"the authoring mount never gained the expansion archive the host's mission lives in")
	assert_false(loose_root.is_runtime_mount(), "and it is still the authoring mount")
	assert_eq(loose_root.get_expansion(), "")
	loose_root.clear()


# --- harness -----------------------------------------------------------------------------

# Run a real GameWorld joiner preload against a listen host advertising `host_expansion`,
# and return the load-failure reason it ends on. Every case ends in a failure by design:
# HOSTMAP.BMS is never a loadable mission, so the reason is the probe.
func _join_against_host(host_expansion: String, dir: String,
		world = null, failures: Array = []) -> String:
	var host := NovaSimulation.new()
	host.configure_host_session({
		"server_name": "Expansion Host",
		"mission_name": "Expansion Probe",
		"mission_file": HOST_MAP,
		"expansion": host_expansion,
		"gametype": 0x30020,
		"max_players": 4,
	})
	assert_true(host.enable_host_listen(0), "the in-process listen host bound a loopback port")
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
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
		if not failures.is_empty():
			break
		host.step()
		await get_tree().process_frame
	host.free()
	assert_false(failures.is_empty(),
		"the preload reached a decision within %d frames" % PRELOAD_PUMP_FRAMES)
	return String(failures[0]) if not failures.is_empty() else ""


func _make_world() -> GameWorld:
	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
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
	_write_pff(dir.path_join("resource.pff"), [{"name": "basetag.txt", "bytes": "BASE"}])
	_write_pff(dir.path_join("expansion/jox01/jox01.pff"), [
		{"name": "exptag.txt", "bytes": "EXP"},
		{"name": HOST_MAP, "bytes": NOT_A_MISSION},
	])
	_dirs.append(dir)
	return dir


func _remove_install(dir: String) -> void:
	for sub in ["resource.pff", "expansion/jox01/jox01.pff", "expansion/jox01", "expansion"]:
		DirAccess.remove_absolute(dir.path_join(sub))
	DirAccess.remove_absolute(dir)


# Minimal PFF3 writer (mirrors the one in resource_root_contract_test): 20-byte header,
# 36-byte entries with a 16-byte name field, then the payloads.
func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "PFF fixture should be writable: %s" % path)
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_payload_offset := header_size + entries.size() * entry_size

	file.store_32(header_size)
	file.store_32(0x33464650)
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)

	for entry in entries:
		var bytes := String(entry.bytes).to_utf8_buffer()
		file.store_32(0)
		file.store_32(next_payload_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		var name_bytes := String(entry.name).to_utf8_buffer()
		for i in range(16):
			file.store_8(name_bytes[i] if i < name_bytes.size() else 0)
		file.store_32(0)
		next_payload_offset += bytes.size()

	for entry in entries:
		file.store_buffer(String(entry.bytes).to_utf8_buffer())
	file.close()
