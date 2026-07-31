class_name DebugEntityPicker
## The one ray recipe both pick inputs share: build a camera ray (screen
## center for the crosshair hotkey, the mouse position for an overlay-open
## click), run NovaSimulation.debug_pick_entity — the exact segment a bullet
## would test — and stamp the pick's provenance onto the returned card so the
## snapshot can replay it later.

## The aim projection distance, matching the binocular rangefinder
## (LocalPlayerPresenter.AIM_PROJECT_RANGE).
const PICK_RANGE_UNITS := 1000.0


## `source` names the input for the card ("crosshair" / "mouse_click").
## Returns the sim's stable-shape pick card (hit=false on any miss), or {}
## when there is no sim/camera to ask.
static func pick_with_camera(sim: Object, camera: Camera3D, screen_pos: Vector2,
		source: String) -> Dictionary:
	if sim == null or not is_instance_valid(sim) \
			or not sim.has_method("debug_pick_entity"):
		return {}
	if camera == null or not is_instance_valid(camera):
		return {}
	var origin := camera.project_ray_origin(screen_pos)
	var direction := camera.project_ray_normal(screen_pos)
	var pick: Dictionary = sim.debug_pick_entity(origin, direction, PICK_RANGE_UNITS)
	pick["source"] = source
	pick["ray_origin_godot"] = origin
	pick["ray_dir_godot"] = direction
	return pick


## The crosshair variant: both camera modes pin the aim reticle to screen
## center, so the center ray IS the aim ray.
static func pick_at_crosshair(sim: Object, camera: Camera3D) -> Dictionary:
	if camera == null or not is_instance_valid(camera):
		return {}
	var viewport := camera.get_viewport()
	if viewport == null:
		return {}
	return pick_with_camera(
			sim, camera, viewport.get_visible_rect().size * 0.5, "crosshair")
