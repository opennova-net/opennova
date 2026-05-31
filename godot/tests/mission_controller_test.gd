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
