extends GutTest

# Phase 3: MissionEditorWorkspace shell hooks. Covers the capability hooks the
# editor shell reads to wire the Open action, resource browser, title, and
# inspector — without standing up the whole EditorWorkstation. The mission "kind"
# (-> mission resources in the resource index) and the mission dialog filters are what make the
# Open flow reach a mission at all.

const MissionWorkspace := preload("res://modtools/mission/mission_workspace.gd")


func test_open_resource_kind_is_mission() -> void:
	var ws = MissionWorkspace.new()
	assert_eq(ws.get_open_resource_kind(), "mission")


func test_open_dialog_offers_bms_and_mis() -> void:
	var ws = MissionWorkspace.new()
	var joined := ""
	for filter in ws.get_open_dialog_filters():
		joined += String(filter)
	assert_string_contains(joined.to_lower(), "bms")
	assert_string_contains(joined.to_lower(), "mis")


func test_save_as_uses_file_dialog_with_bms_and_mis_filters() -> void:
	var ws = MissionWorkspace.new()
	assert_true(ws.uses_save_file_dialog(), "Mission Save As chooses a file path, not just a directory")
	var joined := ""
	for filter in ws.get_save_file_dialog_filters():
		joined += String(filter)
	assert_string_contains(joined.to_lower(), "bms")
	assert_string_contains(joined.to_lower(), "mis")
	assert_eq(ws.get_save_file_dialog_default_name(), "mission.bms")


func test_defaults_without_an_editor() -> void:
	var ws = MissionWorkspace.new()
	assert_eq(ws.get_workspace_id(), "mission")
	assert_eq(ws.get_workspace_label(), "Mission")
	assert_eq(ws.get_project_title(), "Mission", "no mission loaded -> plain title")
	assert_false(ws.has_unsaved_changes(), "an unedited workspace reports no unsaved changes")
	assert_false(ws.can_open(), "Open is gated on a bound terrain editor")
	# Visibility (button is created) is decoupled from enablement (can_save*). The shell
	# only builds buttons on workspace switch, so Save / Save As must be visible up front
	# and merely disabled until a mission is loaded / dirtied.
	assert_true(ws.has_save_action(), "Save button is created up front")
	assert_true(ws.has_save_as_action(), "Save As button is created up front")
	assert_false(ws.can_save(), "Save is disabled until a dirty mission with a path")
	assert_false(ws.can_save_as(), "Save As is disabled until a mission is loaded")
	assert_true(ws.shows_camera_status())


class PreviewContextStubEditor:
	extends Node

	var resource_root: NovaResourceRoot
	var world_root: Node3D
	var current_trn_path := ""
	var is_dirty := false
	var set_context_calls := 0
	var clear_context_calls := 0
	var context_active := false
	var tile_info: NovaTerrainTileInfo
	var time_of_day := NAN

	func set_mission_preview_context(
		value: NovaTerrainTileInfo,
		preview_time_of_day: float = NAN
	) -> void:
		set_context_calls += 1
		context_active = true
		tile_info = value
		time_of_day = preview_time_of_day

	func clear_mission_preview_context() -> void:
		clear_context_calls += 1
		context_active = false
		tile_info = null
		time_of_day = NAN

	func set_viewport_active(_active: bool, _grab_focus: bool) -> void:
		pass

	func get_resource_root() -> NovaResourceRoot:
		return resource_root

	func get_terrain_world_root() -> Node3D:
		return world_root

	func get_current_trn_path() -> String:
		return current_trn_path

	func open_trn(path: String, _timeline: PerfTimeline = null) -> Error:
		current_trn_path = path
		return OK


func test_mission_preview_context_is_scoped_to_workspace_activation() -> void:
	var first := PreviewContextStubEditor.new()
	add_child_autofree(first)
	var ws = MissionWorkspace.new(first)
	assert_false(first.context_active)

	ws.activate()
	assert_true(first.context_active,
		"Mission activation must install blockers on the shared terrain preview.")
	assert_eq(first.set_context_calls, 1)

	var second := PreviewContextStubEditor.new()
	add_child_autofree(second)
	ws.set_terrain_editor(second)
	assert_false(first.context_active, "Rebinding clears Mission state from the old shared editor.")
	assert_true(second.context_active, "The active workspace applies Mission state to the new editor.")

	ws.deactivate()
	assert_false(second.context_active, "Leaving Mission must restore the Terrain workspace's authored context.")
	assert_eq(second.clear_context_calls, 1)


func test_release_viewport_ends_mission_preview_context_before_rebind() -> void:
	var first := PreviewContextStubEditor.new()
	add_child_autofree(first)
	var ws = MissionWorkspace.new(first)

	ws.activate()
	assert_true(first.context_active, "precondition: Mission installed its preview context")
	ws.release_viewport()
	assert_false(first.context_active,
		"releasing the workspace clears Mission state from the detached editor")

	var second := PreviewContextStubEditor.new()
	add_child_autofree(second)
	ws.set_terrain_editor(second)
	assert_false(second.context_active,
		"rebinding after release must not resurrect Mission blockers or foliage anchors")
	assert_eq(second.set_context_calls, 0,
		"only a later activate may install Mission preview context on the replacement editor")


func test_active_mission_context_uses_bms_clock_and_clear_drops_only_the_override() -> void:
	var editor := PreviewContextStubEditor.new()
	add_child_autofree(editor)
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5"))
	editor.resource_root = root
	editor.world_root = Node3D.new()
	add_child_autofree(editor.world_root)
	var ws = MissionWorkspace.new(editor)
	var bms := ProjectSettings.globalize_path("res://../fixtures/bms/ash_i5b.reference.bms")
	assert_eq(ws.open_file(bms), OK)
	var controller = ws.get_editor_document()
	var mission: NovaMissionData = controller.get_mission()
	mission.set_header_int("start_time", 0x0540) # unsigned Q8.8 = 05:15

	ws.activate()
	assert_almost_eq(editor.time_of_day, 515.0, 0.001,
		"Mission activation routes the BMS clock through the preview-context seam.")
	controller.clear()
	await get_tree().process_frame
	assert_true(editor.context_active,
		"Clearing a document does not deactivate the still-selected Mission workspace.")
	assert_true(is_nan(editor.time_of_day),
		"Clearing the loaded mission removes its scoped clock so authored ENV time can render.")

func test_build_inspector_mounts_a_panel() -> void:
	var ws = MissionWorkspace.new()
	var mount := Control.new()
	add_child_autofree(mount)
	ws.build_inspector(mount)
	assert_gt(mount.get_child_count(), 0, "the mission inspector panel is mounted into the mount")


# --- New + undo / redo hooks --------------------------------------------------

func test_new_action_requires_a_terrain_editor() -> void:
	var ws = MissionWorkspace.new()
	assert_eq(ws.get_new_action_label(), "New Mission")
	assert_false(ws.can_new(), "New is gated on a bound terrain editor")
	# new_current with no terrain editor delegates to the controller, which reports it cannot.
	assert_ne(int(ws.new_current()), OK, "New fails (and is surfaced) without a terrain")


func test_undo_redo_hooks_delegate_to_the_controller() -> void:
	var ws = MissionWorkspace.new()
	assert_false(ws.can_undo(), "no history -> cannot undo")
	assert_false(ws.can_redo(), "no history -> cannot redo")
	# Safe no-ops with nothing loaded (the shell may probe / fire these any time).
	ws.undo()
	ws.redo()

	# The history lives on the document; seed it on a from-scratch mission and hand it to the
	# controller. can_undo/can_redo then reflect it -- proving the hooks delegate through the
	# controller without standing up a full terrain + world load.
	var mission := NovaMissionData.new()
	mission.create_default()
	mission.begin_edit()
	mission.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3.ZERO, Vector3.ZERO)
	mission.commit_edit()
	ws._controller._mission = mission
	assert_true(ws.can_undo(), "can_undo reflects the document's undo history")
	assert_false(ws.can_redo(), "nothing to redo yet")
	mission.undo()
	assert_false(ws.can_undo(), "the step was consumed")
	assert_true(ws.can_redo(), "and is now redoable")


# --- Right-dock split: the inspector mounts its editor in %AssetDock ----------

func test_uses_asset_dock() -> void:
	var ws = MissionWorkspace.new()
	assert_true(ws.uses_asset_dock(), "the mission workspace opts into the right dock")


func test_asset_dock_before_build_mounts_the_editor() -> void:
	# The shell forwards the dock (set_asset_dock) BEFORE building the inspector on activation; the
	# adapter caches it so the fresh inspector builds its editor straight into the dock.
	var ws = MissionWorkspace.new()
	var dock := PanelContainer.new()
	add_child_autofree(dock)
	var mount := Control.new()
	add_child_autofree(mount)
	ws.set_asset_dock(dock)
	ws.build_inspector(mount)
	assert_not_null(dock.find_child("MissionPosX", true, false), "the entity editor mounts in the dock")
	assert_null(mount.find_child("MissionPosX", true, false), "and not in the left inspector mount")
	# On switch-away the shell calls set_asset_dock(null) before clearing the dock; the editor must
	# reparent back under the inspector (mount) rather than be freed.
	ws.set_asset_dock(null)
	assert_null(dock.find_child("MissionPosX", true, false), "the dock is emptied of the editor")
	assert_not_null(mount.find_child("MissionPosX", true, false), "the editor is reparented under the inspector mount")


# --- B8: the activate-time re-ground prompt ------------------------------------
# When the SAME terrain's heights changed under the loaded mission, activate()
# pops "Terrain changed under N objects" parented to the shell; confirm applies
# the one-step bulk re-ground, every other dismissal acknowledges (quiet until
# the next height edit). Mirrors the controller-level coverage in
# mission_controller_test.gd through the workspace + dialog layer.

# The terrain-editor seams the mission controller duck-types, plus the B8
# height-revision/sampling pair (see mission_controller_test.gd's stub).
class RegroundStubEditor:
	extends Node

	var resource_root: NovaResourceRoot
	var world_root: Node3D
	var current_trn_path: String = ""
	var is_dirty := false
	var height_revision := 0
	var sample_height := 10.0

	func get_resource_root() -> NovaResourceRoot:
		return resource_root

	func get_terrain_world_root() -> Node3D:
		return world_root

	func get_current_trn_path() -> String:
		return current_trn_path

	func open_trn(path: String, _timeline: PerfTimeline = null) -> Error:
		current_trn_path = path
		return OK

	func get_height_revision() -> int:
		return height_revision

	func sample_height_world(_world_x: float, _world_z: float) -> float:
		return sample_height

	# The batch seam the request builder prefers; loops the scalar fake.
	func sample_heights_world(points: PackedVector2Array) -> PackedFloat32Array:
		var out := PackedFloat32Array()
		out.resize(points.size())
		for i in points.size():
			out[i] = sample_height_world(points[i].x, points[i].y)
		return out

	func set_viewport_active(_active: bool, _grab_focus: bool) -> void:
		pass  # deactivate() pokes the viewport; nothing to do headless


# A workspace with the fixture mission loaded on the stub terrain, its shell a
# themed Control, and a height edit already made underneath (revision bumped +
# the fake surface moved) so the next activate() reports drift.
func _drifted_workspace() -> MissionWorkspace:
	var stub := RegroundStubEditor.new()
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5"))
	stub.resource_root = root
	stub.world_root = Node3D.new()
	add_child_autofree(stub.world_root)
	add_child_autofree(stub)
	var shell := Control.new()
	shell.theme = Theme.new()
	add_child_autofree(shell)
	var ws = MissionWorkspace.new(stub)
	ws.editor_shell = shell
	var bms := ProjectSettings.globalize_path("res://../fixtures/bms/ash_i5b.reference.bms")
	assert_eq(ws._controller.open_mission(bms), OK, "the fixture mission opens on the stub terrain")
	stub.height_revision += 1
	stub.sample_height = 500.0
	return ws


func test_activate_prompts_reground_and_confirm_applies() -> void:
	var ws := _drifted_workspace()
	var shell: Control = ws.editor_shell

	ws.activate()
	var dialog := shell.find_child("MissionRegroundDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog, "activate over a height-edited terrain pops the re-ground confirm")
	assert_eq(dialog.theme, shell.theme, "the dialog adopts the shell theme explicitly")
	assert_eq(dialog.get_ok_button().text, "Re-ground")
	assert_eq(dialog.get_cancel_button().text, "Leave as-is")
	assert_string_contains(dialog.dialog_text, "objects")

	dialog.confirmed.emit()
	assert_eq(ws._controller.undo_depth(), 1, "the whole re-ground is one undo step")
	assert_true(ws._controller.is_dirty(), "the moved mission is dirty")
	assert_eq(ws._controller.reconcile_with_terrain(), 0, "the applied re-ground settles the drift")


func test_reground_prompt_cancel_acknowledges_without_moving() -> void:
	var ws := _drifted_workspace()
	var shell: Control = ws.editor_shell
	var before: Vector3 = ws._controller.get_mission().get_entities(NovaMissionData.KIND_MARKER)[0]["position"]

	ws.activate()
	var dialog := shell.find_child("MissionRegroundDialog", true, false) as ConfirmationDialog
	assert_not_null(dialog)
	dialog.canceled.emit()
	await get_tree().process_frame  # let the dismissed dialog's queue_free land

	var after: Vector3 = ws._controller.get_mission().get_entities(NovaMissionData.KIND_MARKER)[0]["position"]
	assert_eq(after, before, "declining moves nothing")
	assert_eq(ws._controller.undo_depth(), 0, "and pushes no undo step")
	assert_eq(ws._controller.reconcile_with_terrain(), 0, "the drift is acknowledged")

	ws.activate()
	assert_null(shell.find_child("MissionRegroundDialog", true, false),
		"a later activate stays quiet until the next height edit")


func test_workspace_switch_dismisses_the_prompt_without_answering() -> void:
	var ws := _drifted_workspace()
	var shell: Control = ws.editor_shell

	ws.activate()
	assert_not_null(shell.find_child("MissionRegroundDialog", true, false))
	ws.deactivate()
	await get_tree().process_frame  # let the dismissed dialog's queue_free land
	assert_null(shell.find_child("MissionRegroundDialog", true, false),
		"leaving the workspace closes the prompt (it must not float over other workspaces)")

	# Dismissal-by-leaving is not an answer: the drift was neither applied nor
	# acknowledged, so the question re-poses on return.
	ws.activate()
	assert_not_null(shell.find_child("MissionRegroundDialog", true, false),
		"the unanswered question re-poses on the next activate")


func _shelled_workspace() -> MissionWorkspace:
	var ws = MissionWorkspace.new()
	var shell := Control.new()
	add_child_autofree(shell)
	ws.editor_shell = shell
	return ws


func test_workspace_has_no_embedded_game_runtime_or_debug_ui() -> void:
	var ws := _shelled_workspace()
	var mount := Control.new()
	add_child_autofree(mount)
	ws.build_inspector(mount)

	assert_null(mount.find_child("MissionDebugBtn", true, false),
		"F3 belongs to the separately launched game")
	assert_null(mount.find_child("MissionSimBar", true, false),
		"mission testing is launched from the editor toolbar")
	for method in [
		"play_mission", "stop_play_mission", "is_playing_mission", "play_controller",
		"toggle_debug_overlay", "is_debug_overlay_open", "get_active_runtime",
	]:
		assert_false(ws.has_method(method), "workspace must not expose %s" % method)
