extends GutTest

# F3 gate (docs/oned/workspace-maturity-program.md): the "See in game" shell
# action launches the game runtime over the shared authoring directory with
# the engine's own `/d` loose-file override ([orig: `/d` loose-override
# @ 0x4a7310], NovaLaunchFlags). Seams are injected Callables, so the whole
# composition is asserted without touching the OS. Public API only (ADR 0018).


func _exists_none(_path: String) -> bool:
	return false


func test_runtime_flags_always_carry_the_loose_override() -> void:
	assert_eq(ShellGameLaunch.runtime_flags("", "jo"), PackedStringArray(["/d", "/game", "jo"]),
		"The base launch is /d plus an explicit game code — the loose override IS the gesture.")
	assert_eq(ShellGameLaunch.runtime_flags("jox01", "jo"),
		PackedStringArray(["/d", "/exp", "jox01", "/game", "jo"]),
		"A mounted expansion rides along as the retail /exp flag.")
	assert_eq(ShellGameLaunch.runtime_flags("  ", " JODEMO "), PackedStringArray(["/d", "/game", "jodemo"]),
		"Whitespace expansion is dropped; the game code normalizes lowercase.")
	assert_eq(ShellGameLaunch.runtime_flags("", ""), PackedStringArray(["/d", "/game", "jo"]),
		"An empty game code resolves to the JO default, matching NovaLaunchFlags.")


func test_launch_plan_prefers_the_packaged_runtime_beside_the_editor() -> void:
	var exe := "C:/install/opennova-modtools.exe"
	var plan := ShellGameLaunch.launch_plan(exe, "C:/proj", true, "", "jo",
		func(path: String) -> bool: return path == "C:/install/opennova.exe")
	assert_eq(plan.path, "C:/install/opennova.exe",
		"The shipped two-product layout launches the sibling game exe.")
	assert_eq(plan.args, PackedStringArray(["/d", "/game", "jo"]),
		"The packaged runtime takes the retail-style flags directly.")


func test_launch_plan_falls_back_to_the_dev_binary_on_the_game_scene() -> void:
	var plan := ShellGameLaunch.launch_plan("C:/godot/godot.exe", "C:/repo/godot", true, "jox01", "jo",
		Callable(self, "_exists_none"))
	assert_eq(plan.path, "C:/godot/godot.exe", "Running from source re-runs this binary.")
	assert_eq(plan.args[0], "--path", "The dev fallback targets the project.")
	assert_eq(plan.args[1], "C:/repo/godot", "The project dir rides the --path flag.")
	assert_eq(plan.args[2], ShellGameLaunch.RUNTIME_SCENE, "The game scene is the launch target.")
	assert_eq(plan.args[3], "--", "Runtime flags sit behind the user-args separator.")
	assert_eq(plan.args.slice(4), PackedStringArray(["/d", "/exp", "jox01", "/game", "jo"]),
		"The same retail flags follow the separator.")


func test_launch_plan_reports_unavailable_outside_dev_without_a_packaged_exe() -> void:
	var plan := ShellGameLaunch.launch_plan("C:/install/opennova-modtools.exe", "C:/proj", false, "", "jo",
		Callable(self, "_exists_none"))
	assert_null(plan, "No sibling exe and no dev binary means the action is honestly unavailable.")


func _make_launcher(button: Button, dir: String, spawned: Array, statuses: Array,
		spawn_result: int = 1234) -> ShellGameLaunch:
	var launcher := ShellGameLaunch.new()
	launcher.setup(
		button,
		func() -> String: return dir,
		func() -> String: return "",
		func() -> String: return "jo",
		func(path: String, args: PackedStringArray) -> int:
			spawned.append({"path": path, "args": args})
			return spawn_result,
		Callable(self, "_exists_none"),
		func(text: String, _duration: float = 0.0, _kind: StringName = &"info") -> void:
			statuses.append(text)
	)
	return launcher


func test_button_gates_on_the_resource_dir() -> void:
	var no_dir_button := Button.new()
	add_child_autofree(no_dir_button)
	_make_launcher(no_dir_button, "", [], [])
	assert_true(no_dir_button.disabled, "No authoring directory disables the gesture.")
	assert_eq(no_dir_button.tooltip_text, ShellGameLaunch.TOOLTIP_NEEDS_DIR,
		"The tooltip says what to do about it, in artist terms.")

	var ready_button := Button.new()
	add_child_autofree(ready_button)
	var launcher := _make_launcher(ready_button, "C:/assets", [], [])
	# The GUT run is the editor binary, so the dev fallback is available.
	assert_true(launcher.available(), "The dev-binary fallback makes the runtime available under tests.")
	assert_false(ready_button.disabled, "A mounted authoring directory enables the gesture.")
	assert_eq(ready_button.tooltip_text, ShellGameLaunch.TOOLTIP_READY, "The ready tooltip explains the gesture.")


func test_launch_routes_the_plan_through_the_spawn_seam() -> void:
	var button := Button.new()
	add_child_autofree(button)
	var spawned: Array = []
	var statuses: Array = []
	var launcher := _make_launcher(button, "C:/assets", spawned, statuses)

	assert_true(launcher.launch(), "A ready launcher spawns.")
	assert_eq(spawned.size(), 1, "Exactly one process spawn per gesture.")
	var args := spawned[0]["args"] as PackedStringArray
	assert_true(args.has("/d"), "The spawned runtime gets the loose-override flag.")
	assert_true(args.has(ShellGameLaunch.RUNTIME_SCENE), "The dev plan targets the game scene.")
	assert_eq(statuses.size(), 1, "The status line reports the launch.")

	# The button press is the same public path.
	button.pressed.emit()
	assert_eq(spawned.size(), 2, "The top-bar button rides the same launch.")


func test_failed_spawn_reports_and_returns_false() -> void:
	var button := Button.new()
	add_child_autofree(button)
	var spawned: Array = []
	var statuses: Array = []
	var launcher := _make_launcher(button, "C:/assets", spawned, statuses, -1)

	assert_false(launcher.launch(), "A failed spawn reports failure.")
	assert_eq(statuses.size(), 1, "The failure lands on the status line.")
	assert_true(String(statuses[0]).begins_with("Could not launch"), "The message is the honest one.")


func test_launch_without_a_dir_refuses_before_spawning() -> void:
	var button := Button.new()
	add_child_autofree(button)
	var spawned: Array = []
	var launcher := _make_launcher(button, "", spawned, [])
	assert_false(launcher.launch(), "No authoring directory refuses the launch.")
	assert_eq(spawned.size(), 0, "Nothing spawns without a directory.")
