extends GutTest

# PickDebugView: the world highlight for the debug pick list. Draws rows from
# the injected shell-owned model, re-resolves mover positions through the sim
# accessor it names, and clears cleanly when the list empties.

const ViewScript := preload("res://engine/debug/pick/pick_debug_view.gd")


class StubSim:
	extends Node
	var mover_position := Vector3(9, 9, 9)

	func get_world_entity_debug(net_id: int) -> Dictionary:
		if net_id != 212:
			return {}
		return {"net_id": net_id, "position": mover_position}


class StubWorld:
	extends Node
	var sim := StubSim.new()

	func _init() -> void:
		add_child(sim)

	func get_sim() -> StubSim:
		return sim


func _static_pick() -> Dictionary:
	return {
		"hit": true, "entity_handle": 5, "kind": 2, "index": 14, "bms_id": 1484,
		"net_id": 0, "name": "RckS07", "position_godot": Vector3(1, 0, 1),
		"bound_radius": 3.0, "hit_position_godot": Vector3(1, 1, 1),
	}


func _mover_pick() -> Dictionary:
	return {
		"hit": true, "entity_handle": 4130, "kind": 1, "index": 34, "bms_id": 212,
		"net_id": 212, "name": "hmv_2", "position_godot": Vector3(5, 0, 5),
		"bound_radius": 5.0, "hit_position_godot": Vector3(5, 1, 5),
	}


func _make_view(world: StubWorld, picks: NovaDebugPickList) -> PickDebugView:
	var view: PickDebugView = ViewScript.new()
	view.set_pick_list(picks)
	add_child_autofree(view)
	view.setup(world)
	return view


func test_draws_one_labeled_highlight_per_pick() -> void:
	var world := StubWorld.new()
	add_child_autofree(world)
	var picks := NovaDebugPickList.new()
	picks.add(_static_pick())
	picks.add(_mover_pick())
	var view := _make_view(world, picks)
	view.refresh_now()

	var lines := view.get_node("PickDebugLines") as MeshInstance3D
	assert_gt((lines.mesh as ImmediateMesh).get_surface_count(), 0,
			"picks draw wireframe geometry")
	var label0 := view.get_node("PickDebugLabel0") as Label3D
	var label1 := view.get_node("PickDebugLabel1") as Label3D
	assert_true(label0.visible)
	assert_true(label1.visible)
	assert_string_contains(label0.text, "RckS07")
	assert_string_contains(label0.text, "#1484")
	assert_string_contains(label0.text, "(2:14)")
	assert_false((view.get_node("PickDebugLabel2") as Label3D).visible,
			"unused label slots stay hidden")


func test_movers_follow_the_live_sim_position() -> void:
	var world := StubWorld.new()
	add_child_autofree(world)
	var picks := NovaDebugPickList.new()
	picks.add(_mover_pick())
	var view := _make_view(world, picks)
	view.refresh_now()

	var label := view.get_node("PickDebugLabel0") as Label3D
	assert_almost_eq(label.position.x, 9.0, 0.001,
			"the highlight re-resolved the mover's LIVE position (pick-time was x=5)")
	world.sim.mover_position = Vector3(20, 0, 9)
	view.refresh_now()
	assert_almost_eq(label.position.x, 20.0, 0.001,
			"...and keeps following it")


func test_static_picks_keep_their_pick_time_position() -> void:
	var world := StubWorld.new()
	add_child_autofree(world)
	var picks := NovaDebugPickList.new()
	picks.add(_static_pick())
	var view := _make_view(world, picks)
	view.refresh_now()
	var label := view.get_node("PickDebugLabel0") as Label3D
	assert_almost_eq(label.position.x, 1.0, 0.001,
			"statics (net_id 0) never consult the mover accessor")


func test_emptying_the_list_clears_everything() -> void:
	var world := StubWorld.new()
	add_child_autofree(world)
	var picks := NovaDebugPickList.new()
	picks.add(_static_pick())
	var view := _make_view(world, picks)
	view.refresh_now()
	picks.clear()
	view.refresh_now()
	var lines := view.get_node("PickDebugLines") as MeshInstance3D
	assert_eq((lines.mesh as ImmediateMesh).get_surface_count(), 0)
	assert_false((view.get_node("PickDebugLabel0") as Label3D).visible)
