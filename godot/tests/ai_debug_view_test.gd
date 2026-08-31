extends GutTest

# AiDebugView: the F3 AI window's 3D overlay. Drives it with a crafted
# get_ai_debug() dictionary (the same shape Simulation emits) through the
# public render seam to pin: state labels with alert colors, nav-route lines
# with the follower's current node, target/aim lines, perception rings, the
# element gating, the selection-provider ring behavior, and the clear paths.

const ViewScript := preload("res://game/debug/ai_debug_view.gd")


func _payload() -> Dictionary:
	return {
		"valid": true,
		"logic_tick": 62,
		"rows": [
			{
				"ai_index": 0,
				"handle": 0x3001,
				"name": "ALPHA",
				"group": 5,
				"alive": true,
				"infantry": true,
				"pos": Vector3(10, 0, 5),
				"state": 17,
				"state_name": "GROUND_COMBAT",
				"alert": 2,
				"move_mode": 7,
				"out_speed": 0,
				"wp_channel": 1,
				"wp_node": 1,
				"target_valid": true,
				"target_handle": 0x3002,
				"target_pos": Vector3(20, 0, 5),
				"target_name": "BRAVO",
				"aim_valid": true,
				"aim_dir": Vector3(1, 0, 0),
				"muzzle_valid": true,
				"muzzle": Vector3(10.4, 1.4, 5.0),
				"sight_range": 80.0,
				"attack_range": 50.0,
				"combat_timer": 40,
				"fire_delay": 9,
				"damage_timer": 6,
				"combat_move_timer": 11,
			},
			{
				"ai_index": 1,
				"handle": 0x3002,
				"name": "BRAVO",
				"group": 5,
				"alive": false,
				"infantry": true,
				"pos": Vector3(20, 0, 5),
				"state": 23,
				"state_name": "GROUND_DEAD",
				"alert": 0,
				"move_mode": 0,
				"out_speed": 0,
				"wp_channel": 0,
				"wp_node": 0,
				"target_valid": false,
				"aim_valid": false,
				"muzzle_valid": false,
				"sight_range": 80.0,
				"attack_range": 50.0,
			},
		],
		"channels": [
			{
				"index": 1,
				"once": false,
				"followers": 1,
				"nodes": PackedVector3Array([
					Vector3(0, 0, 0), Vector3(10, 0, 0), Vector3(10, 0, 10),
				]),
				"radii": PackedFloat32Array([0.5, 0.5, 0.5]),
			},
		],
		"groups": [{ "id": 5, "alert": 2, "initial_count": 2, "live_count": 1 }],
		"counters": { "brain_count": 2 },
	}


func _make_view() -> Node3D:
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(null)
	return view


func _mesh(view: Node3D, node_name: String) -> ImmediateMesh:
	return (view.get_node(node_name) as MeshInstance3D).mesh as ImmediateMesh


func test_draws_labels_routes_targets_and_rings() -> void:
	var view := _make_view()
	view.render_report(_payload())

	assert_eq(view.get_debug_drawable_count(), 3, "2 brains + 1 route")
	var label0 := view.get_node("AiDebugLabel0") as Label3D
	assert_true(label0.visible, "the first brain gets a label")
	assert_string_contains(label0.text, "ALPHA")
	assert_string_contains(label0.text, "GROUND_COMBAT")
	assert_string_contains(label0.text, "m7")
	assert_string_contains(label0.text, "ch 1 node 1")
	assert_string_contains(label0.text, "-> BRAVO")
	assert_string_contains(label0.text, "fd 9")
	assert_eq(label0.modulate, ViewScript.alert_color(2), "red alert colors the label")
	var label1 := view.get_node("AiDebugLabel1") as Label3D
	assert_true(label1.visible)
	assert_string_contains(label1.text, "DEAD")
	assert_false((view.get_node("AiDebugLabel2") as Label3D).visible,
			"the pool beyond the rows stays hidden")

	assert_gt(_mesh(view, "AiDebugRoutes").get_surface_count(), 0, "routes drawn")
	assert_gt(_mesh(view, "AiDebugTargets").get_surface_count(), 0,
			"target/aim lines drawn")
	assert_gt(_mesh(view, "AiDebugRings").get_surface_count(), 0,
			"the engaged brain draws perception rings")


func test_element_flags_gate_each_layer() -> void:
	var view := _make_view()
	view.set_elements(false, false, false, false)
	view.render_report(_payload())
	assert_false((view.get_node("AiDebugLabel0") as Label3D).visible, "labels off")
	assert_eq(_mesh(view, "AiDebugRoutes").get_surface_count(), 0, "routes off")
	assert_eq(_mesh(view, "AiDebugTargets").get_surface_count(), 0, "targets off")
	assert_eq(_mesh(view, "AiDebugRings").get_surface_count(), 0, "rings off")
	assert_eq(view.get_debug_drawable_count(), 3,
			"drawable count reports the data, not the element gating")

	view.set_elements(true, true, true, true)
	view.render_report(_payload())
	assert_true((view.get_node("AiDebugLabel0") as Label3D).visible, "labels back on")


func test_selection_provider_rings_an_unengaged_brain() -> void:
	var view := _make_view()
	var payload := _payload()
	# Nobody engaged: no rings without a selection...
	var rows: Array = payload["rows"]
	(rows[0] as Dictionary)["target_valid"] = false
	(rows[1] as Dictionary)["alive"] = true
	view.render_report(payload)
	assert_eq(_mesh(view, "AiDebugRings").get_surface_count(), 0,
			"no engaged brains and no selection: no rings")
	# ...but the selected brain always gets its rings.
	view.set_selection_provider(func() -> int: return 0x3002)
	view.render_report(payload)
	assert_gt(_mesh(view, "AiDebugRings").get_surface_count(), 0,
			"the selection rings even while unengaged")


func test_invalid_payload_and_missing_sim_clear() -> void:
	var view := _make_view()
	view.render_report(_payload())
	view.render_report({ "valid": false })
	assert_eq(view.get_debug_drawable_count(), 0)
	assert_false((view.get_node("AiDebugLabel0") as Label3D).visible)
	assert_eq(_mesh(view, "AiDebugRoutes").get_surface_count(), 0)

	view.render_report(_payload())
	# The sim goes away (mission unloaded): the next real refresh resolves no
	# world/sim and clears every surface.
	view.refresh_now()
	assert_eq(view.get_debug_drawable_count(), 0)
	assert_eq(_mesh(view, "AiDebugTargets").get_surface_count(), 0)
	assert_false((view.get_node("AiDebugLabel0") as Label3D).visible)
