extends GutTest

# Phase 1 bindings: NovaMissionData (libs/mission) + NovaItemDatabase (libs/def),
# and the entity item_id -> items.def graphic resolution chain that "populate the
# world" depends on. Uses committed fixtures (the items.def fixture is a small
# 12-item subset, so resolution is partial by design).

const BMS_PATH := "res://../fixtures/bms/ash_i5b.reference.bms"
const ITEMS_PATH := "res://../fixtures/def/items.def"


func _bms_abs() -> String:
	return ProjectSettings.globalize_path(BMS_PATH)


func _items_abs() -> String:
	return ProjectSettings.globalize_path(ITEMS_PATH)


func test_mission_data_parses_header_and_entities() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK, "ash_i5b.reference.bms should parse")
	assert_true(m.is_loaded(), "mission should report loaded")
	assert_eq(m.get_terrain_ref(), "dvxi5", "header terrain reference")
	assert_eq(m.get_environment_ref(), "full_00", "header environment reference")
	assert_false(m.get_mission_name().is_empty(), "mission name should be populated")

	var building_count := m.get_entity_count(NovaMissionData.KIND_BUILDING)
	assert_gt(building_count, 0, "ash_i5b places buildings")

	var buildings := m.get_entities(NovaMissionData.KIND_BUILDING)
	assert_eq(buildings.size(), building_count, "get_entities count matches get_entity_count")

	var first: Dictionary = buildings[0]
	assert_true(first.has("item_id"), "entity dict exposes item_id")
	assert_true(first.has("position"), "entity dict exposes position")
	assert_typeof(first["position"], TYPE_VECTOR3, "position is a Vector3")
	assert_typeof(first["rotation_deg"], TYPE_VECTOR3, "rotation_deg is a Vector3")

	var total := m.get_entity_count(NovaMissionData.KIND_ITEM) \
		+ m.get_entity_count(NovaMissionData.KIND_BUILDING) \
		+ m.get_entity_count(NovaMissionData.KIND_MARKER) \
		+ m.get_entity_count(NovaMissionData.KIND_ORGANIC)
	assert_eq(m.get_all_entities().size(), total, "get_all_entities aggregates every kind")


func test_item_database_loads_and_handles_missing() -> void:
	var db := NovaItemDatabase.new()
	assert_eq(db.load(_items_abs()), OK, "items.def fixture should parse")
	assert_true(db.is_loaded(), "database should report loaded")
	assert_gt(db.get_count(), 0, "items.def yields at least one item")

	assert_false(db.has_item(-99999), "unknown id is absent")
	assert_eq(db.get_graphic(-99999), "", "unknown id has no graphic")
	assert_eq(db.get_item(-99999), {}, "unknown id has empty record")


func test_entities_resolve_to_models() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var db := NovaItemDatabase.new()
	assert_eq(db.load(_items_abs()), OK)

	var resolved := 0
	for e in m.get_all_entities():
		var id: int = e["item_id"]
		if db.has_item(id) and not db.get_graphic(id).is_empty():
			resolved += 1
	assert_gt(resolved, 0, "the item_id -> items.def graphic chain resolves real placements")
