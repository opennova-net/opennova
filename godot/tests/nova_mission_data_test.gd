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


func test_reground_entities_counts_and_applies_with_one_policy() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.create_default(), OK)
	# 5 = building in the witnessed type mapping [orig: ItemDef_ParseProperty @ 0x49eb00]
	# (the pre-D-ITEMDEF-1 invented enum said 4; witnessed 4 = marker, which is
	# mesh-less and skips the anchor bake this test asserts).
	var placed: Dictionary = m.place_entity_grounded(102001, 5, Vector3(100, 50, 10), Vector3(1, 2, 3))
	assert_false(placed.is_empty(), "the fixture entity places")
	var request := {
		"kind": int(placed["kind"]),
		"index": int(placed["index"]),
		"ground_hit_bms": Vector3(100, 50, 14),  # the terrain rose 4 under it
		"ground_anchor_bms": Vector3(1, 2, 3),
	}
	m.mark_clean()
	# Dry run counts the drift without writing or dirtying.
	assert_eq(m.reground_entities([request], 0.01, false), 1, "dry run reports the drifted entity")
	assert_false(m.is_dirty(), "a dry run never dirties the document")
	# Apply moves it (zero-rotation bake keeps x/y offsets, lifts z), dirties once.
	assert_eq(m.reground_entities([request]), 1, "apply moves the drifted entity")
	assert_true(m.is_dirty(), "an applied re-ground dirties the document")
	var moved: Dictionary = m.get_entity(int(placed["kind"]), int(placed["index"]))
	assert_almost_eq((moved["position"] as Vector3).z, 11.0, 0.001, "grounded on the raised terrain (14 - anchor z)")
	# Now everything is on the new ground: both count and apply are no-ops.
	assert_eq(m.reground_entities([request], 0.01, false), 0, "no drift after the apply")
	assert_eq(m.reground_entities([request]), 0)
	# Malformed rows are skipped, never errors.
	assert_eq(m.reground_entities([{ "kind": 0, "index": -1 }, {}]), 0)


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
	var db := NovaItemDatabase.new()
	assert_eq(db.load(tmp), OK)
	assert_eq(db.get_light_transfer(710010), 0.0,
			"an unauthored building keeps the zero-initialized retail value")
	assert_almost_eq(db.get_light_transfer(101216), 0.2, 0.0001,
			"items.def percent is the interior daylight lerp at ItemDef+0x218")
	assert_almost_eq(float(db.get_item(101216)["light_transfer"]), 0.2, 0.0001)
	DirAccess.remove_absolute(tmp)


func test_item_database_mount_config_preserves_presence_and_explicit_zero() -> void:
	# The retail target definition source is phrase_set at itemDef+0x86c. Exercise
	# the production database wrapper, including the state that the old
	# emplaced_pose_variant=0 transport could not represent.
	var tmp := ProjectSettings.globalize_path(
			"user://mount_config_items_%d.def" % Time.get_ticks_usec())
	var file := FileAccess.open(tmp, FileAccess.WRITE)
	assert_not_null(file)
	file.store_string(
			"begin \"Unknown\"\n"
			+ "  id 710001\n"
			+ "end\n"
			+ "begin \"Explicit Zero\"\n"
			+ "  id 710002\n"
			+ "  phrase_set 0\n"
			+ "end\n")
	file.close()
	var db := NovaItemDatabase.new()
	assert_eq(db.load(tmp), OK)
	assert_eq(db.get_mount_config(710001), {"valid": false, "value": 0},
			"absent phrase_set remains unknown")
	assert_eq(db.get_mount_config(710002), {"valid": true, "value": 0},
			"authored zero remains a valid retail config")
	DirAccess.remove_absolute(tmp)

	# Existing retail-shaped fixture witness: the emplaced B50cal target authors 4.
	assert_eq(db.load(_items_abs()), OK)
	assert_eq(db.get_mount_config(101419), {"valid": true, "value": 4},
			"target item definition phrase_set is the production mount config source")


func test_item_database_preserves_emplacement_attachment_variants_and_markers() -> void:
	var tmp := ProjectSettings.globalize_path(
			"user://emplacement_attachment_items_%d.def" % Time.get_ticks_usec())
	var file := FileAccess.open(tmp, FileAccess.WRITE)
	assert_not_null(file)
	file.store_string(
			"begin AttachmentCarrier\n"
			+ "  id 710100\n"
			+ "  addeweap ewep01 710101\n"
			+ "  addeweapG ewep02 710102 70 10 100 100\n"
			+ "  addeweapC ewep03 710103\n"
			+ "  addeweapC ewep04 710104 0 0 0 0\n"
			+ "  addeweapG ignored05 710105\n"
			+ "end\n")
	file.close()
	var db := NovaItemDatabase.new()
	assert_eq(db.load(tmp), OK)
	var rows: Array = db.get_emplacement_attachments(710100)
	assert_eq(rows.size(), 4, "retail stores at most four child emplacement rows")
	assert_eq(rows[0]["key"], "addeweap")
	assert_eq(rows[1]["key"], "addeweapG")
	assert_eq(rows[2]["key"], "addeweapC")
	assert_eq(rows[1]["userpoint"], "ewep02")
	assert_eq(rows[1]["item_id"], 710102, "the public domain keeps the full child item id")
	assert_eq(rows[1]["down_limit_bam"], 70 * 11930464)
	assert_eq(rows[1]["up_limit_bam"], -10 * 11930464)
	assert_true(rows[1]["has_explicit_limits"])
	assert_true(rows[3]["has_explicit_limits"],
			"explicit all-zero limits remain distinct from an omitted fallback")
	assert_eq(db.get_emplacement_attachment_markers(710100),
			{"g_slot": 2, "c_slot": 4},
			"the last stored G/C records retain their 1-based markers")
	assert_true(rows[1]["designated_g"])
	assert_false(rows[2]["designated_c"],
			"a later C row overwrites the earlier C designation")
	assert_true(rows[3]["designated_c"])
	assert_eq(db.get_item(710100)["emplacement_attachments"].size(), 4)
	DirAccess.remove_absolute(tmp)


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


func _temp_mis_path() -> String:
	return ProjectSettings.globalize_path("user://mission_authoring_rt_%d.mis" % Time.get_ticks_usec())


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


# Guards the undo/redo rebake heuristic: object_records_revision() (and the structure_fingerprint
# it feeds) must move for placed-object edits and stay put for non-object edits, so the controller
# re-bakes the ~1600-node world only when an object actually changed and otherwise just refreshes the
# overlay.
func test_object_records_revision_and_fingerprint_track_placed_objects() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)

	var rev0 := m.object_records_revision()
	var fp0 := m.structure_fingerprint()
	assert_true(fp0.has("events") and fp0.has("zones") and fp0.has("object_rev"),
		"structure_fingerprint exposes events / zones / object_rev")
	assert_eq(int(fp0["object_rev"]), rev0, "the fingerprint's object_rev mirrors object_records_revision")

	# A non-object edit (the mission header) must NOT move the placed-object revision -- this is what
	# lets an undo/redo of header / event / zone data skip re-baking the world.
	assert_true(m.set_header_string("designer", "someone-else"), "header edit applies")
	assert_eq(m.object_records_revision(), rev0, "a header edit leaves the object revision unchanged")

	# A zone edit moves the zone count in the fingerprint but still leaves object_rev alone (area
	# triggers are not placed objects), so a zone undo takes the lightweight overlay-only path.
	var zones0 := int(m.structure_fingerprint()["zones"])
	var zone := m.add_area_trigger(Vector3(-1, -1, -1), Vector3(1, 1, 1), true, false, 0)
	assert_false(zone.is_empty(), "a zone is added")
	assert_eq(int(m.structure_fingerprint()["zones"]), zones0 + 1, "the zone count moves in the fingerprint")
	assert_eq(m.object_records_revision(), rev0, "adding a zone does not move the object revision")

	# An object edit (moving a placed entity) MUST move the revision -> a full re-bake on undo/redo.
	var buildings := m.get_entities(NovaMissionData.KIND_BUILDING)
	assert_gt(buildings.size(), 0, "need a building to move")
	var b: Dictionary = buildings[0]
	assert_true(m.set_entity_transform(int(b["kind"]), int(b["index"]),
		Vector3(10.0, 20.0, 30.0), b["rotation_deg"]), "moving a building applies")
	assert_ne(m.object_records_revision(), rev0, "moving a placed entity moves the object revision")


# Game mode is single-select: set_game_mode keeps exactly one mode bit (or none), clears the rest,
# preserves the non-mode option bits, decodes by priority, and rejects invalid bits.
# [orig: sub_402770 decode @0x4050c7 / encode @0x4031cd, dfx2med.exe]
func test_game_mode_single_select_and_priority() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var mask := int(NovaMissionData.ATTRIB_GAME_MODE_MASK)

	# Deathmatch: exactly that mode bit, nothing else in the mode mask.
	assert_true(m.set_game_mode(NovaMissionData.ATTRIB_DEATHMATCH), "set deathmatch")
	assert_eq(int(m.get_info()["attrib_flags"]) & mask, int(NovaMissionData.ATTRIB_DEATHMATCH), "only DM mode bit set")
	assert_eq(int(m.get_game_mode()), int(NovaMissionData.ATTRIB_DEATHMATCH), "get_game_mode reports DM")

	# Switch to Search & destroy (the high bit 0x80000000): replaces DM, no leftovers.
	assert_true(m.set_game_mode(NovaMissionData.ATTRIB_SEARCH_AND_DESTROY), "set S&D")
	assert_eq(int(m.get_info()["attrib_flags"]) & mask, int(NovaMissionData.ATTRIB_SEARCH_AND_DESTROY), "DM cleared, S&D set")
	assert_eq(int(m.get_game_mode()), int(NovaMissionData.ATTRIB_SEARCH_AND_DESTROY), "get reports S&D (high bit survives)")

	# Single player (0) clears all mode bits.
	assert_true(m.set_game_mode(0), "set single player")
	assert_eq(int(m.get_info()["attrib_flags"]) & mask, 0, "no mode bits remain")
	assert_eq(int(m.get_game_mode()), 0, "get reports single player")

	# Non-mode option bits survive a game-mode change.
	assert_true(m.set_header_flag(NovaMissionData.ATTRIB_ROTATE_MAP_180, true), "set rotate option")
	assert_true(m.set_game_mode(NovaMissionData.ATTRIB_COOP), "set coop")
	assert_ne(int(m.get_info()["attrib_flags"]) & int(NovaMissionData.ATTRIB_ROTATE_MAP_180), 0, "rotate option preserved across mode change")
	assert_eq(int(m.get_game_mode()), int(NovaMissionData.ATTRIB_COOP), "coop active")

	# Invalid bits are rejected and change nothing.
	var before := int(m.get_info()["attrib_flags"])
	assert_false(m.set_game_mode(0x4), "an override bit is not a valid game mode")
	assert_false(m.set_game_mode(int(NovaMissionData.ATTRIB_COOP) | int(NovaMissionData.ATTRIB_DEATHMATCH)), "two mode bits is invalid")
	assert_eq(int(m.get_info()["attrib_flags"]), before, "rejected calls leave flags unchanged")

	# A mode set round-trips through save / reopen.
	assert_true(m.set_game_mode(NovaMissionData.ATTRIB_CAPTURE_THE_FLAG), "set CTF")
	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK, "save")
	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(tmp), OK, "reopen")
	assert_eq(int(reopened.get_game_mode()), int(NovaMissionData.ATTRIB_CAPTURE_THE_FLAG), "CTF persists across save/reload")
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


func test_ai_flag_bits_are_distinct_named_bits() -> void:
	# get_ai_flag_bits drives the Behavior > Flags checkboxes; each entry must be a single distinct bit
	# with a label so the inspector's merge-on-write (clear known mask, OR checked bits) is unambiguous.
	var m := NovaMissionData.new()
	var bits := m.get_ai_flag_bits()
	assert_gt(bits.size(), 0, "engine exposes AI attribute flag bits")
	var seen_mask := 0
	var masks: Array = []
	for entry in bits:
		var e := entry as Dictionary
		var value := int(e.get("value", 0))
		assert_false(String(e.get("name", "")).is_empty(), "each flag bit has a label")
		assert_gt(value, 0, "flag value is positive")
		assert_eq(value & (value - 1), 0, "flag value is a single bit (%d)" % value)
		assert_eq(seen_mask & value, 0, "flag bits do not overlap (%d)" % value)
		seen_mask |= value
		masks.append(value)
	# RE-confirmed positions (bms.h BmsiAttributeFlags / dfx2med object dialog).
	assert_true(masks.has(1 << 0), "Blind is bit 0")
	assert_true(masks.has(1 << 1), "Guarding is bit 1")
	assert_true(masks.has(1 << 22), "NavigationWaypoint is bit 22")


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


func test_behavior_spin_table_matches_engine_setter() -> void:
	# Drift guard: every numeric field the inspector's Behavior panel renders (MissionEntityFields,
	# the single UI table) must be accepted by the engine's set_entity_property_int (whose name->member
	# map lives in libs/mission). A typo'd or stale property in the table would otherwise bind a row
	# whose edits are silently rejected. Section markers carry no property and are skipped.
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var kind := _kind_with_entities(m)
	var index := int(m.get_entities(kind)[0]["index"])
	for entry in MissionEntityFields.SPIN_FIELDS:
		if not entry.has("property"):
			continue
		var prop := String(entry["property"])
		var current := int(_entity(m, kind, index).get(prop, 0))
		assert_true(m.set_entity_property_int(kind, index, prop, current),
			"the engine accepts the Behavior-panel field '%s'" % prop)


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


# --- create_default (from-scratch) --------------------------------------------
# create_default() builds a valid, empty mission in memory (no file). The byte fidelity of the
# round-trip is proven at the lib level (tests/mission/mission_bms_test.cpp); here we assert the
# GDScript boundary: loaded + empty, editable, and savable + reopenable.

func test_create_default_is_loaded_and_empty() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.create_default(), OK, "create_default succeeds")
	assert_true(m.is_loaded(), "a default mission reports loaded")
	assert_eq(m.get_source_path(), "", "a default mission has no source path")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_ITEM), 0, "no items")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), 0, "no buildings")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_MARKER), 0, "no markers")
	assert_false(m.is_dirty(), "a freshly-created mission is not dirty")


func test_create_default_save_and_reopen() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.create_default(), OK)
	assert_true(m.set_header_string("terrain", "dvxi5"), "terrain ref is settable")
	m.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3(5, 6, 7), Vector3.ZERO)
	var path := ProjectSettings.globalize_path("user://test_create_default.bms")
	assert_eq(m.save_as(path), OK, "a from-scratch mission saves to disk")

	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(path), OK, "the saved from-scratch mission reopens")
	assert_eq(reopened.get_terrain_ref(), "dvxi5", "terrain ref round-trips")
	assert_eq(reopened.get_entity_count(NovaMissionData.KIND_ITEM), 1, "the placed item round-trips")
	DirAccess.remove_absolute(path)


func test_create_default_save_as_mis_and_reopen() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.create_default(), OK)
	assert_true(m.set_header_string("mission_name", "MIS Binding"))
	assert_true(m.set_header_string("terrain", "dvxi5"))
	m.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3(5, 6, 7), Vector3(1, 90, 3))
	var path := _temp_mis_path()
	assert_eq(m.save_as(path), OK, "save_as chooses the .mis writer by extension")
	var text := FileAccess.get_file_as_string(path)
	assert_true(text.begins_with("// mission metafile\r\n"), ".mis Save As writes the text metafile")
	assert_eq(m.get_source_path(), path, "Save As adopts the .mis path")

	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(path), OK, "the saved .mis reopens through the generic open_file")
	assert_eq(reopened.get_source_path(), path, "open_file records the .mis path")
	assert_eq(reopened.get_mission_name(), "MIS Binding")
	assert_eq(reopened.get_terrain_ref(), "dvxi5")
	assert_eq(reopened.get_entity_count(NovaMissionData.KIND_ITEM), 1)
	DirAccess.remove_absolute(path)


func test_save_as_mis_writes_height_lock_and_staged_base_heights() -> void:
	# A .mis export declares every entity's z ABSOLUTE (height_lock 1) and bakes the
	# editor-sampled terrain height under it as extra_bheight, so the original editor
	# recovers the terrain-relative offset as z - extra_bheight
	# [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll]. See D-MIS-4.
	var m := NovaMissionData.new()
	assert_eq(m.create_default(), OK)
	m.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3(5, 6, 40), Vector3.ZERO)
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


# --- Undo / redo + dirty (in-memory document history) -------------------------
# The history holds in-memory document snapshots (no serialized bytes). begin_edit/commit_edit
# bracket a gesture into one step (commit pushes only on a real change); undo/redo swap the
# document; is_dirty() is exact against the clean baseline set by mark_clean().

func test_begin_commit_undo_rewinds_a_mutation() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	m.mark_clean()
	var before := m.get_entity_count(NovaMissionData.KIND_BUILDING)
	assert_false(m.can_undo(), "a freshly opened mission has no undo history")

	m.begin_edit()
	m.add_entity(NovaMissionData.KIND_BUILDING, 102001, Vector3(1, 2, 3), Vector3.ZERO)
	m.commit_edit()
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before + 1, "the placement landed")
	assert_true(m.can_undo(), "the committed edit is one undo step")
	assert_eq(m.undo_depth(), 1, "exactly one step")

	assert_true(m.undo(), "undo succeeds")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before, "undo rewinds the placement")
	assert_true(m.is_loaded(), "the document is still loaded after an undo")
	assert_true(m.can_redo(), "and is now redoable")


func test_no_change_session_pushes_no_step() -> void:
	# A begin/commit with no actual mutation (or a same-value edit) must add no undo step --
	# this pins bms::equal at the binding boundary.
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	m.begin_edit()
	m.commit_edit()
	assert_false(m.can_undo(), "an empty edit session pushes nothing")

	var index := int(m.get_entities(NovaMissionData.KIND_BUILDING)[0]["index"])
	var pos: Vector3 = m.get_entity(NovaMissionData.KIND_BUILDING, index)["position"]
	var rot: Vector3 = m.get_entity(NovaMissionData.KIND_BUILDING, index)["rotation_deg"]
	m.begin_edit()
	m.set_entity_transform(NovaMissionData.KIND_BUILDING, index, pos, rot) # same value
	m.commit_edit()
	assert_false(m.can_undo(), "a same-value edit pushes nothing")


func test_undo_redo_round_trip() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var before := m.get_entity_count(NovaMissionData.KIND_ITEM)
	m.begin_edit()
	m.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO)
	m.commit_edit()
	assert_true(m.undo(), "undo")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_ITEM), before, "undo removes the item")
	assert_true(m.redo(), "redo")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_ITEM), before + 1, "redo re-adds the item")
	assert_false(m.can_redo(), "the redo step is consumed")


func test_is_dirty_tracks_the_clean_baseline() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	m.mark_clean()
	assert_false(m.is_dirty(), "a freshly-cleaned mission is not dirty")
	m.begin_edit()
	m.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO)
	m.commit_edit()
	assert_true(m.is_dirty(), "an edit dirties the mission")
	assert_true(m.undo(), "undo")
	assert_false(m.is_dirty(), "undoing back to the clean baseline clears dirty")
	assert_true(m.redo(), "redo")
	assert_true(m.is_dirty(), "redo re-dirties")
	m.mark_clean()
	assert_false(m.is_dirty(), "mark_clean rebaselines to the current state")


func test_undo_on_empty_history_returns_false() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_false(m.undo(), "undo with no history returns false")
	assert_false(m.redo(), "redo with no history returns false")


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


func test_set_event_preserves_unmodeled_flag_bits() -> void:
	# Regression (review #2): the inspector rebuilds an event's flags from the exposed checkboxes only
	# (event_flag_bits = ResetAfter/PreMission/PostMission, mask 0x07 — matching the DFX2 editor), so
	# set_event must preserve confirmed internal bits it does not surface (e.g. 0x10) instead of clobbering them,
	# mirroring trigger condition_flags. Otherwise nudging any event attribute silently drops those bits.
	var m := NovaMissionData.new()
	assert_eq(m.create_default(), OK)
	var UNMODELED := 0x10 # confirmed internal bit: no event_flag_bits() checkbox, must survive edits
	# Seed an event carrying the unmodeled bit plus an exposed one (ResetAfter = 0x01).
	var added := m.add_event(UNMODELED | 0x01, 0, 0)
	assert_false(added.is_empty(), "event added")
	var idx := int(added["index"])
	assert_eq(int(m.get_event(idx)["flags"]) & UNMODELED, UNMODELED, "unmodeled bit present after add")
	# An editor edit rebuilds flags from exposed checkboxes only (here PreMission = 0x02, ResetAfter off).
	assert_true(m.set_event(idx, 0x02, 0, 0), "set_event succeeds")
	var flags := int(m.get_event(idx)["flags"])
	assert_eq(flags & UNMODELED, UNMODELED, "internal bit 0x10 preserved across the edit")
	assert_eq(flags & 0x02, 0x02, "exposed PreMission bit applied")
	assert_eq(flags & 0x01, 0, "exposed ResetAfter bit cleared (unchecked)")


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


func test_weapon_loadout_dictionary_and_round_trip() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var entries := m.get_weapon_loadout()
	# The fixture canonicalizes to 7 loadout records in the public four-field view.
	assert_eq(entries.size(), 7, "fixture loadout has 7 weapons")
	var first := entries[0] as Dictionary
	for key in ["index", "name", "ammo_primary", "ammo_secondary", "flags"]:
		assert_true(first.has(key), "loadout dict exposes %s" % key)
	assert_eq(String(first["name"]), "WPN_CAR15AUTO", "first weapon name")
	assert_eq(String(first["ammo_primary"]), "-1", "first primary-ammo request")
	assert_eq(String(first["flags"]), "-1", "first damage class")
	# Edit one entry + append a custom one; persist and reload.
	entries[0]["ammo_primary"] = "5"
	entries[0]["flags"] = "1"
	entries.append({ "name": "WPN_TEST", "ammo_primary": "1", "ammo_secondary": "2", "flags": "2" })
	assert_true(m.set_weapon_loadout(entries), "set_weapon_loadout succeeds")
	assert_true(m.is_modified(), "editing the loadout dirties the mission")
	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var r := NovaMissionData.new()
	assert_eq(r.open_file(tmp), OK)
	var reloaded := r.get_weapon_loadout()
	assert_eq(reloaded.size(), 8, "edited loadout survives reload")
	assert_eq(String((reloaded[0] as Dictionary)["ammo_primary"]), "5", "edited primary-ammo request survives reload")
	assert_eq(String((reloaded[0] as Dictionary)["flags"]), "1", "edited damage class survives reload")
	assert_eq(String((reloaded[7] as Dictionary)["name"]), "WPN_TEST", "appended weapon survives reload")
	assert_eq(String((reloaded[7] as Dictionary)["flags"]), "2", "appended damage class survives reload")
	# Clearing yields an empty list.
	assert_true(m.set_weapon_loadout([]), "clearing the loadout succeeds")
	assert_eq(m.get_weapon_loadout().size(), 0, "loadout is empty after clear")


func test_group_get_set_round_trip() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_eq(m.get_group_count(), 64, "64 fixed group records")
	assert_eq(m.get_groups().size(), m.get_group_count(), "groups list matches count")
	var g := m.get_group(3)
	for key in ["index", "field0", "field8", "field12"]:
		assert_true(g.has(key), "group dict exposes %s" % key)
	# A neighbour's baseline must be untouched by editing group 3.
	var neighbour_before := m.get_group(4)
	assert_true(m.set_group(3, 3, 5678, 10), "set_group succeeds")
	var g2 := m.get_group(3)
	assert_eq(int(g2["field0"]), 3, "group flags written")
	assert_eq(int(g2["field8"]), 5678, "group value written")
	assert_eq(int(g2["field12"]), 10, "group constant remains fixed")
	assert_eq(m.get_group(4), neighbour_before, "neighbouring group untouched")
	# Persist + reload.
	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var r := NovaMissionData.new()
	assert_eq(r.open_file(tmp), OK)
	assert_eq(int(r.get_group(3)["field8"]), 5678, "edited group survives reload")
	# Out-of-range guards.
	assert_eq(m.get_group(999), {}, "out-of-range group get yields {}")
	assert_false(m.set_group(999, 1, 2, 3), "out-of-range group set rejected")
	assert_false(m.set_group(3, 4, 2, 10), "unsupported group flag bits rejected")
	assert_false(m.set_group(3, 3, 2, 11), "noncanonical group constant rejected")


# --- Phase 4: mission scripting (events / triggers / actions) ------------------

func test_event_chain_dictionary_shape() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	assert_gt(m.get_event_count(), 0, "the reference mission has events")
	var chain := m.get_event_chain(0)
	assert_true(chain.has("event"), "the chain carries the event")
	assert_true(chain.has("triggers"), "the chain carries triggers")
	assert_true(chain.has("actions"), "the chain carries actions")
	assert_true(chain.has("references"), "the chain carries references")
	assert_true(chain.has("diagnostics"), "the chain carries diagnostics")
	var summary := m.get_logic_summary()
	assert_eq(int(summary["events"]), m.get_event_count(), "the logic summary event count matches")


func test_enum_tables_reflect_the_engine_names() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	# Group(1)..Player(7) -> 7 named main types.
	assert_eq(m.get_trigger_main_types().size(), 7, "seven trigger main types")
	# A non-contiguous, high-numbered sub-type still appears.
	var found_high := false
	for entry in m.get_trigger_sub_types(2):  # Single
		if int(entry["value"]) == 45:
			found_high = String(entry["name"]) == "SingleDoesNotSeeOrFarther"
	assert_true(found_high, "the high-numbered Single sub-type reflects through")
	var found_reset := false
	for entry in m.get_action_types():
		if int(entry["value"]) == 34:
			found_reset = String(entry["name"]) == "ResetEvent"
	assert_true(found_reset, "ResetEvent appears in the action types")
	assert_eq(m.get_event_flag_bits().size(), 3, "three author-facing event-flag bits (matches dfx2med)")


func test_add_event_with_trigger_and_action_persists_through_save_reload() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var base_events := m.get_event_count()

	var event := m.add_event(1, 7, 3)  # ResetAfter flag, reset_after 7, delay 3
	assert_false(event.is_empty(), "add_event returns the new event dict")
	assert_eq(int(event["index"]), base_events, "the new event is appended at the end")
	var ev_index := int(event["index"])

	# Append a trigger and an action to the new event.
	var with_trigger := m.add_event_trigger(ev_index, {"main_type": 2, "sub_type": 10, "param2": 1, "negated": true})
	assert_false(with_trigger.is_empty(), "add_event_trigger returns the chain")
	assert_eq((with_trigger["triggers"] as Array).size(), 1, "the event now has one trigger")
	var with_action := m.add_event_action(ev_index, {"action_type": 34, "param1": ev_index})
	assert_eq((with_action["actions"] as Array).size(), 1, "the event now has one action")
	assert_true(m.is_modified(), "scripting edits set the modified flag")

	var tmp := _temp_bms_path()
	assert_eq(m.save_as(tmp), OK)
	var r := NovaMissionData.new()
	assert_eq(r.open_file(tmp), OK, "the augmented mission reopens")
	assert_eq(r.get_event_count(), base_events + 1, "the new event survives reload")
	var chain := r.get_event_chain(ev_index)
	var triggers := chain["triggers"] as Array
	var actions := chain["actions"] as Array
	assert_eq(triggers.size(), 1, "the trigger survives reload")
	assert_eq(int((triggers[0] as Dictionary)["main_type"]), 2, "the trigger main type round-trips (Single)")
	assert_eq(int((triggers[0] as Dictionary)["sub_type"]), 10, "the trigger sub type round-trips")
	assert_true(bool((triggers[0] as Dictionary)["negated"]), "the negate flag round-trips")
	assert_eq(int((actions[0] as Dictionary)["action_type"]), 34, "the action type round-trips (ResetEvent)")
	assert_eq(int((chain["event"] as Dictionary)["reset_after"]), 7, "reset_after round-trips")


func test_set_event_trigger_edits_a_param_in_place() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var event := m.add_event(0, 0, 0)
	var ev_index := int(event["index"])
	m.add_event_trigger(ev_index, {"main_type": 1, "sub_type": 1})
	# Overwrite the trigger's param1 + logic flag, leaving its type alone (omitted keys keep their value).
	var chain := m.set_event_trigger(ev_index, 0, {"param1": 42, "logic_or": true})
	assert_false(chain.is_empty(), "set_event_trigger returns the chain")
	var trigger := (chain["triggers"] as Array)[0] as Dictionary
	assert_eq(int(trigger["param1"]), 42, "the edited param lands")
	assert_eq(int(trigger["main_type"]), 1, "the omitted main type is preserved")
	assert_true(bool(trigger["logic_or"]), "the OR logic flag lands")


func test_remove_event_drops_it_and_repairs_reset_references() -> void:
	var m := NovaMissionData.new()
	assert_eq(m.open_file(_bms_abs()), OK)
	var base_events := m.get_event_count()
	# Append an event whose action resets itself, then remove event 0: the self-reference must follow.
	var event := m.add_event(0, 0, 0)
	var ev_index := int(event["index"])
	m.add_event_action(ev_index, {"action_type": 34, "param1": ev_index})  # ResetEvent -> self
	assert_eq(m.get_event_count(), base_events + 1)

	assert_true(m.remove_event(0), "remove_event drops event 0")
	assert_eq(m.get_event_count(), base_events, "the count drops back")
	# The appended event is now at base_events - 1; its ResetEvent must point at the new index.
	var chain := m.get_event_chain(base_events - 1)
	var actions := chain["actions"] as Array
	assert_eq(actions.size(), 1, "the appended event kept its action")
	assert_eq(int((actions[0] as Dictionary)["param1"]), base_events - 1, "the ResetEvent reference was repaired")
	# Out-of-range guards.
	assert_eq(m.get_event(9999), {}, "out-of-range event get yields {}")
	assert_false(m.remove_event(9999), "out-of-range event remove rejected")
