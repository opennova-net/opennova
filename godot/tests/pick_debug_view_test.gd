extends GutTest

# PickDebugView: the world highlight for the debug pick list. Draws rows from
# the injected shell-owned model through the public render seam (mover
# positions arrive as the net_id -> live position map the sim fetch builds),
# and clears cleanly when the list empties.

const ViewScript := preload("res://game/debug/pick/pick_debug_view.gd")


func _card(entity_handle: int, kind: int, index: int, bms_id: int, net_id: int,
		name: String, position: Vector3, bound_radius: float,
		hit_position: Vector3) -> DebugPickCard:
	var card := DebugPickCard.new()
	card.hit = true
	card.entity_handle = entity_handle
	card.kind = kind
	card.index = index
	card.bms_id = bms_id
	card.net_id = net_id
	card.name = name
	card.position_godot = position
	card.bound_radius = bound_radius
	card.hit_position_godot = hit_position
	return card


func _static_pick() -> DebugPickCard:
	return _card(5, 2, 14, 1484, 0, "RckS07", Vector3(1, 0, 1), 3.0, Vector3(1, 1, 1))


func _mover_pick() -> DebugPickCard:
	return _card(4130, 1, 34, 212, 212, "hmv_2", Vector3(5, 0, 5), 5.0, Vector3(5, 1, 5))


func _make_view(picks: DebugPickList) -> PickDebugView:
	var view: PickDebugView = ViewScript.new()
	view.set_pick_list(picks)
	add_child_autofree(view)
	view.setup(null)
	return view


func test_draws_one_labeled_highlight_per_pick() -> void:
	var picks := DebugPickList.new()
	picks.add(_static_pick())
	picks.add(_mover_pick())
	var view := _make_view(picks)
	view.render_picks({212: Vector3(9, 9, 9)})

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
	var picks := DebugPickList.new()
	picks.add(_mover_pick())
	var view := _make_view(picks)
	view.render_picks({212: Vector3(9, 9, 9)})

	var label := view.get_node("PickDebugLabel0") as Label3D
	assert_almost_eq(label.position.x, 9.0, 0.001,
			"the highlight re-resolved the mover's LIVE position (pick-time was x=5)")
	view.render_picks({212: Vector3(20, 0, 9)})
	assert_almost_eq(label.position.x, 20.0, 0.001,
			"...and keeps following it")


func test_static_picks_keep_their_pick_time_position() -> void:
	var picks := DebugPickList.new()
	picks.add(_static_pick())
	var view := _make_view(picks)
	view.render_picks({212: Vector3(9, 9, 9)})
	var label := view.get_node("PickDebugLabel0") as Label3D
	assert_almost_eq(label.position.x, 1.0, 0.001,
			"statics (net_id 0) never consult the mover positions")


func test_emptying_the_list_clears_everything() -> void:
	var picks := DebugPickList.new()
	picks.add(_static_pick())
	var view := _make_view(picks)
	view.render_picks({})
	picks.clear()
	view.render_picks({})
	var lines := view.get_node("PickDebugLines") as MeshInstance3D
	assert_eq((lines.mesh as ImmediateMesh).get_surface_count(), 0)
	assert_false((view.get_node("PickDebugLabel0") as Label3D).visible)
