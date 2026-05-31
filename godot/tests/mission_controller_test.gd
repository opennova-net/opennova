extends GutTest

# Phase 2: MissionController. Covers the load-orchestration paths that are
# testable without a full terrain-editor scene, using a stub that stands in for
# the seams the controller drives (resource root, world root, open_trn, env
# editor). The resolve-miss path is the important one: a mission whose referenced
# terrain is absent must fail cleanly with a clear reason and must not attempt to
# load terrain. Full end-to-end placement is validated against real assets
# out-of-band (see the placer + nova_world paths).

const MissionController := preload("res://modtools/mission/mission_controller.gd")

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
