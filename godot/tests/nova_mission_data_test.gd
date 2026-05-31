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


func test_get_entity_matches_the_scanned_entry() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var first: Dictionary = m.get_entities(NovaMissionData.KIND_BUILDING)[0]
	var index := int(first["index"])
	var direct := m.get_entity(NovaMissionData.KIND_BUILDING, index)
	assert_eq(int(direct.get("index", -1)), index, "get_entity returns the entity at that index")
	assert_eq(direct.get("position"), first.get("position"), "with the same position as the scanned entry")
	assert_eq(int(direct.get("item_id", -1)), int(first.get("item_id", -2)), "and the same item_id")
	assert_eq(m.get_entity(NovaMissionData.KIND_BUILDING, 999999), {}, "an out-of-range index yields an empty dict")
	assert_eq(m.get_entity(NovaMissionData.KIND_BUILDING, -1), {}, "a negative index yields an empty dict")


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


# --- Authoring (Phase 1): mutate + save round-trip ---------------------------
# These assert the write-back spine at the GDScript boundary. Byte-fidelity of
# unedited regions is already proven at the lib level (tests/mission/*), so here we
# assert field-level round-trip: an edited entity persists its new transform and an
# untouched neighbor is unchanged. All saves target a throwaway user:// path so the
# committed fixture is never overwritten.

func _temp_bms_path() -> String:
	return ProjectSettings.globalize_path("user://mission_authoring_rt_%d.bms" % Time.get_ticks_usec())


func test_set_entity_transform_persists_through_save_reload() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.is_modified(), "a freshly opened mission is not modified")

	var buildings := m.get_entities(NovaMissionData.KIND_BUILDING)
	assert_gt(buildings.size(), 1, "need at least two buildings to check a neighbor")
	var target: Dictionary = buildings[0]
	var neighbor_before: Vector3 = buildings[1]["position"]
	var kind := int(target["kind"])
	var index := int(target["index"])
	var rotation: Vector3 = target["rotation_deg"]
	var new_pos := Vector3(123.0, 45.0, -67.0)

	assert_true(m.set_entity_transform(kind, index, new_pos, rotation), "moving a valid entity succeeds")
	assert_true(m.is_modified(), "a mutation sets the modified flag")

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "save_as writes the edited mission")
	assert_false(m.is_modified(), "a successful save clears the modified flag")

	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(tmp), OK, "the saved mission reopens")
	var b2 := reopened.get_entities(NovaMissionData.KIND_BUILDING)
	assert_eq(b2.size(), buildings.size(), "entity count is unchanged by an in-place move")

	var moved: Vector3 = b2[0]["position"]
	assert_almost_eq(moved.x, new_pos.x, 0.02, "moved entity keeps its new X")
	assert_almost_eq(moved.y, new_pos.y, 0.02, "moved entity keeps its new Y")
	assert_almost_eq(moved.z, new_pos.z, 0.02, "moved entity keeps its new Z")

	var neighbor_after: Vector3 = b2[1]["position"]
	assert_almost_eq(neighbor_after.x, neighbor_before.x, 0.02, "untouched neighbor X is unchanged")
	assert_almost_eq(neighbor_after.y, neighbor_before.y, 0.02, "untouched neighbor Y is unchanged")
	assert_almost_eq(neighbor_after.z, neighbor_before.z, 0.02, "untouched neighbor Z is unchanged")

	DirAccess.remove_absolute(tmp)


func test_save_without_edits_is_non_destructive() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entities(NovaMissionData.KIND_BUILDING)

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "saving an unedited mission succeeds")

	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(tmp), OK)
	var after := reopened.get_entities(NovaMissionData.KIND_BUILDING)
	assert_eq(after.size(), before.size(), "save->reload preserves the entity set")
	for i in before.size():
		var p0: Vector3 = before[i]["position"]
		var p1: Vector3 = after[i]["position"]
		assert_almost_eq(p1.x, p0.x, 0.02, "building %d X round-trips" % i)
		assert_almost_eq(p1.y, p0.y, 0.02, "building %d Y round-trips" % i)
		assert_almost_eq(p1.z, p0.z, 0.02, "building %d Z round-trips" % i)

	DirAccess.remove_absolute(tmp)


func test_set_entity_transform_rejects_out_of_range() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.set_entity_transform(NovaMissionData.KIND_BUILDING, 999999, Vector3.ZERO, Vector3.ZERO),
		"an out-of-range index is rejected")
	assert_false(m.set_entity_transform(NovaMissionData.KIND_BUILDING, -1, Vector3.ZERO, Vector3.ZERO),
		"a negative index is rejected")
	assert_false(m.is_modified(), "a rejected mutation does not dirty the document")


func test_save_file_without_path_is_invalid() -> void:
	# A document constructed without open_file has no source path; save_file must
	# return the no-path code (the editor shell then routes to Save As).
	var m := NovaMissionData.new()
	assert_eq(m.save_file(), ERR_INVALID_PARAMETER, "save_file with no path is ERR_INVALID_PARAMETER")


# --- Authoring (Phase 2): edit team / group ----------------------------------
# set_entity_property_int seeds the lib's all-fields property setter from the entity's
# current state and changes only the named field, so editing team must leave group and
# every other AI property untouched (the regression guard for the 13-field overwrite).

# Fields the entity dictionary carries beyond team / group, asserted unchanged when
# only one of team / group is edited.
const _OTHER_PROPERTY_KEYS := [
	"waypoint_id", "wp_number", "ai_flags", "perception", "accuracy", "alert_state",
	"min_engagement_distance", "max_engagement_distance", "max_attack_distance",
	"spawn_count", "max_simultaneous",
]


func _entity(m: NovaMissionData, kind: int, index: int) -> Dictionary:
	for e in m.get_entities(kind):
		if int((e as Dictionary)["index"]) == index:
			return e
	return {}


func test_set_entity_property_int_team_preserves_other_fields() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before: Dictionary = m.get_entities(NovaMissionData.KIND_BUILDING)[0]
	var index := int(before["index"])

	var new_team := int(before.get("team", 0)) + 1
	assert_true(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, index, "team", new_team),
		"editing team on a valid entity succeeds")
	assert_true(m.is_modified(), "a property edit sets the modified flag")

	var after := _entity(m, NovaMissionData.KIND_BUILDING, index)
	assert_eq(int(after["team"]), new_team, "team takes the new value")
	assert_eq(int(after["group"]), int(before["group"]), "group is left untouched by a team edit")
	for key in _OTHER_PROPERTY_KEYS:
		assert_eq(after[key], before[key], "%s is preserved through a team-only edit" % key)


func test_set_entity_property_int_group_roundtrips() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before: Dictionary = m.get_entities(NovaMissionData.KIND_BUILDING)[0]
	var index := int(before["index"])

	var new_group := int(before.get("group", 0)) + 3
	assert_true(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, index, "group", new_group))
	var after := _entity(m, NovaMissionData.KIND_BUILDING, index)
	assert_eq(int(after["group"]), new_group, "group takes the new value")
	assert_eq(int(after["team"]), int(before["team"]), "team is left untouched by a group edit")


func test_set_entity_property_int_persists_through_save_reload() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var index := int(m.get_entities(NovaMissionData.KIND_BUILDING)[0]["index"])
	assert_true(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, index, "team", 4))

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(tmp), OK)
	assert_eq(int(_entity(reopened, NovaMissionData.KIND_BUILDING, index)["team"]), 4,
		"an edited team survives the byte-faithful save and reload")
	DirAccess.remove_absolute(tmp)


func test_set_entity_property_int_unknown_property_is_inert() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, 0, "perception", 9),
		"a property the editor does not expose is rejected")
	assert_false(m.is_modified(), "a rejected property edit does not dirty the document")


func test_set_entity_property_int_rejects_out_of_range() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, 999999, "team", 1),
		"an out-of-range index is rejected")
	assert_false(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, -1, "team", 1),
		"a negative index is rejected")
	assert_false(m.is_modified(), "a rejected edit leaves the document clean")


func test_set_entity_property_int_preserves_other_fields_across_kinds() -> void:
	# Buildings tend to carry all-zero AI fields, so a dropped field in the read-modify-
	# write copy would read 0 == 0 and pass. Organics (and items) carry richer state, so
	# exercise every selectable kind that the fixture provides.
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var covered := 0
	for kind in [NovaMissionData.KIND_ITEM, NovaMissionData.KIND_ORGANIC]:
		var entities := m.get_entities(kind)
		if entities.is_empty():
			continue
		covered += 1
		var before: Dictionary = entities[0]
		var index := int(before["index"])
		assert_true(m.set_entity_property_int(kind, index, "group", int(before.get("group", 0)) + 1),
			"editing group on kind %d succeeds" % kind)
		var after := _entity(m, kind, index)
		assert_eq(int(after["group"]), int(before["group"]) + 1, "group updates on kind %d" % kind)
		assert_eq(int(after["team"]), int(before["team"]), "team untouched on kind %d" % kind)
		for key in _OTHER_PROPERTY_KEYS:
			assert_eq(after[key], before[key], "%s preserved on kind %d" % [key, kind])
	# The fixture is expected to place items and/or organics; flag if neither resolved so
	# this guard never silently degrades to a no-op.
	assert_gt(covered, 0, "the fixture provides at least one item or organic to exercise")
