extends GutTest

# PickClickCatcher: the overlay-open mouse picker. A left-press ray-picks
# through the live camera into the injected list; everything else passes
# through untouched.

class StubSim:
	extends Node
	var calls := 0
	var last_range := 0.0

	func debug_pick_entity(_from: Vector3, _dir: Vector3, range_units: float) -> Dictionary:
		calls += 1
		last_range = range_units
		return {
			"hit": true, "entity_handle": 7, "kind": 1, "index": 3, "bms_id": 42,
			"net_id": 0, "name": "crate", "position_godot": Vector3.ZERO,
			"bound_radius": 1.0, "hit_position_godot": Vector3.FORWARD,
		}


class StubWorld:
	extends Node
	var sim := StubSim.new()

	func _init() -> void:
		add_child(sim)

	func get_sim() -> StubSim:
		return sim


func _make_catcher(list: NovaDebugPickList) -> Array:
	var camera := Camera3D.new()
	add_child_autofree(camera)
	camera.current = true
	var world := StubWorld.new()
	add_child_autofree(world)
	var catcher := PickClickCatcher.new()
	world.add_child(catcher)
	catcher.setup(world, list)
	return [catcher, world]


func test_left_press_picks_into_the_list_with_provenance() -> void:
	var list := NovaDebugPickList.new()
	var made := _make_catcher(list)
	var catcher: PickClickCatcher = made[0]
	var world: StubWorld = made[1]

	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	catcher._unhandled_input(click)

	assert_eq(world.sim.calls, 1, "the click ray-picked through the sim")
	assert_almost_eq(world.sim.last_range, DebugEntityPicker.PICK_RANGE_UNITS, 0.001)
	assert_eq(list.size(), 1, "the hit landed in the pick list")
	var pick: Dictionary = list.get_picks()[0]
	assert_eq(String(pick.get("source", "")), "mouse_click",
			"the card records its input provenance")
	assert_true(pick.has("ray_origin_godot"), "the card records the replayable ray")


func test_other_input_is_ignored() -> void:
	var list := NovaDebugPickList.new()
	var made := _make_catcher(list)
	var catcher: PickClickCatcher = made[0]
	var world: StubWorld = made[1]

	var release := InputEventMouseButton.new()
	release.button_index = MOUSE_BUTTON_LEFT
	release.pressed = false
	catcher._unhandled_input(release)
	var right := InputEventMouseButton.new()
	right.button_index = MOUSE_BUTTON_RIGHT
	right.pressed = true
	catcher._unhandled_input(right)
	var motion := InputEventMouseMotion.new()
	catcher._unhandled_input(motion)

	assert_eq(world.sim.calls, 0, "no ray ever ran")
	assert_eq(list.size(), 0)
