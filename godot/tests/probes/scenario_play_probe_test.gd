extends GutTest

# scenario_play over the REAL shell frame loop (WorldFixture.boot_shell, the
# minimal mission started through the front end): the registered probe, its
# typed arguments, the single-player setup pose (teleport_local_player), the
# script armed on the shared binding model and played by the shell's own
# frames, and the verdict's record; then the wait-false / collect split and
# the refusals. The joiner's pose write rides tests/net/joiner_apply_pose_test.

const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

var _config: TestFs.Snapshot
var _temp_dir := ""
var _shell: MainGame = null


func before_each() -> void:
	_config = TestFs.snapshot(STATE_CONFIG_PATH)
	LaunchFlags.set_args_override(PackedStringArray([]))
	ResourceDirSettings.set_expansion("")


func after_each() -> void:
	var attached := ControlsBindings.model().get_scripted_input()
	if attached != null:
		attached.cancel()
	ControlsBindings.model().set_scripted_input(null)
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	if not _temp_dir.is_empty():
		TestFs.remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	LaunchFlags.clear_args_override()
	_config.restore()


func test_the_catalog_registers_scenario_play() -> void:
	var def := ProbeDef.definition("scenario_play")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/net/"), "the net family")
	assert_false(def.needs_window, "a headless joiner can play a scenario")
	assert_true(def.needs_mission)
	assert_eq(def.input_schema.get("required", []), [], "every argument has a default")
	var defaults := def.validate_args({})
	assert_true(defaults.ok)
	assert_eq_deep(defaults.values, {"scenario": {}, "path": "", "wait": true})
	assert_false(def.validate_args({"scenario": [1]}).ok, "the scenario is an object")


func test_scenario_play_plays_a_script_through_the_shell_frame() -> void:
	if await _booted_in_world() == null:
		return
	var sim := _shell.get_world().get_sim()
	var home: Vector3 = sim.get_local_player_position()
	# Godot (x, y up, z) -> mission (x, -z, y): the pose 6 units east of home.
	var target := Vector3(home.x + 6.0, -home.z, home.y)
	var verdict := await _run({
		"scenario": {
			"setup": {"pose": {"position_bms": [target.x, target.y, target.z],
					"yaw": 90.0, "pitch": 0.0}},
			"steps": [
				{"tick": 0, "down": 152},
				{"tick": 30, "up": 152},
				{"tick": 32, "press": 169},
				{"tick": 40, "end": true},
			],
		},
	})
	assert_true(verdict.ok, verdict.summary)
	var data := verdict.data
	assert_eq(data.state, "finished")
	assert_eq(data.role, "local")
	assert_eq(data.setup.write, "teleport_local_player", "single player keeps the teleport")
	var start := int(data.start_logic_tick)
	assert_gt(start, 0, "the start latched on a live logic tick")
	var steps: Array = data.steps
	assert_eq(steps.size(), 4)
	var scheduled := [0, 30, 32, 40]
	var previous := start
	for i in steps.size():
		assert_eq(int(steps[i].index), i)
		var applied := int(steps[i].applied_logic_tick)
		assert_gte(applied, start + scheduled[i], "step %d applies no earlier than its tick" % i)
		assert_gte(applied, previous, "the steps apply in order")
		previous = applied
	assert_gt(int(steps[2].release_logic_tick), int(steps[2].applied_logic_tick),
			"the press releases on a later sample")
	assert_eq(int(data.end_logic_tick), int(steps[3].applied_logic_tick))
	# Ground-plane checks: a fresh spawn may still be settling onto the
	# terrain, so the teleported height is the motor's to correct.
	var landed: Vector3 = sim.get_local_player_position()
	var landed_flat := Vector2(landed.x, landed.z)
	assert_gt(landed_flat.distance_to(Vector2(home.x, home.z)), 4.0,
			"the setup pose moved the player off home")
	assert_lt(landed_flat.distance_to(Vector2(target.x, -target.y)), 6.0,
			"the walk started from the setup pose (home %s target %s landed %s)" % [
					home, target, landed])
	assert_eq(int(sim.get_local_player_stance()), 1, "the crouch press reached the sim")
	assert_null(ControlsBindings.model().get_scripted_input(),
			"a waited run detaches its device at finish")


func test_wait_false_returns_at_the_start_and_a_bare_run_collects() -> void:
	if await _booted_in_world() == null:
		return
	var armed := await _run({
		"scenario": {"steps": [{"tick": 0, "down": 151}, {"tick": 20, "end": true}]},
		"wait": false,
	})
	assert_true(armed.ok, armed.summary)
	assert_eq(armed.data.state, "running", "returned as soon as the start latched")
	assert_true((armed.data.setup as Dictionary).is_empty(), "no setup pose, none applied")
	assert_not_null(ControlsBindings.model().get_scripted_input(),
			"the script keeps playing past the run")
	var collected := await _run({})
	assert_true(collected.ok, collected.summary)
	assert_eq(collected.data.state, "finished")
	assert_eq(int(collected.data.start_logic_tick), int(armed.data.start_logic_tick),
			"the collect reports the same script")
	assert_null(ControlsBindings.model().get_scripted_input(), "collecting detaches it")
	var nothing := await _run({})
	assert_false(nothing.ok, "nothing left to collect")


func test_refusals_name_what_is_wrong() -> void:
	if await _booted_in_world() == null:
		return
	var both := await _run({"scenario": {"steps": []}, "path": "x.json"})
	assert_false(both.ok)
	assert_string_contains(both.summary, "not both")
	var no_steps := await _run({"scenario": {"setup": {}}})
	assert_false(no_steps.ok)
	assert_string_contains(no_steps.summary, "steps")
	var bad := await _run({"scenario": {"steps": [
		{"tick": 0, "press": 9999}, {"tick": 2, "end": true}]}})
	assert_false(bad.ok)
	assert_string_contains(bad.summary, "action code 9999")
	var missing := await _run({"path": _temp_dir.path_join("absent.json")})
	assert_false(missing.ok)
	assert_string_contains(missing.summary, "no scenario file")
	var pose := await _run({"scenario": {"setup": {"pose": {"position_bms": [1, 2]}},
			"steps": [{"tick": 0, "end": true}]}})
	assert_false(pose.ok)
	assert_string_contains(pose.summary, "position_bms")
	assert_null(ControlsBindings.model().get_scripted_input(), "no refused run attaches a device")


# The registered probe run the way the runner does (validated args, a fresh
# context over the shell, finish() on the way out).
func _run(raw: Dictionary) -> ProbeVerdict:
	var def := ProbeDef.definition("scenario_play")
	var validated := def.validate_args(raw)
	assert_true(validated.ok, str(validated.errors))
	var ctx := ProbeContext.new()
	ctx.name = def.name
	ctx.args = validated.values
	ctx.tree = get_tree()
	ctx.shell = _shell
	var probe := def.load_probe()
	var verdict: ProbeVerdict = await probe.run(ctx)
	ctx.finish()
	return verdict


func _booted_in_world() -> MainGame:
	_shell = await WorldFixture.boot_shell(self)
	_temp_dir = WorldFixture.last_shell_dir()
	if _shell == null:
		return null
	var loaded: bool = await WorldFixture.start_shell_mission(self, _shell)
	assert_true(loaded, "the minimal mission loads through the real front end")
	if not loaded:
		return null
	await get_tree().process_frame
	assert_true(_shell.is_gameplay_input_active(), "gameplay input is live in the world")
	return _shell
