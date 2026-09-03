extends GutTest

# The game_probe machinery (ADR 0041; the four-type probe model of ADR 0043
# d12): ProbeRunner over fixture probes (run/status/cursor/long-poll/cancel/
# one-at-a-time/preconditions/schema/artifacts/restores/log sink/watchdog),
# then the same through the loopback game_probe tool, including the refusal
# while a serial tool job runs.

const FIXTURES := "res://tests/probes/fixtures"
const McpTestClient := preload("res://tests/mcp/mcp_test_client.gd")
const SETTLE_FRAMES := 900

var _runner: ProbeRunner


func before_each() -> void:
	_runner = add_child_autofree(ProbeRunner.new())
	_runner.unresponsive_grace_ms = 200
	_runner.set_definitions(_defs())


func after_each() -> void:
	McpLogHub.instance = null
	Engine.time_scale = 1.0
	TestFs.remove_dir_recursive(ProjectSettings.globalize_path(ProbeRunner.RUNS_DIR))


func _defs() -> Array[ProbeDef]:
	return [
		ProbeDef.make("echo", "echoes its args", FIXTURES + "/echo_fixture.gd", {
			"text": {"type": "string", "default": "hi"},
			"count": {"type": "integer", "minimum": 1, "maximum": 5, "default": 1},
		}),
		ProbeDef.make("slow", "runs for a while", FIXTURES + "/slow_fixture.gd", {
			"frames": {"type": "integer", "minimum": 1, "default": 600},
		}),
		ProbeDef.make("failing", "fails", FIXTURES + "/failing_fixture.gd"),
		ProbeDef.make("erroring", "returns no verdict", FIXTURES + "/erroring_fixture.gd"),
		ProbeDef.make("stubborn", "ignores cancel", FIXTURES + "/stubborn_fixture.gd",
				{}, [], false, false, 100),
		ProbeDef.make("restore", "mutates and restores", FIXTURES + "/restore_fixture.gd"),
		ProbeDef.make("windowed", "needs a window", FIXTURES + "/echo_fixture.gd",
				{}, [], true),
		ProbeDef.make("mission", "needs a mission", FIXTURES + "/echo_fixture.gd",
				{}, [], false, true),
		ProbeDef.make("missing", "not shipped", FIXTURES + "/does_not_exist.gd"),
	]


func _settled(run_id: String) -> Dictionary:
	for _i in SETTLE_FRAMES:
		var status: Dictionary = await _runner.status(run_id, 0, 0)
		if String(status["state"]) != ProbeRunner.STATE_RUNNING:
			return status
		await get_tree().process_frame
	return await _runner.status(run_id, 0, 0)


func test_list_reports_the_catalog_with_availability() -> void:
	var listing := _runner.list()
	var probes: Array = listing["probes"]
	assert_eq(probes.size(), 9)
	var by_name := {}
	for entry in probes:
		by_name[entry["name"]] = entry
	assert_true(bool(by_name["echo"]["available"]))
	assert_false(bool(by_name["missing"]["available"]), "an absent script lists as unavailable")
	assert_true(bool(by_name["windowed"]["needs_window"]))
	assert_true(bool(by_name["mission"]["needs_mission"]))
	assert_eq(int(by_name["stubborn"]["timeout_ms"]), 100)
	assert_eq(by_name["echo"]["input_schema"]["properties"]["count"]["maximum"], 5)
	assert_eq(String(listing["active_run_id"]), "")


func test_run_status_verdict_lines_and_cursor_paging() -> void:
	var started := _runner.start("echo", {"count": 2})
	assert_false(started.has("refused"), str(started))
	var run_id := String(started["run_id"])
	assert_true(run_id.begins_with("echo-"))
	assert_true(DirAccess.dir_exists_absolute(String(started["artifact_dir"])),
			"the artifact directory exists before the probe runs")
	assert_true(_runner.is_running())
	var status := await _settled(run_id)
	assert_eq(String(status["state"]), ProbeRunner.STATE_PASSED)
	assert_true(bool(status["verdict"]["ok"]))
	assert_eq(String(status["verdict"]["summary"]), "echoed")
	assert_eq(status["verdict"]["data"]["args"]["text"], "hi", "the default filled in")
	assert_eq(int(status["verdict"]["data"]["args"]["count"]), 2)
	assert_eq(status["progress"]["step"], 1)
	var texts: Array = []
	for line in status["lines"]:
		texts.append(String(line["text"]))
	assert_true("echo done" in texts, str(texts))
	assert_true(String(texts[0]).begins_with("probe echo started"))
	assert_true(String(texts.back()).begins_with("probe echo passed"))
	var cursor := int(status["next_cursor"])
	assert_eq(cursor, status["lines"].size())
	var page: Dictionary = await _runner.status(run_id, cursor, 0)
	assert_true((page["lines"] as Array).is_empty(), "nothing after the last cursor")
	assert_eq(int(page["next_cursor"]), cursor)
	var partial: Dictionary = await _runner.status(run_id, 1, 0)
	assert_eq(int(partial["lines"][0]["seq"]), 2)
	assert_false(_runner.is_running())


func test_status_long_polls_until_new_lines_and_cancel_stops_a_run() -> void:
	var started := _runner.start("slow", {})
	var run_id := String(started["run_id"])
	var first: Dictionary = await _runner.status(run_id, 0, 0)
	var cursor := int(first["next_cursor"])
	var t0 := Time.get_ticks_msec()
	var polled: Dictionary = await _runner.status(run_id, cursor, 3000)
	assert_true((polled["lines"] as Array).size() > 0, "the long-poll woke for new lines")
	assert_lt(Time.get_ticks_msec() - t0, 3000, "and returned before its wait budget")
	assert_eq(String(polled["state"]), ProbeRunner.STATE_RUNNING)
	var cancelled := _runner.cancel(run_id)
	assert_true(bool(cancelled["cancel_requested"]))
	var status := await _settled(run_id)
	assert_eq(String(status["state"]), ProbeRunner.STATE_CANCELLED)
	assert_true(bool(status["cancel_requested"]))
	assert_false(_runner.is_running())


func test_one_probe_at_a_time() -> void:
	var started := _runner.start("slow", {})
	var second := _runner.start("echo", {})
	assert_true(String(second.get("refused", "")).contains("still running"), str(second))
	_runner.cancel(String(started["run_id"]))
	var status := await _settled(String(started["run_id"]))
	assert_eq(String(status["state"]), ProbeRunner.STATE_CANCELLED)
	var third := _runner.start("echo", {})
	assert_false(third.has("refused"), "a settled run frees the slot")
	await _settled(String(third["run_id"]))


func test_preconditions_schema_and_catalog_refusals() -> void:
	if DisplayServer.get_name() == "headless":
		var windowed := _runner.start("windowed", {})
		assert_true(String(windowed.get("refused", "")).contains("needs a window"), str(windowed))
	var mission := _runner.start("mission", {})
	assert_true(String(mission.get("refused", "")).contains("needs a loaded mission"), str(mission))
	var unknown := _runner.start("nope", {})
	assert_true(String(unknown.get("refused", "")).begins_with("Unknown probe"), str(unknown))
	var invalid := _runner.start("echo", {"count": 9, "bogus": true})
	assert_true(String(invalid.get("refused", "")).contains("invalid"), str(invalid))
	assert_true("'count' must be <= 5" in invalid["details"], str(invalid["details"]))
	assert_true("unknown argument 'bogus'" in invalid["details"], str(invalid["details"]))
	var missing := _runner.start("missing", {})
	assert_true(String(missing.get("refused", "")).contains("not shipped"), str(missing))
	var wrong_shape := _runner.start("echo", [1])
	assert_true(wrong_shape.has("refused"))
	assert_false(_runner.is_running(), "no refusal leaves a run behind")


func test_fail_and_missing_verdict_states() -> void:
	var failing := await _settled(String(_runner.start("failing", {})["run_id"]))
	assert_eq(String(failing["state"]), ProbeRunner.STATE_FAILED)
	assert_false(bool(failing["verdict"]["ok"]))
	assert_eq(int(failing["verdict"]["data"]["measured"]), 3)
	var erroring := await _settled(String(_runner.start("erroring", {})["run_id"]))
	assert_eq(String(erroring["state"]), ProbeRunner.STATE_ERROR)
	assert_null(erroring["verdict"])
	assert_true(String(erroring["error"]).contains("no verdict"), str(erroring))


func test_watchdog_cancels_on_timeout_and_reaps_an_unresponsive_run() -> void:
	var started := _runner.start("stubborn", {})
	var status := await _settled(String(started["run_id"]))
	assert_eq(String(status["state"]), ProbeRunner.STATE_ERROR)
	assert_true(bool(status["cancel_requested"]), "the timeout asked first")
	assert_true(String(status["error"]).contains("unresponsive"), str(status))
	assert_false(_runner.is_running(), "the reaped run frees the slot")
	var next := _runner.start("echo", {})
	assert_false(next.has("refused"), str(next))
	await _settled(String(next["run_id"]))


func test_cancel_and_wait_settles_a_stubborn_run() -> void:
	_runner.unresponsive_grace_ms = ProbeRunner.DEFAULT_UNRESPONSIVE_GRACE_MS
	var started := _runner.start("stubborn", {})
	await _runner.cancel_and_wait(150)
	assert_false(_runner.is_running())
	var status: Dictionary = await _runner.status(String(started["run_id"]), 0, 0)
	assert_eq(String(status["state"]), ProbeRunner.STATE_ERROR)
	assert_true(String(status["error"]).contains("shutdown"), str(status))


func test_restores_run_on_finish_and_artifacts_are_recorded() -> void:
	var status := await _settled(String(_runner.start("restore", {})["run_id"]))
	assert_eq(String(status["state"]), ProbeRunner.STATE_PASSED)
	assert_almost_eq(float(status["verdict"]["data"]["time_scale_during"]), 0.5, 0.001)
	assert_almost_eq(Engine.time_scale, 1.0, 0.001, "finish() restored the time scale")
	var artifacts: Array = status["artifacts"]
	assert_eq(artifacts.size(), 1)
	assert_eq(String(artifacts[0]["label"]), "note")
	assert_eq(String(artifacts[0]["kind"]), "txt")
	assert_gt(int(artifacts[0]["bytes"]), 0)
	assert_eq(String(artifacts[0]["sha256"]).length(), 64)
	assert_true(String(artifacts[0]["path"]).begins_with(String(status["artifact_dir"])))
	var texts: Array = []
	for line in status["lines"]:
		texts.append(String(line["text"]))
	assert_true("restore: custom undo ran" in texts, str(texts))


func test_probe_lines_reach_the_log_hub_as_the_probe_source() -> void:
	# The probe model never names the transport: the service installs the
	# runner's log sink (game_mcp_service.gd does the same wiring).
	var hub := McpLogHub.new()
	McpLogHub.instance = hub
	_runner.log_sink = func(text: String) -> void: hub.note("probe", "info", text)
	await _settled(String(_runner.start("echo", {"text": "hub"})["run_id"]))
	var page := hub.get_entries(0, 50, PackedStringArray(["probe"]))
	var entries: Array = page["entries"]
	assert_gt(entries.size(), 0)
	assert_true(String(entries[0]["text"]).begins_with("[echo] "), str(entries[0]))


func test_leaving_the_tree_reaps_the_active_run() -> void:
	var runner := ProbeRunner.new()
	add_child(runner)
	runner.set_definitions(_defs())
	var started := runner.start("slow", {})
	remove_child(runner)
	var status: Dictionary = await runner.status(String(started["run_id"]), 0, 0)
	assert_eq(String(status["state"]), ProbeRunner.STATE_ERROR)
	assert_false(runner.is_running())
	runner.free()


# --- through the loopback tool ------------------------------------------------

func _service() -> GameMcpService:
	var service: GameMcpService = add_child_autofree(GameMcpService.new())
	var game_adapter: GameMcpAdapter = add_child_autofree(GameMcpAdapter.new())
	assert_eq(service.setup(game_adapter, 0), OK)
	service.probe_runner.set_definitions(_defs())
	service.probe_runner.unresponsive_grace_ms = 200
	return service


func _connected(port: int) -> RefCounted:
	var client := McpTestClient.new()
	assert_true(await client.connect_to(get_tree(), port))
	assert_true(await client.initialize(get_tree()) != null)
	return client


func _structured(envelope: Variant) -> Dictionary:
	assert_true(envelope is Dictionary and (envelope as Dictionary).has("result"), str(envelope))
	var payload: Dictionary = envelope["result"]
	if not payload.has("structuredContent"):
		fail_test("tool result carried no structuredContent: %s" % JSON.stringify(payload))
		return {"state": "missing", "lines": [], "probes": [], "run_id": ""}
	return payload["structuredContent"]


func _call_into(client: RefCounted, name: String, state: Dictionary, key: String, args := {}) -> void:
	state[key] = await client.call_tool(get_tree(), name, args)


func test_game_probe_tool_runs_polls_and_cancels_over_loopback() -> void:
	var service := _service()
	var client: RefCounted = await _connected(service.server.get_port())
	var listing := _structured(await client.call_tool(get_tree(), "game_probe", {"op": "list"}))
	assert_eq((listing["probes"] as Array).size(), 9)
	var started := _structured(await client.call_tool(get_tree(), "game_probe", {
		"op": "run", "name": "slow", "args": {"frames": 400},
	}))
	var run_id := String(started.get("run_id", ""))
	assert_false(run_id.is_empty(), str(started))
	var polled := _structured(await client.call_tool(get_tree(), "game_probe", {
		"op": "status", "run_id": run_id, "cursor": 1, "wait_ms": 2000,
	}))
	assert_eq(String(polled["state"]), ProbeRunner.STATE_RUNNING)
	assert_gt((polled["lines"] as Array).size(), 0)
	var cancelled := _structured(await client.call_tool(get_tree(), "game_probe", {"op": "cancel"}))
	assert_eq(String(cancelled["run_id"]), run_id)
	var final := {}
	for _i in SETTLE_FRAMES:
		final = _structured(await client.call_tool(get_tree(), "game_probe", {"op": "status", "wait_ms": 100}))
		if String(final["state"]) != ProbeRunner.STATE_RUNNING:
			break
	assert_eq(String(final["state"]), ProbeRunner.STATE_CANCELLED)
	var bad: Variant = await client.call_tool(get_tree(), "game_probe", {"op": "status", "wait_ms": 99999})
	assert_true(bool(bad["result"]["isError"]), "wait_ms past the cap is rejected")
	var unknown: Variant = await client.call_tool(get_tree(), "game_probe", {"op": "run", "name": "nope"})
	assert_true(bool(unknown["result"]["isError"]))
	client.close()


func test_game_probe_run_is_refused_while_a_serial_tool_job_runs() -> void:
	var service := _service()
	var gate := {"release": false}
	service.server.registry.register(McpToolDef.make("slow_serial", "holds the queue"),
			func(_args: Dictionary, ctx: McpToolContext) -> Variant:
				while not gate["release"] and not ctx.cancelled:
					await get_tree().process_frame
				return {"done": true})
	var busy: RefCounted = await _connected(service.server.get_port())
	var other: RefCounted = await _connected(service.server.get_port())
	var state := {}
	_call_into(busy, "slow_serial", state, "serial")
	for _i in 30:
		await get_tree().process_frame
	assert_true(service.server.is_tool_running(), "the serial job is in flight")
	var refused: Variant = await other.call_tool(get_tree(), "game_probe", {"op": "run", "name": "echo"})
	assert_true(bool(refused["result"]["isError"]))
	assert_true(String(refused["result"]["content"][0]["text"]).contains("serial tool call"))
	var listing: Variant = await other.call_tool(get_tree(), "game_probe", {"op": "list"})
	assert_false(bool(listing["result"]["isError"]), "reads still work alongside the job")
	gate["release"] = true
	for _i in SETTLE_FRAMES:
		if state.has("serial"):
			break
		await get_tree().process_frame
	assert_true(state.has("serial"))
	busy.close()
	other.close()


func test_game_control_quit_cancels_the_active_probe_first() -> void:
	var service := _service()
	var client: RefCounted = await _connected(service.server.get_port())
	var started := _structured(await client.call_tool(get_tree(), "game_probe", {"op": "run", "name": "slow"}))
	assert_true(service.probe_runner.is_running())
	# The base adapter's quit reports unavailable, but the tool cancels the
	# probe before asking the shell.
	var quit: Variant = await client.call_tool(get_tree(), "game_control", {"action": "quit"})
	assert_true(quit is Dictionary)
	assert_false(service.probe_runner.is_running(), "quit settled the probe before the shell verb")
	var status: Dictionary = await service.probe_runner.status(String(started["run_id"]), 0, 0)
	assert_eq(String(status["state"]), ProbeRunner.STATE_CANCELLED)
	client.close()
