extends GutTest

const GameSession := preload("res://modtools/editor/shell/shell_game_session.gd")

# Managed standalone run shell
# action launches the game runtime over the shared authoring directory with
# the engine's own `/d` loose-file override ([orig: `/d` loose-override
# @ 0x4a7310], NovaLaunchFlags). Seams are injected Callables, so the whole
# composition is asserted without touching the OS. Public API only (ADR 0018).


func _make_launcher(button: Button, dir: String, spawned: Array, statuses: Array,
		spawn_result: int = 1234,
		unsaved_workspaces: Callable = Callable(),
		mission_button: Button = null,
		stop_button: Button = null,
		current_mission: Callable = Callable(),
		file_exists: Callable = Callable()) -> ShellGameLaunch:
	var launcher := ShellGameLaunch.new()
	launcher.setup(
		button,
		func() -> String: return dir,
		func() -> String: return "",
		func() -> String: return "jo",
		func(path: String, args: PackedStringArray) -> int:
			spawned.append({"path": path, "args": args})
			return spawn_result,
		file_exists if file_exists.is_valid() \
				else func(_path: String) -> bool: return false,
		unsaved_workspaces,
		func(text: String, _duration: float = 0.0, kind: StringName = &"info") -> void:
			statuses.append({"text": text, "kind": kind}),
		current_mission,
		func(_pid: int) -> bool: return false,
		func(_pid: int) -> int: return OK,
		Callable(),
		mission_button,
		stop_button
	)
	return launcher


func test_button_gates_on_the_resource_dir() -> void:
	var no_dir_button := Button.new()
	var no_dir_mission_button := Button.new()
	add_child_autofree(no_dir_button)
	add_child_autofree(no_dir_mission_button)
	_make_launcher(no_dir_button, "", [], [], 1234, Callable(),
			no_dir_mission_button)
	assert_true(no_dir_button.disabled, "No authoring directory disables the gesture.")
	assert_eq(no_dir_button.tooltip_text, ShellGameLaunch.TOOLTIP_NEEDS_DIR,
		"The tooltip says what to do about it, in artist terms.")
	assert_string_contains(no_dir_button.tooltip_text, "F5")
	assert_eq(no_dir_mission_button.tooltip_text,
			ShellGameLaunch.TOOLTIP_MISSION_NEEDS_DIR)
	assert_string_contains(no_dir_mission_button.tooltip_text, "F6")

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
	assert_true(args.has(GameSession.RUNTIME_SCENE), "The dev plan targets the game scene.")
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
	assert_true(String(statuses[0]["text"]).begins_with("Could not launch"), "The message is the honest one.")


func test_launch_without_a_dir_refuses_before_spawning() -> void:
	var button := Button.new()
	add_child_autofree(button)
	var spawned: Array = []
	var launcher := _make_launcher(button, "", spawned, [])
	assert_false(launcher.launch(), "No authoring directory refuses the launch.")
	assert_eq(spawned.size(), 0, "Nothing spawns without a directory.")


func test_unsaved_workspaces_warn_at_launch_without_changing_the_tooltip() -> void:
	var button := Button.new()
	add_child_autofree(button)
	var spawned: Array = []
	var statuses: Array = []
	var launcher := _make_launcher(button, "C:/assets", spawned, statuses, 1234,
		func() -> PackedStringArray:
			return PackedStringArray(["Terrain", "Environment"]))

	assert_eq(button.tooltip_text, ShellGameLaunch.TOOLTIP_READY,
		"The button describes the invariant saved-assets run path.")
	assert_true(launcher.launch(), "Dirty work never blocks a saved-assets run.")
	assert_eq(spawned.size(), 1)
	assert_eq(statuses[0]["kind"], &"warn")
	assert_string_contains(String(statuses[0]["text"]), "Terrain, Environment")


func test_clean_launch_uses_the_generic_saved_assets_copy() -> void:
	var button := Button.new()
	add_child_autofree(button)
	var statuses: Array = []
	var launcher := _make_launcher(button, "C:/assets", [], statuses)

	assert_eq(button.tooltip_text, ShellGameLaunch.TOOLTIP_READY,
		"The ready tooltip has no workspace staging/export advice.")
	assert_true(launcher.launch(), "The generic gesture still launches.")
	assert_string_contains(String(statuses[0]["text"]), "saved loose assets",
		"The clean launch status reports the saved-assets run path.")


func test_optional_f6_and_f8_buttons_share_the_managed_session() -> void:
	var game_button := add_child_autofree(Button.new()) as Button
	var mission_button := add_child_autofree(Button.new()) as Button
	var stop_button := add_child_autofree(Button.new()) as Button
	var spawned: Array = []
	var launcher := _make_launcher(
		game_button,
		"C:/assets",
		spawned,
		[],
		1234,
		Callable(),
		mission_button,
		stop_button,
		func() -> Dictionary: return {"path": "C:/assets/current.bms"},
		func(path: String) -> bool: return path.ends_with("current.bms"))

	assert_false(game_button.disabled)
	assert_false(mission_button.disabled,
			"an existing saved top-level BMS arms the visible F6 gesture")
	assert_true(stop_button.disabled)

	game_button.pressed.emit()
	assert_eq(spawned.size(), 1)
	assert_false(stop_button.disabled,
			"the visible F8 gesture arms while the managed child is owned")

	mission_button.pressed.emit()
	assert_eq(spawned.size(), 2,
			"F6 replaces the same managed child through the session")
	var args := spawned[1]["args"] as PackedStringArray
	assert_true(args.has("--loose-mission"))
	assert_true(args.has("current.bms"))

	stop_button.pressed.emit()
	assert_true(stop_button.disabled)
	assert_false(launcher.get_session().is_running())
