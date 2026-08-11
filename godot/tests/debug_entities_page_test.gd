extends GutTest

# DebugEntitiesPage behavior over row data (selection identity, edit policy,
# pick takeover) plus DebugEntities.list against a REAL minimal sim. The
# context is typed (ADR 0034), so scenario rows are fabricated in the
# DebugEntities.list output shape and fed through a typed page harness
# overriding _list_rows(); mutations record on a session-registered sim
# target, the same registered-contract seam runtime MCP drives.

const PageScript := preload("res://game/debug/pages/debug_entities_page.gd")
const MissionPresentation := preload("res://game/world/mission_presentation.gd")


## The scenario model: the two AI cards plus the presented-row order knobs the
## old sim double carried, now producing row DATA in the list() output shape.
class RowModel:
	extends RefCounted

	var vehicle_first := false
	var guard_present := true
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

	func rows() -> Array[Dictionary]:
		var out: Array[Dictionary] = []
		var guard := _presented_row(_card_for_net(42), 1,
				42, 1002, 501, 1042, Vector3(11.0, 2.0, -31.0))
		var vehicle := _presented_row(_card_for_net(77), _ai_index_for_net(77),
				77, 2001, 777, 1077, Vector3(20.0, 4.0, -40.0))
		if vehicle_first or not guard_present:
			out.append(vehicle)
		if guard_present:
			out.append(guard)
		if not vehicle_first and guard_present:
			out.append(vehicle)
		# Host-only AI diagnostics for cards without a presented row.
		for ai_index in range(cards.size()):
			var card: Dictionary = cards[ai_index]
			var net_id := int(card.get("net_id", 0))
			var seen := false
			for row in out:
				if int(row.get("ai_index", -1)) == ai_index:
					seen = true
			if seen:
				continue
			out.append(_diagnostic_row(card, ai_index))
		for index in range(out.size()):
			out[index]["index"] = index
		return out

	func _card_for_net(net_id: int) -> Dictionary:
		for card in cards:
			if int(card.get("net_id", 0)) == net_id:
				return (card as Dictionary).duplicate(true)
		# A presented client entity without an AI record: the world card.
		if net_id == 77:
			return {
				"name": "Client vehicle",
				"state_name": "driving",
				"health": 400,
				"team": 3,
				"alive": true,
			}
		return {}

	func _ai_index_for_net(net_id: int) -> int:
		for ai_index in range(cards.size()):
			if int(cards[ai_index].get("net_id", 0)) == net_id:
				return ai_index
		return -1

	func _presented_row(detail: Dictionary, ai_index: int, net_id: int,
			wire_handle: int, type_id: int, bms_id: int,
			position: Vector3) -> Dictionary:
		var registry_present := not detail.has("pool") \
				or int(detail.get("pool", -1)) >= 0
		detail["position"] = position
		detail["net_id"] = net_id
		detail["wire_handle"] = wire_handle
		detail["type_id"] = type_id
		detail["alive"] = true
		detail["hidden"] = false
		return {
			"index": -1,
			"view_index": -1,
			"ai_index": ai_index,
			"editable": ai_index >= 0 and registry_present,
			"presented": true,
			"registry_present": registry_present,
			"kind": 1,
			"source_index": net_id,
			"bms_id": bms_id,
			"net_id": net_id,
			"type_id": type_id,
			"wire_handle": wire_handle,
			"name": String(detail.get("name", "")),
			"state": String(detail.get("state_name", "")),
			"health": int(detail.get("health", 0)),
			"team": int(detail.get("team", -1)),
			"alive": true,
			"hidden": false,
			"world_position": position,
			"mission_position": Vector3(position.x, -position.z, position.y),
			"detail": detail,
		}

	func _diagnostic_row(card: Dictionary, ai_index: int) -> Dictionary:
		var detail := card.duplicate(true)
		var registry_present := int(detail.get("pool", -1)) >= 0
		var position: Vector3 = detail.get("position", Vector3.ZERO)
		detail["ai_index"] = ai_index
		detail["presented"] = false
		detail["registry_present"] = registry_present
		return {
			"index": -1,
			"view_index": -1,
			"ai_index": ai_index,
			"editable": ai_index >= 0 and registry_present,
			"presented": false,
			"registry_present": registry_present,
			"kind": int(detail.get("kind", -1)),
			"source_index": int(detail.get("index", -1)),
			"bms_id": int(detail.get("bms_id", 0)),
			"net_id": int(detail.get("net_id", 0)),
			"type_id": int(detail.get("item_id", 0)),
			"wire_handle": int(detail.get("wire_handle", 0)),
			"name": String(detail.get("name", "")),
			"state": String(detail.get("state_name", "")),
			"health": int(detail.get("health", 0)),
			"team": int(detail.get("team", -1)),
			"alive": bool(detail.get("alive", false)),
			"hidden": false,
			"world_position": position,
			"mission_position": Vector3(position.x, -position.z, position.y),
			"detail": detail,
		}


## The typed page harness: IS the page, with the row source overridden to the
## scenario model.
class PageHarness:
	extends DebugEntitiesPage

	var model: RowModel = null

	func _list_rows() -> Array[Dictionary]:
		return model.rows() if model != null else []


## The session-registered mutation target (the same seam the live catalog
## binds the real Simulation to). It carries the COMPLETE sim-target control
## surface the catalog declares — the session reads every registered control
## back from its target.
class ActionSim:
	extends RefCounted

	var action_error: Error = OK
	var health_calls: Array[Dictionary] = []
	var position_calls: Array[Dictionary] = []
	var wac_paused := false

	func debug_set_entity_health(index: int, health: int) -> Error:
		health_calls.append({"index": index, "health": health})
		return action_error

	func debug_set_entity_position(index: int, position: Vector3) -> Error:
		position_calls.append({"index": index, "position": position})
		return action_error

	func debug_teleport_local_player(
			_position: Vector3, _yaw: float, _pitch: float) -> Error:
		return OK

	func is_wac_paused() -> bool:
		return wac_paused

	func set_wac_paused(value: bool) -> void:
		wac_paused = value

	func set_mission_variable(_index: int, _value: int) -> void:
		pass


class Fixture:
	extends RefCounted
	var model := RowModel.new()
	var actions := ActionSim.new()
	var page: PageHarness = null


func _make_fixture(picks: DebugPickList = null) -> Fixture:
	var fixture := Fixture.new()
	var ctx := DebugContext.new()
	ctx.runtime_source = func(): return null
	ctx.world_source = func(): return null
	ctx.pick_list = picks
	ctx.options = DebugOptionState.new()
	ctx.session = DebugSession.new()
	DebugCatalog.install(ctx.session)
	DebugCatalog.bind_runtime_targets(
			ctx.session, ctx.runtime_source, ctx.world_source)
	ctx.session.set_target_source(DebugCatalog.TARGET_SIM,
			func(): return fixture.actions, "No simulation is active.")
	ctx.session.set_authority_source(func(): return true)
	ctx.session.set_edit_unlocked(true)
	var page := PageHarness.new()
	page.model = fixture.model
	page.setup(ctx)
	add_child_autofree(page)
	page.refresh()
	fixture.page = page
	return fixture


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


func test_real_sim_rows_join_ai_cards_with_edit_identity() -> void:
	# DebugEntities.list against the REAL typed Simulation: a minimal
	# in-memory mission (one authored organic + the auto-spawned host player).
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	mission.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)  # KIND_ORGANIC
	var container := Node3D.new()
	add_child_autofree(container)
	var runtime := MissionPresentation.new()
	add_child_autofree(runtime)
	assert_eq(runtime.setup(mission, container,
			{"placer": MissionObjectPlacer.new()}), 2)
	assert_true(runtime.tick(), "one presented tick fills the client snapshot")

	var rows := DebugEntities.list(runtime.get_sim())
	assert_eq(rows.size(), 2, "both live entities discover exactly once")
	for row in rows:
		assert_gte(int(row["ai_index"]), 0,
				"every host row maps back to its AI pool entry")
		assert_true(bool(row["editable"]),
				"registry-present AI rows expose the authoritative edit target")
	assert_ne(int(rows[0]["ai_index"]), int(rows[1]["ai_index"]),
			"rows keep distinct AI identities")


func test_page_shows_non_ai_rows_but_only_edits_through_ai_index() -> void:
	var fixture := _make_fixture()
	var page := fixture.page
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
	assert_eq(fixture.actions.health_calls, [{"index": 1, "health": 37}],
			"row zero edits its mapped AI pool entry, not row zero")


func test_mutation_editors_appear_only_after_an_editable_selection() -> void:
	var page := _make_fixture().page
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
	var picks := DebugPickList.new()
	var fixture := _make_fixture(picks)
	var page := fixture.page
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
	assert_eq(fixture.actions.health_calls, [{"index": 1, "health": 39}],
			"pick-driven selection still mutates through the authoritative AI index")


func test_picked_entity_card_can_reselect_its_inspector_row() -> void:
	var picks := DebugPickList.new()
	picks.add(_guard_pick())
	var page := _make_fixture(picks).page
	var list := page.find_child("EntityList", true, false) as ItemList
	list.deselect_all()

	var select_pick := page.find_child("SelectPick0", true, false) as Button
	assert_not_null(select_pick,
			"picked cards are navigation affordances, not passive labels")
	if select_pick != null:
		select_pick.pressed.emit()
		assert_true(list.is_selected(0))


func test_pick_waits_for_a_new_live_row_instead_of_retaining_an_old_selection() -> void:
	var picks := DebugPickList.new()
	var fixture := _make_fixture(picks)
	var page := fixture.page
	var list := page.find_child("EntityList", true, false) as ItemList
	var saved_cards: Array = fixture.model.cards.duplicate(true)

	# The click can arrive one presentation beat before the picked actor's row.
	# Keep another row selected so a simple "select last pick only when empty"
	# implementation cannot accidentally pass.
	fixture.model.cards = []
	fixture.model.guard_present = false
	page.refresh()
	list.select(0)
	list.item_selected.emit(0)
	assert_string_contains(list.get_item_text(0), "ssn 77")
	picks.add(_guard_pick())
	assert_true(list.is_selected(0), "the old vehicle remains while the guard is absent")

	fixture.model.cards = saved_cards
	fixture.model.guard_present = true
	page.refresh()
	assert_true(list.is_selected(0),
			"the pending pick wins as soon as its guard row appears")
	assert_string_contains(list.get_item_text(0), "ssn 42")
	var detail := page.find_child("EntityDetail", true, false) as Label
	assert_string_contains(detail.text, "Live guard")


func test_removing_a_pending_pick_cancels_its_future_auto_selection() -> void:
	var picks := DebugPickList.new()
	var fixture := _make_fixture(picks)
	var page := fixture.page
	var saved_cards: Array = fixture.model.cards.duplicate(true)
	fixture.model.cards = []
	fixture.model.guard_present = false
	page.refresh()
	var list := page.find_child("EntityList", true, false) as ItemList

	picks.add(_vehicle_pick())
	picks.add(_guard_pick())
	assert_true(list.is_selected(0))
	picks.remove_at(1)
	assert_eq(picks.size(), 1, "the unrelated vehicle pick remains")

	fixture.model.cards = saved_cards
	fixture.model.guard_present = true
	page.refresh()
	var selected := list.get_selected_items()
	assert_eq(selected.size(), 1)
	assert_eq(int(selected[0]), 1,
			"removing the pending guard keeps the selected vehicle after rows reorder")
	assert_string_contains(list.get_item_text(selected[0]), "ssn 77")
	await wait_process_frames(2)


func test_position_editor_labels_axes_and_stays_compact() -> void:
	var page := _make_fixture().page
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
	var fixture := _make_fixture()
	var page := fixture.page
	var list := page.find_child("EntityList", true, false) as ItemList
	var health := page.find_child("SetEntityHealth", true, false) as Button
	var value := page.find_child("EntityHealthValue", true, false) as SpinBox
	list.item_selected.emit(0)
	value.value = 61

	# Reorder between the visible refresh and the click. The mutation performs
	# one last identity lookup, so row zero's new vehicle cannot receive it.
	fixture.model.vehicle_first = true
	health.pressed.emit()
	assert_eq(fixture.actions.health_calls, [{"index": 1, "health": 61}])

	# The next normal refresh also moves ItemList selection to the same guard's
	# new row rather than preserving a transient numeric discovery index.
	page.refresh()
	assert_true(list.is_selected(1))
	value.value = 62
	health.pressed.emit()
	assert_eq(fixture.actions.health_calls.back(), {"index": 1, "health": 62})


func test_refresh_preserves_a_staged_value_after_tabbing_to_its_apply_button() -> void:
	var page := _make_fixture().page
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
	var picks := DebugPickList.new()
	var fixture := _make_fixture(picks)
	fixture.model.cards.append({
		"name": "Client vehicle",
		"net_id": 77,
		"position": Vector3(20.0, 4.0, -40.0),
		"pool": 2,
		"wire_handle": 2001,
		"alive": true,
		"health": 400,
		"ai_health": 400,
	})
	var page := fixture.page
	page.refresh()
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
	assert_eq(fixture.actions.health_calls.back(), {"index": 2, "health": 400},
			"the focused editor cannot apply entity A's value to picked entity B")


func test_despawned_ai_pool_entries_are_explicit_and_not_editable() -> void:
	var page := _make_fixture().page
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
	var fixture := _make_fixture()
	var page := fixture.page
	var list := page.find_child("EntityList", true, false) as ItemList
	var health := page.find_child("SetEntityHealth", true, false) as Button
	var health_editor := page.find_child(
			"EntityEditHealth", true, false) as Control
	var status := page.find_child("EntityEditStatus", true, false) as Label
	list.item_selected.emit(0)
	health.pressed.emit()
	assert_eq(status.text, "Health updated.")

	fixture.model.cards[1]["pool"] = -1
	page.refresh()
	assert_false(health_editor.visible)
	assert_eq(status.text,
			"Read only: this AI entry has despawned from the world.",
			"a live policy change replaces stale mutation feedback")

	fixture.model.cards[1]["pool"] = 1
	page.refresh()
	assert_true(health_editor.visible)
	assert_eq(status.text, "",
			"restored editability clears the stale read-only policy")


func test_runtime_action_failure_always_has_visible_feedback() -> void:
	var fixture := _make_fixture()
	var page := fixture.page
	var list := page.find_child("EntityList", true, false) as ItemList
	list.item_selected.emit(0)
	fixture.actions.action_error = ERR_UNAVAILABLE

	var health := page.find_child("SetEntityHealth", true, false) as Button
	assert_false(health.disabled)
	health.pressed.emit()
	assert_eq(fixture.actions.health_calls[0]["index"], 1)

	var status := page.find_child("EntityEditStatus", true, false) as Label
	assert_false(status.text.strip_edges().is_empty())
	assert_string_contains(status.text, "Could not set entity health")
