extends GutTest

# stage_slot's catalog contract: the registered probe, its required non-empty
# `staging` array of objects, and the binding it drives
# (Simulation.debug_stage_slot over engine runtime/inmatch/slot_staging.h, whose
# slot walk and Position write the ctest npruntime_slot_staging pins). Off a host
# session the binding refuses with ERR_UNAVAILABLE.


func test_the_catalog_registers_stage_slot() -> void:
	var def := ProbeDef.definition("stage_slot")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/net/"), "the net family")
	assert_false(def.needs_window, "a headless host can stage")
	assert_true(def.needs_mission)
	assert_eq(def.input_schema.get("required", []), ["staging"])
	assert_false(def.validate_args({}).ok, "staging is required")
	assert_false(def.validate_args({"staging": []}).ok, "at least one entry")
	assert_false(def.validate_args({"staging": [1]}).ok, "entries are objects")
	assert_true(def.validate_args({"staging": [{"slot": 1, "position_bms": [1.0, 2.0, 3.0]}]}).ok)


func test_the_binding_refuses_without_a_host_session() -> void:
	var sim := Simulation.new() # RefCounted: held by this local, never freed
	assert_eq(sim.debug_stage_slot(1, 1.0, 2.0, 3.0), ERR_UNAVAILABLE)
