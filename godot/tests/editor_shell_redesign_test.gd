extends GutTest

## The shell redesign: the vertical workspace dock bar's Ctrl+digit hotkeys,
## Ctrl+P quick-open, the app-close guard over unsaved work, and the inspector
## empty-state panel. Structural bar/header assertions live in
## terrain_editor_workstation_test.gd; these cover the new behaviors.

const EditorWorkstationScene = preload("res://modtools/editor/editor_workstation.tscn")
const EditorWorkstationScript = preload("res://modtools/editor/editor_workstation.gd")


func _shell() -> Node:
	return add_child_autofree(EditorWorkstationScene.instantiate())


# --- Ctrl+1..9 workspace hotkeys ---------------------------------------------


func test_ctrl_digit_switches_workspace_and_records_back() -> void:
	var shell := _shell()
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.MISSION,
		"Mission is the default workspace.")
	var ev := InputEventKey.new()
	ev.keycode = KEY_4  # 1-based: Mission, Terrain, Object, Avatars, Fonts, Credits
	ev.ctrl_pressed = true
	ev.pressed = true
	shell._unhandled_input(ev)
	assert_eq(shell.get_active_workspace_id(), EditorWorkstationScript.Workspace.AVATARS,
		"Ctrl+4 jumps to the fourth dock entry (Avatars).")
	var back_btn: Button = shell.get_node("%NavBackButton")
	assert_false(back_btn.disabled,
		"A hotkey jump records a Back entry exactly like a click on the dock button.")


func test_dock_buttons_advertise_their_ctrl_hotkeys() -> void:
	var shell := _shell()
	var rail: BoxContainer = shell.get_node("%WorkspaceRail")
	var n := 0
	for child in rail.get_children():
		if child is Button:
			n += 1
			# Only Ctrl+1..9 are wired; a 10th-or-later button carries no digit.
			if n <= 9:
				assert_true((child as Button).tooltip_text.begins_with("Ctrl+%d" % n),
					"dock button %d should advertise its Ctrl+%d hotkey in the tooltip" % [n, n])
			else:
				assert_false((child as Button).tooltip_text.begins_with("Ctrl+"),
					"the 10th dock button has no digit hotkey to advertise")
	assert_gte(n, 9, "every non-popup workspace gets a dock button")


# --- Ctrl+P quick-open --------------------------------------------------------


func test_ctrl_p_opens_quick_open_browser() -> void:
	var shell := _shell()
	var mount: Control = shell.get_node("%ResourceBrowserPaneMount")
	# A prior session may have persisted the pane open (shared user:// state), so
	# establish the hidden precondition rather than assuming it.
	shell._layout.set_browser_pane_visible(false)
	assert_false(mount.visible, "precondition: the browser pane is hidden")
	var ev := InputEventKey.new()
	ev.keycode = KEY_P
	ev.ctrl_pressed = true
	ev.pressed = true
	shell._unhandled_input(ev)
	assert_true(mount.visible, "Ctrl+P reveals the resource browser pane")
	assert_not_null(shell._layout.browser_pane(), "Ctrl+P instantiates the browser pane")
	# Headless has no display, so the deferred search-focus grab is not asserted.
	# Restore the hidden default: save_browser_state() writes user:// unconditionally,
	# so a left-open pane would leak into other suites that assert it defaults hidden.
	shell._layout.set_browser_pane_visible(false)


# --- Unsaved-changes close guard ---------------------------------------------


func _configure_failed_running_game(shell: Node) -> ShellGameSession:
	var session: ShellGameSession = shell.get_game_run_session()
	session.setup(
			func() -> String: return "C:/assets",
			func() -> String: return "",
			func() -> String: return "jo",
			Callable(),
			Callable(),
			func(_path: String, _args: PackedStringArray) -> int: return 4017,
			func(_path: String) -> bool: return false,
			Callable(),
			func(pid: int) -> bool: return pid == 4017,
			func(_pid: int) -> int: return FAILED)
	assert_true(session.start_mode("game"))
	return session


func _configure_dirty_environment(shell: Node) -> EnvironmentEditor:
	var environment_editor: EnvironmentEditor = add_child_autofree(
			EnvironmentEditor.new())
	environment_editor.create_default_environment(false)
	environment_editor.env_file.set_env_name("Dirty")
	shell.get_workspace_adapter(EditorWorkstationScript.Workspace.ENVIRONMENT) \
			.set_environment_editor(environment_editor)
	return environment_editor


func test_clean_close_aborts_when_managed_game_cannot_stop() -> void:
	var shell := _shell()
	var session := _configure_failed_running_game(shell)

	shell.request_close()
	await get_tree().process_frame

	assert_eq(session.get_state()["state"], "running")
	assert_true(shell.is_inside_tree(),
			"a failed child shutdown keeps the editor alive")
	assert_string_contains(
			(shell.get_node("%StatusToolLabel") as Label).text,
			"Could not stop")


func test_close_guard_prompts_when_a_workspace_is_dirty() -> void:
	var shell := _shell()
	_configure_dirty_environment(shell)
	shell.request_close()
	await get_tree().process_frame
	var dialog := shell.get_node_or_null("CloseGuardDialog") as ConfirmationDialog
	assert_not_null(dialog, "a dirty workspace builds the close-guard dialog")
	if dialog == null:
		return
	assert_true(dialog.visible, "the guard is shown instead of quitting")
	assert_string_contains(dialog.dialog_text, "Environment")
	# Cancel dismisses without quitting; never emit confirmed (it quits the runner).
	dialog.get_cancel_button().pressed.emit()
	await get_tree().process_frame
	assert_false(dialog.visible, "Keep editing dismisses the guard and leaves the editor open")


func test_dirty_close_confirmation_also_aborts_on_game_shutdown_failure() -> void:
	var shell := _shell()
	var session := _configure_failed_running_game(shell)
	_configure_dirty_environment(shell)
	shell.request_close()
	await get_tree().process_frame
	var dialog := shell.get_node_or_null("CloseGuardDialog") as ConfirmationDialog
	assert_not_null(dialog)
	if dialog == null:
		return

	dialog.confirmed.emit()
	await get_tree().process_frame

	assert_eq(session.get_state()["state"], "running")
	assert_true(shell.is_inside_tree())
	assert_string_contains(
			(shell.get_node("%StatusToolLabel") as Label).text,
			"Could not stop")


# --- Inspector empty state ----------------------------------------------------


# The shell renders this panel when a workspace's inspector has no content. It
# offers the active workspace's own New/Open plus quick-open; Fonts declares both
# capabilities without an injected editor, so it is a clean fixture.
func test_empty_state_panel_offers_new_open_and_browse() -> void:
	var shell := _shell()
	shell.set_active_workspace(EditorWorkstationScript.Workspace.FONTS)
	await get_tree().process_frame
	var mount: Control = add_child_autofree(PanelContainer.new())
	shell._build_empty_state_panel(mount, "Nothing open here.", &"fonts")
	assert_not_null(mount.find_child("EmptyStatePanel", true, false), "builds the empty-state panel")
	assert_not_null(mount.find_child("EmptyStateMessage", true, false), "carries a guidance message")
	assert_not_null(mount.find_child("EmptyStateNewActionButton", true, false),
		"offers New for a New-capable active workspace")
	assert_not_null(mount.find_child("EmptyStateOpenActionButton", true, false),
		"offers Open for an Open-capable active workspace")
	assert_not_null(mount.find_child("EmptyStateBrowseButton", true, false),
		"offers a quick-open browse button")


# --- B10: toast severity ---------------------------------------------------

func test_status_severity_maps_variation_and_default_duration() -> void:
	var shell := _shell()
	var bar = shell._status

	bar.show_status_message("saved", 0.0, &"success")
	shell._process(0.0)
	var tool_label := shell.get_node("%StatusToolLabel") as Label
	assert_eq(tool_label.theme_type_variation, &"Success", "success renders green")

	bar.show_status_message("broke", 0.0, &"error")
	shell._process(0.0)
	assert_eq(tool_label.theme_type_variation, &"Error", "error renders red")

	bar.show_status_message("hm", 0.0, &"made-up")
	shell._process(0.0)
	assert_eq(tool_label.theme_type_variation, &"Info", "unknown severity reads as info")

	# Per-severity default durations apply when duration <= 0.
	var now := Time.get_ticks_msec() / 1000.0
	bar.show_status_message("e", 0.0, &"error")
	assert_almost_eq(bar._message_until - now, 8.0, 0.25, "error default 8s")
	bar.show_status_message("s", 0.0, &"success")
	now = Time.get_ticks_msec() / 1000.0
	assert_almost_eq(bar._message_until - now, 3.5, 0.25, "success default 3.5s")
	bar.show_status_message("i", 2.0, &"error")
	now = Time.get_ticks_msec() / 1000.0
	assert_almost_eq(bar._message_until - now, 2.0, 0.25, "explicit duration wins")


func test_theme_carries_all_four_severity_variations() -> void:
	var shell := _shell()
	var theme: Theme = shell.theme
	assert_not_null(theme)
	for variation in [&"Info", &"Success", &"Warn", &"Error"]:
		assert_true(theme.has_color(&"font_color", variation),
			"%s label variation exists in the editor theme" % variation)


# --- Standalone run: shell-wide unsaved summary ------------------------------

func test_run_toolbar_exposes_the_f5_f6_f8_session_gestures() -> void:
	var shell := _shell()
	var game_button: Button = shell.get_node("%PlayInGameButton")
	var mission_button: Button = shell.get_node("%PlayCurrentMissionButton")
	var stop_button: Button = shell.get_node("%StopGameButton")
	assert_not_null(game_button)
	assert_not_null(mission_button)
	assert_not_null(stop_button)
	assert_string_contains(game_button.tooltip_text, "F5")
	assert_string_contains(mission_button.tooltip_text, "F6")
	assert_string_contains(stop_button.tooltip_text, "managed game")


func test_dirty_popup_workspace_joins_run_warning_without_changing_tooltip() -> void:
	var shell := _shell()
	var environment_editor: EnvironmentEditor = add_child_autofree(EnvironmentEditor.new())
	environment_editor.create_default_environment(false)
	environment_editor.env_file.set_env_name("Dirty")
	shell.get_workspace_adapter(EditorWorkstationScript.Workspace.ENVIRONMENT) \
		.set_environment_editor(environment_editor)

	# A real (non-user-data) resource dir arms the gesture; the GUT run is the
	# editor binary, so the dev-runtime fallback is available.
	var root: String = OS.get_cache_dir().path_join(
		"opennova_shell_dirty_summary_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	shell.set_resource_root_dir(root, false, false)

	var play_button: Button = shell.get_node("%PlayInGameButton")
	assert_false(play_button.disabled, "a mounted directory arms the run button")
	var ready_tooltip := play_button.tooltip_text
	assert_string_contains(ready_tooltip, "saved loose assets")
	assert_true(shell.get_unsaved_workspace_labels().has("Environment"),
		"popup workspaces participate in the same shell-wide dirty summary")

	var sun_button: Button = shell.get_node("%EnvironmentToggleButton")
	sun_button.toggled.emit(true)
	await get_tree().process_frame
	shell.sync_from_editor_state()
	assert_eq(play_button.tooltip_text, ready_tooltip,
		"opening a dirty workspace cannot add staging/export advice to F5")

	environment_editor.set_current_path(root.path_join("full_08.env"))
	shell.sync_from_editor_state()
	assert_eq(play_button.tooltip_text, ready_tooltip,
		"document paths do not change the invariant saved-assets tooltip")

	sun_button.toggled.emit(false)
	await get_tree().process_frame
	shell.sync_from_editor_state()
	assert_eq(play_button.tooltip_text, ready_tooltip)

	shell.set_resource_root_dir("", false, false)
	DirAccess.remove_absolute(root)
