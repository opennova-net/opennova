extends GutTest

# DebugSession is the shared seam for F3 and runtime MCP. These tests use
# only public fake target methods and drive the same interface both callers use.


class FakeTarget:
	extends RefCounted

	var enabled := false
	var amount := 1.0
	var mode := 0
	var setter_error: Error = OK
	var calls: Array = []

	func is_enabled() -> bool:
		return enabled

	func set_enabled(value: bool) -> void:
		enabled = value

	func get_amount() -> float:
		return amount

	func set_amount(value: float) -> void:
		amount = value

	func set_fallible_amount(value: float) -> Error:
		calls.append(["set_fallible_amount", value])
		if setter_error == OK:
			amount = value
		return setter_error

	func get_mode() -> int:
		return mode

	func set_mode(value: int) -> void:
		mode = value

	func act(value: String) -> String:
		calls.append(value)
		return value.to_upper()

	func fallible(value: int) -> Error:
		calls.append(value)
		return OK if value == 1 else ERR_UNAVAILABLE


## Carries the COMPLETE sim-target control surface DebugCatalog declares —
## the session reads every registered control back from its target.
class ValidationSim:
	extends RefCounted
	var calls: Array = []
	var action_error: Error = OK
	var wac_paused := false
	var net_diagnostics := false
	func send_deployment_pick(param: int) -> bool:
		calls.append(["deploy_pick", param])
		return true
	func debug_kill_group(group: int) -> int:
		calls.append(["kill_group", group])
		return 0
	func debug_crew_vehicle(occupant_ssn: int, vehicle_ssn: int) -> Error:
		calls.append(["crew_vehicle", occupant_ssn, vehicle_ssn])
		return action_error
	func debug_crew_local_player(vehicle_ssn: int) -> Error:
		calls.append(["crew_local_player", vehicle_ssn])
		return action_error
	func is_joiner_network_diagnostics_enabled() -> bool:
		return net_diagnostics
	func set_joiner_network_diagnostics_enabled(value: bool) -> void:
		net_diagnostics = value
	func debug_teleport_local_player(
			position: Vector3, yaw: float, pitch: float) -> Error:
		calls.append(["teleport", position, yaw, pitch])
		return action_error
	func debug_set_entity_health(index: int, health: int) -> Error:
		calls.append(["health", index, health])
		return action_error
	func debug_set_entity_position(index: int, position: Vector3) -> Error:
		calls.append(["position", index, position])
		return action_error
	func set_mission_variable(index: int, value: int) -> void:
		calls.append(["variable", index, value])
	func is_wac_paused() -> bool:
		return wac_paused
	func set_wac_paused(value: bool) -> void:
		wac_paused = value


func _catalog(target_box: Array) -> DebugSession:
	var session := DebugSession.new()
	session.set_target_source(&"fake", func(): return target_box[0], "Fake is gone.")
	session.set_status_source(func(): return {"label": "test runtime"})
	session.register_control(DebugControlDef.check(
			&"enabled", &"Test", "Enabled", "A check.", &"fake",
			&"is_enabled", &"set_enabled"))
	session.register_control(DebugControlDef.slider(
			&"amount", &"Test", "Amount", "A slider.", &"fake",
			&"get_amount", &"set_amount", 1.0, 0.0, 4.0, 0.25))
	session.register_control(DebugControlDef.enum_control(
			&"mode", &"Test", "Mode", "An enum.", &"fake",
			&"get_mode", &"set_mode", 0, ["One", "Two", "Three"]))
	session.register_control(DebugControlDef.action_control(
			&"act", &"Test", "Act", "An action.", &"fake", &"act"))
	return session


func test_catalog_is_json_safe_and_reads_authoritative_values() -> void:
	var target := FakeTarget.new()
	target.enabled = true
	target.amount = 2.5
	target.mode = 2
	var session := _catalog([target])

	var rows := session.list_controls(&"Test")
	assert_eq(rows.size(), 4)
	assert_eq(rows[0]["kind"], "check")
	assert_eq(rows[0]["state"]["value"], true)
	assert_true(rows[0]["state"]["authoritative"])
	assert_eq(rows[1]["state"]["value"], 2.5)
	assert_eq(rows[2]["choices"], ["One", "Two", "Three"])
	assert_ne(JSON.stringify(session.capture_snapshot()), "",
			"the entire MCP snapshot is JSON-safe")

	target.enabled = false
	assert_eq(session.get_control_state(&"enabled").value, false,
			"readback mirrors an external public-owner change")
	assert_eq(session.list_controls(&"", "slider").size(), 1,
			"the catalog filter searches descriptions as well as labels")


func test_writes_validate_and_re_resolve_replaced_targets() -> void:
	var first := FakeTarget.new()
	var target_box: Array = [first]
	var session := _catalog(target_box)

	assert_eq(session.set_control_value(&"enabled", true), OK)
	assert_true(first.enabled)
	assert_eq(session.set_control_value(&"amount", 3.13), OK)
	assert_almost_eq(first.amount, 3.25, 0.001, "sliders snap to their public step")
	assert_eq(session.set_control_value(&"mode", 99), ERR_INVALID_PARAMETER)
	assert_eq(session.set_control_value(&"enabled", 1), ERR_INVALID_PARAMETER)
	for invalid in [INF, -INF, NAN]:
		assert_eq(session.set_control_value(&"amount", invalid),
				ERR_INVALID_PARAMETER,
				"non-finite slider values are rejected")
		assert_almost_eq(first.amount, 3.25, 0.001,
				"invalid slider input never reaches the public target")

	var replacement := FakeTarget.new()
	target_box[0] = replacement
	session.sync()
	assert_true(replacement.enabled,
			"desired session state replays onto a replacement target")
	assert_almost_eq(replacement.amount, 3.25, 0.001)

	target_box[0] = null
	var state := session.get_control_state(&"enabled")
	assert_false(state.available)
	assert_string_contains(state.reason, "Fake is gone")


func test_unedited_live_values_do_not_leak_into_a_replacement_target() -> void:
	var first := FakeTarget.new()
	first.amount = 2.0
	var target_box: Array = [first]
	var session := _catalog(target_box)
	assert_eq(session.get_control_state(&"amount").value, 2.0)

	var replacement := FakeTarget.new()
	replacement.amount = 3.0
	target_box[0] = replacement
	session.sync()
	assert_eq(replacement.amount, 3.0,
			"a read-only snapshot from one mission is not replayed into the next")
	assert_eq(session.get_control_state(&"amount").value, 3.0)


func test_authoritative_replay_waits_for_current_authority_and_unlock() -> void:
	var first := FakeTarget.new()
	var target_box: Array = [first]
	var session := _catalog(target_box)
	var control := session.definition(&"enabled")
	control.requires_unlock = true
	control.authority = DebugControlDef.Authority.HOST_ONLY
	var host_authority := [true]
	session.set_authority_source(func(): return host_authority[0])
	session.set_edit_unlocked(true)
	assert_eq(session.set_control_value(&"enabled", true), OK)

	var replacement := FakeTarget.new()
	target_box[0] = replacement
	host_authority[0] = false
	session.sync()
	assert_false(replacement.enabled,
			"a joiner target never receives retained authoritative intent")

	host_authority[0] = true
	session.sync()
	assert_true(replacement.enabled,
			"the pending replay applies only once current host authority returns")

	session.set_edit_unlocked(false)
	var locked_replacement := FakeTarget.new()
	target_box[0] = locked_replacement
	session.sync()
	assert_false(locked_replacement.enabled,
			"a prior edit does not authorize future target mutation while relocked")
	session.set_edit_unlocked(true)
	assert_true(locked_replacement.enabled)


func test_debug_views_apply_immediately_and_persist_without_presentation() -> void:
	# Debug visualizers stay live whether or not the F3 overlay (or an
	# automation capture) is open: toggling applies immediately, nothing
	# resets it on hide, and only an explicit toggle turns it back off.
	var target := FakeTarget.new()
	var session := _catalog([target])
	assert_eq(session.set_control_value(&"enabled", true), OK)
	assert_true(target.enabled,
			"a view toggled with no debug presentation open applies immediately")
	assert_eq(session.set_control_value(&"mode", 2), OK)
	assert_eq(target.mode, 2)

	session.sync()
	assert_true(target.enabled, "a refresh does not reset live views")
	assert_eq(target.mode, 2)

	assert_eq(session.set_control_value(&"enabled", false), OK)
	assert_false(target.enabled, "only an explicit toggle turns the view off")


func test_edit_confirmation_and_host_authority_are_distinct_gates() -> void:
	var target := FakeTarget.new()
	var session := _catalog([target])
	var action := session.definition(&"act")
	action.requires_unlock = true
	action.authority = DebugControlDef.Authority.HOST_ONLY
	var host_authority := [false]
	session.set_authority_source(func(): return host_authority[0])

	assert_eq(int(session.invoke_control(&"act", "no", true)["error"]),
			ERR_UNAUTHORIZED,
			"explicit confirmation cannot override joiner authority")
	host_authority[0] = true
	assert_eq(int(session.invoke_control(&"act", "locked")["error"]),
			ERR_UNAUTHORIZED, "the UI gate defaults locked")
	var confirmed := session.invoke_control(&"act", "go", true)
	assert_eq(int(confirmed["error"]), OK,
			"MCP confirmation can stand in for the UI unlock")
	assert_eq(confirmed["result"], "GO")
	assert_eq(target.calls, ["go"])

	session.set_edit_unlocked(true)
	assert_eq(int(session.invoke_control(&"act", "ui")["error"]), OK)
	assert_eq(target.calls, ["go", "ui"])


func test_state_rows_report_writability_for_a_confirmed_authority_caller() -> void:
	var target := FakeTarget.new()
	var session := _catalog([target])
	session.definition(&"enabled").requires_unlock = true

	var locked := session.get_control_state(&"enabled")
	assert_false(locked.writable, "the F3 view stays locked without the latch")
	assert_string_contains(locked.reason, "Live edits")

	var confirmed := session.get_control_state(&"enabled", true)
	assert_true(confirmed.writable,
			"a per-call authority confirmation sees the write it may make")
	assert_eq(confirmed.reason, "")

	var rows := session.list_controls(&"Test", "enabled", true)
	assert_eq(rows.size(), 1)
	assert_true(bool(rows[0]["state"]["writable"]),
			"listed rows reflect the same caller authority")

	var action := session.definition(&"act")
	action.requires_unlock = true
	var outcome := session.invoke_control(&"act", "go", true)
	assert_eq(int(outcome["error"]), OK)
	assert_true(bool(outcome["state"]["writable"]),
			"the result row cannot claim the accepted invoke was locked")


func test_confirmed_authority_never_overrides_missing_host_authority() -> void:
	var target := FakeTarget.new()
	var session := _catalog([target])
	var control := session.definition(&"enabled")
	control.authority = DebugControlDef.Authority.HOST_ONLY
	session.set_authority_source(func(): return false)

	var state := session.get_control_state(&"enabled", true)
	assert_false(state.writable,
			"joiner authority is not a per-call confirmation matter")
	assert_string_contains(state.reason, "host")


func test_action_validation_and_error_returns_stop_false_success() -> void:
	var target := FakeTarget.new()
	var session := _catalog([target])
	var action := DebugControlDef.action_control(
			&"fallible", &"Test", "Fallible", "Validated action.",
			&"fake", &"fallible")
	action.action_validator = func(args: Array):
		return args.size() == 1 and typeof(args[0]) == TYPE_INT
	action.action_returns_error = true
	assert_true(session.register_control(action))

	assert_eq(int(session.invoke_control(&"fallible", "bad")["error"]),
			ERR_INVALID_PARAMETER)
	assert_eq(target.calls, [])
	assert_eq(int(session.invoke_control(&"fallible", 2)["error"]),
			ERR_UNAVAILABLE)
	assert_eq(target.calls, [2])
	assert_eq(int(session.invoke_control(&"fallible", 1)["error"]), OK)
	assert_eq(target.calls, [2, 1])


func test_error_returning_setter_propagates_rejected_write() -> void:
	var target := FakeTarget.new()
	var session := _catalog([target])
	var control := DebugControlDef.slider(
			&"fallible_amount", &"Test", "Fallible amount",
			"An Error-returning setter.", &"fake",
			&"get_amount", &"set_fallible_amount", 1.0, 0.0, 4.0, 0.25)
	control.setter_returns_error = true
	assert_true(session.register_control(control))

	target.setter_error = ERR_UNAVAILABLE
	assert_eq(session.set_control_value(&"fallible_amount", 2.0),
			ERR_UNAVAILABLE)
	assert_eq(target.amount, 1.0)
	assert_eq(session.get_control_state(&"fallible_amount").desired_value,
			1.0, "a rejected setter does not retain false desired state")

	target.setter_error = OK
	assert_eq(session.set_control_value(&"fallible_amount", 2.0), OK)
	assert_eq(target.amount, 2.0)


func test_builtin_mutations_reject_values_that_overflow_engine_storage() -> void:
	var sim := ValidationSim.new()
	var session := DebugSession.new()
	DebugCatalog.install(session)
	session.set_target_source(DebugCatalog.TARGET_SIM, func(): return sim)
	session.set_authority_source(func(): return true)
	session.set_edit_unlocked(true)

	assert_eq(int(session.invoke_control(&"set_entity_health",
			[0, 32768])["error"]), ERR_INVALID_PARAMETER)
	assert_eq(int(session.invoke_control(&"set_entity_position",
			[0, Vector3(32768.0, 0.0, 0.0)])["error"]), ERR_INVALID_PARAMETER)
	assert_eq(int(session.invoke_control(&"teleport_local_player",
			[Vector3.ZERO, 0.0, 91.0])["error"]), ERR_INVALID_PARAMETER)
	assert_eq(int(session.invoke_control(&"set_entity_health",
			["0", 25])["error"]), ERR_INVALID_PARAMETER,
			"the shared seam never coerces an entity string to entity zero")
	assert_eq(int(session.invoke_control(&"set_mission_variable",
			[17.5, -3])["error"]), ERR_INVALID_PARAMETER,
			"integer-valued controls reject fractional JSON numbers")
	assert_eq(int(session.invoke_control(&"teleport_local_player",
			[Vector3.ZERO, "0", 0.0])["error"]), ERR_INVALID_PARAMETER,
			"angles must be numeric before the public engine call")
	assert_true(sim.calls.is_empty())

	assert_eq(int(session.invoke_control(&"set_entity_health",
			[2, 32767])["error"]), OK)
	assert_eq(int(session.invoke_control(&"set_entity_position",
			[2, Vector3(32767.0, -32768.0, 0.0)])["error"]), OK)
	assert_eq(int(session.invoke_control(&"teleport_local_player",
			[Vector3.ZERO, 360.0, -90.0])["error"]), OK)
	assert_eq(sim.calls.size(), 3)

	sim.action_error = ERR_UNAVAILABLE
	assert_eq(int(session.invoke_control(&"set_entity_health",
			[2, 10])["error"]), ERR_UNAVAILABLE)
	assert_eq(int(session.invoke_control(&"set_entity_position",
			[2, Vector3.ZERO])["error"]), ERR_UNAVAILABLE)
	assert_eq(int(session.invoke_control(&"teleport_local_player",
			[Vector3.ZERO, 0.0, 0.0])["error"]), ERR_UNAVAILABLE)
	assert_eq(sim.calls.size(), 6,
			"all three built-in actions propagate their public Error result")


func test_automation_actions_validate_their_typed_arguments() -> void:
	var sim := ValidationSim.new()
	var session := DebugSession.new()
	DebugCatalog.install(session)
	session.set_target_source(DebugCatalog.TARGET_SIM, func(): return sim)
	session.set_authority_source(func(): return true)
	session.set_edit_unlocked(true)

	assert_eq(int(session.invoke_control(&"deploy_pick", [-1])["error"]),
			ERR_INVALID_PARAMETER)
	assert_eq(int(session.invoke_control(&"kill_group", [0])["error"]),
			ERR_INVALID_PARAMETER)
	assert_eq(int(session.invoke_control(&"crew_vehicle", [3])["error"]),
			ERR_INVALID_PARAMETER)
	assert_eq(int(session.invoke_control(&"crew_local_player", ["11"])["error"]),
			ERR_INVALID_PARAMETER,
			"an SSN string is never coerced to an entity")
	assert_true(sim.calls.is_empty())

	assert_eq(int(session.invoke_control(&"deploy_pick", [0])["error"]), OK)
	assert_eq(int(session.invoke_control(&"kill_group", [14])["error"]), OK)
	assert_eq(int(session.invoke_control(&"crew_vehicle", [1766, 11])["error"]), OK)
	assert_eq(int(session.invoke_control(&"crew_local_player", [11])["error"]), OK)
	assert_eq(sim.calls, [["deploy_pick", 0], ["kill_group", 14],
			["crew_vehicle", 1766, 11], ["crew_local_player", 11]])

	sim.action_error = ERR_UNAVAILABLE
	assert_eq(int(session.invoke_control(&"crew_local_player", [11])["error"]),
			ERR_UNAVAILABLE, "the seat refusal propagates as the action result")

	assert_eq(session.get_control_state(&"net_joiner_diagnostics").value, false)
	assert_eq(session.set_control_value(&"net_joiner_diagnostics", true), OK)
	assert_true(sim.net_diagnostics)
