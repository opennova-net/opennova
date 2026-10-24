extends GutTest

# model_list_dump's catalog contract and the binding it reads
# (Simulation.debug_mission_start_model_nodes / debug_loaded_model_list_count over engine
# runtime/inmatch/loaded_model_list.h loaded_model_list_entries, whose head-first order,
# load and page indices the ctest npruntime_loaded_model_list
# test_the_readout_entries pins): nothing is frozen until a load finalizes the list, and a
# row reads as the typed node.


func test_the_catalog_registers_model_list_dump() -> void:
	var def := ProbeDef.definition("model_list_dump")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/net/"), "the net family")
	assert_false(def.needs_window, "a headless joiner can read it")
	assert_true(def.needs_mission)
	assert_true(def.validate_args({}).ok, "every argument has a default")
	assert_true(def.validate_args({"log_names": true}).ok)
	assert_false(def.validate_args({"log_names": 1.5}).ok, "log_names is a flag")


func test_nothing_is_frozen_before_a_load_finalizes_the_list() -> void:
	var sim := Simulation.new() # RefCounted: held by this local, never freed
	assert_eq(sim.debug_mission_start_model_nodes().size(), 0)
	assert_eq(sim.debug_loaded_model_list_count(), 0)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_true(sim.load_from_mission_data(mission))
	assert_eq(sim.debug_mission_start_model_nodes().size(), 0,
			"the shell's load, not the mission's, freezes the list")
	assert_eq(sim.debug_loaded_model_list_count(), 0)


func test_a_row_reads_as_the_typed_node() -> void:
	var row := ModelListNodeRow.new()
	assert_eq(row.name, "")
	assert_false(row.foliage)
	assert_eq(row.load_index, 0)
	assert_eq(row.page_index, -1, "a default node sits on no page")
	var json: Dictionary = row.to_json_value()
	for key in ["name", "foliage", "load_index", "page_index"]:
		assert_true(json.has(key), "the probe's JSON edge carries %s" % key)
