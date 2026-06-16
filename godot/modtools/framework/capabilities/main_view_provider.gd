class_name MainViewProvider
extends EditorCapability

## Capability that yields a 2D Control to be mounted in the workspace's main viewport area.

var main_view: Control

func _init(p_main_view: Control) -> void:
	main_view = p_main_view
