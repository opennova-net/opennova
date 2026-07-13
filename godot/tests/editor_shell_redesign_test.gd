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
	var host: Control = shell.get_node("%ResourceBrowserPaneHost")
	# A prior session may have persisted the pane open (shared user:// state), so
	# establish the hidden precondition rather than assuming it.
	shell._layout.set_browser_pane_visible(false)
	assert_false(host.visible, "precondition: the browser pane is hidden")
	var ev := InputEventKey.new()
	ev.keycode = KEY_P
	ev.ctrl_pressed = true
	ev.pressed = true
	shell._unhandled_input(ev)
	assert_true(host.visible, "Ctrl+P reveals the resource browser pane")
	assert_not_null(shell._layout.browser_pane(), "Ctrl+P instantiates the browser pane")
	# Headless has no display, so the deferred search-focus grab is not asserted.
	# Restore the hidden default: save_browser_state() writes user:// unconditionally,
	# so a left-open pane would leak into other suites that assert it defaults hidden.
	shell._layout.set_browser_pane_visible(false)


# --- Unsaved-changes close guard ---------------------------------------------


class DirtyWorkspaceStub:
	extends EditorWorkspace

	func has_unsaved_changes() -> bool:
		return true

	func get_workspace_label() -> String:
		return "Scratch"


func test_close_guard_prompts_when_a_workspace_is_dirty() -> void:
	var shell := _shell()
	# Inject a dirty workspace so the guard has something to report. Without it,
	# _handle_close_request would quit the test runner (nothing unsaved).
	shell._workspaces[4242] = DirtyWorkspaceStub.new()
	shell._handle_close_request()
	await get_tree().process_frame
	var dialog: ConfirmationDialog = shell._close_guard_dialog
	assert_not_null(dialog, "a dirty workspace builds the close-guard dialog")
	if dialog == null:
		shell._workspaces.erase(4242)
		return
	assert_true(dialog.visible, "the guard is shown instead of quitting")
	assert_string_contains(dialog.dialog_text, "Scratch")
	# Cancel dismisses without quitting; never emit confirmed (it quits the runner).
	dialog.get_cancel_button().pressed.emit()
	await get_tree().process_frame
	assert_false(dialog.visible, "Keep editing dismisses the guard and leaves the editor open")
	shell._workspaces.erase(4242)


# --- Inspector empty state ----------------------------------------------------


# The shell renders this panel when a workspace's inspector has no content. It
# offers the active workspace's own New/Open plus quick-open; Fonts declares both
# capabilities without an injected editor, so it is a clean fixture.
func test_empty_state_panel_offers_new_open_and_browse() -> void:
	var shell := _shell()
	shell.set_active_workspace(EditorWorkstationScript.Workspace.FONTS)
	await get_tree().process_frame
	var host: Control = add_child_autofree(PanelContainer.new())
	shell._build_empty_state_panel(host, "Nothing open here.", &"fonts")
	assert_not_null(host.find_child("EmptyStatePanel", true, false), "builds the empty-state panel")
	assert_not_null(host.find_child("EmptyStateMessage", true, false), "carries a guidance message")
	assert_not_null(host.find_child("EmptyStateNewActionButton", true, false),
		"offers New for a New-capable active workspace")
	assert_not_null(host.find_child("EmptyStateOpenActionButton", true, false),
		"offers Open for an Open-capable active workspace")
	assert_not_null(host.find_child("EmptyStateBrowseButton", true, false),
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


# --- See in game: popup-workspace note routing (ENV-1) ------------------------
# Environment is a popup over the active view (never the active workspace), so
# while its panel is open the launcher's See-in-game note comes from it; closed,
# the gesture returns to the active workspace's copy. Public seams only.

func test_open_environment_panel_supplies_the_see_in_game_note() -> void:
	var shell := _shell()
	var environment_editor: EnvironmentEditor = add_child_autofree(EnvironmentEditor.new())
	environment_editor.create_default_environment(false)
	shell.get_workspace_adapter(EditorWorkstationScript.Workspace.ENVIRONMENT) \
		.set_environment_editor(environment_editor)

	# A real (non-user-data) resource dir arms the gesture; the GUT run is the
	# editor binary, so the dev-runtime fallback is available.
	var root: String = OS.get_cache_dir().path_join(
		"opennova_shell_launch_note_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	shell.set_resource_root_dir(root, false, false)

	var play_button: Button = shell.get_node("%PlayInGameButton")
	assert_false(play_button.disabled, "a mounted directory arms the See-in-game button")
	assert_false(play_button.tooltip_text.contains("Save your environment"),
		"with the panel closed, the active workspace (Mission, no note) keeps the generic copy")

	var sun_button: Button = shell.get_node("%EnvironmentToggleButton")
	sun_button.toggled.emit(true)
	await get_tree().process_frame
	shell.sync_from_editor_state()
	assert_string_contains(play_button.tooltip_text, "Save your environment",
		"an open environment panel's staging note rides the launch tooltip")

	environment_editor.set_current_path(root.path_join("full_08.env"))
	shell.sync_from_editor_state()
	assert_string_contains(play_button.tooltip_text, "full_08.env",
		"a clean save into the game folder flips the note to the staged pointer")

	sun_button.toggled.emit(false)
	await get_tree().process_frame
	shell.sync_from_editor_state()
	assert_false(play_button.tooltip_text.contains("full_08.env"),
		"closing the panel returns the gesture to the active workspace's copy")

	shell.set_resource_root_dir("", false, false)
	DirAccess.remove_absolute(root)
