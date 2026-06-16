class_name ViewportProvider
extends EditorCapability

## Capability that provides a custom 3D Viewport/Camera setup for the workspace.

var camera: Camera3D
var environment: Environment
var world_3d: World3D

func _init(p_camera: Camera3D = null, p_world_3d: World3D = null, p_environment: Environment = null) -> void:
	camera = p_camera
	world_3d = p_world_3d
	environment = p_environment
