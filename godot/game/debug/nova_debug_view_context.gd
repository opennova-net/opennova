class_name DebugViewContext
extends RefCounted

## Typed shell-to-overlay record for the exact gameplay camera that drives
## foliage culling. camera_mode_known distinguishes first person from a shell
## that cannot report the resolved camera mode.
var camera: Camera3D
var camera_mode_known := false
var third_person := false
