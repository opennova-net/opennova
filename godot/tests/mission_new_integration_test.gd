extends GutTest

# End-to-end: create a mission from scratch on a REAL terrain loaded through the real terrain
# editor (not the stub the controller unit tests use), then place / undo / redo / save / reopen --
# the exact flow the user drives via the New Mission button. Guards the headline fix ("could not
# create a mission from scratch") against regressions in the full editor wiring. Mirrors the
# real-terrain setup in terrain_editor_import_export_test.gd.

const EditorMainScene = preload("res://modtools/editor/editor_main.tscn")
const MissionWorkspace := preload("res://modtools/mission/mission_workspace.gd")

const DVXI5_TRN := "res://../fixtures/godot/dvxi5/Dvxi5.trn"
const ITEMS_PATH := "res://../fixtures/def/items.def"
const SAVE_DIR := "user://mission_new_e2e"


func _abs(p: String) -> String:
	return ProjectSettings.globalize_path(p)


func after_each() -> void:
	var dir := _abs(SAVE_DIR)
	if DirAccess.dir_exists_absolute(dir):
		var d := DirAccess.open(dir)
		if d != null:
			for f in d.get_files():
				DirAccess.remove_absolute(dir.path_join(f))
		DirAccess.remove_absolute(dir)


func test_new_mission_end_to_end_on_a_real_terrain() -> void:
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	assert_eq(editor.open_trn(_abs(DVXI5_TRN)), OK, "the real dvxi5 terrain loads through the editor")

	# The New Mission flow the shell drives: can_new lights the button, new_current creates it.
	var ws = MissionWorkspace.new(editor)
	assert_true(ws.can_new(), "New Mission is available once a terrain is loaded")
	assert_eq(int(ws.new_current()), OK, "New Mission creates a mission from scratch")

	var controller = ws._controller
	assert_true(controller.is_loaded(), "a mission is now loaded")
	assert_eq(controller.get_current_path(), "", "the new mission has no file yet")
	assert_eq(controller.get_mission().get_terrain_ref().to_lower(), "dvxi5",
		"the new mission adopts the loaded terrain")
	assert_false(controller.is_dirty(), "and is clean until the first edit")

	# Place an object. The dvxi5 fixture dir has no items.def, so inject the fixture db (the same
	# the controller unit tests use) so the kind mapping + palette resolve.
	var db := NovaItemDatabase.new()
	assert_eq(db.load(_abs(ITEMS_PATH)), OK, "items.def fixture loads")
	controller._placer.item_db = db
	assert_gt(controller.get_placeable_items().size(), 0, "the placement palette is live")

	assert_true(controller.place_entity_at_world(102001, Vector3(64.0, 10.0, -64.0)),
		"placing a building into the from-scratch mission works")
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_BUILDING), 1,
		"the building landed")
	assert_true(controller.is_dirty(), "the first placement dirties the mission")
	assert_true(controller.can_undo(), "and is undoable")

	# Undo back to the empty baseline clears dirty; redo restores the placement.
	controller.undo()
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_BUILDING), 0,
		"undo removes the placement")
	assert_false(controller.is_dirty(), "undo back to the clean baseline clears the dirty marker")
	controller.redo()
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_BUILDING), 1,
		"redo restores the placement")
	assert_true(controller.is_dirty(), "and re-dirties")

	# Place a marker (e.g. a player start) from the same palette -- a mesh-less general entity.
	assert_true(controller.place_entity_at_world(100001, Vector3(72.0, 10.0, -72.0)),
		"placing a marker from the palette works")
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_MARKER), 1,
		"the marker landed")
	var ps_type := int(controller.get_mission().get_entity(NovaMissionData.KIND_MARKER, 0)["type_id"])

	# Add a WAYPOINT marker via the Waypoints tool: it must be the engine waypoint type (6005), NOT a
	# copy of the player-start-style marker placed above (the headline marker-type bug).
	controller.set_waypoint_mode(true)
	controller.select_new_waypoint_path()
	assert_true(controller.add_marker_to_active_path_at_world(Vector3(80.0, 10.0, -80.0)),
		"adding a waypoint marker to a path works")
	assert_eq(controller.get_mission().get_entity_count(NovaMissionData.KIND_MARKER), 2,
		"the waypoint marker is a second marker entity")
	var wp_idx := int(controller.get_selected_marker()["marker_index"])
	assert_eq(int(controller.get_mission().get_entity(NovaMissionData.KIND_MARKER, wp_idx)["type_id"]), 6005,
		"the waypoint marker is the engine waypoint type (6005)")
	assert_ne(int(controller.get_mission().get_entity(NovaMissionData.KIND_MARKER, wp_idx)["type_id"]), ps_type,
		"and a distinct type from the player-start-style marker")

	# Save As, then reopen the written .bms and confirm the terrain ref + placed object + markers survived.
	assert_eq(int(ws.save_as(_abs(SAVE_DIR))), OK, "Save As writes the from-scratch mission")
	assert_false(controller.is_dirty(), "a saved mission is clean")
	var path := _abs(SAVE_DIR).path_join("mission.bms")
	assert_true(FileAccess.file_exists(path), "mission.bms was written")

	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(path), OK, "the saved from-scratch mission reopens")
	assert_eq(reopened.get_terrain_ref().to_lower(), "dvxi5", "terrain ref round-trips")
	assert_eq(reopened.get_entity_count(NovaMissionData.KIND_BUILDING), 1,
		"the placed building round-trips through save + reopen")
	assert_eq(reopened.get_entity_count(NovaMissionData.KIND_MARKER), 2,
		"both markers (player-start-style + waypoint) round-trip through save + reopen")
	var reopened_marker_types: Array = []
	for i in reopened.get_entity_count(NovaMissionData.KIND_MARKER):
		reopened_marker_types.append(int(reopened.get_entity(NovaMissionData.KIND_MARKER, i)["type_id"]))
	assert_true(reopened_marker_types.has(6005), "the waypoint marker (type 6005) survives the round-trip")
	assert_true(reopened_marker_types.has(ps_type), "and the player-start-style marker keeps its own type")
