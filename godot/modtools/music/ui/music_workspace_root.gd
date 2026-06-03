class_name MusicWorkspaceRoot
extends Control

# The unified Music screen. The three legacy panels (Bank / Script / Live) are
# instanced WHOLE inside live_mode.tscn -- Tracks (Bank) and the Advanced drawer
# (Script) dock around the live-lit section map -- so each panel's standalone
# tests keep passing untouched. This root just wraps that screen and fans the
# shared document out to every panel.
#
# Because Bank and Script now live nested inside the embedded Live screen rather
# than as direct children, panel lookups go through find_child (recursive,
# owner-agnostic) instead of a direct-child get_node.

signal workflow_requested(workflow_id: int)

var _document: RefCounted   # MusicEditorDocument (loose-typed; defined in Phase B)


func bind_document(document: RefCounted) -> void:
	_document = document
	for n in ["Bank", "Script", "Live"]:
		var node := get_panel(n)
		if node != null and node.has_method("bind_document"):
			node.bind_document(document)
	_wire_script_mode()


func _ready() -> void:
	_wire_script_mode()


# Recursive, owner-agnostic lookup. Bank and Script are nested inside the
# embedded Live screen, so a plain get_node("Bank") (direct child only) misses
# them; find_child walks into the instanced sub-scene.
func get_panel(panel_name: String) -> Node:
	return find_child(panel_name, true, false)


# Single-screen: the workflows are coexisting docks, not mutually-exclusive
# panels, so there is nothing to show/hide. Kept as a no-op for the workspace
# adapter's set_active_workflow call.
func set_active_workflow(_workflow_id: int) -> void:
	pass


func _wire_script_mode() -> void:
	var script_node := get_panel("Script")
	if script_node == null or not script_node.has_signal("compile_and_run_requested"):
		return
	if not script_node.compile_and_run_requested.is_connected(_on_compile_and_run_requested):
		script_node.compile_and_run_requested.connect(_on_compile_and_run_requested)


func _on_compile_and_run_requested() -> void:
	# One screen, no tab to switch to: just start the live director. The Script
	# panel already compiled clean before emitting this.
	var live_node := get_panel("Live")
	if live_node != null and live_node.has_method("start_from_script_mode"):
		live_node.start_from_script_mode()
