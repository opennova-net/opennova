extends GutTest

# joiner_net_trace's catalog contract and the binding it reads
# (Simulation.debug_joiner_net_trace / debug_joiner_net_trace_next_seq over engine
# runtime/inmatch/joiner_net_trace.h, whose per-frame records the ctest
# npruntime_client_loading_pump run_the_net_readout_records_each_frame pins): the readout
# is empty without a client runtime, and a row reads as the typed record.


func test_the_catalog_registers_joiner_net_trace() -> void:
	var def := ProbeDef.definition("joiner_net_trace")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/net/"), "the net family")
	assert_false(def.needs_window, "a headless joiner can read it")
	assert_true(def.needs_mission)
	assert_true(def.validate_args({}).ok, "every argument has a default")
	assert_false(def.validate_args({"from_seq": -1}).ok, "from_seq is a seq")
	assert_false(def.validate_args({"watch_ms": 1.5}).ok, "watch_ms is whole milliseconds")
	assert_false(def.validate_args({"watch_ms": 600_000}).ok, "watch_ms stays inside the timeout")
	assert_true(def.validate_args({"from_seq": 3, "watch_ms": 30_000}).ok)


func test_the_readout_is_empty_without_a_runtime() -> void:
	var sim := Simulation.new() # RefCounted: held by this local, never freed
	assert_eq(sim.debug_joiner_net_trace(0).size(), 0)
	assert_eq(sim.debug_joiner_net_trace_next_seq(), 0)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_true(sim.load_from_mission_data(mission))
	assert_eq(sim.debug_joiner_net_trace(0).size(), 0, "a single player runs no joiner frame")


func test_a_row_reads_as_the_typed_record() -> void:
	var row := JoinerNetTraceRow.new()
	assert_eq(row.seq, 0)
	assert_eq(row.pump_loop, "network_frame", "the loop reads as its name")
	assert_false(row.block_open)
	assert_false(row.uplink_sent)
	assert_false(row.quality_folded)
	var json: Dictionary = row.to_json_value()
	for key in ["seq", "frame_tick", "client_tick", "wall_ms", "pump_loop", "countdown_before",
			"holdoff_ticks", "block_open", "uplink_sent", "frames_applied", "quality_folded",
			"fold_fps", "fold_deploy_hold", "fold_spawn_gate", "fold_preround", "fold_bandwidth",
			"fold_ping", "window_quality", "host_metric", "level", "report_queued",
			"report_level"]:
		assert_true(json.has(key), "the probe's JSON edge carries %s" % key)
