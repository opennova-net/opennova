class_name NovaDebugViewContext
extends RefCounted

## Typed host-to-overlay record for the exact gameplay camera that drives
## foliage culling. camera_mode_known distinguishes first person from a host
## that cannot report F4 mode.
var camera: Camera3D
var camera_mode_known := false
var third_person := false
