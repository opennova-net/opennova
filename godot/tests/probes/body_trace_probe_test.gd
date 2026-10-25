extends GutTest

# body_trace's catalog contract and the binding it reads (Simulation.debug_body_trace /
# debug_body_trace_next_seq / debug_set_body_trace_watch over engine
# runtime/world/body_trace.h, whose records the ctest body_trace pins): a watched handle's
# per-tick record reaches the readout as a typed row, an unwatched world records nothing, and
# an empty watch clears it.


func test_the_catalog_registers_body_trace() -> void:
	var def := ProbeDef.definition("body_trace")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/net/"), "the net family")
	assert_false(def.needs_window, "a headless host or joiner can read it")
	assert_true(def.needs_mission)
	assert_true(def.validate_args({}).ok, "every argument has a default (the run then refuses)")
	assert_true(def.validate_args({"watch_handles": [0x22, 0x23, 0x1014, 0x1035]}).ok)
	assert_true(def.validate_args({"watch_local": true}).ok)
	assert_false(def.validate_args({"watch_local": 1.5}).ok, "watch_local is a boolean")
	assert_false(def.validate_args({"watch_handles": [1, 2, 3, 4, 5, 6, 7, 8, 9]}).ok,
			"at most eight handles")
	assert_false(def.validate_args({"watch_handles": [-1]}).ok, "a handle is a registry word")
	assert_true(def.validate_args({"watch_handles": [0x22], "trigger_handle": 0x1014,
			"pre_ticks": 62, "post_ticks": 310, "watch_ms": 120_000}).ok)
	assert_false(def.validate_args({"watch_handles": [0x22], "trigger_handle": -2}).ok,
			"-1 is the only no-trigger value")


func test_the_readout_is_empty_without_a_kernel() -> void:
	var sim := Simulation.new() # RefCounted: held by this local, never freed
	assert_eq(sim.debug_body_trace(0).size(), 0)
	assert_eq(sim.debug_body_trace_next_seq(), 0)
	assert_eq(sim.debug_local_body_handle(), -1)
	sim.debug_set_body_trace_watch(PackedInt32Array([0x22])) # no kernel: nothing to watch


func test_a_watched_body_reaches_the_readout() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new() # RefCounted: held by this local, never freed
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var handle := sim.get_local_player_wire_handle()
	assert_eq(sim.debug_local_body_handle(), handle, "off a joiner the body's handle is its wire one")
	var from := sim.debug_body_trace_next_seq()
	sim.step()
	assert_eq(sim.debug_body_trace_next_seq(), from, "nothing watched: nothing recorded")

	sim.debug_set_body_trace_watch(PackedInt32Array([handle, 0x1FFE]))
	sim.step()
	var rows: Array[BodyTraceRow] = sim.debug_body_trace(from)
	var ticks: Array = rows.filter(func(row: BodyTraceRow) -> bool: return row.event == "tick")
	assert_eq(ticks.size(), 2, "one tick record per watched handle")
	if ticks.size() == 2:
		var own: BodyTraceRow = ticks[0]
		assert_eq(own.handle, handle)
		assert_true(own.present)
		assert_true(own.logic_tick <= sim.get_logic_tick(), "this step's entity pass")
		assert_eq(own.pos.size(), 3)
		assert_eq(own.vel.size(), 3)
		assert_gt(own.health, 0)
		assert_false(own.mounted)
		var json: Dictionary = own.to_json_value()
		assert_eq(String(json.event), "tick", "the probe's JSON edge carries the row")
		assert_eq(int(json.handle), handle)
		var absent: BodyTraceRow = ticks[1]
		assert_eq(absent.handle, 0x1FFE)
		assert_false(absent.present, "a handle the world does not hold reads absent")

	sim.debug_set_body_trace_watch(PackedInt32Array())
	var cleared_from := sim.debug_body_trace_next_seq()
	sim.step()
	assert_eq(sim.debug_body_trace_next_seq(), cleared_from, "an empty watch clears it")
