extends GutTest

# PickClickCatcher: the overlay-open mouse picker. A left-press ray-picks
# through the live typed sim into the injected list; everything else passes
# through untouched. The catcher reads the sim through the narrow WorldView
# (rule 11): a fake view lends it a REAL (worldless) Simulation, whose picks
# are honest misses — provenance stamping is pinned on DebugEntityPicker.


## The narrow world view, lending the catcher a real sim.
class SimView:
	extends WorldView
	var sim_value: Simulation = null

	func sim() -> Simulation:
		return sim_value


func _make_catcher(list: DebugPickList) -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 240)
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.current = true
	# The catcher needs only its view plus a viewport.
	var view := SimView.new()
	view.sim_value = Simulation.new()
	autofree(view.sim_value)
	var catcher := PickClickCatcher.new()
	viewport.add_child(catcher)
	catcher.setup(view, list)
	return {"catcher": catcher, "view": view, "viewport": viewport}


func test_left_press_ray_picks_through_the_live_sim() -> void:
	var list := DebugPickList.new()
	var made := _make_catcher(list)
	var catcher: PickClickCatcher = made["catcher"]
	var viewport: SubViewport = made["viewport"]

	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	click.position = Vector2(160, 120)
	catcher.handle_click(click)

	assert_true(viewport.is_input_handled(),
			"the click ray-picked through the sim and consumed the event")
	assert_eq(catcher.last_pick_position, Vector2(160, 120),
			"the ray starts from the event's own viewport-local position")
	assert_eq(list.get_picks().size(), 0,
			"a worldless sim answers an honest miss, which the list rejects")


func test_double_click_repeat_is_ignored() -> void:
	var list := DebugPickList.new()
	var made := _make_catcher(list)
	var catcher: PickClickCatcher = made["catcher"]
	var viewport: SubViewport = made["viewport"]

	var repeat := InputEventMouseButton.new()
	repeat.button_index = MOUSE_BUTTON_LEFT
	repeat.pressed = true
	repeat.double_click = true
	repeat.position = Vector2(10, 10)
	catcher.handle_click(repeat)

	assert_false(viewport.is_input_handled(), "the second press of a double-click never re-picks")
	assert_eq(catcher.last_pick_position, Vector2.INF, "no ray ran")


func test_other_input_is_ignored() -> void:
	var list := DebugPickList.new()
	var made := _make_catcher(list)
	var catcher: PickClickCatcher = made["catcher"]
	var viewport: SubViewport = made["viewport"]

	var release := InputEventMouseButton.new()
	release.button_index = MOUSE_BUTTON_LEFT
	release.pressed = false
	catcher.handle_click(release)
	var right := InputEventMouseButton.new()
	right.button_index = MOUSE_BUTTON_RIGHT
	right.pressed = true
	catcher.handle_click(right)
	var motion := InputEventMouseMotion.new()
	catcher.handle_click(motion)

	assert_false(viewport.is_input_handled(), "no ray ever ran")
	assert_eq(list.get_picks().size(), 0)


func test_detached_pick_list_never_consumes_the_click() -> void:
	var made := _make_catcher(null)
	var catcher: PickClickCatcher = made["catcher"]
	var viewport: SubViewport = made["viewport"]
	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	catcher.handle_click(click)
	assert_false(viewport.is_input_handled(),
			"no list means no pick and an unconsumed event")


func test_picker_stamps_provenance_and_replayable_ray() -> void:
	# The shared ray recipe both pick inputs use, against the REAL sim binding:
	# even a worldless miss keeps the stable card shape plus the provenance
	# fields the snapshot writer replays.
	var sim := Simulation.new()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	camera.current = true
	var pick := DebugEntityPicker.pick_with_camera(
			sim, camera, Vector2(10, 10), "mouse_click")
	assert_not_null(pick, "the sim always answers the stable card")
	assert_eq(pick.source, "mouse_click",
			"the card records its input provenance")
	assert_eq(pick.ray_origin_godot, camera.project_ray_origin(Vector2(10, 10)),
			"the card records the replayable ray")
	assert_eq(pick.ray_dir_godot, camera.project_ray_normal(Vector2(10, 10)))
	assert_false(pick.hit, "worldless picks are honest misses")
