extends GutTest

# load_timeline over the REAL shell (WorldFixture.boot_shell, the minimal mission started through the
# front end): the registered probe, and its read after the load, which carries every span the load
# bracketed (godot/src/world/game_world_load.cpp: a single player's load runs the head's environment
# and terrain, then the objects and the runtime) in GameWorld.last_load_timeline()'s order, each one
# closed, with the two clocks at the read.

const STATE_CONFIG_PATH := "user://opennova.cfg"

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


func test_the_catalog_registers_load_timeline() -> void:
	var def := ProbeDef.definition("load_timeline")
	assert_not_null(def)
	if def == null:
		return
	assert_true(def.script_path.begins_with("res://probes/runtime/"), "the runtime family")
	assert_false(def.needs_window, "a headless host can read it")
	assert_false(def.needs_mission, "a failed load keeps its timeline too")
	assert_true(def.validate_args({}).ok, "it takes no argument")


func test_the_read_after_a_load_carries_its_spans() -> void:
	_shell = await WorldFixture.boot_shell(self)
	_temp_dir = WorldFixture.last_shell_dir()
	if _shell == null:
		return
	var loaded: bool = await WorldFixture.start_shell_mission(self, _shell)
	assert_true(loaded, "the minimal mission loads through the real front end")
	if not loaded:
		return
	var timeline: LoadTimeline = _shell.get_world().last_load_timeline()
	assert_not_null(timeline, "the load kept its timeline")
	if timeline == null:
		return
	var def := ProbeDef.definition("load_timeline")
	var ctx := ProbeContext.new()
	ctx.name = def.name
	ctx.args = def.validate_args({}).values
	ctx.tree = get_tree()
	ctx.shell = _shell
	ctx.artifact_dir = _temp_dir
	var verdict: ProbeVerdict = await def.load_probe().run(ctx)
	ctx.finish()
	assert_true(verdict.ok, verdict.summary)
	if not verdict.ok:
		return
	var names := []
	var last_end_us := 0
	for span: Dictionary in verdict.data.get("spans", []):
		names.append(String(span.name))
		assert_gt(int(span.end_us), 0, "%s is closed" % span.name)
		assert_true(int(span.duration_us) >= 0, "%s has a duration" % span.name)
		last_end_us = maxi(last_end_us, int(span.end_us))
	assert_eq(names, Array(timeline.span_names()), "every span, in the load's order")
	assert_true(names.size() >= 8, "the load bracketed its stages")
	if names.size() >= 8:
		assert_eq(names.slice(0, 8),
				["mount", "document", "effect_system", "texture_atlases", "environment", "terrain",
					"objects", "runtime"],
				"the entry's mount and document, the effect system with its texture atlases, the head's environment and terrain, then the objects and the runtime")
	assert_eq(String(verdict.data.label), timeline.label)
	assert_true(int(verdict.data.ticks_usec) >= last_end_us, "the read follows the load's last span")
	assert_gt(int(verdict.data.unix_ms), 0)
	assert_true(FileAccess.file_exists(_temp_dir.path_join("load_timeline.json")),
			"the record goes to the run directory")
