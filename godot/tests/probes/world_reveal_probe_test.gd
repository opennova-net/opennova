extends GutTest

# world_reveal over the REAL shell (WorldFixture.boot_shell, the minimal mission started through the front end):
# the registered probe and its typed argument, and a world already revealed passing at once with no splash
# dismissed (a headless shell never raises the start-mission splash: loading_screen_splash_test.gd's
# test_maybe_begin_never_raises_headless; the splash's programmatic dismissal it calls is pinned there too).

const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

var _config: TestFs.Snapshot
var _temp_dir := ""
var _shell: MainGame = null


func before_each() -> void:
	_config = TestFs.snapshot(STATE_CONFIG_PATH)
	LaunchFlags.set_args_override(PackedStringArray([]))


func after_each() -> void:
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	if not _temp_dir.is_empty():
		TestFs.remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	LaunchFlags.clear_args_override()
	_config.restore()


func test_the_catalog_registers_world_reveal() -> void:
	var def := ProbeDef.definition("world_reveal")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/runtime/"), "the runtime family")
	assert_false(def.needs_window, "a headless game can be asked")
	assert_true(def.needs_mission, "it reveals a loaded mission")
	var defaults := def.validate_args({})
	assert_true(defaults.ok)
	assert_eq_deep(defaults.values, {"timeout_ms": 60000})
	assert_false(def.validate_args({"timeout_ms": 10}).ok, "a timeout under a second is refused")


func test_a_revealed_world_passes_at_once() -> void:
	_shell = await WorldFixture.boot_shell(self)
	_temp_dir = WorldFixture.last_shell_dir()
	if _shell == null:
		return
	var loaded: bool = await WorldFixture.start_shell_mission(self, _shell)
	assert_true(loaded, "the minimal mission loads through the real front end")
	if not loaded:
		return
	await get_tree().process_frame
	assert_false(_shell.is_world_loading(), "a headless load raises no splash")
	var def := ProbeDef.definition("world_reveal")
	var ctx := ProbeContext.new()
	ctx.name = def.name
	ctx.args = def.validate_args({}).values
	ctx.tree = get_tree()
	ctx.shell = _shell
	var verdict: ProbeVerdict = await def.load_probe().run(ctx)
	ctx.finish()
	assert_true(verdict.ok, verdict.summary)
	assert_eq_deep(verdict.data, {"held": false, "dismissed": false})
	assert_eq(verdict.summary, "the world is revealed")
