extends GutTest

# Phase 2: MissionController. Covers the load-orchestration paths that are
# testable without a full terrain-editor scene, using a stub that stands in for
# the seams the controller drives (resource root, world root, open_trn, env
# editor). The resolve-miss path is the important one: a mission whose referenced
# terrain is absent must fail cleanly with a clear reason and must not attempt to
# load terrain. Full end-to-end placement is validated against real assets
# out-of-band (see the placer + nova_world paths).

const MissionController := preload("res://modtools/mission/mission_controller.gd")
const Placer := preload("res://engine/mission/mission_object_placer.gd")
const WaypointOverlay := preload("res://engine/mission/mission_waypoint_overlay.gd")

const BMS_PATH := "res://../fixtures/bms/ash_i5b.reference.bms"


func _abs(res_path: String) -> String:
	return ProjectSettings.globalize_path(res_path)


# A minimal terrain-editor stand-in exposing only the seams the controller calls.
# Intentionally omits get_environment_editor/get_environment_node so the env step
# is skipped (kept out of these terrain-focused tests).
class StubTerrainEditor:
	extends Node

	var resource_root: NovaResourceRoot
	var world_root: Node3D
	var opened_trn: String = ""
	var open_trn_result: Error = OK
	var current_trn_path: String = ""
	# Placement raycast seam (Phase 3): the controller grounds a placed object via these.
	var terrain_hit: Vector3 = Vector3(64.0, 10.0, -64.0)
	var terrain_hit_valid: bool = true

	func get_resource_root() -> NovaResourceRoot:
		return resource_root

	func get_terrain_world_root() -> Node3D:
		return world_root

	func get_current_trn_path() -> String:
		return current_trn_path

	func open_trn(path: String) -> Error:
		# Mirror the real TerrainEditor: a successful open records the current .trn.
		opened_trn = path
		if open_trn_result == OK:
			current_trn_path = path
		return open_trn_result

	func raycast_terrain_at(_mouse: Vector2) -> Vector3:
		return terrain_hit

	func is_valid_terrain_hit(_hit: Vector3) -> bool:
		return terrain_hit_valid


# A resource root over the repo's real dvxi5 terrain fixture (the terrain the test
# mission references), so open_mission resolves + "loads" its terrain via the stub.
func _dvxi5_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/godot/dvxi5"))
	return root


func test_open_mission_without_editor_is_unavailable() -> void:
	var controller := MissionController.new(null)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), ERR_UNAVAILABLE)
	assert_false(controller.is_loaded(), "nothing loads without an editor")


func test_open_mission_without_resource_root_is_unconfigured() -> void:
	var stub := StubTerrainEditor.new()
	add_child_autofree(stub)  # resource_root left null on purpose
	var controller := MissionController.new(stub)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), ERR_UNCONFIGURED)
	assert_eq(stub.opened_trn, "", "no terrain load is attempted")


func test_open_mission_reports_missing_terrain() -> void:
	var stub := StubTerrainEditor.new()
	var root := NovaResourceRoot.new()
	# fixtures/ has the reference .bms but no matching .trn, so the terrain the
	# mission references cannot be resolved.
	root.set_root_dir(_abs("res://../fixtures"))
	stub.resource_root = root
	stub.world_root = Node3D.new()
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)

	var controller := MissionController.new(stub)
	var err := controller.open_mission(_abs(BMS_PATH))

	assert_eq(err, ERR_FILE_NOT_FOUND, "a missing referenced .trn fails the open")
	assert_eq(stub.opened_trn, "", "terrain load is not attempted when the .trn is missing")
	assert_string_contains(controller.get_last_status(), ".trn")
	assert_false(controller.is_loaded())


func test_mission_title_is_default_when_empty() -> void:
	var controller := MissionController.new(null)
	assert_eq(controller.get_mission_title(), "Mission", "no mission -> plain label")
	assert_false(controller.is_dirty(), "a fresh controller with no edits is not dirty")
	assert_eq(controller.get_stats().size(), 0, "no placement stats before a load")
	assert_eq(controller.get_selection_summary(), {}, "nothing is selected before a load")


# --- Authoring (Phase 1) ------------------------------------------------------

func test_save_without_mission_is_unavailable() -> void:
	# The save hooks must fail cleanly before anything is loaded (the shell may probe
	# them); they must never touch disk in that state.
	var controller := MissionController.new(null)
	assert_eq(controller.save_current(), ERR_UNAVAILABLE, "save_current with no mission is unavailable")
	assert_eq(controller.save_as("user://nope"), ERR_UNAVAILABLE, "save_as with no mission is unavailable")


func test_handle_viewport_input_is_safe_without_a_mission() -> void:
	# The controller is wired as the viewport input target; with no mission (or no
	# terrain editor) every event must be an inert no-op, never a crash or a dirty.
	var controller := MissionController.new(null)
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	controller.handle_viewport_input(press)
	controller.cancel_drag()
	assert_false(controller.is_dirty(), "input on an empty controller changes nothing")
	assert_eq(controller.get_selection_summary(), {}, "nothing gets selected without a mission")


func test_ray_aabb_entry_hits_and_misses() -> void:
	# Unit-test the analytic picker math used for click selection (no scene needed).
	var controller := MissionController.new(null)
	var cube := AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2))  # 2-unit cube at the origin

	# Straight-on hit: enters the near face at z = -1, starting 10 units back.
	var t_hit: float = controller._ray_aabb_entry(cube, Vector3(0, 0, -10), Vector3(0, 0, 1))
	assert_almost_eq(t_hit, 9.0, 0.001, "ray entering the near face reports the entry distance")

	# Off to the side in X: never crosses the cube.
	assert_eq(controller._ray_aabb_entry(cube, Vector3(5, 5, -10), Vector3(0, 0, 1)), -1.0,
		"a ray that misses returns -1")

	# Pointing away from the box: behind the camera, not a hit.
	assert_eq(controller._ray_aabb_entry(cube, Vector3(0, 0, -10), Vector3(0, 0, -1)), -1.0,
		"a box entirely behind the ray is not a hit")


# --- Authoring (Phase 2): numeric / property edits ----------------------------
# The inspector pushes pos / rot / team / group edits through these controller setters.
# Picking needs a camera + resolved render batch, neither of which the headless fixture
# has, so the tests select via the private _select() seam (consistent with the
# ray-vs-AABB test above) and assert against the data model, which is what the setters
# actually write. The render index being empty is fine: the setters fall through their
# zero-record loops harmlessly and still commit to the record.

func _loaded_with_selection() -> MissionController:
	var stub := StubTerrainEditor.new()
	stub.resource_root = _dvxi5_root()
	stub.world_root = Node3D.new()
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)
	var controller := MissionController.new(stub)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), OK, "the fixture mission opens")
	var buildings := controller.get_mission().get_entities(NovaMissionData.KIND_BUILDING)
	assert_gt(buildings.size(), 0, "the fixture places buildings to select")
	controller._select(NovaMissionData.KIND_BUILDING, int(buildings[0]["index"]))
	return controller


func test_selection_edits_without_a_selection_are_inert() -> void:
	var controller := MissionController.new(null)
	controller.set_selected_team(2)
	controller.set_selected_group(3)
	controller.set_selected_position(Vector3.ONE)
	controller.set_selected_rotation(Vector3(0, 90, 0))
	assert_false(controller.is_dirty(), "the setters do nothing without a selected entity")
	assert_eq(controller.get_selected_entity(), {}, "and report no selection")
	assert_eq(controller.get_selected_position(), Vector3.ZERO)


func test_set_selected_team_marks_dirty_and_reads_back() -> void:
	var controller := _loaded_with_selection()
	var before := int(controller.get_selected_entity().get("team", 0))
	controller.set_selected_team(before + 1)
	assert_true(controller.is_dirty(), "a team edit dirties the mission")
	assert_eq(int(controller.get_selected_entity()["team"]), before + 1, "the new team reads back")


func test_set_selected_group_reads_back() -> void:
	var controller := _loaded_with_selection()
	controller.set_selected_group(7)
	assert_true(controller.is_dirty())
	assert_eq(int(controller.get_selected_entity()["group"]), 7, "the new group reads back")


func test_set_selected_property_edits_a_behavior_field() -> void:
	# The generic setter the Behavior panel drives: a non-team/group field writes and reads
	# back, and (unlike team / group) is not clamped to a byte.
	var controller := _loaded_with_selection()
	controller.set_selected_property("waypoint_id", 6)
	assert_true(controller.is_dirty(), "a behavior-field edit dirties the mission")
	assert_eq(int(controller.get_selected_entity()["waypoint_id"]), 6, "the new waypoint_id reads back")

	controller.set_selected_property("max_engagement_distance", 5000)
	assert_eq(int(controller.get_selected_entity()["max_engagement_distance"]), 5000,
		"an int32 behavior field is not clamped to 0..255")


func test_set_selected_property_without_a_selection_is_inert() -> void:
	var controller := MissionController.new(null)
	controller.set_selected_property("waypoint_id", 3)
	assert_false(controller.is_dirty(), "the generic setter does nothing without a selected entity")


func test_set_selected_property_edit_is_undoable() -> void:
	var controller := _loaded_with_selection()
	var entity := controller.get_selected_entity()
	var kind := int(entity["kind"])
	var index := int(entity["index"])
	var before := int(entity.get("waypoint_id", 0))
	controller.set_selected_property("waypoint_id", before + 9)
	assert_true(controller.can_undo(), "a behavior-field edit is its own undo step")
	# Read from the record, not the selection: undo re-bakes and drops the selection.
	assert_eq(int(controller.get_mission().get_entity(kind, index)["waypoint_id"]), before + 9,
		"precondition: the edit applied to the record")
	controller.undo()
	assert_eq(int(controller.get_mission().get_entity(kind, index)["waypoint_id"]), before,
		"undo restores the prior waypoint_id")


func test_set_selected_position_persists_to_record() -> void:
	var controller := _loaded_with_selection()
	var target := controller.get_selected_position() + Vector3(5.0, 0.0, -3.0)
	controller.set_selected_position(target)
	assert_true(controller.is_dirty())
	var after: Vector3 = controller.get_selected_entity()["position"]
	assert_almost_eq(after.x, target.x, 0.02, "edited X persists to the record")
	assert_almost_eq(after.y, target.y, 0.02, "edited Y persists to the record")
	assert_almost_eq(after.z, target.z, 0.02, "edited Z persists to the record")


func test_set_selected_rotation_persists_to_record() -> void:
	var controller := _loaded_with_selection()
	controller.set_selected_rotation(Vector3(0, 90, 0))
	assert_true(controller.is_dirty())
	var rot: Vector3 = controller.get_selected_entity()["rotation_deg"]
	assert_almost_eq(rot.y, 90.0, 0.5, "edited yaw persists (rounded to the format's integer degrees)")


func test_set_selected_position_round_trips_under_an_offset_container() -> void:
	# Regression: position editing must not route through the objects container's world
	# transform. With the container parented under an offset/scaled world root, a numeric
	# position edit must still write exactly the mission-space value it was given.
	var stub := StubTerrainEditor.new()
	stub.resource_root = _dvxi5_root()
	stub.world_root = Node3D.new()
	stub.world_root.transform = Transform3D(Basis().scaled(Vector3(2, 2, 2)), Vector3(1000, 50, -200))
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)
	var controller := MissionController.new(stub)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), OK)
	var buildings := controller.get_mission().get_entities(NovaMissionData.KIND_BUILDING)
	controller._select(NovaMissionData.KIND_BUILDING, int(buildings[0]["index"]))

	var target := Vector3(321.0, 12.0, -654.0)
	controller.set_selected_position(target)
	var after: Vector3 = controller.get_selected_entity()["position"]
	assert_almost_eq(after.x, target.x, 0.02, "X is the value given, independent of the container transform")
	assert_almost_eq(after.y, target.y, 0.02, "Y is the value given, independent of the container transform")
	assert_almost_eq(after.z, target.z, 0.02, "Z is the value given, independent of the container transform")


# --- Authoring (Phase 3): place new objects -----------------------------------
# The palette arms an items.def item; a terrain click places a new instance. These
# drive the data path (the headless fixture resolves no .3di, so nothing renders, but
# add_entity still writes the record and the kind mapping still applies). The dvxi5
# fixture dir has no items.def, so the item database is injected into the retained
# placer (a white-box seam, like _select above) to give the palette + mapping data.

const ITEMS_PATH := "res://../fixtures/def/items.def"


func _loaded_with_item_db() -> MissionController:
	var stub := StubTerrainEditor.new()
	stub.resource_root = _dvxi5_root()
	stub.world_root = Node3D.new()
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)
	var controller := MissionController.new(stub)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), OK, "the fixture mission opens")
	var db := NovaItemDatabase.new()
	assert_eq(db.load(_abs(ITEMS_PATH)), OK, "the items.def fixture loads")
	# The dvxi5 fixture dir carries no items.def, so the open left the placer's db null;
	# inject the fixture db so the palette + kind mapping have real item types.
	controller._placer.item_db = db
	return controller


func test_kind_for_item_type_matches_shipping_data() -> void:
	# The empirically verified 1:1 mapping (185k entities across 114 JO missions). The
	# non-obvious part is Decoration AND Foliage sharing the Building list with Building.
	var c := MissionController.new(null)
	assert_eq(c._kind_for_item_type(NovaItemDatabase.TYPE_PERSON), NovaMissionData.KIND_ORGANIC, "person -> organic")
	assert_eq(c._kind_for_item_type(NovaItemDatabase.TYPE_BUILDING), NovaMissionData.KIND_BUILDING, "building -> building")
	assert_eq(c._kind_for_item_type(NovaItemDatabase.TYPE_DECORATION), NovaMissionData.KIND_BUILDING, "decoration -> building")
	assert_eq(c._kind_for_item_type(NovaItemDatabase.TYPE_FOLIAGE), NovaMissionData.KIND_BUILDING, "foliage -> building")
	assert_eq(c._kind_for_item_type(NovaItemDatabase.TYPE_MARKER), NovaMissionData.KIND_MARKER, "marker -> marker")
	for t in [NovaItemDatabase.TYPE_VEHICLE, NovaItemDatabase.TYPE_OBJECT, NovaItemDatabase.TYPE_POWERUP, NovaItemDatabase.TYPE_UNKNOWN]:
		assert_eq(c._kind_for_item_type(t), NovaMissionData.KIND_ITEM, "type %d -> item" % t)


func test_get_placeable_items_excludes_markers() -> void:
	var controller := _loaded_with_item_db()
	var items := controller.get_placeable_items()
	assert_eq(items.size(), 12, "the 13-item fixture yields 12 placeable (its one marker is excluded)")
	for it in items:
		assert_ne(int(it["type"]), NovaItemDatabase.TYPE_MARKER, "no marker is offered for placement")
		assert_true(it.has("id") and it.has("display_name"), "each palette entry has id + name")


func test_open_mission_auto_resolves_items_db_for_the_palette() -> void:
	# The production load path with NO injection: open_mission builds the placer, and the
	# palette reaches items.def via _ensure_item_db -> resolve_file("items.def"). The
	# dvxi5 fixture dir carries a copy of items.def for exactly this; a filename typo or
	# resolution miss in that chain would leave the palette empty and fail here.
	var stub := StubTerrainEditor.new()
	stub.resource_root = _dvxi5_root()
	stub.world_root = Node3D.new()
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)
	var controller := MissionController.new(stub)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), OK)
	# No db injection here: the items must come from the auto-resolve chain.
	assert_eq(controller.get_placeable_items().size(), 12,
		"items.def auto-resolves from the resource root (13 items, 1 marker excluded)")


func test_arm_and_disarm_placement() -> void:
	var controller := _loaded_with_item_db()
	assert_false(controller.is_placement_armed(), "nothing is armed initially")
	controller.arm_placement(102001)  # Guard Tower (building)
	assert_true(controller.is_placement_armed(), "arming a known item enters placement mode")
	assert_eq(controller.get_placement_item_id(), 102001, "the armed id is exposed")
	controller.disarm_placement()
	assert_false(controller.is_placement_armed(), "disarm leaves placement mode")


func test_arm_rejects_unknown_and_marker_items() -> void:
	var controller := _loaded_with_item_db()
	controller.arm_placement(999999)  # not in items.def
	assert_false(controller.is_placement_armed(), "an unknown id cannot be armed")
	controller.arm_placement(100001)  # Marker Alpha (type marker, mesh-less)
	assert_false(controller.is_placement_armed(), "a marker cannot be armed (no mesh)")


func test_place_entity_routes_to_the_kind_its_type_maps_to() -> void:
	var controller := _loaded_with_item_db()
	var mission := controller.get_mission()
	var hit := Vector3(50.0, 10.0, -50.0)

	# Building -> Building list.
	var buildings := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	assert_true(controller.place_entity_at_world(102001, hit), "placing a building succeeds")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), buildings + 1, "a building lands in the Building list")

	# Foliage -> ALSO the Building list (the verified non-obvious case).
	var b2 := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	assert_true(controller.place_entity_at_world(103001, hit), "placing foliage succeeds")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), b2 + 1, "foliage lands in the Building list, not Item")

	# Vehicle -> Item list.
	var items := mission.get_entity_count(NovaMissionData.KIND_ITEM)
	assert_true(controller.place_entity_at_world(101291, hit), "placing a vehicle succeeds")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_ITEM), items + 1, "a vehicle lands in the Item list")

	# Person -> Organic list.
	var organics := mission.get_entity_count(NovaMissionData.KIND_ORGANIC)
	assert_true(controller.place_entity_at_world(105311, hit), "placing a person succeeds")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_ORGANIC), organics + 1, "a person lands in the Organic list")

	assert_true(controller.is_dirty(), "placement dirties the mission")


func test_place_entity_selects_the_new_entity_at_the_hit_point() -> void:
	var controller := _loaded_with_item_db()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	var hit := Vector3(120.0, 5.0, -80.0)

	assert_true(controller.place_entity_at_world(102001, hit))
	var sel := controller.get_selection_summary()
	assert_eq(int(sel.get("kind", -1)), NovaMissionData.KIND_BUILDING, "the new entity is selected")
	assert_eq(int(sel.get("index", -1)), before, "and it is the just-appended (last) one")

	# The stored mission-space position is the inverse of the world hit through the
	# objects container (identity here), so it must equal godot_to_bms_position(hit).
	var expected: Vector3 = Placer.godot_to_bms_position(hit)
	var stored: Vector3 = controller.get_selected_entity()["position"]
	assert_almost_eq(stored.x, expected.x, 0.05, "placed X maps back from the world hit")
	assert_almost_eq(stored.y, expected.y, 0.05, "placed Y maps back from the world hit")
	assert_almost_eq(stored.z, expected.z, 0.05, "placed Z maps back from the world hit")


func test_place_entity_without_a_mission_is_inert() -> void:
	var controller := MissionController.new(null)
	assert_false(controller.place_entity_at_world(102001, Vector3.ONE), "no mission -> placement fails")
	assert_false(controller.is_dirty(), "and nothing is dirtied")


func test_armed_left_click_places_via_the_viewport_path() -> void:
	# The viewport input router forwards a left-press; while armed that must place (not
	# select/drag). Uses the real raycast seam on the stub terrain editor.
	var controller := _loaded_with_item_db()
	var mission := controller.get_mission()
	controller.arm_placement(102001)
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)

	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	press.position = Vector2(64, 64)
	controller.handle_viewport_input(press)

	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before + 1,
		"an armed left-click placed a new building")
	assert_true(controller.is_placement_armed(), "placement stays armed so several can be placed")


func test_right_click_disarms_placement() -> void:
	var controller := _loaded_with_item_db()
	controller.arm_placement(102001)
	var rclick := InputEventMouseButton.new()
	rclick.button_index = MOUSE_BUTTON_RIGHT
	rclick.pressed = true
	controller.handle_viewport_input(rclick)
	assert_false(controller.is_placement_armed(), "a right-click drops the placement tool")


func test_escape_disarms_placement() -> void:
	var controller := _loaded_with_item_db()
	controller.arm_placement(102001)
	assert_true(controller.is_placement_armed())
	var esc := InputEventKey.new()
	esc.pressed = true
	esc.keycode = KEY_ESCAPE
	controller.handle_viewport_input(esc)
	assert_false(controller.is_placement_armed(), "Escape drops the placement tool")


func test_armed_click_off_terrain_places_nothing() -> void:
	# An armed left-click that misses the terrain (an invalid raycast hit) must place
	# nothing and stay armed, so a stray click into the sky cannot drop a garbage object.
	var controller := _loaded_with_item_db()
	var mission := controller.get_mission()
	controller.arm_placement(102001)
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	controller.terrain_editor.terrain_hit_valid = false  # the raycast now reports a miss
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	press.position = Vector2(64, 64)
	controller.handle_viewport_input(press)
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before,
		"an armed click off the terrain places nothing")
	assert_true(controller.is_placement_armed(), "and the tool stays armed")


func test_place_entity_round_trips_under_an_offset_container() -> void:
	# Regression (the Phase 2 bug class): the world hit must be inverted through the
	# objects container before being stored, so an offset/scaled world root must not leak
	# into the stored mission-space position. The expected value is derived through the
	# SAME container inverse the production path applies -- deriving it from the bare hit
	# would pass even if the inverse were dropped, which is what makes this discriminating.
	var stub := StubTerrainEditor.new()
	stub.resource_root = _dvxi5_root()
	stub.world_root = Node3D.new()
	stub.world_root.transform = Transform3D(Basis().scaled(Vector3(2, 2, 2)), Vector3(1000, 50, -200))
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)
	var controller := MissionController.new(stub)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), OK)
	var db := NovaItemDatabase.new()
	assert_eq(db.load(_abs(ITEMS_PATH)), OK)
	controller._placer.item_db = db

	var hit := Vector3(120.0, 5.0, -80.0)
	assert_true(controller.place_entity_at_world(102001, hit))

	var container: Node3D = stub.world_root.get_node("MissionObjects")
	var local: Vector3 = container.global_transform.affine_inverse() * hit
	var expected: Vector3 = Placer.godot_to_bms_position(local)
	var stored: Vector3 = controller.get_selected_entity()["position"]
	assert_almost_eq(stored.x, expected.x, 0.05, "placed X accounts for the container transform")
	assert_almost_eq(stored.y, expected.y, 0.05, "placed Y accounts for the container transform")
	assert_almost_eq(stored.z, expected.z, 0.05, "placed Z accounts for the container transform")


# --- Authoring (Phase 4): delete the selected entity --------------------------
# delete_selected removes the selected entity via the binding, then re-bakes the world
# (which resets the selection). The headless dvxi5 fixture resolves no .3di, so nothing
# renders, but the data path -- remove + count drop + deselect + dirty, all driven off
# the retained placer -- is exactly what runs in the editor.

func test_delete_selected_removes_clears_and_dirties() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	assert_false(controller.get_selection_summary().is_empty(), "precondition: a building is selected")

	assert_true(controller.delete_selected(), "deleting the selected entity succeeds")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before - 1, "the building count drops by one")
	assert_true(controller.is_dirty(), "a delete dirties the mission")
	assert_eq(controller.get_selection_summary(), {}, "the selection clears after the delete")


func test_delete_selected_without_a_selection_is_inert() -> void:
	var controller := _loaded_with_selection()
	controller._deselect()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	assert_false(controller.delete_selected(), "delete with nothing selected is a no-op")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before, "no entity is removed")


func test_delete_selected_without_a_mission_is_inert() -> void:
	var controller := MissionController.new(null)
	assert_false(controller.delete_selected(), "no mission -> delete fails")
	assert_false(controller.is_dirty(), "and nothing is dirtied")


func test_delete_reuses_the_retained_placer() -> void:
	# The re-bake must reuse the retained placer so its model + batch caches survive a
	# delete; building a fresh placer would re-harvest every model. Pin the identity.
	var controller := _loaded_with_selection()
	var placer_before = controller._placer
	assert_not_null(placer_before, "a placer is retained after open")
	assert_true(controller.delete_selected())
	assert_eq(controller._placer, placer_before, "the delete re-bakes through the same placer instance")


func test_delete_key_deletes_via_the_viewport_path() -> void:
	# The viewport input router forwards a Delete key; with an entity selected that must
	# remove it -- the same controller path the inspector's Delete button uses.
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	var key := InputEventKey.new()
	key.pressed = true
	key.keycode = KEY_DELETE
	controller.handle_viewport_input(key)
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before - 1, "Delete removed the selected building")
	assert_eq(controller.get_selection_summary(), {}, "and cleared the selection")


func test_delete_key_with_no_selection_is_inert() -> void:
	var controller := _loaded_with_selection()
	controller._deselect()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	var key := InputEventKey.new()
	key.pressed = true
	key.keycode = KEY_DELETE
	controller.handle_viewport_input(key)
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before, "Delete with nothing selected removes nothing")


func test_delete_key_while_armed_is_inert() -> void:
	# Arming a placement tool clears the selection, so a Delete keystroke mid-placement must
	# not destroy a phantom selection. This pins the arm->deselect coupling the Delete-key
	# branch relies on: a regression where arm_placement stopped deselecting would otherwise
	# let an armed Delete silently delete an entity, and nothing else would fail.
	var controller := _loaded_with_item_db()
	var mission := controller.get_mission()
	var buildings := mission.get_entities(NovaMissionData.KIND_BUILDING)
	controller._select(NovaMissionData.KIND_BUILDING, int(buildings[0]["index"]))
	assert_false(controller.get_selection_summary().is_empty(), "precondition: a building is selected")

	controller.arm_placement(102001)  # Guard Tower (building)
	assert_true(controller.is_placement_armed(), "the placement tool is armed")
	assert_eq(controller.get_selection_summary(), {}, "arming cleared the selection")

	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	var key := InputEventKey.new()
	key.pressed = true
	key.keycode = KEY_DELETE
	controller.handle_viewport_input(key)
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before, "Delete while armed removes nothing")
	assert_true(controller.is_placement_armed(), "and the placement tool stays armed")


func test_delete_key_is_suppressed_while_a_text_field_has_focus() -> void:
	# A focused text field (e.g. mid-edit of a coordinate SpinBox) owns the keyboard, so a
	# viewport Delete/Backspace must not destroy the selected entity behind the user's back.
	# Mirrors the focus-owner guard the credits / font editors use on the same router path.
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)

	var field := LineEdit.new()
	add_child_autofree(field)
	field.grab_focus()
	if not field.has_focus():
		pass_test("this headless build does not route GUI focus; the focus guard is exercised manually")
		return

	var key := InputEventKey.new()
	key.pressed = true
	key.keycode = KEY_DELETE
	controller.handle_viewport_input(key)
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before,
		"Delete is inert while a text field has focus (the field, not the viewport, owns the key)")
	assert_false(controller.get_selection_summary().is_empty(), "and the selection survives")


func test_delete_rebakes_pickable_index_and_frees_the_selection_box() -> void:
	# The render-side contract of the structural re-bake: deleting an entity rebuilds the
	# whole container, so the pickable index is re-derived against the post-delete (shifted)
	# record and the old selection box is freed with the container. Headless .3di does not
	# resolve, so seed the placer's per-graphic batch cache with a dummy mesh (the placer
	# test technique) to get REAL MultiMesh batches + pickable records to delete against.
	var controller := _loaded_with_item_db()
	var mission := controller.get_mission()
	var mesh := BoxMesh.new()
	mesh.size = Vector3(2, 2, 2)
	# 105004 is the committed static (no-anim_def) fixture item; its graphic is StaticCrate1.
	controller._placer._static_batch_cache["StaticCrate1"] = [{
		"mesh": mesh, "material": null, "offset": Transform3D.IDENTITY, "submesh": 0,
	}]

	# Place two instances so the survivor's index must shift down when the first is deleted.
	assert_true(controller.place_entity_at_world(105004, Vector3(10, 0, -10)))
	var first := controller.get_selection_summary()
	var kind := int(first["kind"])
	var first_index := int(first["index"])
	assert_true(controller.place_entity_at_world(105004, Vector3(20, 0, -20)))
	assert_eq(int(controller.get_selection_summary()["index"]), first_index + 1, "the two placements are consecutive")
	assert_eq(controller._pickable.size(), 2, "both rendered placements are in the pickable index")

	# Reselect the first, capture its (container-child) selection box, then delete it.
	controller._select(kind, first_index)
	var box = controller._selection_box
	assert_true(is_instance_valid(box), "selecting a rendered entity builds a selection box")
	var count_before := mission.get_entity_count(kind)

	assert_true(controller.delete_selected())
	assert_eq(mission.get_entity_count(kind), count_before - 1, "the entity is removed from its kind's list")
	assert_true(box.is_queued_for_deletion(), "the old selection box is freed with the container on re-bake")

	# The re-bake re-derived the pickable index from the post-delete record: a record for the
	# survivor now sits at the deleted index (it shifted down), and none points past the list.
	var found_survivor := false
	for rec in controller._pickable:
		var rk := int(rec["kind"])
		assert_lt(int(rec["index"]), mission.get_entity_count(rk), "no pickable record points past its kind's list")
		if rk == kind and int(rec["index"]) == first_index:
			found_survivor = true
	assert_true(found_survivor, "the surviving instance shifted into the deleted index and was re-harvested")


func test_open_loads_then_reconcile_drops_on_terrain_swap() -> void:
	var stub := StubTerrainEditor.new()
	stub.resource_root = _dvxi5_root()
	stub.world_root = Node3D.new()
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)

	var controller := MissionController.new(stub)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), OK, "mission opens when its terrain (dvxi5) resolves and loads")
	assert_true(controller.is_loaded())
	assert_ne(stub.opened_trn, "", "the referenced terrain was loaded through the editor")

	# Terrain still mounted -> reconcile keeps the mission.
	controller.reconcile_with_terrain()
	assert_true(controller.is_loaded(), "reconcile keeps a mission while its terrain is mounted")

	# A different terrain was opened underneath -> reconcile drops the stale mission.
	stub.current_trn_path = "X:/some/other.trn"
	controller.reconcile_with_terrain()
	assert_false(controller.is_loaded(), "reconcile clears a mission whose terrain was swapped out")
	assert_eq(controller.get_current_path(), "", "the cleared mission forgets its path")


func test_failed_terrain_load_clears_prior_mission() -> void:
	var stub := StubTerrainEditor.new()
	stub.resource_root = _dvxi5_root()
	stub.world_root = Node3D.new()
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)

	var controller := MissionController.new(stub)
	assert_eq(controller.open_mission(_abs(BMS_PATH)), OK)
	assert_true(controller.is_loaded())

	# A second open whose terrain resolves but fails to load wipes the editor's
	# terrain; the controller must not keep describing the now-gone prior mission.
	stub.open_trn_result = ERR_CANT_OPEN
	var err := controller.open_mission(_abs(BMS_PATH))
	assert_eq(err, ERR_CANT_OPEN, "a terrain load failure surfaces as the open error")
	assert_false(controller.is_loaded(), "a failed open clears the prior mission state")
	assert_string_contains(controller.get_last_status(), "Could not load")


# --- Authoring (Phase 5): undo / redo -----------------------------------------
# Undo/redo are whole-document byte snapshots through the real serializer. Each edit
# pushes the pre-edit state; undo/redo swap the current state onto the opposite stack,
# restore the popped snapshot, and re-bake. Headless .3di does not resolve, so these
# assert via the mission document, the selection summary, and the controller's stacks
# (never rendered transforms), the same way the Phase 4 delete tests do. The re-bake on
# restore runs through the retained placer over the stub world root.

func test_undo_reverts_a_placement_and_deselects() -> void:
	var controller := _loaded_with_item_db()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	assert_false(controller.can_undo(), "a freshly opened mission has no undo history")

	assert_true(controller.place_entity_at_world(102001, Vector3(50, 10, -50)))
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before + 1, "the placement landed")
	assert_true(controller.can_undo(), "a placement is undoable")

	controller.undo()
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before, "undo removes the placed entity")
	assert_eq(controller.get_selection_summary(), {}, "undo drops the selection (indices may have shifted)")
	assert_false(controller.can_undo(), "the only step was consumed")
	assert_true(controller.can_redo(), "and is now redoable")


func test_redo_replays_a_placement() -> void:
	var controller := _loaded_with_item_db()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_BUILDING)
	controller.place_entity_at_world(102001, Vector3(50, 10, -50))
	controller.undo()
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before, "precondition: undone")

	controller.redo()
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before + 1, "redo re-applies the placement")
	assert_true(controller.can_undo(), "the redone placement is undoable again")
	assert_false(controller.can_redo(), "and the redo step is consumed")


func test_undo_restores_a_deleted_entity() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	var buildings := mission.get_entities(NovaMissionData.KIND_BUILDING)
	var before := buildings.size()
	var deleted_item := int(buildings[0]["item_id"])
	var deleted_pos: Vector3 = buildings[0]["position"]
	controller._select(NovaMissionData.KIND_BUILDING, int(buildings[0]["index"]))

	assert_true(controller.delete_selected())
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before - 1, "precondition: deleted")

	controller.undo()
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_BUILDING), before, "undo restores the deleted entity")
	var restored := mission.get_entity(NovaMissionData.KIND_BUILDING, 0)
	assert_eq(int(restored["item_id"]), deleted_item, "the restored entity is back at its original index")
	var rp: Vector3 = restored["position"]
	assert_almost_eq(rp.x, deleted_pos.x, 0.02, "with its original X")
	assert_almost_eq(rp.z, deleted_pos.z, 0.02, "and its original Z")


func test_undo_restores_a_moved_position() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	var kind := NovaMissionData.KIND_BUILDING
	var index := int(mission.get_entities(kind)[0]["index"])
	controller._select(kind, index)
	var original: Vector3 = mission.get_entity(kind, index)["position"]

	controller.set_selected_position(original + Vector3(10, 0, -5))
	controller._flush_edit()  # close the transform session so it is on the undo stack
	var moved: Vector3 = mission.get_entity(kind, index)["position"]
	assert_almost_eq(moved.x, original.x + 10.0, 0.05, "precondition: the move applied")

	controller.undo()
	var restored: Vector3 = mission.get_entity(kind, index)["position"]
	assert_almost_eq(restored.x, original.x, 0.05, "undo restores the original X")
	assert_almost_eq(restored.z, original.z, 0.05, "undo restores the original Z")


func test_multi_axis_edit_coalesces_to_one_step() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	var kind := NovaMissionData.KIND_BUILDING
	var index := int(mission.get_entities(kind)[0]["index"])
	controller._select(kind, index)
	var p := controller.get_selected_position()

	# A run of axis edits + a rotation on the same entity is one editing session.
	controller.set_selected_position(Vector3(p.x + 1.0, p.y, p.z))
	controller.set_selected_position(Vector3(p.x + 1.0, p.y + 2.0, p.z))
	controller.set_selected_position(Vector3(p.x + 1.0, p.y + 2.0, p.z + 3.0))
	controller.set_selected_rotation(Vector3(0, 45, 0))
	assert_eq(controller._undo_stack.size(), 0, "the open session is not on the stack until it is flushed")

	controller._flush_edit()
	assert_eq(controller._undo_stack.size(), 1, "X/Y/Z and a rotation coalesce into a single undo step")

	# One undo reverts the whole session.
	controller.undo()
	var after: Vector3 = mission.get_entity(kind, index)["position"]
	assert_almost_eq(after.x, p.x, 0.05, "one undo reverts every coalesced axis")
	assert_almost_eq(after.z, p.z, 0.05, "including Z")


func test_an_edit_session_with_no_change_pushes_no_step() -> void:
	# The contract the drag path and a plain click rely on: begin..commit with no change
	# (a click that selects but does not move) adds nothing; a cancelled drag likewise.
	var controller := _loaded_with_selection()
	controller.begin_edit()
	controller.commit_edit()
	assert_eq(controller._undo_stack.size(), 0, "a session that changed nothing adds no undo step")
	controller.begin_edit()
	controller.cancel_drag()
	assert_eq(controller._undo_stack.size(), 0, "a cancelled drag adds no undo step")


func test_a_new_edit_clears_the_redo_stack() -> void:
	var controller := _loaded_with_item_db()
	controller.place_entity_at_world(102001, Vector3(10, 0, -10))
	controller.undo()
	assert_true(controller.can_redo(), "the undone placement is redoable")

	# A fresh edit invalidates the redo history.
	controller.place_entity_at_world(102001, Vector3(20, 0, -20))
	assert_false(controller.can_redo(), "a new edit clears the redo stack")


func test_undo_unwinds_edits_in_reverse_order_across_kinds() -> void:
	var controller := _loaded_with_item_db()
	var m := controller.get_mission()
	var b0 := m.get_entity_count(NovaMissionData.KIND_BUILDING)
	var i0 := m.get_entity_count(NovaMissionData.KIND_ITEM)

	controller.place_entity_at_world(102001, Vector3(10, 0, -10))  # building
	controller.place_entity_at_world(101291, Vector3(20, 0, -20))  # vehicle -> item
	assert_eq(m.get_entity_count(NovaMissionData.KIND_ITEM), i0 + 1, "precondition: item placed")

	controller.undo()
	assert_eq(m.get_entity_count(NovaMissionData.KIND_ITEM), i0, "first undo removes the most recent edit (the item)")
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), b0 + 1, "and leaves the earlier building")

	controller.undo()
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), b0, "second undo removes the building")


func test_ctrl_z_and_ctrl_y_drive_undo_redo_via_the_viewport() -> void:
	var controller := _loaded_with_item_db()
	var m := controller.get_mission()
	var before := m.get_entity_count(NovaMissionData.KIND_BUILDING)
	controller.place_entity_at_world(102001, Vector3(10, 0, -10))

	controller.handle_viewport_input(_ctrl_key(KEY_Z))
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before, "Ctrl+Z undid the placement")

	controller.handle_viewport_input(_ctrl_key(KEY_Y))
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before + 1, "Ctrl+Y redid the placement")

	# Ctrl+Shift+Z is the other redo binding.
	controller.handle_viewport_input(_ctrl_key(KEY_Z))
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before, "Ctrl+Z undid again")
	controller.handle_viewport_input(_ctrl_key(KEY_Z, true))
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), before + 1, "Ctrl+Shift+Z redid the placement")


func test_ctrl_z_is_suppressed_while_a_text_field_has_focus() -> void:
	# A focused SpinBox / LineEdit keeps its own text undo, so a viewport Ctrl+Z must not
	# reach the controller. Mirrors the Delete-key focus guard test above.
	var controller := _loaded_with_item_db()
	var m := controller.get_mission()
	controller.place_entity_at_world(102001, Vector3(10, 0, -10))
	var after_place := m.get_entity_count(NovaMissionData.KIND_BUILDING)

	var field := LineEdit.new()
	add_child_autofree(field)
	field.grab_focus()
	if not field.has_focus():
		pass_test("this headless build does not route GUI focus; the focus guard is exercised manually")
		return

	controller.handle_viewport_input(_ctrl_key(KEY_Z))
	assert_eq(m.get_entity_count(NovaMissionData.KIND_BUILDING), after_place,
		"Ctrl+Z is inert while a text field has focus")


func test_open_clears_the_undo_history() -> void:
	var controller := _loaded_with_item_db()
	controller.place_entity_at_world(102001, Vector3(10, 0, -10))
	assert_true(controller.can_undo(), "precondition: a placement is undoable")

	assert_eq(controller.open_mission(_abs(BMS_PATH)), OK, "re-opening the mission succeeds")
	assert_false(controller.can_undo(), "re-opening clears the undo history")
	assert_false(controller.can_redo(), "and the redo history")


func test_undo_to_the_original_clears_the_dirty_flag() -> void:
	var controller := _loaded_with_selection()
	assert_false(controller.is_dirty(), "a freshly opened mission is clean")
	var before := int(controller.get_selected_entity().get("team", 0))

	controller.set_selected_team(before + 1)
	assert_true(controller.is_dirty(), "an edit dirties the mission")

	controller.undo()
	assert_false(controller.is_dirty(), "undoing back to the opened bytes clears the dirty marker")


func test_undo_and_redo_with_empty_history_are_inert() -> void:
	var controller := _loaded_with_selection()
	assert_false(controller.can_undo())
	assert_false(controller.can_redo())
	controller.undo()
	controller.redo()
	assert_false(controller.is_dirty(), "undo/redo with no history changes nothing")
	assert_true(controller.is_loaded(), "and the mission is untouched")


# Build a Ctrl(+Shift) key-down event for the undo/redo shortcut tests.
func _ctrl_key(keycode: int, shift: bool = false) -> InputEventKey:
	var key := InputEventKey.new()
	key.pressed = true
	key.keycode = keycode
	key.ctrl_pressed = true
	key.shift_pressed = shift
	return key


# --- Waypoints (P7b): mode + path + marker selection --------------------------
# Waypoints mode switches the viewport to authoring the active path's markers. Picking
# needs a camera the headless stub does not provide, so (like the object Phase 2 tests)
# these drive the public mode/path API and the white-box marker-select seam, asserting
# against the document + overlay rather than a rendered click. The overlay builds under the
# MissionObjects container the open created, so its pickable index is real.

func _first_empty_path(mission) -> int:
	for s in mission.get_waypoint_summaries():
		if int((s as Dictionary)["marker_count"]) == 0:
			return int((s as Dictionary)["index"])
	return -1


func _marker_item_id(mission) -> int:
	var markers: Array = mission.get_entities(NovaMissionData.KIND_MARKER)
	return int(markers[0]["item_id"]) if not markers.is_empty() else 100001


func test_set_waypoint_mode_enters_and_clears_object_selection() -> void:
	var controller := _loaded_with_selection()  # an object is selected
	assert_false(controller.get_selection_summary().is_empty(), "precondition: an object is selected")
	controller.set_waypoint_mode(true)
	assert_true(controller.is_waypoint_mode(), "the controller enters waypoint mode")
	assert_eq(controller.get_selection_summary(), {}, "entering waypoint mode clears the object selection")
	controller.set_waypoint_mode(false)
	assert_false(controller.is_waypoint_mode(), "and exits back to objects mode")


func test_set_waypoint_mode_focuses_a_populated_path() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	mission.add_waypoint_marker(_first_empty_path(mission), _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	controller.set_waypoint_mode(true)
	var active := controller.get_active_waypoint_path()
	assert_false(active.is_empty(), "entering waypoint mode focuses a path")
	assert_gt(int(active["marker_count"]), 0, "and the focused path has markers")


func test_select_waypoint_path_updates_active() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_waypoint_mode(true)
	var path := _first_empty_path(mission)
	mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(3, 0, -3), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	assert_eq(controller.get_selected_waypoint_path_index(), path, "the chosen path becomes active")
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 1, "and its markers are reported")


func test_get_waypoint_summaries_passthrough() -> void:
	var controller := _loaded_with_selection()
	assert_eq(controller.get_waypoint_summaries().size(), 128, "the controller surfaces all 128 waypoint records")
	assert_eq(MissionController.new(null).get_waypoint_summaries(), [], "no mission -> empty summaries")


func test_waypoint_overlay_harvests_a_pickable_per_active_marker() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_waypoint_mode(true)
	var path := _first_empty_path(mission)
	mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(2, 0, -2), Vector3.ZERO, -1)
	mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(4, 0, -4), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	controller._refresh_waypoint_overlay()  # reflect the just-added markers
	assert_eq(controller.get_selected_waypoint_path_index(), path)
	assert_eq(controller._marker_pickable.size(), 2, "the overlay harvested one pickable per active-path marker")


func test_select_marker_reports_position_and_deselect_clears() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_waypoint_mode(true)
	var path := _first_empty_path(mission)
	var r := mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(7, 1, -7), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	var marker_index := int((r["marker"] as Dictionary)["index"])
	controller._select_marker(path, marker_index)
	var sel := controller.get_selected_marker()
	assert_eq(int(sel["marker_index"]), marker_index, "the selected marker is reported")
	assert_almost_eq((sel["position"] as Vector3).x, 7.0, 0.05, "with its position")
	controller._deselect_marker()
	assert_eq(controller.get_selected_marker(), {}, "deselect clears the marker")


func test_left_press_in_waypoint_mode_does_not_select_objects() -> void:
	# A viewport click in waypoint mode must route to marker picking, never object selection.
	# The headless stub has no camera, so the marker pick misses; the key assertion is that
	# the object-selection path did not run.
	var controller := _loaded_with_item_db()
	controller.set_waypoint_mode(true)
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	press.position = Vector2(64, 64)
	controller.handle_viewport_input(press)
	assert_eq(controller.get_selection_summary(), {}, "a viewport click in waypoint mode selects no object")


# --- Waypoints (P7c): path flags + ordered marker selection -------------------

func test_set_waypoint_flags_sets_and_is_undoable() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_waypoint_mode(true)
	var path := _first_empty_path(mission)
	mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)

	# A fresh path loops (DoesNotLoop clear) with no team. Turn loop off + flag it blue.
	controller.set_waypoint_flags(false, true, false)
	var after := controller.get_active_waypoint_path()
	assert_eq(int(after["flags"]) & NovaMissionData.WP_FLAG_DOES_NOT_LOOP, NovaMissionData.WP_FLAG_DOES_NOT_LOOP,
		"loop off sets the DoesNotLoop bit")
	assert_eq(int(after["flags"]) & NovaMissionData.WP_FLAG_BLUE_TEAM, NovaMissionData.WP_FLAG_BLUE_TEAM,
		"the blue team flag is set")
	assert_true(controller.is_dirty(), "a flag edit dirties the mission")

	controller.undo()
	var reverted := controller.get_active_waypoint_path()
	assert_eq(int(reverted["flags"]) & NovaMissionData.WP_FLAG_DOES_NOT_LOOP, 0,
		"undo restores the looping flag (and leaves the marker in place)")


func test_select_waypoint_marker_selects_on_the_active_path() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_waypoint_mode(true)
	var path := _first_empty_path(mission)
	var r := mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(2, 0, -2), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	var marker_index := int((r["marker"] as Dictionary)["index"])
	controller.select_waypoint_marker(marker_index)
	assert_eq(int(controller.get_selected_marker()["marker_index"]), marker_index,
		"select_waypoint_marker selects the marker on the active path")


# --- Waypoints (P7d): add / drag / reorder / delete / clear -------------------

func _wp_ready() -> MissionController:
	# A controller in waypoint mode focused on an empty path, ready to author into.
	var controller := _loaded_with_selection()
	controller.set_waypoint_mode(true)
	controller.select_waypoint_path(_first_empty_path(controller.get_mission()))
	return controller


func test_arm_and_disarm_marker_placement() -> void:
	var controller := _wp_ready()
	assert_false(controller.is_marker_placement_armed(), "nothing armed initially")
	controller.arm_marker_placement()
	assert_true(controller.is_marker_placement_armed(), "the add-marker tool arms")
	controller.disarm_marker_placement()
	assert_false(controller.is_marker_placement_armed(), "and disarms")


func test_add_marker_to_active_path_adds_selects_and_is_undoable() -> void:
	var controller := _wp_ready()
	var mission := controller.get_mission()
	var before := mission.get_entity_count(NovaMissionData.KIND_MARKER)
	assert_true(controller.add_marker_to_active_path_at_world(Vector3(50, 10, -50)), "adding a marker succeeds")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before + 1, "a marker entity was created")
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 1, "and linked into the active path")
	assert_false(controller.get_selected_marker().is_empty(), "the new marker is selected")
	assert_true(controller.is_dirty())
	controller.undo()
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before, "undo removes the added marker")
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 0, "and unlinks it from the path")


func test_armed_left_click_adds_a_marker_via_the_viewport() -> void:
	var controller := _wp_ready()
	var mission := controller.get_mission()
	controller.arm_marker_placement()
	var before := mission.get_entity_count(NovaMissionData.KIND_MARKER)
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	press.position = Vector2(64, 64)
	controller.handle_viewport_input(press)
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before + 1, "an armed click added a marker")
	assert_true(controller.is_marker_placement_armed(), "and the tool stays armed for more")


func test_armed_marker_click_off_terrain_adds_nothing() -> void:
	var controller := _wp_ready()
	var mission := controller.get_mission()
	controller.arm_marker_placement()
	controller.terrain_editor.terrain_hit_valid = false  # the raycast now misses
	var before := mission.get_entity_count(NovaMissionData.KIND_MARKER)
	var press := InputEventMouseButton.new()
	press.button_index = MOUSE_BUTTON_LEFT
	press.pressed = true
	press.position = Vector2(64, 64)
	controller.handle_viewport_input(press)
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before, "a click off the terrain adds no marker")


func test_right_click_disarms_marker_placement() -> void:
	var controller := _wp_ready()
	controller.arm_marker_placement()
	var rclick := InputEventMouseButton.new()
	rclick.button_index = MOUSE_BUTTON_RIGHT
	rclick.pressed = true
	controller.handle_viewport_input(rclick)
	assert_false(controller.is_marker_placement_armed(), "a right-click drops the add-marker tool")


func test_move_selected_marker_reorders_and_is_undoable() -> void:
	var controller := _wp_ready()
	var mission := controller.get_mission()
	var path := controller.get_selected_waypoint_path_index()
	var a := mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	var b := mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(2, 0, -2), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	var a_index := int((a["marker"] as Dictionary)["index"])
	var b_index := int((b["marker"] as Dictionary)["index"])
	controller.select_waypoint_marker(b_index)
	controller.move_selected_marker(-1)  # move b ahead of a
	var indices: PackedInt32Array = controller.get_active_waypoint_path()["marker_indices"]
	assert_eq(indices[0], b_index, "b moved to the front of the path")
	assert_eq(indices[1], a_index, "a is now second")
	controller.undo()
	var reverted: PackedInt32Array = controller.get_active_waypoint_path()["marker_indices"]
	assert_eq(reverted[0], a_index, "undo restores the original order")


func test_move_selected_marker_at_the_end_is_inert() -> void:
	var controller := _wp_ready()
	var mission := controller.get_mission()
	var path := controller.get_selected_waypoint_path_index()
	var a := mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(2, 0, -2), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	controller.select_waypoint_marker(int((a["marker"] as Dictionary)["index"]))  # first marker
	controller.move_selected_marker(-1)  # already first -> no move
	assert_false(controller.can_undo(), "moving the first marker up adds no undo step")


func test_delete_selected_marker_removes_repairs_and_is_undoable() -> void:
	var controller := _wp_ready()
	var mission := controller.get_mission()
	var path := controller.get_selected_waypoint_path_index()
	var a := mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(2, 0, -2), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	var before := mission.get_entity_count(NovaMissionData.KIND_MARKER)
	controller.select_waypoint_marker(int((a["marker"] as Dictionary)["index"]))
	assert_true(controller.delete_selected_marker(), "deleting the selected marker succeeds")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before - 1, "the marker entity is removed")
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 1, "and dropped from the path (repaired)")
	assert_eq(controller.get_selected_marker(), {}, "the marker selection clears")
	controller.undo()
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before, "undo restores the deleted marker")
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 2, "and re-links it into the path")


func test_clear_active_path_removes_markers_and_is_undoable() -> void:
	var controller := _wp_ready()
	var mission := controller.get_mission()
	var path := controller.get_selected_waypoint_path_index()
	mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(2, 0, -2), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	var before := mission.get_entity_count(NovaMissionData.KIND_MARKER)
	assert_true(controller.clear_active_path(), "clearing the path succeeds")
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 0, "the path is now empty")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before - 2, "its markers are deleted (no orphans)")
	controller.undo()
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 2, "undo restores the path's markers")
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before, "and the marker entities")


func test_delete_key_deletes_marker_in_waypoint_mode() -> void:
	var controller := _wp_ready()
	var mission := controller.get_mission()
	var path := controller.get_selected_waypoint_path_index()
	var r := mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	controller.select_waypoint_marker(int((r["marker"] as Dictionary)["index"]))
	var before := mission.get_entity_count(NovaMissionData.KIND_MARKER)
	var key := InputEventKey.new()
	key.pressed = true
	key.keycode = KEY_DELETE
	controller.handle_viewport_input(key)
	assert_eq(mission.get_entity_count(NovaMissionData.KIND_MARKER), before - 1, "Delete removed the selected marker")
	assert_eq(controller.get_selected_marker(), {}, "and cleared the marker selection")


func test_marker_drag_commits_the_new_position() -> void:
	# Picking needs a camera the headless stub lacks, so white-box the drag: begin it, drag to
	# the stub's terrain hit, release. The container is identity here, so the stored position
	# is godot_to_bms_position(hit).
	var controller := _wp_ready()
	var mission := controller.get_mission()
	var path := controller.get_selected_waypoint_path_index()
	var r := mission.add_waypoint_marker(path, _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	controller.select_waypoint_path(path)
	var marker_index := int((r["marker"] as Dictionary)["index"])
	controller.select_waypoint_marker(marker_index)

	controller._drag_active = true
	controller._drag_moved = false
	controller.begin_edit()
	controller._on_marker_drag(Vector2(10, 10))  # stub raycast -> terrain_hit
	controller._on_marker_left_release()

	var expected: Vector3 = Placer.godot_to_bms_position(controller.terrain_editor.terrain_hit)
	var stored: Vector3 = mission.get_entity(NovaMissionData.KIND_MARKER, marker_index)["position"]
	assert_almost_eq(stored.x, expected.x, 0.05, "the dragged marker's X is written to the record")
	assert_almost_eq(stored.z, expected.z, 0.05, "and its Z")
	assert_true(controller.is_dirty(), "a committed marker drag dirties the mission")
	controller.undo()
	var reverted: Vector3 = mission.get_entity(NovaMissionData.KIND_MARKER, marker_index)["position"]
	assert_almost_eq(reverted.x, 1.0, 0.05, "undo restores the marker's original X")


# --- Waypoints (P7 review fixes) ----------------------------------------------

func test_select_new_waypoint_path_enables_from_scratch_authoring() -> void:
	# Review fix: the path list only shows populated paths, so an all-empty mission needs an
	# entry point to make an empty path active; otherwise Add marker is permanently disabled.
	var controller := _loaded_with_selection()
	controller.set_waypoint_mode(true)
	var idx := controller.select_new_waypoint_path()
	assert_true(idx >= 0, "an empty path is available to start a new route")
	assert_eq(controller.get_selected_waypoint_path_index(), idx, "and it becomes the active path")
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 0, "the new path starts empty")
	# Authoring into the freshly-focused path now works end to end.
	assert_true(controller.add_marker_to_active_path_at_world(Vector3(10, 5, -10)),
		"a marker can be added to the new path")
	assert_eq(int(controller.get_active_waypoint_path()["marker_count"]), 1, "and lands on it")


func test_switching_paths_clears_a_shared_marker_highlight() -> void:
	# Review fix: selecting a marker on path A then switching to path B that references the
	# SAME marker index must not leave a stale overlay highlight. The controller drops the
	# selection AND tells the overlay; the overlay no longer self-re-applies its cache.
	var controller := _wp_ready()  # active = first empty path (A)
	var mission := controller.get_mission()
	var path_a := controller.get_selected_waypoint_path_index()
	var r := mission.add_waypoint_marker(path_a, _marker_item_id(mission), Vector3(1, 0, -1), Vector3.ZERO, -1)
	var m := int((r["marker"] as Dictionary)["index"])
	controller._refresh_waypoint_overlay()  # reflect the new marker on A (A is already active)
	var path_b := controller._first_empty_path()
	assert_true(path_b >= 0 and path_b != path_a, "need a distinct empty path for B")
	# Loaded data can reference one marker from two paths (the editor never authors that).
	mission.set_waypoint_path(path_b, PackedInt32Array([m]), 0)

	controller.select_waypoint_marker(m)
	assert_eq(controller._waypoint_overlay._selected_marker_index, m, "precondition: overlay highlights m on A")

	controller.select_waypoint_path(path_b)  # B shares marker m
	assert_eq(controller.get_selected_marker(), {}, "switching paths drops the marker selection")
	assert_eq(controller._waypoint_overlay._selected_marker_index, -1,
		"and the overlay clears its highlight (no stale bleed onto B's shared marker)")


# --- Polish: action feedback + name resolution (P1 / P2) ----------------------

# Seed the placer's per-graphic batch cache with a dummy mesh so StaticCrate1 (item 105004)
# renders to a real MultiMesh under headless .3di, the same technique the delete test uses.
func _seed_crate_batch(controller) -> void:
	var mesh := BoxMesh.new()
	mesh.size = Vector3.ONE
	controller._placer._static_batch_cache["StaticCrate1"] = [{
		"mesh": mesh, "material": null, "offset": Transform3D.IDENTITY, "submesh": 0,
	}]


func test_place_and_delete_report_status_and_resolve_names() -> void:
	var controller := _loaded_with_item_db()
	_seed_crate_batch(controller)
	var crate_name: String = controller._item_db().get_display_name(105004)
	assert_false(crate_name.is_empty(), "precondition: the items.def fixture names item 105004")

	assert_true(controller.place_entity_at_world(105004, Vector3(10, 0, -10)))
	assert_eq(controller.get_selected_display_name(), crate_name, "the selection resolves its model name")
	assert_string_contains(controller.get_last_status(), "Placed", "placing reports a status")
	assert_string_contains(controller.get_last_status(), crate_name, "and names the placed model")

	assert_true(controller.delete_selected())
	assert_string_contains(controller.get_last_status(), "Deleted", "deleting reports a status")


func test_display_name_is_empty_without_an_item_database() -> void:
	# _loaded_with_selection opens over a dir with no items.def, so the placer carries no
	# database and a name cannot resolve; the inspector then shows the kind + index instead.
	var controller := _loaded_with_selection()
	assert_eq(controller.get_selected_display_name(), "", "no item database -> no resolvable name")


func test_undo_redo_report_status() -> void:
	var controller := _loaded_with_item_db()
	_seed_crate_batch(controller)
	assert_true(controller.place_entity_at_world(105004, Vector3(5, 0, -5)))

	controller.undo()
	assert_string_contains(controller.get_last_status(), "Undid", "undo reports what happened")
	controller.redo()
	assert_string_contains(controller.get_last_status(), "Redid", "redo reports what happened")

	controller.undo()  # back to the opened state
	controller.undo()  # nothing left on the stack
	assert_string_contains(controller.get_last_status(), "Nothing to undo", "an exhausted undo says so")


func test_status_reported_signal_fires_on_an_action() -> void:
	var controller := _loaded_with_item_db()
	_seed_crate_batch(controller)
	watch_signals(controller)
	assert_true(controller.place_entity_at_world(105004, Vector3(1, 0, -1)))
	assert_signal_emitted(controller, "status_reported", "placing emits status_reported for the shell to relay")


func test_marker_gizmo_carries_a_route_order_label() -> void:
	# Each in-world marker gizmo carries a 1-based route-order Label3D so the path reads in
	# the viewport (matches the inspector's "1. marker #..." list).
	var overlay = WaypointOverlay.new()
	add_child_autofree(overlay)
	overlay._ensure_built()
	var giz = overlay._make_gizmo(Vector3.ZERO, Color.WHITE, 3)
	add_child_autofree(giz)
	var label: Label3D = null
	for child in giz.get_children():
		if child is Label3D:
			label = child
			break
	assert_not_null(label, "each marker gizmo carries a Label3D order number")
	if label != null:
		assert_eq(label.text, "3", "the label shows the 1-based route order")


# --- Polish: trust / correctness micro-fixes (P3) -----------------------------

func test_off_terrain_drag_reports_and_moves_nothing() -> void:
	# A drag that only ever samples off the terrain leaves the object put, commits no undo
	# step, and tells the user why (rather than silently doing nothing).
	var controller := _loaded_with_selection()  # stub terrain hits are valid by default
	assert_false(controller.is_dirty(), "precondition: a freshly opened mission is clean")
	controller._drag_active = true
	controller._drag_moved = false
	controller._drag_off_terrain = false
	controller.begin_edit()
	controller.terrain_editor.terrain_hit_valid = false  # every raycast now misses
	controller._on_drag(Vector2(5, 5))
	controller._on_left_release()
	assert_string_contains(controller.get_last_status(), "off the terrain", "the miss is explained")
	assert_false(controller.is_dirty(), "an all-off-terrain drag changes nothing")


func test_default_marker_item_id_reuses_an_existing_marker() -> void:
	# Shipped data keeps its own marker type: a new marker copies an existing marker's id when
	# the mission carries one. (The from-scratch DB-scan branch needs a marker-free mission,
	# which the fixture is not, so it is covered by reading rather than asserted here.)
	var controller := _loaded_with_item_db()
	var id := controller._default_marker_item_id()
	assert_gt(id, 0, "a new marker seeds a positive item id")
	var markers := controller.get_mission().get_entities(NovaMissionData.KIND_MARKER)
	if not markers.is_empty():
		assert_eq(id, int((markers[0] as Dictionary)["item_id"]),
			"an existing marker's id is reused so shipped data round-trips")


# --- Area triggers / zones (Phase 2) ------------------------------------------
# Zone authoring is driven through the controller's public mode/zone API. Picking needs a
# camera the headless stub lacks, so (like the marker tests) these white-box the drag seam and
# assert against the document + the overlay's harvested pickables, which build under the real
# MissionObjects container the open created.

func test_set_mode_enters_area_trigger_and_clears_object_selection() -> void:
	var controller := _loaded_with_selection()  # an object is selected
	assert_true(controller.is_objects_mode(), "precondition: objects mode")
	assert_false(controller.get_selection_summary().is_empty(), "precondition: an object is selected")
	controller.set_mode(MissionController.Mode.AREA_TRIGGERS)
	assert_true(controller.is_area_trigger_mode(), "the controller enters area-trigger mode")
	assert_eq(controller.get_mode(), MissionController.Mode.AREA_TRIGGERS, "get_mode reports the new mode")
	assert_eq(controller.get_selection_summary(), {}, "entering the mode clears the object selection")
	assert_false(controller.is_waypoint_mode(), "and is not waypoint mode")


func test_add_area_trigger_default_adds_selects_and_is_undoable() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_mode(MissionController.Mode.AREA_TRIGGERS)
	var before := mission.get_area_trigger_count()
	var idx := controller.add_area_trigger_default()
	assert_eq(idx, before, "the new zone takes the next index")
	assert_eq(mission.get_area_trigger_count(), before + 1, "a zone was added")
	assert_eq(controller.get_selected_zone_index(), idx, "and is selected")
	assert_true(controller.is_dirty(), "adding a zone dirties the mission")
	var zone := controller.get_selected_zone()
	assert_true(bool(zone["active"]), "a fresh zone is active")
	assert_false(bool(zone["constrain_z"]), "with Z unbounded by default")
	controller.undo()
	assert_eq(mission.get_area_trigger_count(), before, "undo removes the added zone")


func test_set_selected_zone_bounds_and_flags_read_back_and_undo() -> void:
	var controller := _loaded_with_selection()
	controller.set_mode(MissionController.Mode.AREA_TRIGGERS)
	var idx := controller.add_area_trigger_default()
	controller.set_selected_zone_bounds(Vector3(-100, -200, -10), Vector3(100, 200, 10))
	var zone := controller.get_selected_zone()
	assert_eq((zone["min"] as Vector3), Vector3(-100, -200, -10), "min reads back")
	assert_eq((zone["max"] as Vector3), Vector3(100, 200, 10), "max reads back")
	controller.set_selected_zone_flags(true, true)
	zone = controller.get_selected_zone()
	assert_true(bool(zone["constrain_z"]), "constrain_z set")
	assert_true(bool(zone["active"]), "active set")
	controller.set_selected_zone_flags(false, false)
	zone = controller.get_selected_zone()
	assert_false(bool(zone["active"]), "active cleared")
	controller.undo()  # undo the flag clear (rebakes + drops the zone selection, like object undo)
	# Re-fetch by index from the mission: undo restores the flags-true state on the zone.
	var reverted := controller.get_mission().get_area_trigger(idx)
	assert_true(bool(reverted["active"]), "undo restores the active flag")


func test_zone_overlay_harvests_a_pickable_per_zone() -> void:
	var controller := _loaded_with_selection()
	var base := controller.get_mission().get_area_trigger_count()
	controller.set_mode(MissionController.Mode.AREA_TRIGGERS)
	controller.add_area_trigger_default()
	controller.add_area_trigger_default()
	assert_eq(controller._zone_pickable.size(), base + 2, "the overlay harvested one pickable per zone")


func test_delete_selected_area_trigger_removes_and_is_undoable() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_mode(MissionController.Mode.AREA_TRIGGERS)
	var idx := controller.add_area_trigger_default()
	var after_add := mission.get_area_trigger_count()
	controller.select_area_trigger(idx)
	assert_true(controller.delete_selected_area_trigger(), "delete succeeds")
	assert_eq(mission.get_area_trigger_count(), after_add - 1, "the zone is removed")
	assert_eq(controller.get_selected_zone_index(), -1, "and the selection cleared")
	controller.undo()
	assert_eq(mission.get_area_trigger_count(), after_add, "undo restores the deleted zone")


func test_delete_key_deletes_zone_in_area_trigger_mode() -> void:
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_mode(MissionController.Mode.AREA_TRIGGERS)
	controller.add_area_trigger_default()
	var before := mission.get_area_trigger_count()
	var key := InputEventKey.new()
	key.pressed = true
	key.keycode = KEY_DELETE
	controller.handle_viewport_input(key)
	assert_eq(mission.get_area_trigger_count(), before - 1, "Delete removed the selected zone")
	assert_eq(controller.get_selected_zone_index(), -1, "and cleared the zone selection")


func test_zone_drag_translates_the_box_and_commits() -> void:
	# White-box the translate drag (no camera): begin it, drag to the stub's terrain hit,
	# release. The container is identity here, so the box centre moves to the hit's footprint.
	var controller := _loaded_with_selection()
	var mission := controller.get_mission()
	controller.set_mode(MissionController.Mode.AREA_TRIGGERS)
	var idx := controller.add_area_trigger_default()
	controller.select_area_trigger(idx)
	var start := controller.get_selected_zone()
	var start_min: Vector3 = start["min"]
	controller._drag_active = true
	controller._drag_moved = false
	controller._zone_drag_min = start_min
	controller._zone_drag_max = start["max"]
	controller._zone_preview_min = start_min
	controller._zone_preview_max = start["max"]
	controller.begin_edit()
	# First drag sample anchors; a second moves it. Use two samples with the same hit so the
	# delta is zero only if the anchor logic is wrong; here we move the second hit.
	controller.terrain_editor.terrain_hit = Vector3(0, 10, 0)
	controller._on_zone_drag(Vector2(5, 5))         # anchor
	controller.terrain_editor.terrain_hit = Vector3(32, 10, -16)
	controller._on_zone_drag(Vector2(9, 9))         # move
	controller._on_zone_left_release()
	var moved := controller.get_selected_zone()
	# godot delta (32,0,-16) -> mission delta (32, 16, 0); the box min shifts by that.
	assert_almost_eq((moved["min"] as Vector3).x, start_min.x + 32.0, 0.5, "the box translated in mission X")
	assert_almost_eq((moved["min"] as Vector3).y, start_min.y + 16.0, 0.5, "and in mission Y")
	assert_true(controller.is_dirty(), "a committed zone drag dirties the mission")
	controller.undo()  # rebakes + drops the zone selection; re-fetch the zone by index
	assert_almost_eq((controller.get_mission().get_area_trigger(idx)["min"] as Vector3).x, start_min.x, 0.5, "undo restores the box position")


func test_set_weapon_loadout_edits_and_is_undoable() -> void:
	var controller := _loaded_with_selection()
	var entries := controller.get_weapon_loadout()
	var base := entries.size()
	assert_gt(base, 0, "the fixture ships a non-empty loadout")
	entries.append({ "name": "WPN_TEST", "value1": "1", "value2": "2" })
	controller.set_weapon_loadout(entries)
	assert_eq(controller.get_weapon_loadout().size(), base + 1, "the weapon was appended")
	assert_true(controller.is_dirty(), "editing the loadout dirties the mission")
	controller.undo()
	assert_eq(controller.get_weapon_loadout().size(), base, "undo restores the loadout")
	controller.redo()
	assert_eq(controller.get_weapon_loadout().size(), base + 1, "redo replays the loadout edit")


func test_set_group_writes_three_ints_and_is_undoable() -> void:
	var controller := _loaded_with_selection()
	assert_eq(controller.get_group_count(), 64, "64 fixed groups")
	var before := controller.get_group(5)
	controller.set_group(5, 4321, 8765, 1357)
	var after := controller.get_group(5)
	assert_eq(int(after["field0"]), 4321, "field0 written")
	assert_eq(int(after["field8"]), 8765, "field8 written")
	assert_eq(int(after["field12"]), 1357, "field12 written")
	assert_true(controller.is_dirty(), "editing a group dirties the mission")
	controller.undo()
	assert_eq(controller.get_group(5), before, "undo restores the group")
	controller.redo()
	assert_eq(int(controller.get_group(5)["field0"]), 4321, "redo replays the group edit")


# --- Phase 4: mission scripting forwarders ------------------------------------

func test_add_event_default_appends_selects_and_is_undoable() -> void:
	var controller := _loaded_with_selection()
	controller.set_mode(MissionController.Mode.SCRIPTING)
	var base := controller.get_event_count()
	var idx := controller.add_event_default()
	assert_eq(idx, base, "the new event is appended at the end")
	assert_eq(controller.get_event_count(), base + 1, "event count grows")
	assert_eq(controller.get_selected_event_index(), base, "the new event is selected")
	assert_true(controller.is_dirty(), "adding an event dirties the mission")
	controller.undo()
	assert_eq(controller.get_event_count(), base, "undo removes the event")
	controller.redo()
	assert_eq(controller.get_event_count(), base + 1, "redo re-adds the event")


func test_scripting_trigger_and_action_ops_edit_the_selected_event() -> void:
	var controller := _loaded_with_selection()
	controller.set_mode(MissionController.Mode.SCRIPTING)
	controller.add_event_default()  # selects the fresh, empty event

	controller.add_selected_event_trigger()
	assert_eq((controller.get_selected_event_chain()["triggers"] as Array).size(), 1, "a trigger was added")
	controller.set_selected_event_trigger(0, {"main_type": 2, "sub_type": 10, "param2": 1})
	var trig := (controller.get_selected_event_chain()["triggers"] as Array)[0] as Dictionary
	assert_eq(int(trig["main_type"]), 2, "the trigger edit lands (Single)")
	assert_eq(int(trig["sub_type"]), 10, "the sub type lands (SingleIsWithinArea)")

	controller.add_selected_event_action()
	assert_eq((controller.get_selected_event_chain()["actions"] as Array).size(), 1, "an action was added")
	assert_true(controller.is_dirty(), "scripting edits dirty the mission")

	# Each op is its own undo step: undoing once removes only the last (the action add).
	controller.undo()
	assert_eq((controller.get_selected_event_chain()["actions"] as Array).size(), 0, "undo removes the action")
	assert_eq((controller.get_selected_event_chain()["triggers"] as Array).size(), 1, "the trigger remains")


func test_set_selected_event_attributes_is_undoable() -> void:
	var controller := _loaded_with_selection()
	controller.set_mode(MissionController.Mode.SCRIPTING)
	controller.add_event_default()
	controller.set_selected_event(1, 9, 4)  # ResetAfter flag, reset_after 9, delay 4
	var event := controller.get_selected_event_chain()["event"] as Dictionary
	assert_eq(int(event["reset_after"]), 9, "reset_after is set")
	assert_eq(int(event["delay"]), 4, "delay is set")
	assert_eq(int(event["flags"]) & 1, 1, "the Reset-after flag bit is set")
	controller.undo()
	var restored := controller.get_selected_event_chain()["event"] as Dictionary
	assert_eq(int(restored["reset_after"]), 0, "undo restores reset_after")


func test_delete_selected_event_is_undoable() -> void:
	var controller := _loaded_with_selection()
	controller.set_mode(MissionController.Mode.SCRIPTING)
	var base := controller.get_event_count()
	controller.add_event_default()
	assert_eq(controller.get_event_count(), base + 1)
	assert_true(controller.delete_selected_event(), "the selected event deletes")
	assert_eq(controller.get_event_count(), base, "the count drops back")
	controller.undo()
	assert_eq(controller.get_event_count(), base + 1, "undo restores the deleted event")


func test_scripting_edits_are_inert_without_a_selected_event() -> void:
	var controller := _loaded_with_selection()
	controller.set_mode(MissionController.Mode.SCRIPTING)
	# Drive the selection out of range (no event focused) and confirm the mutators no-op safely.
	controller.select_event(-1)
	controller.add_selected_event_trigger()
	controller.set_selected_event(1, 2, 3)
	assert_false(controller.is_dirty(), "scripting mutators do nothing without a selected event")


func test_move_selected_event_trigger_reorders_the_chain() -> void:
	var controller := _loaded_with_selection()
	controller.set_mode(MissionController.Mode.SCRIPTING)
	controller.add_event_default()
	controller.add_selected_event_trigger()
	controller.add_selected_event_trigger()
	# Tag the two triggers by param so the reorder is observable end-to-end (real C++ std::swap).
	controller.set_selected_event_trigger(0, {"param1": 100})
	controller.set_selected_event_trigger(1, {"param1": 200})
	assert_eq((controller.get_selected_event_chain()["triggers"] as Array).size(), 2, "two triggers present")

	controller.move_selected_event_trigger(1, -1)  # move the 2nd trigger toward the front
	var triggers := controller.get_selected_event_chain()["triggers"] as Array
	assert_eq(int((triggers[0] as Dictionary)["param1"]), 200, "the moved trigger is now first")
	assert_eq(int((triggers[1] as Dictionary)["param1"]), 100, "the displaced trigger is now second")
	controller.undo()
	var restored := controller.get_selected_event_chain()["triggers"] as Array
	assert_eq(int((restored[0] as Dictionary)["param1"]), 100, "undo restores the original order")


func test_move_selected_event_action_reorders_the_chain() -> void:
	var controller := _loaded_with_selection()
	controller.set_mode(MissionController.Mode.SCRIPTING)
	controller.add_event_default()
	controller.add_selected_event_action()
	controller.add_selected_event_action()
	controller.set_selected_event_action(0, {"param1": 11})
	controller.set_selected_event_action(1, {"param1": 22})

	controller.move_selected_event_action(0, 1)  # move the 1st action toward the back
	var actions := controller.get_selected_event_chain()["actions"] as Array
	assert_eq(int((actions[0] as Dictionary)["param1"]), 22, "the displaced action is now first")
	assert_eq(int((actions[1] as Dictionary)["param1"]), 11, "the moved action is now second")
