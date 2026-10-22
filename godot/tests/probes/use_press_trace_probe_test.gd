extends GutTest

# use_press_trace's catalog contract and the binding it reads
# (Simulation.debug_use_press_trace / debug_use_press_next_seq /
# debug_attach_requests_sent over engine runtime/world/use_press_trace.h, whose
# press, toggle-gate and seat-scan records and framed-0x26 count the ctest
# npruntime_use_press_trace pins): a single player's USE press and its release's
# toggle gate reach the readout as typed rows, from the seq the caller names.


func test_the_catalog_registers_use_press_trace() -> void:
	var def := ProbeDef.definition("use_press_trace")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/net/"), "the net family")
	assert_false(def.needs_window, "a headless joiner can read it")
	assert_true(def.needs_mission)
	assert_true(def.validate_args({}).ok, "every argument has a default")
	assert_false(def.validate_args({"from_seq": -1}).ok, "from_seq is a seq")
	assert_false(def.validate_args({"watch_ms": 1.5}).ok, "watch_ms is whole milliseconds")
	assert_true(def.validate_args({"from_seq": 3, "watch_ms": 5000}).ok)
	assert_true(def.validate_args({"watch_handle": 0x102E}).ok)
	assert_false(def.validate_args({"watch_handle": -2}).ok, "-1 is the only no-watch value")


func test_the_readout_is_empty_without_a_kernel() -> void:
	var sim := Simulation.new() # RefCounted: held by this local, never freed
	assert_eq(sim.debug_use_press_trace(0).size(), 0)
	assert_eq(sim.debug_use_press_next_seq(), 0)
	assert_eq(sim.debug_attach_requests_sent(), 0)


func test_a_press_and_its_toggle_gate_reach_the_readout() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new() # RefCounted: held by this local, never freed
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 2))
	var from := sim.debug_use_press_next_seq()
	assert_eq(sim.debug_attach_requests_sent(), 0, "no client runtime frames a 0x26 here")

	assert_eq(sim.take_use_item_press(), Simulation.USE_ITEM_ARM_MOUNT_CHAIN,
			"off every volume the press takes the mount chain")
	sim.local_player_toggle_mount()
	var rows: Array[UsePressRow] = sim.debug_use_press_trace(from)
	assert_eq(rows.size(), 2, "the press, then the release's toggle gate")
	assert_eq(sim.debug_use_press_next_seq(), from + 2)
	if rows.size() != 2:
		return
	var press := rows[0]
	assert_eq(press.seq, from)
	assert_eq(press.event, "press")
	assert_eq(press.arm, "mount_chain")
	assert_eq(press.logic_tick, sim.get_logic_tick())
	assert_true(press.has_player)
	assert_eq(press.flags, 0, "no zone or queued-mount bit")
	assert_eq(press.engine_flags, 0)
	assert_false(press.mounted)
	assert_true(press.has_slot, "the personal slot is the active slot")
	var gate := rows[1]
	assert_eq(gate.seq, from + 1)
	assert_eq(gate.event, "toggle_gate")
	assert_true(gate.allowed, "an idle slot passes the weapon-busy gate")
	assert_eq(gate.outcome, "none", "the single player runs no joiner scan")
	assert_eq(gate.scan_gate, "none", "and so no scan watch")
	var json: Dictionary = gate.to_json_value()
	assert_eq(String(json.event), "toggle_gate", "the probe's JSON edge carries the row")
	assert_eq(int(json.seq), from + 1)

	assert_eq(sim.debug_use_press_trace(from + 1).size(), 1,
			"from_seq skips the records before it")
	assert_eq(press.watch, 0xFFFF, "no watch named: none carried")

	# A watched handle rides every record; one the world does not hold reads absent.
	sim.debug_set_use_press_watch(0x1FFE)
	sim.take_use_item_press()
	var watched: Array[UsePressRow] = sim.debug_use_press_trace(from + 2)
	assert_eq(watched.size(), 1)
	if watched.size() == 1:
		assert_eq(watched[0].watch, 0x1FFE)
		assert_false(watched[0].watch_present)
	sim.debug_set_use_press_watch(-1)
	sim.take_use_item_press()
	var cleared: Array[UsePressRow] = sim.debug_use_press_trace(from + 3)
	assert_eq(cleared.size(), 1)
	if cleared.size() == 1:
		assert_eq(cleared[0].watch, 0xFFFF, "-1 clears the watch")
