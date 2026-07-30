extends GutTest

const PageScript := preload("res://engine/debug/pages/debug_entities_page.gd")


class StubSim:
	extends Node

	var action_error: Error = OK
	var vehicle_first := false
	var health_calls: Array[Dictionary] = []
	var position_calls: Array[Dictionary] = []
	var cards := [
		{
			"name": "Despawned guard",
			"net_id": 41,
			"position": Vector3.ZERO,
			"pool": -1,
			"wire_handle": 1001,
			"alive": false,
			"health": 0,
			"ai_health": 0,
		},
		{
			"name": "Live guard",
			"net_id": 42,
			"position": Vector3(1.0, 2.0, 3.0),
			"pool": 1,
			"wire_handle": 1002,
			"alive": true,
			"health": 80,
			"ai_health": 80,
		},
	]

	func get_entity_count() -> int:
		return cards.size()

	func get_entity_debug(index: int) -> Dictionary:
		return cards[index].duplicate(true) \
				if index >= 0 and index < cards.size() else {}

	func get_present_stride() -> int:
		return NovaSimulation.PF_STRIDE

	func get_present_snapshot() -> PackedFloat32Array:
		var snapshot := PackedFloat32Array()
		var guard := _present_row(
				42, 1002, 501, Vector3(11.0, 2.0, -31.0))
		var vehicle := _present_row(
				77, 2001, 777, Vector3(20.0, 4.0, -40.0))
		snapshot.append_array(vehicle if vehicle_first else guard)
		snapshot.append_array(guard if vehicle_first else vehicle)
		return snapshot

	func get_world_entity_debug(net_id: int) -> Dictionary:
		if net_id != 77:
			return {}
		return {
			"name": "Client vehicle",
			"state_name": "driving",
			"health": 400,
			"team": 3,
			"alive": true,
		}

	func debug_set_entity_health(index: int, health: int) -> Error:
		health_calls.append({"index": index, "health": health})
		return action_error

	func debug_set_entity_position(index: int, position: Vector3) -> Error:
		position_calls.append({"index": index, "position": position})
		return action_error

	func _present_row(
			net_id: int,
			wire_handle: int,
			type_id: int,
			position: Vector3) -> PackedFloat32Array:
		var row := PackedFloat32Array()
		row.resize(NovaSimulation.PF_STRIDE)
		row[NovaSimulation.PF_TYPE_ID] = type_id
		row[NovaSimulation.PF_NET_ID] = net_id
		row[NovaSimulation.PF_WIRE_HANDLE] = wire_handle
		row[NovaSimulation.PF_KIND] = 1
		row[NovaSimulation.PF_INDEX] = net_id
		row[NovaSimulation.PF_BMS_ID] = 1000 + net_id
		row[NovaSimulation.PF_POS_X] = position.x
		row[NovaSimulation.PF_POS_Y] = position.y
		row[NovaSimulation.PF_POS_Z] = position.z
		row[NovaSimulation.PF_ALIVE] = 1.0
		return row


class StubRuntime:
	extends Node

	var sim := StubSim.new()

	func _init() -> void:
		add_child(sim)

	func get_sim() -> StubSim:
		return sim


func _make_page(runtime: StubRuntime) -> DebugEntitiesPage:
	add_child_autofree(runtime)
	var ctx := NovaDebugContext.new()
	ctx.runtime_source = func(): return runtime
	ctx.world_source = func(): return null
	ctx.options = NovaDebugOptionState.new()
	ctx.session = NovaDebugSession.new()
	NovaDebugCatalog.install(ctx.session)
	NovaDebugCatalog.bind_runtime_targets(
			ctx.session, ctx.runtime_source, ctx.world_source)
	ctx.session.set_authority_source(func(): return true)
	ctx.session.set_edit_unlocked(true)
	var page: DebugEntitiesPage = PageScript.new()
	page.setup(ctx)
	add_child_autofree(page)
	page.refresh()
	return page


func test_client_present_rows_keep_order_and_ai_edit_identity() -> void:
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	var rows := NovaDebugEntities.list(runtime.sim)

	assert_eq(rows.size(), 3)
	assert_eq(rows[0]["net_id"], 42)
	assert_eq(rows[0]["ai_index"], 1,
			"the first client-present row maps back to AI pool entry one")
	assert_true(rows[0]["editable"])
	assert_eq(rows[0]["world_position"], Vector3(11.0, 2.0, -31.0),
			"presentation position wins over the older AI detail-card position")
	assert_eq(rows[1]["net_id"], 77)
	assert_eq(rows[1]["name"], "Client vehicle")
	assert_eq(rows[1]["ai_index"], -1)
	assert_false(rows[1]["editable"],
			"a visible client entity without an AI record remains read-only")
	assert_eq(rows[2]["net_id"], 41)
	assert_eq(rows[2]["ai_index"], 0)
	assert_false(rows[2]["presented"],
			"host-only AI diagnostics append after the client-present rows")
	assert_false(rows[2]["editable"])


func test_page_shows_non_ai_rows_but_only_edits_through_ai_index() -> void:
	var runtime := StubRuntime.new()
	var page := _make_page(runtime)
	var list := page.find_child("EntityList", true, false) as ItemList
	assert_eq(list.item_count, 3)
	assert_string_contains(list.get_item_text(0), "ssn 42")
	assert_string_contains(list.get_item_text(1), "ssn 77")
	assert_string_contains(list.get_item_text(2), "ssn 41")

	var health := page.find_child("SetEntityHealth", true, false) as Button
	var value := page.find_child("EntityHealthValue", true, false) as SpinBox
	list.item_selected.emit(1)
	assert_true(health.disabled,
			"the client-present vehicle is inspectable but has no AI edit target")

	list.item_selected.emit(0)
	value.value = 37
	assert_false(health.disabled)
	health.pressed.emit()
	assert_eq(runtime.sim.health_calls, [{"index": 1, "health": 37}],
			"row zero edits its mapped AI pool entry, not row zero")


func test_position_editor_labels_axes_and_stays_compact() -> void:
	var page := _make_page(StubRuntime.new())
	var editor := page.find_child("EntityEditPosition", true, false) as Control
	var move := page.find_child("SetEntityPosition", true, false) as Button
	assert_not_null(editor)
	assert_not_null(move)

	for axis in ["X", "Y", "Z"]:
		var label := page.find_child(
				"EntityPosition%sLabel" % axis, true, false) as Label
		var value := page.find_child(
				"EntityPosition%s" % axis, true, false) as SpinBox
		assert_not_null(label, "%s has a visible axis label" % axis)
		assert_not_null(value, "%s keeps its stable editor control" % axis)
		if label != null:
			assert_eq(label.text, axis)

	assert_lte(editor.get_combined_minimum_size().x, 280.0,
			"the labeled position editor fits the narrow debug-page budget")
	await wait_process_frames(2)
	var z_value := page.find_child(
			"EntityPositionZ", true, false) as SpinBox
	assert_gt(move.get_global_rect().position.y, z_value.get_global_rect().end.y,
			"Move sits below the coordinate fields instead of widening their row")


func test_live_reorder_cannot_retarget_the_selected_entity_edit() -> void:
	var runtime := StubRuntime.new()
	var page := _make_page(runtime)
	var list := page.find_child("EntityList", true, false) as ItemList
	var health := page.find_child("SetEntityHealth", true, false) as Button
	var value := page.find_child("EntityHealthValue", true, false) as SpinBox
	list.item_selected.emit(0)
	value.value = 61

	# Reorder between the visible refresh and the click. The mutation performs
	# one last identity lookup, so row zero's new vehicle cannot receive it.
	runtime.sim.vehicle_first = true
	health.pressed.emit()
	assert_eq(runtime.sim.health_calls, [{"index": 1, "health": 61}])

	# The next normal refresh also moves ItemList selection to the same guard's
	# new row rather than preserving a transient numeric discovery index.
	page.refresh()
	assert_true(list.is_selected(1))
	value.value = 62
	health.pressed.emit()
	assert_eq(runtime.sim.health_calls.back(), {"index": 1, "health": 62})


func test_despawned_ai_pool_entries_are_explicit_and_not_editable() -> void:
	var page := _make_page(StubRuntime.new())
	var list := page.find_child("EntityList", true, false) as ItemList
	assert_string_contains(list.get_item_text(2), "[despawned]")

	list.item_selected.emit(2)

	var health := page.find_child("SetEntityHealth", true, false) as Button
	var position := page.find_child("SetEntityPosition", true, false) as Button
	var status := page.find_child("EntityEditStatus", true, false) as Label
	assert_true(health.disabled)
	assert_true(position.disabled)
	assert_string_contains(status.text, "despawned")


func test_runtime_action_failure_always_has_visible_feedback() -> void:
	var runtime := StubRuntime.new()
	var page := _make_page(runtime)
	var list := page.find_child("EntityList", true, false) as ItemList
	list.item_selected.emit(0)
	runtime.sim.action_error = ERR_UNAVAILABLE

	var health := page.find_child("SetEntityHealth", true, false) as Button
	assert_false(health.disabled)
	health.pressed.emit()
	assert_eq(runtime.sim.health_calls[0]["index"], 1)

	var status := page.find_child("EntityEditStatus", true, false) as Label
	assert_false(status.text.strip_edges().is_empty())
	assert_string_contains(status.text, "Could not set entity health")
