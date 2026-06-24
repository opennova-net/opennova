extends GutTest

# Phase 3: MissionEditorWorkspace shell hooks. Covers the capability hooks the
# editor shell reads to wire the Open action, resource browser, title, and
# inspector — without standing up the whole EditorWorkstation. The mission "kind"
# (-> mission resources in the resource index) and the mission dialog filters are what make the
# Open flow reach a mission at all.

const MissionWorkspace := preload("res://modtools/editor/mission_workspace.gd")


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


func test_build_inspector_mounts_a_panel() -> void:
	var ws = MissionWorkspace.new()
	var host := Control.new()
	add_child_autofree(host)
	ws.build_inspector(host)
	assert_gt(host.get_child_count(), 0, "the mission inspector panel is mounted into the host")


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


func test_asset_dock_before_build_hosts_the_editor() -> void:
	# The shell forwards the dock (set_asset_dock) BEFORE building the inspector on activation; the
	# adapter caches it so the fresh inspector builds its editor straight into the dock.
	var ws = MissionWorkspace.new()
	var dock := PanelContainer.new()
	add_child_autofree(dock)
	var host := Control.new()
	add_child_autofree(host)
	ws.set_asset_dock(dock)
	ws.build_inspector(host)
	assert_not_null(dock.find_child("MissionPosX", true, false), "the entity editor mounts in the dock")
	assert_null(host.find_child("MissionPosX", true, false), "and not in the left inspector host")
	# On switch-away the shell calls set_asset_dock(null) before clearing the dock; the editor must
	# reparent back under the inspector (host) rather than be freed.
	ws.set_asset_dock(null)
	assert_null(dock.find_child("MissionPosX", true, false), "the dock is emptied of the editor")
	assert_not_null(host.find_child("MissionPosX", true, false), "the editor is reparented under the inspector host")


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


# --- C12: the editor debug-overlay summon ---------------------------------------
# The sim bar's Debug toggle summons the SAME NovaDebugOverlay the game opens
# with F3, parented under the shell with variable edits write-locked; its
# runtime source follows the active mode (PIE first, else the in-place sim).

class StubDebugController:
	extends RefCounted
	# Only what _debug_runtime_source / _on_overlay_transport touch; the real
	# controller is replaced wholesale so no mission/terrain needs standing up.
	var sim_runtime: Object = null
	var stop_calls := 0
	var notify_calls := 0
	func get_sim_runtime():
		return sim_runtime
	func sim_stop() -> void:
		stop_calls += 1
	func notify_sim_transport_changed() -> void:
		notify_calls += 1


class StubPlayNode:
	extends Control
	# Doubles as the play controller AND its world: is_playing()/get_world()
	# come from MissionPlayController, get_runtime() from NovaWorld.
	var playing := false
	var runtime: Object = null
	func is_playing() -> bool:
		return playing
	func get_world():
		return self
	func get_runtime():
		return runtime


func _shelled_workspace() -> MissionWorkspace:
	var ws = MissionWorkspace.new()
	var shell := Control.new()
	add_child_autofree(shell)
	ws.editor_shell = shell
	return ws


func test_debug_button_summons_a_locked_overlay_over_the_shell() -> void:
	var ws := _shelled_workspace()
	var shell: Control = ws.editor_shell
	var host := Control.new()
	add_child_autofree(host)
	ws.build_inspector(host)

	var btn := host.find_child("MissionDebugBtn", true, false) as Button
	assert_not_null(btn, "build_inspector wires the Debug toggle through set_debug_hooks")
	assert_true(btn.visible, "valid hooks show the toggle")
	var bar := host.find_child("MissionSimBar", true, false) as Control
	assert_true(bar != null and bar.visible,
		"the sim bar stays up with no mission so the Debug toggle is actually REACHABLE — it is the overlay's only close affordance in the editor")

	btn.button_pressed = true  # user press (emits toggled)
	var overlay = shell.find_child("MissionDebugOverlay", true, false)
	assert_not_null(overlay, "the first press builds the overlay under the shell")
	assert_true(overlay.visible)
	assert_true(overlay._writes_check.disabled,
		"the editor's overlay mounts write-locked")
	assert_string_contains(overlay._writes_check.tooltip_text, "simulating from the editor")
	assert_true(btn.button_pressed, "the toggle stays latched while open")

	btn.button_pressed = false
	assert_false(overlay.visible, "the second press hides it")
	assert_true(is_instance_valid(overlay), "...without freeing")
	btn.button_pressed = true
	assert_eq(shell.find_child("MissionDebugOverlay", true, false), overlay,
		"a re-summon reuses the same instance (tab/filter state survives)")


func test_debug_button_does_not_latch_without_a_shell() -> void:
	# Headless host (no editor_shell): the summon has nothing to float over, so
	# the toggle must re-sync to off instead of latching pressed.
	var ws = MissionWorkspace.new()
	var host := Control.new()
	add_child_autofree(host)
	ws.build_inspector(host)

	var btn := host.find_child("MissionDebugBtn", true, false) as Button
	assert_not_null(btn)
	btn.button_pressed = true
	assert_false(ws.is_debug_overlay_open())
	assert_false(btn.button_pressed, "the failed summon re-syncs the toggle off")


func test_debug_runtime_source_prefers_pie_then_sim() -> void:
	var ws := _shelled_workspace()
	var stub_controller := StubDebugController.new()
	ws._controller = stub_controller
	var sim_rt := Node.new()
	add_child_autofree(sim_rt)
	stub_controller.sim_runtime = sim_rt
	assert_eq(ws._debug_runtime_source(), sim_rt,
		"no PIE -> the in-place Simulate driver")

	var play_rt := Node.new()
	add_child_autofree(play_rt)
	var stub_play := StubPlayNode.new()
	stub_play.runtime = play_rt
	var play_host := Control.new()
	add_child_autofree(play_host)
	ws._play_mount = ViewportMount.new(&"MissionPlayViewport", func() -> Control: return stub_play)
	ws._play_mount.mount(play_host)
	stub_play.playing = true
	assert_eq(ws._debug_runtime_source(), play_rt,
		"Play Mission wins while playing (play_mission() sim_stops first, so both can never be live)")

	stub_play.playing = false
	assert_eq(ws._debug_runtime_source(), sim_rt, "Stop falls back to the sim driver")

	stub_controller.sim_runtime = null
	assert_null(ws._debug_runtime_source(),
		"idle editor -> null (the overlay shows its no-mission state)")


func test_overlay_transport_relays_to_the_controller() -> void:
	# The overlay drives the runtime directly (it is host-neutral), bypassing
	# the controller's sim_* methods whose `changed` is all the sim bar listens
	# to. The workspace relay keeps them in step: Stop completes the editor-side
	# stop (frees the driver -> editing unlocks), everything else re-emits
	# changed via notify_sim_transport_changed.
	var ws := _shelled_workspace()
	var stub_controller := StubDebugController.new()
	ws._controller = stub_controller

	ws._on_overlay_transport("pause")
	assert_eq(stub_controller.notify_calls, 1, "pause relays a changed re-emit")
	ws._on_overlay_transport("play")
	ws._on_overlay_transport("step")
	ws._on_overlay_transport("wac_pause")
	assert_eq(stub_controller.notify_calls, 4, "play/step/script-pause relay too")
	assert_eq(stub_controller.stop_calls, 0)

	ws._on_overlay_transport("stop")
	assert_eq(stub_controller.stop_calls, 1,
		"overlay Stop completes the editor stop (sim_stop frees the driver, unlocking edits)")
	assert_eq(stub_controller.notify_calls, 4, "...and does not double-notify")

	# The summoned overlay is WIRED to the relay (not just the method existing).
	ws.toggle_debug_overlay()
	assert_true(ws._debug_overlay.transport_used.is_connected(ws._on_overlay_transport),
		"the summoned overlay's transport_used feeds the relay")

	# PIE: the play world owns its transport; the editor controller must not hear it.
	var stub_play := StubPlayNode.new()
	stub_play.playing = true
	var play_host := Control.new()
	add_child_autofree(play_host)
	ws._play_mount = ViewportMount.new(&"MissionPlayViewport", func() -> Control: return stub_play)
	ws._play_mount.mount(play_host)
	ws._on_overlay_transport("stop")
	ws._on_overlay_transport("pause")
	assert_eq(stub_controller.stop_calls, 1, "PIE transport never reaches the in-place controller")
	assert_eq(stub_controller.notify_calls, 4)


func test_deactivate_hides_the_overlay_and_release_frees_it() -> void:
	var ws := _shelled_workspace()
	var shell: Control = ws.editor_shell
	ws.toggle_debug_overlay()
	var overlay = shell.find_child("MissionDebugOverlay", true, false)
	assert_not_null(overlay)
	assert_true(ws.is_debug_overlay_open())

	ws.deactivate()
	assert_false(overlay.visible,
		"leaving the workspace hides the overlay (it must not float over other workspaces)")
	assert_true(is_instance_valid(overlay), "hidden, not freed: one click brings it back")
	assert_true(overlay._timer.paused, "hidden pauses its refresh timer too")

	ws.toggle_debug_overlay()
	assert_true(ws.is_debug_overlay_open(), "re-summon after deactivate reuses the instance")

	ws.release_viewport()
	await get_tree().process_frame  # queue_free lands
	assert_null(shell.find_child("MissionDebugOverlay", true, false),
		"release_viewport tears the overlay down with the workspace")
	assert_false(ws.is_debug_overlay_open())
