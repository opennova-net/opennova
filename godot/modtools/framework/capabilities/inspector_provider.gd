class_name InspectorProvider
extends EditorCapability

## Capability that yields the control to be mounted in the workspace's right-side inspector panel.

var inspector_control: Control

func _init(p_inspector: Control) -> void:
	inspector_control = p_inspector
