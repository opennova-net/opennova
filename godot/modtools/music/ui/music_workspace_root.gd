class_name MusicWorkspaceRoot
extends Control

# Holds three mode sub-scenes, only one visible at a time. The workspace
# adapter sets the active mode via set_active_workflow.

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
