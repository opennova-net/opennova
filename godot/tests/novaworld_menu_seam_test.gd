extends GutTest

# The NovaWorld menu seam: a control named by one of novaworld_control_names,
# when pressed, makes the menu host emit novaworld_requested. This is the same
# Command-by-name convention the start/exit/return controls use. The live
# server flow (browse, host, a second client seeing the row) is the manual
# two-client smoke recorded in plan/status.md; this pins the wiring.

const MenuShell := preload("res://game/nova_menu_shell.gd")


func test_novaworld_control_names_default() -> void:
	var host = MenuShell.new()
	assert_true(host.novaworld_control_names.has("NW_MULTI_PLAYER"),
		"the shipped JO main-menu NovaWorld button is covered")
	host.free()


func test_pressing_a_novaworld_control_emits_request() -> void:
	var host = MenuShell.new()
	add_child_autofree(host)
	watch_signals(host)

	# Simulate the host wiring a found control to its handler, the way
	# _connect_named does, then the player pressing it.
	var button := Button.new()
	button.name = "NW_MULTI_PLAYER"
	add_child_autofree(button)
	button.pressed.connect(host._on_novaworld_control)

	button.emit_signal("pressed")
	assert_signal_emitted(host, "novaworld_requested",
		"pressing the NovaWorld control asks the host to open NovaWorld")


func test_panel_class_is_available() -> void:
	# The panel the host opens exists and instantiates headless without a
	# server (it shows an artist-facing status, never crashes).
	var panel := NovaWorldPanel.new()
	add_child_autofree(panel)
	await wait_frames(2)
	assert_not_null(panel, "NovaWorldPanel instantiates")
