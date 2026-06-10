extends GutTest

# NovaWacProgram + the sim's WAC wiring: the binding face of libs/wac. Until
# this slice, NovaSimulation created a WacSystem every load but nothing ever
# installed a program — mission .wac scripts were silently inert. These pin the
# compile surface, the resource-root layering, and the end-to-end execution
# through the faithful 62-tick divider.


func test_compile_good_source() -> void:
	var wac := NovaWacProgram.new()
	assert_eq(wac.compile_source("if never() then set(v1,1) endif\n"), OK)
	assert_true(wac.is_ok())
	assert_eq(wac.get_error_count(), 0)
	assert_eq(wac.get_event_count(), 1, "one top-level rule")
	assert_gt(wac.get_code_size(), 0)


func test_lenient_compile_surfaces_warnings() -> void:
	# The compiler is faithfully LENIENT (the original's parser recovers rather
	# than rejecting): an unknown command still compiles, with a warning
	# diagnostic, and the program installs. is_ok() reflects hard errors only.
	var wac := NovaWacProgram.new()
	assert_eq(wac.compile_source("if never() then bogus_command_xyz(1) endif" + "\n"), OK)
	assert_true(wac.is_ok())
	assert_eq(wac.get_error_count(), 0)
	var diags: Array = wac.get_diagnostics()
	assert_gt(diags.size(), 0, "the unknown command produced a diagnostic")
	var first: Dictionary = diags[0]
	assert_true(first.has("line") and first.has("message"), "diagnostics carry line + message")
	assert_false(bool(first["error"]), "lenient: a warning, not an error")


func test_compile_sources_numbers_events_across_files() -> void:
	var wac := NovaWacProgram.new()
	var sources := PackedStringArray([
		"if never() then set(v1,1) endif\n",
		"if never() then set(v2,1) endif\nif never() then set(v3,1) endif\n",
	])
	assert_eq(wac.compile_sources(sources), OK)
	assert_eq(wac.get_event_count(), 3, "events number across the layered sources")


func test_compile_from_resource_root_layers_and_skips_absent() -> void:
	# A loose root with only <mission>.wac: game.wac/server.wac skip silently
	# [orig: WacScript_InitAndLoad], the mission file compiles.
	var dir := OS.get_cache_dir().path_join("wac_program_test_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(dir.path_join("m01.wac"), FileAccess.WRITE)
	f.store_string("if never() then set(v1,1) endif\n")
	f.close()
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)

	var wac := NovaWacProgram.new()
	assert_eq(wac.compile_from_resource_root(root, "m01"), OK)
	assert_eq(wac.get_event_count(), 1)

	# No .wac anywhere: the BMS-only case — callers leave the VM unloaded.
	var empty_dir := dir.path_join("empty")
	DirAccess.make_dir_recursive_absolute(empty_dir)
	var empty_root := NovaResourceRoot.new()
	assert_eq(empty_root.set_root_dir(empty_dir), OK)
	assert_eq(wac.compile_from_resource_root(empty_root, "m01"), ERR_DOES_NOT_EXIST)

	DirAccess.remove_absolute(dir.path_join("m01.wac"))
	DirAccess.remove_absolute(empty_dir)
	DirAccess.remove_absolute(dir)


func test_sim_runs_an_installed_program_at_the_62_tick_divider() -> void:
	var sim := NovaSimulation.new()
	autofree(sim)
	sim.build_demo_mission()
	assert_true(sim.is_loaded())
	var state: Dictionary = sim.get_wac_state()
	assert_false(bool(state["loaded"]), "no program installed -> VM unloaded (BMS-only case)")

	# v1 starts 0, so eq(v1,0) fires on the first VM execution and sets v2=7.
	assert_true(sim.compile_and_set_wac(PackedStringArray(["if eq(v1,0) then set(v2,7) endif\n"])),
		"the registry-aware compile installs")
	state = sim.get_wac_state()
	assert_true(bool(state["loaded"]))
	assert_eq(int(state["event_count"]), 1)

	# The VM executes only every 62nd tick [orig: dword_C6EAD4 / cmp 0x3E].
	for _i in range(61):
		sim.step()
	assert_eq(sim.get_mission_variable(2), 0, "61 ticks: the divider has not fired yet")
	sim.step()
	assert_eq(sim.get_mission_variable(2), 7, "tick 62: the program ran and set v2")
	assert_eq(int(sim.get_wac_state()["runs"]), 1, "one completed execution counted")

	# Pause gate [orig: dword_C6EB28]: a paused script never advances.
	sim.set_mission_variable(2, 0)
	sim.set_wac_paused(true)
	for _i in range(124):
		sim.step()
	assert_eq(sim.get_mission_variable(2), 0, "paused: no executions")
	sim.set_wac_paused(false)

	# restart() rewinds: the system's on_load resets the accumulator + runs.
	sim.restart()
	assert_eq(int(sim.get_wac_state()["runs"]), 0, "restart resets the completed-runs counter")


func test_set_wac_program_survives_a_reload() -> void:
	var sim := NovaSimulation.new()
	autofree(sim)
	var wac := NovaWacProgram.new()
	assert_eq(wac.compile_source("if eq(v1,0) then set(v2,5) endif\n"), OK)
	sim.set_wac_program(wac)  # installed before any load: applied by finish_load
	sim.build_demo_mission()
	assert_true(bool(sim.get_wac_state()["loaded"]), "finish_load applied the held program")
	sim.build_demo_mission()  # reload: reset_world recreates the WacSystem
	assert_true(bool(sim.get_wac_state()["loaded"]), "the held program re-applies on reload")
	for _i in range(62):
		sim.step()
	assert_eq(sim.get_mission_variable(2), 5, "the re-applied program executes")
