class_name DebugEntityPicker
## The one ray recipe both pick inputs share: build a camera ray (screen
## center for the crosshair hotkey, the event's viewport-local position for a
## tools-open click), run Simulation.debug_pick_entity — the exact segment a
## bullet would test — and stamp the pick's provenance onto the returned card.

## The aim projection distance in world units (the binocular aim range).
const PICK_RANGE_UNITS := 1000.0


## `source` names the input for the card ("crosshair" / "mouse_click");
## `screen_pos` is in the camera viewport's own pixels. Returns the sim's
## stable-shape DebugPickCard (hit=false on any miss), or null when there is
## no sim/camera to ask.
static func pick_with_camera(sim: Simulation, camera: Camera3D, screen_pos: Vector2,
		source: String) -> DebugPickCard:
	if sim == null or not is_instance_valid(sim):
		return null
	if camera == null or not is_instance_valid(camera):
		return null
	var origin := camera.project_ray_origin(screen_pos)
	var direction := camera.project_ray_normal(screen_pos)
	var pick := sim.debug_pick_entity(origin, direction, PICK_RANGE_UNITS)
	pick.source = source
	pick.ray_origin_godot = origin
	pick.ray_dir_godot = direction
	return pick


## The crosshair variant: both camera modes pin the aim reticle to screen
## center, so the center ray IS the aim ray.
static func pick_at_crosshair(sim: Simulation, camera: Camera3D) -> DebugPickCard:
	if camera == null or not is_instance_valid(camera):
		return null
	var viewport := camera.get_viewport()
	if viewport == null:
		return null
	return pick_with_camera(
			sim, camera, viewport.get_visible_rect().size * 0.5, "crosshair")
