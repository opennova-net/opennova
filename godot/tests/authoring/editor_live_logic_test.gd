extends GutTest

## The editor's link to a running game's mission logic (ADR 0046 S24, Live) over the real stream: the editor
## booted headless from its scene, a project with a mission of two events, Play started on it (the child an idle
## Godot, live_idle_game.gd, so nothing else listens on Play's MCP port), and the game's MCP endpoint answering on
## that port in this process: the game's own server (McpServer) with the catalog's game_debug tool, its op=invoke
## answered here over the real debug-control table of the mission booted here (the debug rows
## mission_logic_snapshot, fire_event and set_mission_variable, master's #1026) as the game's tool answers it. The
## editor's
## StreamPeerTCP link asks the game for its logic while the mission's Logic pane shows, the editor's live op agrees
## with what the game holds, play_debug fires an event and sets a variable through the game's rows, and the game's
## refusal reaches Output in its words.

const EditorFixture := preload("res://tests/authoring/editor_fixture.gd")
const IDLE_GAME := "res://tests/authoring/live_idle_game.gd"
const MISSION := "missions/logic.bms"


var _dirs: Array[String] = []
var _app: Node = null
var _seam: RefCounted = null
var _server: McpServer = null
var _sim: Simulation = null
var _controls: DebugControlTable = null


func before_each() -> void:
	_seam = await EditorFixture.boot(self, "live", _dirs)
	_app = _seam.app if _seam != null else null


func after_each() -> void:
	if _server != null and is_instance_valid(_server):
		_server.stop()
	_server = null
	_sim = null
	_controls = null
	if is_instance_valid(_app):
		_seam.stop_play()
		var deadline := Time.get_ticks_msec() + 10000
		while _seam.get_play_state() != "stopped" and Time.get_ticks_msec() < deadline:
			await get_tree().create_timer(0.05).timeout
			_app.pump()
	await EditorFixture.teardown(self, _dirs)


# A request through the wire seam, settled.
func _request(fields: Dictionary) -> Dictionary:
	return _seam.request(fields)


# The editor's `live` op over the mission.
func _live() -> Dictionary:
	return _seam.query("mission_logic", {"path": MISSION, "op": "live"})


# The editor and the game run until `condition` holds (the editor pumped, the game's logic stepped, a frame
# for the game's server), or `timeout_ms` passes: whether it held.
func _until(condition: Callable, timeout_ms := 30000) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		_app.pump()
		if _sim != null:
			_sim.step()
		await get_tree().process_frame
		if condition.call():
			return true
	return false


# The game's game_debug tool as its op=invoke answers (game_mcp_tools.gd): the row's own arg schema marshals the
# arguments, the table invokes it for the caller's confirmed authority, a refusal carries the row's words.
func _game_debug(args: Dictionary, _ctx: McpToolContext) -> Variant:
	var id := StringName(String(args.get("id", "")))
	if String(args.get("op", "")) != "invoke" or id.is_empty():
		return McpToolResult.error("This test's game answers game_debug op=invoke alone.")
	var marshalled: DebugMarshalResult = _controls.marshal_invoke_args(id, args.get("args"))
	if marshalled.refused:
		return McpToolResult.error(marshalled.reason)
	var outcome: DebugInvokeResult = _controls.invoke(id, marshalled.args, bool(args.get("confirm_authority", false)))
	if outcome.error != OK:
		var why := String(outcome.result) if outcome.result is String else error_string(outcome.error)
		return McpToolResult.error("Debug control '%s' refused: %s" % [id, why])
	return outcome.to_json_value()


# The Output's lines, joined.
func _output() -> String:
	return "\n".join(_seam.get_output_lines())


func test_the_live_link_reads_the_game_and_fires_through_its_debug_table() -> void:
	if _app == null:
		return
	var root := OS.get_cache_dir().path_join("opennova live project %d" % Time.get_ticks_usec())
	_dirs.append(root)
	assert_true(_seam.new_project(root, "Live"), _output())
	_seam.create_missing_files()
	# The mission: event 1 waits on V1 == 1 (MissionVariable 4 / IsEqual 1) and then sets V3 to 7 (MisvarChange 5 /
	# Set 1); event 2 is a pre-mission event (flags 2), which the game refuses to fire.
	var mission := WorldFixture.default_mission(0)
	assert_gte(mission.add_event(0, 0, 0), 0)
	assert_true(mission.add_event_trigger(0, 4, 1, 1, 1))
	assert_true(mission.add_event_action(0, 5, 1, 3, 7))
	assert_gte(mission.add_event(2, 0, 0), 1)
	assert_eq(DirAccess.make_dir_recursive_absolute(root.path_join("missions")), OK)
	assert_eq(mission.save_as(root.path_join(MISSION)), OK)
	_request({"kind": "rescan"})
	assert_true(_seam.open_document(MISSION), _output())
	_request({"kind": "set_workspace", "workspace": {"document": {"path": MISSION, "logic": true}}})

	# Play the mission on an idle child: nothing listens on its MCP port but the game this test runs.
	_app.set("play_engine_args", PackedStringArray(["--headless", "--script", IDLE_GAME]))
	var played: Dictionary = _request({"kind": "play", "mission": "logic.bms"}).get("outcome", {})
	assert_true(bool(played.get("done", false)), str(played))
	assert_true(await _until(func() -> bool: return _seam.get_play_state() == "running", 120000), _output())
	var port: int = _seam.get_play_mcp_port()
	assert_gt(port, 0)
	var waiting: Dictionary = _live()
	assert_true(bool(waiting.get("reachable", false)) and bool(waiting.get("polling", false)), str(waiting))
	assert_false(bool(waiting.get("playing", true)), "nothing reported before the game answers")

	# The game: the same mission booted here as Play's game would boot it (its file named, so its report names
	# the mission), its logic's clock open (the script admission), its debug table behind the game's own tools.
	var options := MissionSetupOptions.new()
	options.set_mission_file("logic.bms")
	var runtime := WorldFixture.boot_mission_data(self, mission, options)
	_sim = runtime.get_sim()
	assert_true(_sim.compile_and_set_wac(PackedStringArray(["set(ticks,-1)"])))
	_controls = DebugControlTable.new()
	_controls.setup(DebugHostFixture.for_runtime(runtime, self))
	_server = add_child_autofree(McpServer.new())
	for def in GameMcpCatalog.definitions():
		if def.name == "game_debug":
			assert_eq(_server.registry.register(def, _game_debug), OK)
	assert_eq(_server.start(port), OK, "the game's endpoint on Play's port")

	# The link reads the game: event 1's trigger checked and false (V1 is 0), the editor's live op agreeing with
	# the game's own report, the event named by the document's row.
	var checked := func() -> bool:
		var events: Array = _live().get("events", [])
		return events.size() == 2 and int(events[0].get("last_checked_tick", -1)) >= 0
	assert_true(await _until(checked), str(_live()) + "\n" + _output())
	var live := _live()
	assert_true(bool(live.get("playing", false)) and not bool(live.get("stale", true)), str(live))
	var first: Dictionary = live["events"][0]
	assert_true(first.has("event"), "the played event is the document's row at its place: %s" % first)
	assert_eq(int(first["triggers"][0]["truth"]), 0, "V1 is 0: the term read false")
	assert_false(bool(first.get("active", true)))
	var game: Dictionary = _controls.invoke(&"mission_logic_snapshot", []).result
	assert_eq(int(game["events"][0]["last_checked_tick"]) >= int(first["last_checked_tick"]), true,
			"the editor's report is one the game gave")

	# Fire event 1 through play_debug: the game's row takes it, the event fires, its action sets V3.
	var fire := _request({"kind": "play_debug", "event": 0})
	assert_true(bool(fire.get("outcome", {}).get("done", false)), str(fire))
	var fired := func() -> bool:
		var events: Array = _live().get("events", [])
		return events.size() == 2 and bool(events[0].get("fired", false))
	assert_true(await _until(fired), str(_live()) + "\n" + _output())
	assert_eq(_sim.get_mission_variable(3), 7, "its action ran in the game")
	assert_string_contains(_output(), "The game took the call to fire event 1")

	# The pre-mission event: the game refuses it, its words in Output.
	_request({"kind": "play_debug", "event": 1})
	var refused := func() -> bool: return _output().contains("The game refused to fire event 2")
	assert_true(await _until(refused), _output())
	assert_string_contains(_output(), "pre-mission")

	# Set V12 to 5: the game's variable, then the editor's report of it.
	_request({"kind": "play_debug", "variable": 12, "value": 5})
	var set_twelve := func() -> bool:
		for variable in _live().get("variables", []):
			if int(variable.get("index", -1)) == 12 and int(variable.get("value", 0)) == 5:
				return true
		return false
	assert_true(await _until(set_twelve), str(_live()) + "\n" + _output())
	assert_eq(_sim.get_mission_variable(12), 5)

	# The Logic pane closed: nothing asked, the report gone.
	_request({"kind": "set_workspace", "workspace": {"document": {"path": MISSION, "logic": false}}})
	assert_true(await _until(func() -> bool: return not bool(_live().get("playing", true))), str(_live()))
	assert_false(bool(_live().get("polling", true)))
