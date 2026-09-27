class_name DebugEntityPicker
## The one ray recipe both pick inputs share: build a camera ray (screen
## center for the crosshair hotkey, the event's viewport-local position for a
## tools-open click), run Simulation.debug_pick_entity — the exact segment a
## bullet would test — and stamp the pick's provenance onto the returned card.
##
## The ray is built through the view the surface actually shows: while the
## local player presenter's aspect mode draws through its stretched target,
## the target camera at the surface point scaled into the target's pixels
## (the full-surface blit's stretch, target size / surface size); the
## gameplay camera then carries only a CULLING SUPERSET of the frustum
## (LocalPlayerPresenter.view_projection) and its ray would miss on one axis.
## A bare camera (no presenter, or none live) picks in its own pixels.

## The aim projection distance in world units (the binocular aim range).
const PICK_RANGE_UNITS := 1000.0


## The camera the surface's pixels come from: the presenter's target camera
## while its aspect-mode target is live, else `camera` itself.
static func view_camera(camera: Camera3D, presenter: LocalPlayerPresenter) -> Camera3D:
	if presenter == null or not is_instance_valid(presenter):
		return camera
	var through: Camera3D = presenter.projection_camera()
	if through == null or presenter.projection_viewport() == null:
		return camera
	return through


## `screen_pos` (the camera viewport's own pixels) in the view camera's
## viewport pixels: the blit stretches the target over the whole surface, so
## a surface point scales by target size / surface size; a bare camera's
## point is its own.
static func view_point(camera: Camera3D, screen_pos: Vector2,
		presenter: LocalPlayerPresenter) -> Vector2:
	if view_camera(camera, presenter) == camera:
		return screen_pos
	var surface := camera.get_viewport()
	var target: SubViewport = presenter.projection_viewport()
	if surface == null or target == null:
		return screen_pos
	var surface_size := surface.get_visible_rect().size
	if surface_size.x <= 0.0 or surface_size.y <= 0.0:
		return screen_pos
	# The visible rect, not target.size: under the XR-served NVG projection the
	# target is the 512 square while its camera projects in the override frame.
	var target_size := target.get_visible_rect().size
	return Vector2(screen_pos.x * target_size.x / surface_size.x,
			screen_pos.y * target_size.y / surface_size.y)


## `source` names the input for the card ("crosshair" / "mouse_click");
## `screen_pos` is in the camera viewport's own pixels; `presenter` is the
## presenter the surface draws through (the world's local view presenter), or
## null for a bare camera. Returns the sim's stable-shape DebugPickCard
## (hit=false on any miss), or null when there is no sim/camera to ask.
static func pick_with_camera(sim: Simulation, camera: Camera3D, screen_pos: Vector2,
		source: String, presenter: LocalPlayerPresenter) -> DebugPickCard:
	if sim == null or not is_instance_valid(sim):
		return null
	if camera == null or not is_instance_valid(camera):
		return null
	var through := view_camera(camera, presenter)
	var point := view_point(camera, screen_pos, presenter)
	var origin := through.project_ray_origin(point)
	var direction := through.project_ray_normal(point)
	var pick := sim.debug_pick_entity(origin, direction, PICK_RANGE_UNITS)
	pick.source = source
	pick.ray_origin_godot = origin
	pick.ray_dir_godot = direction
	return pick


## The crosshair variant: both camera modes pin the aim reticle to screen
## center, so the center ray IS the aim ray (the surface center maps to the
## target center, so the stretch changes nothing here).
static func pick_at_crosshair(sim: Simulation, camera: Camera3D,
		presenter: LocalPlayerPresenter) -> DebugPickCard:
	if camera == null or not is_instance_valid(camera):
		return null
	var viewport := camera.get_viewport()
	if viewport == null:
		return null
	return pick_with_camera(
			sim, camera, viewport.get_visible_rect().size * 0.5, "crosshair", presenter)
