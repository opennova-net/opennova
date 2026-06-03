class_name MusicWorkspaceRoot
extends Control

# Holds three mode sub-scenes, only one visible at a time. The workspace
# adapter sets the active mode via set_active_workflow.

signal workflow_requested(workflow_id: int)

const WORKFLOW_LIVE := 2

var _document: RefCounted   # MusicEditorDocument (loose-typed; defined in Phase B)
var _active_workflow: int = 0  # MusicEditorWorkspace.Workflow.BANK


func bind_document(document: RefCounted) -> void:
	_document = document
	# Forward to each mode panel that knows how to bind. Phase C wires Bank;
	# Script and Live land in later phases.
	var names := ["Bank", "Script", "Live"]
	for n in names:
		var node := get_node_or_null(n)
		if node != null and node.has_method("bind_document"):
			node.bind_document(document)
	_wire_script_mode()


func _ready() -> void:
	_wire_script_mode()


func set_active_workflow(workflow_id: int) -> void:
	_active_workflow = workflow_id
	_refresh_visibility()


func _refresh_visibility() -> void:
	# Each child mode panel is named "Bank", "Script", "Live".
	var names := ["Bank", "Script", "Live"]
	for i in range(names.size()):
		var node := get_node_or_null(names[i])
		if node and node is Control:
			(node as Control).visible = (i == _active_workflow)


func _wire_script_mode() -> void:
	var script_node := get_node_or_null("Script")
	if script_node == null or not script_node.has_signal("compile_and_run_requested"):
		return
	if not script_node.compile_and_run_requested.is_connected(_on_compile_and_run_requested):
		script_node.compile_and_run_requested.connect(_on_compile_and_run_requested)


func _on_compile_and_run_requested() -> void:
	workflow_requested.emit(WORKFLOW_LIVE)
	var live_node := get_node_or_null("Live")
	if live_node != null and live_node.has_method("start_from_script_mode"):
		live_node.start_from_script_mode()
