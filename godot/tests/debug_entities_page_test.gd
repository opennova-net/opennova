extends GutTest

const PageScript := preload("res://engine/debug/pages/debug_entities_page.gd")


class StubSim:
	extends Node

	var action_error: Error = OK
	var vehicle_first := false
	var guard_present := true
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
		if vehicle_first or not guard_present:
			snapshot.append_array(vehicle)
		if guard_present:
			snapshot.append_array(guard)
		if not vehicle_first and guard_present:
			snapshot.append_array(vehicle)
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


func _make_page(
		runtime: StubRuntime,
		picks: NovaDebugPickList = null) -> DebugEntitiesPage:
	add_child_autofree(runtime)
	var ctx := NovaDebugContext.new()
	ctx.runtime_source = func(): return runtime
	ctx.world_source = func(): return null
	ctx.pick_list = picks
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


func _guard_pick() -> Dictionary:
	return {
		"hit": true,
		"entity_handle": 1002,
		"pool": 1,
		"kind": 1,
		"index": 42,
		"bms_id": 1042,
		"net_id": 42,
		# Pick cards carry the visual/authored item id while PF_TYPE_ID is the
		# runtime type. Exact wire/net identity must win when those domains differ.
		"item_id": 9001,
		"name": "Live guard",
		"position_godot": Vector3(11.0, 2.0, -31.0),
		"distance_units": 12.0,
		"tick": 77,
	}


func _vehicle_pick() -> Dictionary:
	return {
		"hit": true,
		"entity_handle": 2001,
		"pool": 2,
		"kind": 1,
		"index": 77,
		"bms_id": 1077,
		"net_id": 77,
		"item_id": 7007,
		"name": "Client vehicle",
		"position_godot": Vector3(20.0, 4.0, -40.0),
		"distance_units": 20.0,
		"tick": 76,
	}


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
	var health_editor := page.find_child(
			"EntityEditHealth", true, false) as Control
	var position_editor := page.find_child(
			"EntityEditPosition", true, false) as Control
	var status := page.find_child("EntityEditStatus", true, false) as Label
	list.item_selected.emit(1)
	assert_true(health.disabled,
			"the client-present vehicle is inspectable but has no AI edit target")
	assert_false(health_editor.visible)
	assert_false(position_editor.visible)
	assert_eq(status.text, "Read only: no authoritative AI edit target.")

	list.item_selected.emit(0)
	value.value = 37
	assert_false(health.disabled)
	health.pressed.emit()
	assert_eq(runtime.sim.health_calls, [{"index": 1, "health": 37}],
			"row zero edits its mapped AI pool entry, not row zero")


func test_mutation_editors_appear_only_after_an_editable_selection() -> void:
	var page := _make_page(StubRuntime.new())
	var list := page.find_child("EntityList", true, false) as ItemList
	var health_editor := page.find_child(
			"EntityEditHealth", true, false) as Control
	var position_editor := page.find_child(
			"EntityEditPosition", true, false) as Control

	assert_false(health_editor.visible,
			"an empty selection does not present dead health inputs")
	assert_false(position_editor.visible,
			"an empty selection does not present dead position inputs")

	list.item_selected.emit(0)
	assert_true(health_editor.visible,
			"an editable AI-backed selection exposes health editing")
	assert_true(position_editor.visible,
			"an editable AI-backed selection exposes position editing")


func test_successful_world_pick_selects_and_opens_the_matching_inspector() -> void:
	var runtime := StubRuntime.new()
	var picks := NovaDebugPickList.new()
	var page := _make_page(runtime, picks)
	var list := page.find_child("EntityList", true, false) as ItemList
	var health_editor := page.find_child(
			"EntityEditHealth", true, false) as Control
	var value := page.find_child("EntityHealthValue", true, false) as SpinBox
	var set_health := page.find_child("SetEntityHealth", true, false) as Button

	assert_true(list.get_selected_items().is_empty())
	picks.add(_guard_pick())

	assert_true(list.is_selected(0),
			"a successful pick selects the same live entity in the inspector")
	assert_true(health_editor.visible,
			"the selected AI-backed pick exposes its mutation affordances")
	assert_false(set_health.disabled)
	value.value = 39
	set_health.pressed.emit()
	assert_eq(runtime.sim.health_calls, [{"index": 1, "health": 39}],
			"pick-driven selection still mutates through the authoritative AI index")


func test_picked_entity_card_can_reselect_its_inspector_row() -> void:
	var runtime := StubRuntime.new()
	var picks := NovaDebugPickList.new()
	picks.add(_guard_pick())
	var page := _make_page(runtime, picks)
	var list := page.find_child("EntityList", true, false) as ItemList
	list.deselect_all()

	var select_pick := page.find_child("SelectPick0", true, false) as Button
	assert_not_null(select_pick,
			"picked cards are navigation affordances, not passive labels")
	if select_pick != null:
		select_pick.pressed.emit()
		assert_true(list.is_selected(0))


func test_pick_waits_for_a_new_live_row_instead_of_retaining_an_old_selection() -> void:
	var runtime := StubRuntime.new()
	var picks := NovaDebugPickList.new()
	var page := _make_page(runtime, picks)
	var list := page.find_child("EntityList", true, false) as ItemList
	var saved_cards: Array = runtime.sim.cards.duplicate(true)

	# The click can arrive one presentation beat before the picked actor's row.
	# Keep another row selected so a simple "select last pick only when empty"
	# implementation cannot accidentally pass.
	runtime.sim.cards = []
	runtime.sim.guard_present = false
	page.refresh()
	list.select(0)
	list.item_selected.emit(0)
	assert_string_contains(list.get_item_text(0), "ssn 77")
	picks.add(_guard_pick())
	assert_true(list.is_selected(0), "the old vehicle remains while the guard is absent")

	runtime.sim.cards = saved_cards
	runtime.sim.guard_present = true
	page.refresh()
	assert_true(list.is_selected(0),
			"the pending pick wins as soon as its guard row appears")
	assert_string_contains(list.get_item_text(0), "ssn 42")
	var detail := page.find_child("EntityDetail", true, false) as Label
	assert_string_contains(detail.text, "Live guard")


func test_removing_a_pending_pick_cancels_its_future_auto_selection() -> void:
	var runtime := StubRuntime.new()
	var saved_cards: Array = runtime.sim.cards.duplicate(true)
	runtime.sim.cards = []
	runtime.sim.guard_present = false
	var picks := NovaDebugPickList.new()
	var page := _make_page(runtime, picks)
	var list := page.find_child("EntityList", true, false) as ItemList

	picks.add(_vehicle_pick())
	picks.add(_guard_pick())
	assert_true(list.is_selected(0))
	picks.remove_at(1)
	assert_eq(picks.size(), 1, "the unrelated vehicle pick remains")

	runtime.sim.cards = saved_cards
	runtime.sim.guard_present = true
	page.refresh()
	var selected := list.get_selected_items()
	assert_eq(selected.size(), 1)
	assert_eq(int(selected[0]), 1,
			"removing the pending guard keeps the selected vehicle after rows reorder")
	assert_string_contains(list.get_item_text(selected[0]), "ssn 77")
	await wait_process_frames(2)


func test_position_editor_labels_axes_and_stays_compact() -> void:
	var page := _make_page(StubRuntime.new())
	var list := page.find_child("EntityList", true, false) as ItemList
	list.item_selected.emit(0)
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


func test_refresh_preserves_a_staged_value_after_tabbing_to_its_apply_button() -> void:
	var page := _make_page(StubRuntime.new())
	var list := page.find_child("EntityList", true, false) as ItemList
	list.item_selected.emit(0)
	var value := page.find_child("EntityHealthValue", true, false) as SpinBox
	var apply := page.find_child("SetEntityHealth", true, false) as Button
	value.value = 33
	apply.grab_focus()

	page.refresh()

	assert_eq(value.value, 33.0,
			"the live refresh cannot overwrite an edit staged for the focused Set button")


func test_pick_selection_replaces_staged_values_before_retargeting_edits() -> void:
	var runtime := StubRuntime.new()
	runtime.sim.cards.append({
		"name": "Client vehicle",
		"net_id": 77,
		"position": Vector3(20.0, 4.0, -40.0),
		"pool": 2,
		"wire_handle": 2001,
		"alive": true,
		"health": 400,
		"ai_health": 400,
	})
	var picks := NovaDebugPickList.new()
	var page := _make_page(runtime, picks)
	var list := page.find_child("EntityList", true, false) as ItemList
	var value := page.find_child("EntityHealthValue", true, false) as SpinBox
	var apply := page.find_child("SetEntityHealth", true, false) as Button
	list.item_selected.emit(0)
	value.value = 33
	apply.grab_focus()

	picks.add(_vehicle_pick())

	assert_true(list.is_selected(1))
	assert_eq(value.value, 400.0,
			"changing entity identity replaces the previous target's staged value")
	apply.pressed.emit()
	assert_eq(runtime.sim.health_calls.back(), {"index": 2, "health": 400},
			"the focused editor cannot apply entity A's value to picked entity B")


func test_despawned_ai_pool_entries_are_explicit_and_not_editable() -> void:
	var page := _make_page(StubRuntime.new())
	var list := page.find_child("EntityList", true, false) as ItemList
	assert_string_contains(list.get_item_text(2), "[despawned]")

	list.item_selected.emit(2)

	var health := page.find_child("SetEntityHealth", true, false) as Button
	var position := page.find_child("SetEntityPosition", true, false) as Button
	var health_editor := page.find_child(
			"EntityEditHealth", true, false) as Control
	var position_editor := page.find_child(
			"EntityEditPosition", true, false) as Control
	var status := page.find_child("EntityEditStatus", true, false) as Label
	assert_true(health.disabled)
	assert_true(position.disabled)
	assert_false(health_editor.visible)
	assert_false(position_editor.visible)
	assert_eq(status.text, "Read only: this AI entry has despawned from the world.")


func test_retained_selection_recomputes_edit_policy_after_live_despawn() -> void:
	var runtime := StubRuntime.new()
	var page := _make_page(runtime)
	var list := page.find_child("EntityList", true, false) as ItemList
	var health := page.find_child("SetEntityHealth", true, false) as Button
	var health_editor := page.find_child(
			"EntityEditHealth", true, false) as Control
	var status := page.find_child("EntityEditStatus", true, false) as Label
	list.item_selected.emit(0)
	health.pressed.emit()
	assert_eq(status.text, "Health updated.")

	runtime.sim.cards[1]["pool"] = -1
	page.refresh()
	assert_false(health_editor.visible)
	assert_eq(status.text,
			"Read only: this AI entry has despawned from the world.",
			"a live policy change replaces stale mutation feedback")

	runtime.sim.cards[1]["pool"] = 1
	page.refresh()
	assert_true(health_editor.visible)
	assert_eq(status.text, "",
			"restored editability clears the stale read-only policy")


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
