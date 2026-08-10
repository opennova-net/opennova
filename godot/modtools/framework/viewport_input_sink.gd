class_name ViewportInputSink
extends RefCounted

## Typed contract for a non-terrain consumer of workspace-viewport input (the
## Mission controller picks/drags entities through it while the terrain brush
## stays dormant). TerrainViewportInputRouter forwards every unhandled event
## here; the default swallows nothing and does nothing.


func handle_viewport_input(_event: InputEvent) -> void:
	pass
