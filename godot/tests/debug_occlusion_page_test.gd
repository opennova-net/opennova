extends GutTest

# DebugOcclusionPage is a decision inspector: the frame summary answers what
# the occlusion pass did, while the selectable building list puts exceptional
# states first and explains the selected building in plain language.

const PageScript := preload("res://engine/debug/pages/debug_occlusion_page.gd")


class StubSim:
	extends RefCounted
	var occlusion: Dictionary = {}

	func get_occlusion_debug() -> Dictionary:
		return occlusion.duplicate(true)


class StubRuntime:
	extends Node
	var sim := StubSim.new()

	func get_sim() -> Object:
		return sim


func _make_page(runtime: StubRuntime) -> DebugOcclusionPage:
	var ctx := NovaDebugContext.new()
	ctx.options = NovaDebugOptionState.new()
	ctx.runtime_source = func(): return runtime
	var page: DebugOcclusionPage = PageScript.new()
	page.setup(ctx)
	add_child_autofree(page)
	return page


func _snapshot(buildings: Array, welds: Array = []) -> Dictionary:
	var drawn := 0
	var culled := 0
	var batched := 0
	var open_now := 0
	for building_v in buildings:
		var building: Dictionary = building_v
		if bool(building.get("batched", false)):
			batched += 1
			if bool(building.get("visible", false)):
				drawn += 1
			else:
				culled += 1
		if bool(building.get("open_flagged", false)):
			open_now += 1
	return {
		"active": true,
		"camera_indoors": true,
		"exterior_visible": false,
		"water_visible": false,
		"local_blink_flags": 0x2,
		"counts": {
			"instances": buildings.size(),
			"batched": batched,
			"visible": drawn,
			"toc_culled": culled,
			"slots": 7,
			"window_groups": 2,
			"viewthru_groups": 1,
			"welds": welds.size(),
			"culled_entities": 4,
		},
		"buildings": buildings,
		"welds": welds,
	}


func _building(
		bms_id: int,
		batched: bool,
		visible: bool,
		open_flagged: bool,
		mask: int,
		pos: Vector3 = Vector3.ZERO,
		records: int = 0,
		windows: int = 0,
		portals: int = 0,
		links: int = 0) -> Dictionary:
	return {
		"bms_id": bms_id,
		"pos": pos,
		"batched": batched,
		"visible": visible,
		"open_flagged": open_flagged,
		"mask": mask,
		"has_open": open_flagged,
		"has_windows": windows > 0,
		"has_links": links > 0,
		"records": records,
		"windows": windows,
		"portals": portals,
		"links": links,
	}


func test_attention_order_is_selectable_and_explains_the_selected_building() -> void:
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	runtime.sim.occlusion = _snapshot([
		_building(10, true, true, false, 0xB, Vector3(1, 2, 3), 6, 2, 1, 0),
		_building(20, false, false, false, 0xFFFFFFFF, Vector3(20, 0, 0)),
		_building(30, true, true, true, 0x7, Vector3(30, 0, 0), 3, 0, 1, 0),
		_building(40, true, false, false, 0, Vector3(40, 2, -5), 4, 1, 2, 1),
	])
	var page := _make_page(runtime)
	page.refresh()

	var list := page.find_child("OcclusionBuildings", true, false) as ItemList
	assert_eq(list.item_count, 4)
	assert_string_contains(list.get_item_text(0), "OPEN NOW")
	assert_string_contains(list.get_item_text(0), "#30")
	assert_string_contains(list.get_item_text(1), "CULLED")
	assert_string_contains(list.get_item_text(1), "#40")
	assert_string_contains(list.get_item_text(2), "OUT OF BATCH")
	assert_string_contains(list.get_item_text(2), "#20")
	assert_string_contains(list.get_item_text(3), "DRAWN")
	assert_string_contains(list.get_item_text(3), "#10")
	for index in range(list.item_count):
		assert_true(list.is_item_selectable(index), "every building row is inspectable")

	var detail := page.find_child("OcclusionBuildingDetail", true, false) as Label
	assert_string_contains(detail.text, "Building #30 - OPEN NOW · DRAWN",
			"open is a trait; it does not hide the building's render decision")
	list.select(1)
	list.item_selected.emit(1)
	assert_not_null(detail)
	if detail == null:
		return
	assert_string_contains(detail.text, "Building #40")
	assert_string_contains(detail.text, "CULLED")
	assert_string_contains(detail.text,
			"The occluder pass rejected this building after it entered the frame batch.")
	assert_string_contains(detail.text, "Position: 40.0, 2.0, -5.0")
	assert_string_contains(detail.text, "Visible sections: none")
	assert_string_contains(detail.text, "4 records")
	assert_string_contains(detail.text, "1 window")
	assert_string_contains(detail.text, "2 portals")
	assert_string_contains(detail.text, "1 welded link")

	var summary := page.find_child("OcclusionStatus", true, false) as Label
	assert_string_contains(summary.text, "Camera: INDOORS")
	assert_string_contains(summary.text, "2 drawn",
			"open-and-visible buildings still count as drawn")
	assert_string_contains(summary.text, "1 culled")
	assert_string_contains(summary.text, "1 out of batch")
	assert_string_contains(summary.text, "1 open now")


func test_selection_follows_bms_identity_across_reorder_and_state_change() -> void:
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	runtime.sim.occlusion = _snapshot([
		_building(10, true, true, false, 0x3),
		_building(20, true, false, false, 0),
		_building(30, true, true, true, 0x7),
	])
	var page := _make_page(runtime)
	page.refresh()
	var list := page.find_child("OcclusionBuildings", true, false) as ItemList
	var selected_index := -1
	for index in range(list.item_count):
		if int(list.get_item_metadata(index)) == 10:
			selected_index = index
			break
	assert_gte(selected_index, 0)
	if selected_index < 0:
		return
	list.select(selected_index)
	list.item_selected.emit(selected_index)

	# The same BMS building changes from DRAWN to CULLED, moving in the
	# attention sort while the sim also changes enumeration order.
	runtime.sim.occlusion = _snapshot([
		_building(30, true, true, true, 0x7),
		_building(20, true, true, false, 0x1),
		_building(10, true, false, false, 0x4),
	])
	page.refresh()

	var retained_items := list.get_selected_items()
	assert_eq(retained_items.size(), 1)
	if retained_items.is_empty():
		return
	var retained_index := retained_items[0]
	assert_eq(int(list.get_item_metadata(retained_index)), 10,
			"selection follows the building's stable BMS id, not its old row index")
	assert_string_contains(list.get_item_text(retained_index), "CULLED")
	var detail := page.find_child("OcclusionBuildingDetail", true, false) as Label
	assert_string_contains(detail.text, "Building #10")
	assert_string_contains(detail.text, "CULLED")
	assert_string_contains(detail.text, "Visible sections: 2")


func test_selected_detail_shows_only_welds_related_to_that_building() -> void:
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	runtime.sim.occlusion = _snapshot([
		_building(10, true, false, false, 0, Vector3.ZERO, 3, 0, 1, 2),
		_building(20, true, true, false, 0x1),
		_building(30, true, true, false, 0x1),
	], [
		{"own_bms": 10, "own_section": 1, "other_bms": 20, "other_section": 2},
		{"own_bms": 30, "own_section": 3, "other_bms": 10, "other_section": 4},
		{"own_bms": 20, "own_section": 5, "other_bms": 30, "other_section": 6},
	])
	var page := _make_page(runtime)
	page.refresh()
	var list := page.find_child("OcclusionBuildings", true, false) as ItemList
	assert_eq(list.item_count, 3, "welds no longer masquerade as building rows")
	var building_index := -1
	for index in range(list.item_count):
		if int(list.get_item_metadata(index)) == 10:
			building_index = index
			break
	assert_gte(building_index, 0)
	if building_index < 0:
		return
	list.select(building_index)
	list.item_selected.emit(building_index)

	var detail := page.find_child("OcclusionBuildingDetail", true, false) as Label
	assert_string_contains(detail.text, "Welded connections: 2")
	assert_string_contains(detail.text, "Section 1 -> building #20 section 2")
	assert_string_contains(detail.text, "Section 4 -> building #30 section 3")
	assert_false(detail.text.contains("section 5"),
			"connections unrelated to BMS 10 stay out of its detail")
	assert_false(detail.text.contains("section 6"),
			"connections unrelated to BMS 10 stay out of its detail")


func test_zero_id_buildings_keep_distinct_details_and_do_not_merge_welds() -> void:
	var runtime := StubRuntime.new()
	add_child_autofree(runtime)
	runtime.sim.occlusion = _snapshot([
		_building(0, true, false, false, 0, Vector3(10, 0, 0), 2, 1, 0, 1),
		_building(0, true, true, false, 0x4, Vector3(20, 0, 0), 5, 0, 2, 1),
	], [
		{"own_bms": 0, "own_section": 1, "other_bms": 0, "other_section": 2},
	])
	var page := _make_page(runtime)
	page.refresh()
	var list := page.find_child("OcclusionBuildings", true, false) as ItemList
	var detail := page.find_child("OcclusionBuildingDetail", true, false) as Label
	assert_eq(list.item_count, 2)
	assert_string_contains(detail.text, "Position: 10.0, 0.0, 0.0",
			"the first zero-ID row resolves its own detail")

	# Attention order puts the culled building first, so the drawn building at
	# x=20 is the second zero-ID row.
	list.select(1)
	list.item_selected.emit(1)
	assert_string_contains(detail.text, "Position: 20.0, 0.0, 0.0",
			"zero-ID rows resolve their own detail instead of the final zero-ID row")
	assert_string_contains(detail.text, "5 records")
	assert_string_contains(detail.text, "unavailable",
			"the snapshot cannot safely attribute BMS-only welds to one zero-ID building")

	runtime.sim.occlusion = _snapshot([
		_building(0, true, true, false, 0x4, Vector3(20, 0, 0), 5, 0, 2, 1),
		_building(0, false, false, false, 0, Vector3(10, 0, 0), 2, 1, 0, 1),
	])
	page.refresh()
	assert_string_contains(detail.text, "Position: 20.0, 0.0, 0.0",
			"selection follows a zero-ID building across source and attention reorder")
