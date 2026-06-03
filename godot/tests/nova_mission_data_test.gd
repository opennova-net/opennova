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
	assert_false(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, 0, "not_a_real_property", 9),
		"a property the binding does not map is rejected")
	assert_false(m.is_modified(), "a rejected property edit does not dirty the document")


# The generalized setter exposes the AI + waypoint fields (not just team / group). Each
# must write its own field and read back, leaving every other field untouched -- the same
# read-modify-write contract the team test pins, extended across the full editable set.
const _BEHAVIOR_PROPERTY_KEYS := [
	"waypoint_id", "wp_number", "perception", "accuracy", "alert_state",
	"min_engagement_distance", "max_engagement_distance", "max_attack_distance",
	"spawn_count", "max_simultaneous", "ai_flags",
]


func _kind_with_entities(m: NovaMissionData) -> int:
	# Prefer an organic (richest AI fields), then item, then building (the fixture always
	# has buildings), so the round-trip exercises real non-zero state where it exists.
	for kind in [NovaMissionData.KIND_ORGANIC, NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING]:
		if m.get_entity_count(kind) > 0:
			return kind
	return NovaMissionData.KIND_BUILDING


func test_set_entity_property_int_supports_behavior_fields() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var kind := _kind_with_entities(m)
	for prop in _BEHAVIOR_PROPERTY_KEYS:
		var before: Dictionary = m.get_entities(kind)[0]
		var index := int(before["index"])
		var target := int(before.get(prop, 0)) + 1
		assert_true(m.set_entity_property_int(kind, index, prop, target),
			"%s is an editable property" % prop)
		var after := _entity(m, kind, index)
		assert_eq(int(after[prop]), target, "%s takes its new value" % prop)
		# Every OTHER editable field is preserved by this single-field write.
		assert_eq(int(after["team"]), int(before["team"]), "team preserved through a %s edit" % prop)
		assert_eq(int(after["group"]), int(before["group"]), "group preserved through a %s edit" % prop)
		for other in _BEHAVIOR_PROPERTY_KEYS:
			if other != prop:
				assert_eq(int(after[other]), int(before[other]), "%s preserved through a %s edit" % [other, prop])


func test_set_entity_property_int_behavior_field_persists_through_save_reload() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var kind := _kind_with_entities(m)
	var index := int(m.get_entities(kind)[0]["index"])
	assert_true(m.set_entity_property_int(kind, index, "waypoint_id", 5),
		"a unit can be assigned to a waypoint path")

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(tmp), OK)
	assert_eq(int(_entity(reopened, kind, index)["waypoint_id"]), 5,
		"the waypoint assignment survives the byte-faithful save and reload")
	DirAccess.remove_absolute(tmp)


func test_set_entity_property_int_rejects_out_of_range() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, 999999, "team", 1),
		"an out-of-range index is rejected")
	assert_false(m.set_entity_property_int(NovaMissionData.KIND_BUILDING, -1, "team", 1),
		"a negative index is rejected")
	assert_false(m.is_modified(), "a rejected edit leaves the document clean")


# --- Authoring (Phase 3): place a new entity + item enumeration --------------
# add_entity appends a new record of a kind for an items.def id and returns it; the
# binding is the write side the editor's place-object palette drives. NovaItemDatabase
# enumeration (get_items / get_item_ids) is the read side that fills the palette.

func test_add_entity_appends_and_returns_the_new_record() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_count(NovaMissionData.KIND_BUILDING)
	assert_false(m.is_modified(), "a freshly opened mission is not modified")

	var pos := Vector3(111.0, 22.0, -33.0)
	var record := m.add_entity(NovaMissionData.KIND_BUILDING, 102001, pos, Vector3.ZERO)
	assert_false(record.is_empty(), "add_entity returns the new record")
	assert_true(m.is_modified(), "adding an entity dirties the document")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before + 1, "the kind's count grows by one")
	assert_eq(int(record["index"]), before, "the new entity is appended at the end of its list")
	assert_eq(int(record["item_id"]), 102001, "the record carries the placed item id")
	assert_eq(int(record["kind"]), NovaMissionData.KIND_BUILDING, "the record carries the requested kind")

	var fetched := m.get_entity(NovaMissionData.KIND_BUILDING, before)
	assert_eq(int(fetched.get("item_id", -1)), 102001, "get_entity finds the appended entity")
	var fp: Vector3 = fetched["position"]
	assert_almost_eq(fp.x, pos.x, 0.02, "stored X matches what was placed")
	assert_almost_eq(fp.y, pos.y, 0.02, "stored Y matches what was placed")
	assert_almost_eq(fp.z, pos.z, 0.02, "stored Z matches what was placed")


func test_add_entity_persists_through_save_reload() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_count(NovaMissionData.KIND_ITEM)
	var record := m.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3(7.0, 8.0, 9.0), Vector3(0, 90, 0))
	var new_index := int(record["index"])

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "the mission with a new entity saves")
	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(tmp), OK, "and reopens")
	assert_eq(reopened.get_entity_count(NovaMissionData.KIND_ITEM), before + 1, "the added entity survives save+reload")
	var roundtripped := reopened.get_entity(NovaMissionData.KIND_ITEM, new_index)
	assert_eq(int(roundtripped.get("item_id", -1)), 101291, "the placed item id round-trips")
	var rot: Vector3 = roundtripped["rotation_deg"]
	assert_almost_eq(rot.y, 90.0, 0.5, "the placed yaw round-trips (rounded to integer degrees)")
	DirAccess.remove_absolute(tmp)


func test_add_entity_without_a_mission_is_rejected() -> void:
	var m := NovaMissionData.new()  # never opened -> no document loaded
	assert_eq(m.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO), {},
		"adding to an unloaded mission returns an empty dict")
	assert_false(m.is_modified(), "a rejected add does not dirty the document")


func test_item_database_enumeration_is_sorted_and_complete() -> void:
	var db := NovaItemDatabase.new()
	assert_eq(db.load(_items_abs()), OK)
	var items := db.get_items()
	var ids := db.get_item_ids()
	assert_eq(items.size(), db.get_count(), "get_items returns every item")
	assert_eq(ids.size(), db.get_count(), "get_item_ids returns every id")

	# Stable order: sorted with Godot's natural, case-insensitive comparator, matching
	# NovaItemDatabase::sorted_items (so the assertion can actually catch a sort
	# regression, not just lexicographic ordering that happens to coincide).
	for i in range(1, items.size()):
		var prev := String(items[i - 1]["display_name"])
		var cur := String(items[i]["display_name"])
		assert_true(prev.naturalnocasecmp_to(cur) <= 0,
			"items are ordered by natural display name (%s <= %s)" % [prev, cur])

	# get_item_ids() must enumerate in the same order as get_items().
	for i in items.size():
		assert_eq(int(ids[i]), int(items[i]["id"]), "get_item_ids matches get_items order at row %d" % i)

	# Each enumerated entry matches the single-id getter.
	var sample: Dictionary = items[0]
	var direct := db.get_item(int(sample["id"]))
	assert_eq(direct, sample, "an enumerated item matches get_item for its id")
	assert_true(sample.has("graphic") and sample.has("type"), "enumerated items carry graphic + type")


# --- Authoring (Phase 4): remove an entity -----------------------------------
# remove_entity erases an entity from its kind's list; every later entity of that kind
# shifts down one index (std::vector::erase semantics, proven byte-faithful at the lib
# level). The binding is the write side the editor's delete action drives.

func test_remove_entity_drops_count_and_reindexes() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.is_modified(), "a freshly opened mission is not modified")
	var buildings := m.get_entities(NovaMissionData.KIND_BUILDING)
	assert_gt(buildings.size(), 2, "need a few buildings to check reindexing")

	# Capture the entity at index 1: after removing index 0 it must shift down to index 0,
	# carrying its own data (so the removal is not just a truncation of the last element).
	var was_at_1_item: int = int(buildings[1]["item_id"])
	var was_at_1_pos: Vector3 = buildings[1]["position"]

	assert_true(m.remove_entity(NovaMissionData.KIND_BUILDING, 0), "removing a valid entity succeeds")
	assert_true(m.is_modified(), "a removal dirties the document")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), buildings.size() - 1, "the kind's count drops by one")

	var new_first := m.get_entity(NovaMissionData.KIND_BUILDING, 0)
	assert_eq(int(new_first["item_id"]), was_at_1_item, "the entity at index 1 shifted down into index 0")
	var np: Vector3 = new_first["position"]
	assert_almost_eq(np.x, was_at_1_pos.x, 0.02, "the shifted entity kept its X position")
	assert_almost_eq(np.z, was_at_1_pos.z, 0.02, "the shifted entity kept its Z position")


func test_remove_entity_persists_through_save_reload() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_count(NovaMissionData.KIND_BUILDING)
	var other_kind_before := m.get_entity_count(NovaMissionData.KIND_ITEM)
	assert_true(m.remove_entity(NovaMissionData.KIND_BUILDING, 0))

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "the mission with a removed entity saves")
	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(tmp), OK, "and reopens")
	assert_eq(reopened.get_entity_count(NovaMissionData.KIND_BUILDING), before - 1, "the removal survives save+reload")
	assert_eq(reopened.get_entity_count(NovaMissionData.KIND_ITEM), other_kind_before, "removing a building leaves the item list untouched")
	DirAccess.remove_absolute(tmp)


func test_remove_entity_rejects_out_of_range() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.remove_entity(NovaMissionData.KIND_BUILDING, 999999), "an out-of-range index is rejected")
	assert_false(m.remove_entity(NovaMissionData.KIND_BUILDING, -1), "a negative index is rejected")
	assert_false(m.is_modified(), "a rejected removal does not dirty the document")


func test_remove_entity_without_a_mission_is_rejected() -> void:
	var m := NovaMissionData.new()  # never opened -> no document loaded
	assert_false(m.remove_entity(NovaMissionData.KIND_BUILDING, 0), "removing from an unloaded mission fails")
	assert_false(m.is_modified(), "a rejected removal does not dirty the document")


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


# --- Waypoints (P7): paths + markers -----------------------------------------
# A mission carries 128 fixed waypoint paths; a path is an ordered list of marker
# indices (into the KIND_MARKER entity list) plus flags. The lib round-trips all of it;
# these assert the GDScript boundary: summaries enumerate, add_waypoint_marker creates a
# marker AND links it, set/clear rewrite the references, and it all survives save+reload.

func _first_empty_waypoint_path(m: NovaMissionData) -> int:
	for s in m.get_waypoint_summaries():
		if int((s as Dictionary)["marker_count"]) == 0:
			return int((s as Dictionary)["index"])
	return -1


func _any_marker_item_id(m: NovaMissionData) -> int:
	# Reuse a real marker's item id when the fixture has markers; otherwise any id works
	# (the lib stores it without validating against items.def).
	var markers := m.get_entities(NovaMissionData.KIND_MARKER)
	return int(markers[0]["item_id"]) if not markers.is_empty() else 100001


func test_get_waypoint_summaries_enumerates_all_paths() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var summaries := m.get_waypoint_summaries()
	assert_eq(summaries.size(), 128, "a mission has 128 fixed waypoint records")
	for s in summaries:
		assert_true((s as Dictionary).has("index") and (s as Dictionary).has("marker_count"),
			"each summary carries index + marker_count")


func test_add_waypoint_marker_creates_marker_and_links_it() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var path_index := _first_empty_waypoint_path(m)
	assert_true(path_index >= 0, "the fixture has at least one empty waypoint path to author into")
	var markers_before := m.get_entity_count(NovaMissionData.KIND_MARKER)
	assert_false(m.is_modified(), "a freshly opened mission is not modified")

	var result := m.add_waypoint_marker(path_index, _any_marker_item_id(m), Vector3(5, 1, -5), Vector3.ZERO, -1)
	assert_false(result.is_empty(), "add_waypoint_marker returns the new marker + path")
	assert_true(result.has("marker") and result.has("path"), "the result carries both halves")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_MARKER), markers_before + 1,
		"one marker entity was created (no separate add_entity needed)")
	assert_eq(int((result["path"] as Dictionary)["marker_count"]), 1, "the path now references one marker")
	assert_true(m.is_modified(), "authoring a marker dirties the document")


func test_add_waypoint_marker_round_trips_through_save_reload() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var path_index := _first_empty_waypoint_path(m)
	assert_true(path_index >= 0)
	m.add_waypoint_marker(path_index, _any_marker_item_id(m), Vector3(5, 1, -5), Vector3.ZERO, -1)

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(tmp), OK)
	assert_eq(int(reopened.get_waypoint_path(path_index)["marker_count"]), 1,
		"the authored marker survives save+reload")
	DirAccess.remove_absolute(tmp)


func test_set_waypoint_path_reorders_and_flags() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var path_index := _first_empty_waypoint_path(m)
	assert_true(path_index >= 0)
	var mid := _any_marker_item_id(m)
	m.add_waypoint_marker(path_index, mid, Vector3(1, 0, -1), Vector3.ZERO, -1)
	var second := m.add_waypoint_marker(path_index, mid, Vector3(2, 0, -2), Vector3.ZERO, -1)
	var indices: PackedInt32Array = (second["path"] as Dictionary)["marker_indices"]
	assert_eq(indices.size(), 2, "the path has two markers to reorder")

	var reversed := PackedInt32Array([indices[1], indices[0]])
	assert_true(m.set_waypoint_path(path_index, reversed, NovaMissionData.WP_FLAG_DOES_NOT_LOOP),
		"set_waypoint_path accepts a reordered list + flags")
	var after := m.get_waypoint_path(path_index)
	var after_indices: PackedInt32Array = after["marker_indices"]
	assert_eq(after_indices[0], indices[1], "the order was reversed")
	assert_eq(int(after["flags"]) & NovaMissionData.WP_FLAG_DOES_NOT_LOOP, NovaMissionData.WP_FLAG_DOES_NOT_LOOP,
		"the DoesNotLoop flag was set")
	assert_eq(int(after["marker_count"]), 2, "reorder does not change the marker count")


func test_clear_waypoint_path_empties_it() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var path_index := _first_empty_waypoint_path(m)
	assert_true(path_index >= 0)
	m.add_waypoint_marker(path_index, _any_marker_item_id(m), Vector3(1, 0, -1), Vector3.ZERO, -1)
	assert_eq(int(m.get_waypoint_path(path_index)["marker_count"]), 1, "precondition: the path has a marker")

	assert_true(m.clear_waypoint_path(path_index), "clearing a path succeeds")
	assert_eq(int(m.get_waypoint_path(path_index)["marker_count"]), 0, "the path is now empty")


func test_waypoint_methods_reject_out_of_range_paths() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_eq(m.get_waypoint_path(128), {}, "path index 128 is out of range (0..127)")
	assert_eq(m.get_waypoint_path(-1), {}, "a negative path index is out of range")
	assert_false(m.set_waypoint_path(128, PackedInt32Array(), 0), "set rejects an out-of-range path")
	assert_false(m.clear_waypoint_path(-1), "clear rejects a negative path")
	assert_eq(m.add_waypoint_marker(-1, _any_marker_item_id(m), Vector3.ZERO, Vector3.ZERO, -1), {},
		"add_waypoint_marker rejects a negative path")


# --- Authoring (Phase 5): snapshot / restore (undo/redo spine) ----------------
# snapshot() serializes the whole document through the same byte-faithful writer as
# save (write_bms_bytes), and restore_snapshot() re-parses it (load_bms_bytes). The
# editor's undo stack holds these re-serialized states; restoring one rewinds the
# whole document without touching the filesystem. The byte fidelity itself is proven
# at the lib level (tests/mission/mission_bms_test.cpp); here we assert the GDScript
# boundary: a snapshot round-trips, captures the live (not the on-disk) state, and
# fails cleanly on garbage.

func test_snapshot_restore_rewinds_a_mutation() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_count(NovaMissionData.KIND_BUILDING)
	var clean := m.snapshot()
	assert_false(clean.is_empty(), "a loaded mission snapshots to non-empty bytes")

	# Mutate, then restore the pre-mutation snapshot: the added entity must be gone.
	m.add_entity(NovaMissionData.KIND_BUILDING, 102001, Vector3(1, 2, 3), Vector3.ZERO)
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before + 1, "the placement landed")
	assert_true(m.restore_snapshot(clean), "restoring a valid snapshot succeeds")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before, "restore rewinds the placement")
	assert_true(m.is_loaded(), "the document is still loaded after a restore")


func test_snapshot_captures_the_live_state_not_the_file() -> void:
	# The snapshot is a re-serialization of the current in-memory document, not the bytes
	# opened from disk. So a snapshot taken after one edit, restored after a second edit,
	# must land on the first-edit state (proving no raw-input passthrough).
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var index := int(m.get_entities(NovaMissionData.KIND_BUILDING)[0]["index"])
	var rotation: Vector3 = m.get_entity(NovaMissionData.KIND_BUILDING, index)["rotation_deg"]

	m.set_entity_transform(NovaMissionData.KIND_BUILDING, index, Vector3(10, 20, 30), rotation)
	var after_first := m.snapshot()
	m.set_entity_transform(NovaMissionData.KIND_BUILDING, index, Vector3(99, 88, 77), rotation)
	assert_true(m.restore_snapshot(after_first), "restoring the first-edit snapshot succeeds")

	var restored: Vector3 = m.get_entity(NovaMissionData.KIND_BUILDING, index)["position"]
	assert_almost_eq(restored.x, 10.0, 0.02, "restore lands on the first edit's X, not the second")
	assert_almost_eq(restored.y, 20.0, 0.02, "restore lands on the first edit's Y")
	assert_almost_eq(restored.z, 30.0, 0.02, "restore lands on the first edit's Z")


func test_restore_snapshot_leaves_the_dirty_flag_to_the_caller() -> void:
	# The editor owns its own dirty state; restore must not clear modified (an undo can
	# leave the document dirty relative to disk).
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var clean := m.snapshot()
	m.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO)
	assert_true(m.is_modified(), "the placement set the dirty flag")
	assert_true(m.restore_snapshot(clean), "restore succeeds")
	assert_true(m.is_modified(), "restore does not touch the dirty flag (the caller recomputes it)")


func test_snapshot_without_a_mission_is_empty() -> void:
	var m := NovaMissionData.new()  # never opened -> nothing to serialize
	assert_true(m.snapshot().is_empty(), "snapshot of an unloaded mission is an empty array")


func test_restore_snapshot_of_garbage_returns_false() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var garbage := PackedByteArray([0, 1, 2, 3, 4, 5, 6, 7])
	assert_false(m.restore_snapshot(garbage), "restoring unparseable bytes returns false")


# --- Phase 1: hidden entity fields + mission-header editing --------------------

func test_entity_dictionary_exposes_hidden_fields() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var e: Dictionary = m.get_entities(NovaMissionData.KIND_BUILDING)[0]
	assert_true(e.has("no_less_than"), "dict carries no_less_than (byte 75)")
	assert_true(e.has("map_symbol"), "dict carries map_symbol (byte 81)")
	assert_true(e.has("name1"), "dict carries name1 (AI class)")
	assert_true(e.has("name2"), "dict carries name2 (AI script)")
	assert_true(e.has("max_simultaneous"), "no_more_than stays exposed as max_simultaneous (byte 74)")


func test_set_entity_property_string_round_trips_through_save_reload() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var target: Dictionary = m.get_entities(NovaMissionData.KIND_BUILDING)[0]
	var kind := int(target["kind"])
	var index := int(target["index"])
	assert_true(m.set_entity_property_string(kind, index, "name1", "rifle"), "name1 write succeeds")
	assert_true(m.set_entity_property_string(kind, index, "name2", "patrol"), "name2 write succeeds")
	assert_false(m.set_entity_property_string(kind, index, "bogus", "x"), "unknown string property rejected")
	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var r := NovaMissionData.new()
	assert_eq(r.open_file(tmp), OK)
	var e2: Dictionary = r.get_entity(kind, index)
	assert_eq(String(e2["name1"]), "rifle", "name1 survives save/reload")
	assert_eq(String(e2["name2"]), "patrol", "name2 survives save/reload")


func test_set_hidden_int_fields_round_trip() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var target: Dictionary = m.get_entities(NovaMissionData.KIND_BUILDING)[0]
	var kind := int(target["kind"])
	var index := int(target["index"])
	assert_true(m.set_entity_property_int(kind, index, "no_less_than", 9))
	assert_true(m.set_entity_property_int(kind, index, "map_symbol", 17))
	var e2: Dictionary = m.get_entity(kind, index)
	assert_eq(int(e2["no_less_than"]), 9, "no_less_than reads back")
	assert_eq(int(e2["map_symbol"]), 17, "map_symbol reads back")


func test_set_header_string_and_int_round_trip() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_true(m.set_header_string("mission_name", "Grill Test"), "name set")
	assert_true(m.set_header_int("climate", 2), "climate set")
	assert_false(m.set_header_string("bogus_field", "x"), "unknown header field rejected")
	assert_true(m.is_modified(), "a header edit dirties the mission")
	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var r := NovaMissionData.new()
	assert_eq(r.open_file(tmp), OK)
	var info := r.get_info()
	assert_eq(String(info["mission_name"]), "Grill Test", "mission_name survives reload")
	assert_eq(int(info["climate"]), 2, "climate survives reload")


func test_set_header_flag_toggles_one_bit_and_preserves_others() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := int(m.get_info()["attrib_flags"])
	assert_true(m.set_header_flag(NovaMissionData.ATTRIB_COOP, true), "set COOP")
	var after := int(m.get_info()["attrib_flags"])
	assert_eq(after & NovaMissionData.ATTRIB_COOP, NovaMissionData.ATTRIB_COOP, "COOP bit is set")
	var other_mask := ~NovaMissionData.ATTRIB_COOP
	assert_eq(after & other_mask, before & other_mask, "other attrib bits are preserved")


# --- Phase 2: area-trigger / zone binding -------------------------------------

func test_area_trigger_dictionary_shape() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_eq(m.get_area_triggers().size(), m.get_area_trigger_count(), "list size matches count")
	# Add one so the shape is exercised even if the fixture carries none.
	var z := m.add_area_trigger(Vector3(-5, -6, -7), Vector3(5, 6, 7), true, false, 3)
	assert_false(z.is_empty(), "add returns the new zone dict")
	for key in ["index", "id", "min", "max", "active", "constrain_z", "raw_flags"]:
		assert_true(z.has(key), "zone dict exposes %s" % key)
	assert_typeof(z["min"], TYPE_VECTOR3, "min is a Vector3")
	assert_typeof(z["max"], TYPE_VECTOR3, "max is a Vector3")
	assert_eq(int(z["id"]), 3, "zone id carried through")
	assert_true(bool(z["active"]), "active flag set")
	assert_false(bool(z["constrain_z"]), "constrain_z flag clear")


func test_area_trigger_add_set_remove_round_trip() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var base := m.get_area_trigger_count()
	# Crossed corners must be normalized to min<=max (the engine does not auto-swap).
	var z := m.add_area_trigger(Vector3(10, 20, 8), Vector3(-10, -20, -8), true, true, 0)
	assert_eq(m.get_area_trigger_count(), base + 1, "count grew by one")
	var idx := int(z["index"])
	assert_eq((z["min"] as Vector3), Vector3(-10, -20, -8), "min normalized to the lower corner")
	assert_eq((z["max"] as Vector3), Vector3(10, 20, 8), "max normalized to the upper corner")
	assert_true(m.is_modified(), "adding a zone dirties the mission")
	# Edit it: move max, drop constrain_z.
	var z2 := m.set_area_trigger(idx, Vector3(-10, -20, -8), Vector3(30, 20, 8), true, false, 0)
	assert_false(z2.is_empty(), "set returns the updated dict")
	assert_eq((z2["max"] as Vector3).x, 30.0, "max_x updated")
	assert_false(bool(z2["constrain_z"]), "constrain_z cleared")
	assert_true(bool(z2["active"]), "active preserved")
	# Persist + reload: the new zone survives a byte round-trip.
	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var r := NovaMissionData.new()
	assert_eq(r.open_file(tmp), OK)
	assert_eq(r.get_area_trigger_count(), base + 1, "zone count survives reload")
	var rz := r.get_area_trigger(idx)
	assert_eq((rz["max"] as Vector3).x, 30.0, "edited max_x survives reload")
	assert_true(bool(rz["active"]), "active survives reload")
	# Remove it: count returns to baseline; out-of-range guards return false/empty.
	assert_true(m.remove_area_trigger(idx), "remove succeeds")
	assert_eq(m.get_area_trigger_count(), base, "count back to baseline")
	assert_false(m.remove_area_trigger(999999), "out-of-range remove is rejected")
	assert_eq(m.get_area_trigger(999999), {}, "out-of-range get yields {}")
	assert_eq(m.set_area_trigger(999999, Vector3.ZERO, Vector3.ONE, true, true, 0), {}, "out-of-range set yields {}")
