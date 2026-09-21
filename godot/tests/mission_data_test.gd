extends GutTest

# Phase 1 bindings: MissionData (engine/runtime/mission) + ItemDatabase (engine/formats/def),
# and the entity item_id -> items.def graphic resolution chain that "populate the
# world" depends on. Uses committed fixtures (the items.def fixture is a small
# 12-item subset, so resolution is partial by design).

const BMS_PATH := "res://../fixtures/bms/synth_dense.bms"
const ITEMS_PATH := "res://../fixtures/def/items.def"


func _bms_abs() -> String:
	return ProjectSettings.globalize_path(BMS_PATH)


func _items_abs() -> String:
	return ProjectSettings.globalize_path(ITEMS_PATH)


func test_mission_data_parses_header_and_entities() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK, "synth_dense.bms should parse")
	assert_true(m.is_loaded(), "mission should report loaded")
	assert_eq(m.get_terrain_ref(), "Tmap", "header terrain reference")
	assert_eq(m.get_environment_ref(), "synth_full", "header environment reference")
	assert_false(m.get_mission_name().is_empty(), "mission name should be populated")

	var building_count := m.get_entity_count(MissionData.KIND_BUILDING)
	assert_gt(building_count, 0, "the synthetic mission places buildings")

	var buildings := m.get_entity_refs(MissionData.KIND_BUILDING)
	assert_eq(buildings.size(), building_count, "get_entity_refs count matches get_entity_count")

	var first: EntityRef = buildings[0]
	assert_true("item_id" in first, "entity record exposes item_id")
	assert_true("position" in first, "entity record exposes position")
	assert_typeof(first.position, TYPE_VECTOR3, "position is a Vector3")

	var total := m.get_entity_count(MissionData.KIND_ITEM) \
		+ m.get_entity_count(MissionData.KIND_BUILDING) \
		+ m.get_entity_count(MissionData.KIND_MARKER) \
		+ m.get_entity_count(MissionData.KIND_ORGANIC)
	assert_eq(m.get_all_entity_refs().size(), total, "get_all_entity_refs aggregates every kind")


func test_get_entity_matches_the_scanned_entry() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var first: EntityRef = m.get_entity_refs(MissionData.KIND_BUILDING)[0]
	var index := first.index
	var direct := m.get_entity_ref(MissionData.KIND_BUILDING, index)
	assert_eq(direct.index, index, "get_entity_ref returns the entity at that index")
	assert_eq(direct.position, first.position, "with the same position as the scanned entry")
	assert_eq(direct.item_id, first.item_id, "and the same item_id")
	assert_null(m.get_entity_ref(MissionData.KIND_BUILDING, 999999), "an out-of-range index yields null")
	assert_null(m.get_entity_ref(MissionData.KIND_BUILDING, -1), "a negative index yields null")


func test_item_database_loads_and_handles_missing() -> void:
	var db := ItemDatabase.new()
	assert_eq(db.load(_items_abs()), OK, "items.def fixture should parse")
	assert_true(db.is_loaded(), "database should report loaded")
	assert_gt(db.get_count(), 0, "items.def yields at least one item")

	assert_false(db.has_item(-99999), "unknown id is absent")
	assert_eq(db.get_graphic(-99999), "", "unknown id has no graphic")
	assert_eq(db.get_display_name(-99999), "", "unknown id has no display name")


func test_item_database_exposes_retail_interior_light_transfer() -> void:
	var tmp := ProjectSettings.globalize_path(
			"user://light_transfer_items_%d.def" % Time.get_ticks_usec())
	var file := FileAccess.open(tmp, FileAccess.WRITE)
	assert_not_null(file)
	file.store_string(
			"begin \"Absent\"\n"
			+ "  id 710010\n"
			+ "end\n"
			+ "begin \"Ihq01\"\n"
			+ "  id 101216\n"
			+ "  light_transfer 20\n"
			+ "end\n")
	file.close()
	var db := ItemDatabase.new()
	assert_eq(db.load(tmp), OK)
	assert_eq(db.get_light_transfer(710010), 0.0,
			"an unauthored building keeps the zero-initialized retail value")
	assert_almost_eq(db.get_light_transfer(101216), 0.2, 0.0001,
			"items.def percent is the interior daylight lerp at ItemDef+0x218")
	DirAccess.remove_absolute(tmp)




func test_entities_resolve_to_models() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var db := ItemDatabase.new()
	assert_eq(db.load(_items_abs()), OK)

	var resolved := 0
	for e in m.get_all_entity_refs():
		var id: int = e.item_id
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


func _temp_mis_path() -> String:
	return ProjectSettings.globalize_path("user://mission_authoring_rt_%d.mis" % Time.get_ticks_usec())


func test_set_entity_transform_persists_through_save_reload() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.is_modified(), "a freshly opened mission is not modified")

	var buildings := m.get_entity_refs(MissionData.KIND_BUILDING)
	assert_gt(buildings.size(), 1, "need at least two buildings to check a neighbor")
	var target: EntityRef = buildings[0]
	var neighbor_before: Vector3 = buildings[1].position
	var kind := target.kind
	var index := target.index
	var rotation: Vector3 = m.get_entity_rotation(kind, index)
	var new_pos := Vector3(123.0, 45.0, -67.0)

	assert_true(m.set_entity_transform(kind, index, new_pos, rotation), "moving a valid entity succeeds")
	assert_true(m.is_modified(), "a mutation sets the modified flag")

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "save_as writes the edited mission")
	assert_false(m.is_modified(), "a successful save clears the modified flag")

	var reopened := MissionData.new()
	assert_eq(reopened.open_file(tmp), OK, "the saved mission reopens")
	var b2 := reopened.get_entity_refs(MissionData.KIND_BUILDING)
	assert_eq(b2.size(), buildings.size(), "entity count is unchanged by an in-place move")

	var moved: Vector3 = b2[0].position
	assert_almost_eq(moved.x, new_pos.x, 0.02, "moved entity keeps its new X")
	assert_almost_eq(moved.y, new_pos.y, 0.02, "moved entity keeps its new Y")
	assert_almost_eq(moved.z, new_pos.z, 0.02, "moved entity keeps its new Z")

	var neighbor_after: Vector3 = b2[1].position
	assert_almost_eq(neighbor_after.x, neighbor_before.x, 0.02, "untouched neighbor X is unchanged")
	assert_almost_eq(neighbor_after.y, neighbor_before.y, 0.02, "untouched neighbor Y is unchanged")
	assert_almost_eq(neighbor_after.z, neighbor_before.z, 0.02, "untouched neighbor Z is unchanged")

	DirAccess.remove_absolute(tmp)


# Guards the undo/redo rebake heuristic: object_records_revision() (beside the
# event / zone counts the editor captures before a restore) must move for
# placed-object edits and stay put for non-object edits, so the controller
# re-bakes the ~1600-node world only when an object actually changed and
# otherwise just refreshes the overlay.
func test_object_records_revision_tracks_placed_objects() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)

	var rev0 := m.object_records_revision()
	var events0 := m.get_event_count()

	# A non-object edit (the mission header) must NOT move the placed-object revision -- this is what
	# lets an undo/redo of header / event / zone data skip re-baking the world.
	assert_true(m.set_header_string("designer", "someone-else"), "header edit applies")
	assert_eq(m.object_records_revision(), rev0, "a header edit leaves the object revision unchanged")

	# A zone edit moves the zone count in the fingerprint but still leaves object_rev alone (area
	# triggers are not placed objects), so a zone undo takes the lightweight overlay-only path.
	var zones0 := m.get_area_trigger_count()
	var zone := m.add_area_trigger(Vector3(-1, -1, -1), Vector3(1, 1, 1), true, false, 0)
	assert_gte(zone, 0, "a zone is added")
	assert_eq(m.get_area_trigger_count(), zones0 + 1, "the zone count moves")
	assert_eq(m.get_event_count(), events0, "adding a zone leaves the event count alone")
	assert_eq(m.object_records_revision(), rev0, "adding a zone does not move the object revision")

	# An object edit (moving a placed entity) MUST move the revision -> a full re-bake on undo/redo.
	var buildings := m.get_entity_refs(MissionData.KIND_BUILDING)
	assert_gt(buildings.size(), 0, "need a building to move")
	var b: EntityRef = buildings[0]
	assert_true(m.set_entity_transform(b.kind, b.index,
		Vector3(10.0, 20.0, 30.0), m.get_entity_rotation(b.kind, b.index)), "moving a building applies")
	assert_ne(m.object_records_revision(), rev0, "moving a placed entity moves the object revision")


# Game mode is single-select: set_game_mode keeps exactly one mode bit (or none), clears the rest,
# preserves the non-mode option bits, decodes by priority, and rejects invalid bits.
# [orig: sub_402770 decode @0x4050c7 / encode @0x4031cd, dfx2med.exe]
func test_game_mode_single_select_and_priority() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var mask := int(MissionData.ATTRIB_GAME_MODE_MASK)

	# Deathmatch: exactly that mode bit, nothing else in the mode mask.
	assert_true(m.set_game_mode(MissionData.ATTRIB_DEATHMATCH), "set deathmatch")
	assert_eq(m.get_info().attrib_flags & mask, int(MissionData.ATTRIB_DEATHMATCH), "only DM mode bit set")
	assert_eq(int(m.get_game_mode()), int(MissionData.ATTRIB_DEATHMATCH), "get_game_mode reports DM")

	# Switch to Search & destroy (the high bit 0x80000000): replaces DM, no leftovers.
	assert_true(m.set_game_mode(MissionData.ATTRIB_SEARCH_AND_DESTROY), "set S&D")
	assert_eq(m.get_info().attrib_flags & mask, int(MissionData.ATTRIB_SEARCH_AND_DESTROY), "DM cleared, S&D set")
	assert_eq(int(m.get_game_mode()), int(MissionData.ATTRIB_SEARCH_AND_DESTROY), "get reports S&D (high bit survives)")

	# Single player (0) clears all mode bits.
	assert_true(m.set_game_mode(0), "set single player")
	assert_eq(m.get_info().attrib_flags & mask, 0, "no mode bits remain")
	assert_eq(int(m.get_game_mode()), 0, "get reports single player")

	# Non-mode option bits survive a game-mode change.
	assert_true(m.set_header_flag(MissionData.ATTRIB_ROTATE_MAP_180, true), "set rotate option")
	assert_true(m.set_game_mode(MissionData.ATTRIB_COOP), "set coop")
	assert_ne(m.get_info().attrib_flags & int(MissionData.ATTRIB_ROTATE_MAP_180), 0, "rotate option preserved across mode change")
	assert_eq(int(m.get_game_mode()), int(MissionData.ATTRIB_COOP), "coop active")

	# Invalid bits are rejected and change nothing.
	var before := m.get_info().attrib_flags
	assert_false(m.set_game_mode(0x4), "an override bit is not a valid game mode")
	assert_false(m.set_game_mode(int(MissionData.ATTRIB_COOP) | int(MissionData.ATTRIB_DEATHMATCH)), "two mode bits is invalid")
	assert_eq(m.get_info().attrib_flags, before, "rejected calls leave flags unchanged")

	# A mode set round-trips through save / reopen.
	assert_true(m.set_game_mode(MissionData.ATTRIB_CAPTURE_THE_FLAG), "set CTF")
	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "save")
	var reopened := MissionData.new()
	assert_eq(reopened.open_file(tmp), OK, "reopen")
	assert_eq(int(reopened.get_game_mode()), int(MissionData.ATTRIB_CAPTURE_THE_FLAG), "CTF persists across save/reload")
	DirAccess.remove_absolute(tmp)


func test_save_without_edits_is_non_destructive() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_refs(MissionData.KIND_BUILDING)

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "saving an unedited mission succeeds")

	var reopened := MissionData.new()
	assert_eq(reopened.open_file(tmp), OK)
	var after := reopened.get_entity_refs(MissionData.KIND_BUILDING)
	assert_eq(after.size(), before.size(), "save->reload preserves the entity set")
	for i in before.size():
		var p0: Vector3 = before[i].position
		var p1: Vector3 = after[i].position
		assert_almost_eq(p1.x, p0.x, 0.02, "building %d X round-trips" % i)
		assert_almost_eq(p1.y, p0.y, 0.02, "building %d Y round-trips" % i)
		assert_almost_eq(p1.z, p0.z, 0.02, "building %d Z round-trips" % i)

	DirAccess.remove_absolute(tmp)


func test_set_entity_transform_rejects_out_of_range() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.set_entity_transform(MissionData.KIND_BUILDING, 999999, Vector3.ZERO, Vector3.ZERO),
		"an out-of-range index is rejected")
	assert_false(m.set_entity_transform(MissionData.KIND_BUILDING, -1, Vector3.ZERO, Vector3.ZERO),
		"a negative index is rejected")
	assert_false(m.is_modified(), "a rejected mutation does not dirty the document")


func test_save_file_without_path_is_invalid() -> void:
	# A document constructed without open_file has no source path; save_file must
	# return the no-path code (the editor shell then routes to Save As).
	var m := MissionData.new()
	assert_eq(m.save_file(), ERR_INVALID_PARAMETER, "save_file with no path is ERR_INVALID_PARAMETER")


# --- Authoring (Phase 2): edit team / group ----------------------------------
# set_entity_property_int seeds the lib's all-fields property setter from the entity's
# current state and changes only the named field, so editing team must leave group and
# every other AI property untouched (the regression guard for the 13-field overwrite).



func _entity(m: MissionData, kind: int, index: int) -> EntityRef:
	for e in m.get_entity_refs(kind):
		if e.index == index:
			return e
	return null


func test_set_entity_property_int_group_roundtrips() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before: EntityRef = m.get_entity_refs(MissionData.KIND_BUILDING)[0]
	var index := before.index

	var new_group := before.group + 3
	assert_true(m.set_entity_property_int(MissionData.KIND_BUILDING, index, "group", new_group))
	var after := _entity(m, MissionData.KIND_BUILDING, index)
	assert_eq(after.group, new_group, "group takes the new value")
	assert_eq(after.team, before.team, "team is left untouched by a group edit")


func test_set_entity_property_int_persists_through_save_reload() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var index := int(m.get_entity_refs(MissionData.KIND_BUILDING)[0].index)
	assert_true(m.set_entity_property_int(MissionData.KIND_BUILDING, index, "team", 4))

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var reopened := MissionData.new()
	assert_eq(reopened.open_file(tmp), OK)
	assert_eq(int(_entity(reopened, MissionData.KIND_BUILDING, index).team), 4,
		"an edited team survives the byte-faithful save and reload")
	DirAccess.remove_absolute(tmp)


func test_set_entity_property_int_unknown_property_is_inert() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.set_entity_property_int(MissionData.KIND_BUILDING, 0, "not_a_real_property", 9),
		"a property the binding does not map is rejected")
	assert_false(m.is_modified(), "a rejected property edit does not dirty the document")




func test_set_entity_property_int_rejects_out_of_range() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.set_entity_property_int(MissionData.KIND_BUILDING, 999999, "team", 1),
		"an out-of-range index is rejected")
	assert_false(m.set_entity_property_int(MissionData.KIND_BUILDING, -1, "team", 1),
		"a negative index is rejected")
	assert_false(m.is_modified(), "a rejected edit leaves the document clean")


# --- Authoring (Phase 3): place a new entity + item enumeration --------------
# add_entity appends a new record of a kind for an items.def id and returns it; the
# binding is the write side the editor's place-object palette drives. ItemDatabase
# enumeration (get_items / get_item_ids) is the read side that fills the palette.

func test_add_entity_appends_and_returns_the_new_record() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_count(MissionData.KIND_BUILDING)
	assert_false(m.is_modified(), "a freshly opened mission is not modified")

	var pos := Vector3(111.0, 22.0, -33.0)
	var record := m.add_entity(MissionData.KIND_BUILDING, 102001, pos, Vector3.ZERO)
	assert_not_null(record, "add_entity returns the new record")
	assert_true(m.is_modified(), "adding an entity dirties the document")
	assert_eq(m.get_entity_count(MissionData.KIND_BUILDING), before + 1, "the kind's count grows by one")
	assert_eq(record.index, before, "the new entity is appended at the end of its list")
	assert_eq(record.item_id, 102001, "the record carries the placed item id")
	assert_eq(record.kind, MissionData.KIND_BUILDING, "the record carries the requested kind")

	var fetched := m.get_entity_ref(MissionData.KIND_BUILDING, before)
	assert_eq(fetched.item_id, 102001, "get_entity_ref finds the appended entity")
	var fp: Vector3 = fetched.position
	assert_almost_eq(fp.x, pos.x, 0.02, "stored X matches what was placed")
	assert_almost_eq(fp.y, pos.y, 0.02, "stored Y matches what was placed")
	assert_almost_eq(fp.z, pos.z, 0.02, "stored Z matches what was placed")


func test_add_entity_persists_through_save_reload() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_count(MissionData.KIND_ITEM)
	var record := m.add_entity(MissionData.KIND_ITEM, 101291, Vector3(7.0, 8.0, 9.0), Vector3(0, 90, 0))
	var new_index := record.index

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "the mission with a new entity saves")
	var reopened := MissionData.new()
	assert_eq(reopened.open_file(tmp), OK, "and reopens")
	assert_eq(reopened.get_entity_count(MissionData.KIND_ITEM), before + 1, "the added entity survives save+reload")
	var roundtripped := reopened.get_entity_ref(MissionData.KIND_ITEM, new_index)
	assert_eq(roundtripped.item_id, 101291, "the placed item id round-trips")
	var rot: Vector3 = reopened.get_entity_rotation(MissionData.KIND_ITEM, new_index)
	assert_almost_eq(rot.y, 90.0, 0.5, "the placed yaw round-trips (rounded to integer degrees)")
	DirAccess.remove_absolute(tmp)


func test_add_entity_without_a_mission_is_rejected() -> void:
	var m := MissionData.new()  # never opened -> no document loaded
	assert_null(m.add_entity(MissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO), "adding to an unloaded mission returns an empty dict")
	assert_false(m.is_modified(), "a rejected add does not dirty the document")


func test_item_database_enumeration_is_sorted_and_complete() -> void:
	var db := ItemDatabase.new()
	assert_eq(db.load(_items_abs()), OK)
	var ids := db.get_item_ids()
	assert_eq(ids.size(), db.get_count(), "get_item_ids returns every id")

	# Stable order: sorted with Godot's natural, case-insensitive comparator, matching
	# the order ItemDatabase computes at load (so the assertion can actually catch a
	# sort regression, not just lexicographic ordering that happens to coincide).
	for i in range(1, ids.size()):
		var prev := db.get_display_name(ids[i - 1])
		var cur := db.get_display_name(ids[i])
		assert_true(prev.naturalnocasecmp_to(cur) <= 0,
			"items are ordered by natural display name (%s <= %s)" % [prev, cur])

	# Every enumerated id resolves through the single-id getters.
	for id in ids:
		assert_true(db.has_item(id), "enumerated id %d is an item" % id)
	assert_false(db.get_graphic(ids[0]).is_empty(), "enumerated items carry their graphic")


# --- Authoring (Phase 4): remove an entity -----------------------------------
# remove_entity erases an entity from its kind's list; every later entity of that kind
# shifts down one index (std::vector::erase semantics, proven byte-faithful at the lib
# level). The binding is the write side the editor's delete action drives.

func test_remove_entity_drops_count_and_reindexes() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.is_modified(), "a freshly opened mission is not modified")
	var buildings := m.get_entity_refs(MissionData.KIND_BUILDING)
	assert_gt(buildings.size(), 2, "need a few buildings to check reindexing")

	# Capture the entity at index 1: after removing index 0 it must shift down to index 0,
	# carrying its own data (so the removal is not just a truncation of the last element).
	var was_at_1_item: int = int(buildings[1].item_id)
	var was_at_1_pos: Vector3 = buildings[1].position

	assert_true(m.remove_entity(MissionData.KIND_BUILDING, 0), "removing a valid entity succeeds")
	assert_true(m.is_modified(), "a removal dirties the document")
	assert_eq(m.get_entity_count(MissionData.KIND_BUILDING), buildings.size() - 1, "the kind's count drops by one")

	var new_first := m.get_entity_ref(MissionData.KIND_BUILDING, 0)
	assert_eq(new_first.item_id, was_at_1_item, "the entity at index 1 shifted down into index 0")
	var np: Vector3 = new_first.position
	assert_almost_eq(np.x, was_at_1_pos.x, 0.02, "the shifted entity kept its X position")
	assert_almost_eq(np.z, was_at_1_pos.z, 0.02, "the shifted entity kept its Z position")


func test_remove_entity_persists_through_save_reload() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_count(MissionData.KIND_BUILDING)
	var other_kind_before := m.get_entity_count(MissionData.KIND_ITEM)
	assert_true(m.remove_entity(MissionData.KIND_BUILDING, 0))

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "the mission with a removed entity saves")
	var reopened := MissionData.new()
	assert_eq(reopened.open_file(tmp), OK, "and reopens")
	assert_eq(reopened.get_entity_count(MissionData.KIND_BUILDING), before - 1, "the removal survives save+reload")
	assert_eq(reopened.get_entity_count(MissionData.KIND_ITEM), other_kind_before, "removing a building leaves the item list untouched")
	DirAccess.remove_absolute(tmp)


func test_remove_entity_rejects_out_of_range() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.remove_entity(MissionData.KIND_BUILDING, 999999), "an out-of-range index is rejected")
	assert_false(m.remove_entity(MissionData.KIND_BUILDING, -1), "a negative index is rejected")
	assert_false(m.is_modified(), "a rejected removal does not dirty the document")


func test_remove_entity_without_a_mission_is_rejected() -> void:
	var m := MissionData.new()  # never opened -> no document loaded
	assert_false(m.remove_entity(MissionData.KIND_BUILDING, 0), "removing from an unloaded mission fails")
	assert_false(m.is_modified(), "a rejected removal does not dirty the document")


func test_create_default_is_loaded_and_empty() -> void:
	var m := MissionData.new()
	assert_eq(m.create_default(), OK, "create_default succeeds")
	assert_true(m.is_loaded(), "a default mission reports loaded")
	assert_eq(m.get_source_path(), "", "a default mission has no source path")
	assert_eq(m.get_entity_count(MissionData.KIND_ITEM), 0, "no items")
	assert_eq(m.get_entity_count(MissionData.KIND_BUILDING), 0, "no buildings")
	assert_eq(m.get_entity_count(MissionData.KIND_MARKER), 0, "no markers")


func test_create_default_save_and_reopen() -> void:
	var m := MissionData.new()
	assert_eq(m.create_default(), OK)
	assert_true(m.set_header_string("terrain", "dvxi5"), "terrain ref is settable")
	m.add_entity(MissionData.KIND_ITEM, 101291, Vector3(5, 6, 7), Vector3.ZERO)
	var path := ProjectSettings.globalize_path("user://test_create_default.bms")
	assert_eq(m.save_as(path), OK, "a from-scratch mission saves to disk")

	var reopened := MissionData.new()
	assert_eq(reopened.open_file(path), OK, "the saved from-scratch mission reopens")
	assert_eq(reopened.get_terrain_ref(), "dvxi5", "terrain ref round-trips")
	assert_eq(reopened.get_entity_count(MissionData.KIND_ITEM), 1, "the placed item round-trips")
	DirAccess.remove_absolute(path)


func test_create_default_save_as_mis_and_reopen() -> void:
	var m := MissionData.new()
	assert_eq(m.create_default(), OK)
	assert_true(m.set_header_string("mission_name", "MIS Binding"))
	assert_true(m.set_header_string("terrain", "dvxi5"))
	m.add_entity(MissionData.KIND_ITEM, 101291, Vector3(5, 6, 7), Vector3(1, 90, 3))
	var path := _temp_mis_path()
	assert_eq(m.save_as(path), OK, "save_as chooses the .mis writer by extension")
	var text := FileAccess.get_file_as_string(path)
	assert_true(text.begins_with("// mission metafile\r\n"), ".mis Save As writes the text metafile")
	assert_eq(m.get_source_path(), path, "Save As adopts the .mis path")

	var reopened := MissionData.new()
	assert_eq(reopened.open_file(path), OK, "the saved .mis reopens through the generic open_file")
	assert_eq(reopened.get_source_path(), path, "open_file records the .mis path")
	assert_eq(reopened.get_mission_name(), "MIS Binding")
	assert_eq(reopened.get_terrain_ref(), "dvxi5")
	assert_eq(reopened.get_entity_count(MissionData.KIND_ITEM), 1)
	DirAccess.remove_absolute(path)


func test_save_as_mis_writes_height_lock_and_staged_base_heights() -> void:
	# A .mis export declares every entity's z ABSOLUTE (height_lock 1) and bakes the
	# editor-sampled terrain height under it as extra_bheight, so the original editor
	# recovers the terrain-relative offset as z - extra_bheight
	# [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll]. See D-MIS-4.
	var m := MissionData.new()
	assert_eq(m.create_default(), OK)
	m.add_entity(MissionData.KIND_ITEM, 101291, Vector3(5, 6, 40), Vector3.ZERO)
	var path := _temp_mis_path()
	# One 16.16 base height (25.0), flat in write order (items, buildings, markers, organics).
	m.set_mis_base_heights(PackedInt32Array([25 * 65536]))
	assert_eq(m.save_as(path), OK, "save_as consumes the staged heights")
	var text := FileAccess.get_file_as_string(path)
	assert_string_contains(text, "height_lock 1", "the item z is declared absolute")
	assert_string_contains(text, "extra_bheight 1638400", "the staged base height is baked")

	# Staged heights are consumed by the save: a second save without re-staging falls back
	# to the entity's own interchange value (0 for an editor-authored entity).
	assert_eq(m.save_as(path), OK, "a re-save without staging succeeds")
	text = FileAccess.get_file_as_string(path)
	assert_string_contains(text, "extra_bheight 0", "no stale heights leak into the next save")
	assert_string_contains(text, "height_lock 1", "the absolute declaration is unconditional")
	DirAccess.remove_absolute(path)


# --- Phase 1: hidden entity fields + mission-header editing --------------------

func test_set_header_string_and_int_round_trip() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_true(m.set_header_string("mission_name", "Grill Test"), "name set")
	assert_true(m.set_header_int("climate", 2), "climate set")
	assert_false(m.set_header_string("bogus_field", "x"), "unknown header field rejected")
	assert_true(m.is_modified(), "a header edit dirties the mission")
	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var r := MissionData.new()
	assert_eq(r.open_file(tmp), OK)
	var info := r.get_info()
	assert_eq(info.mission_name, "Grill Test", "mission_name survives reload")
	assert_eq(info.climate, 2, "climate survives reload")


func test_set_header_flag_toggles_one_bit_and_preserves_others() -> void:
	var m := MissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_info().attrib_flags
	assert_true(m.set_header_flag(MissionData.ATTRIB_COOP, true), "set COOP")
	var after := m.get_info().attrib_flags
	assert_eq(after & MissionData.ATTRIB_COOP, MissionData.ATTRIB_COOP, "COOP bit is set")
	var other_mask := ~MissionData.ATTRIB_COOP
	assert_eq(after & other_mask, before & other_mask, "other attrib bits are preserved")
