class_name MusicWorkspaceRoot
extends Control

# The unified Music screen. The legacy Bank panel is instanced WHOLE inside
# live_mode.tscn (Tracks docks around the live-lit section map), so its standalone
# tests keep passing untouched. This root just wraps that screen and fans the shared
# document out to every panel. The blueprint graph is the sole authoring surface;
# the old right-dock inspector and the raw-script drawer are gone.
#
# Bank lives nested inside the embedded Live screen, so panel lookups go through
# find_child (recursive, owner-agnostic) instead of a direct-child get_node.

signal workflow_requested(workflow_id: int)

## Relayed panel failures (Bank WAV import/replace); the workspace turns these
## into shell toasts. Standalone scene owners (tests) can just ignore it.
signal error_reported(message: String)

var _document: RefCounted   # MusicEditorDocument


func _ready() -> void:
	# Bank lives nested in the embedded Live screen; relay its errors upward so
	# the workspace (which alone holds the shell seam) can toast them.
	var bank := get_panel("Bank") as MusicBankMode
	if bank != null:
		bank.error_reported.connect(_on_bank_error)


func _on_bank_error(message: String) -> void:
	error_reported.emit(message)


func bind_document(document: RefCounted) -> void:
	_document = document
	for n in ["Bank", "Live"]:
		var node := get_panel(n)
		if node != null and node.has_method("bind_document"):
			node.bind_document(document)


# Recursive, owner-agnostic lookup. Bank is nested inside the embedded Live screen,
# so a plain get_node("Bank") (direct child only) misses it; find_child walks into
# the instanced sub-scene.
func get_panel(panel_name: String) -> Node:
	return find_child(panel_name, true, false)


# Single-screen: the workflows are coexisting docks, not mutually-exclusive panels,
# so there is nothing to show/hide. Kept as a no-op for the workspace adapter's
# set_active_workflow call.
func set_active_workflow(_workflow_id: int) -> void:
	pass
