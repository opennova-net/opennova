class_name MissionEditorWorkspace
extends EditorWorkspace

# Mission workspace: create or open a .bms/.mis mission and author its world. The mission's header
# selects the terrain + environment, which load into the shared terrain viewport, and its placed
# objects are instanced under the terrain world root by the host-agnostic MissionObjectPlacer.
# New Mission builds an empty mission on the currently-loaded terrain; from there objects, zones,
# waypoints, and scripting are editable (with undo/redo + Save / Save As).
#
# The create + load + resolve + place + edit work lives in MissionController and the placer; this
# adapter is the EditorWorkspace shell binding (capability hooks + inspector).

const TerrainViewportScript = preload("res://modtools/terrain/terrain_viewport.gd")
const DebugViewContext = preload("res://engine/debug/nova_debug_view_context.gd")
const MissionControllerScript = preload("res://modtools/mission/mission_controller.gd")
const MissionInspectorScript = preload("res://modtools/mission/mission_inspector.gd")
const MissionPlayControllerScript = preload("res://modtools/mission/mission_play_controller.gd")

var terrain_editor: Node
var _controller  # MissionController
var _mount: ViewportMount
# The live inspector + the shell's right dock (%AssetDock). The inspector splits its UI across the
# left pane (browser) and the dock (per-selection editor + Mission form). The shell forwards the
# dock via set_asset_dock BEFORE build_inspector on activation, so we cache it and hand it over when
# the inspector is built (or push it into an already-built inspector on a re-sync / teardown).
var _inspector  # MissionInspector (preloaded, no class_name)
var _detail_host: Control
# --- Play-in-editor -------------------------------------------------------
# A second viewport mount holding the play controller (the real game world in a
# SubViewport). Play swaps it in for the edit viewport; Stop swaps back. The
# edit world (terrain root, selection, undo) survives unmounted, untouched.
var _play_mount: ViewportMount
# The shell's viewport host, cached at mount so Play/Stop can swap mounts.
var _viewport_host: Control
# The live "Terrain changed under N objects" confirm (see _prompt_reground), so a
# re-activate while it is open cannot stack a second one.
var _reground_dialog: ConfirmationDialog
# The mission debug overlay summoned over the editor (lazily built on the first
# toggle, parented under the shell like _prompt_reground's dialog). Variable
# edits are write-locked for its whole life; the runtime source follows the
# active mode per refresh (see _debug_runtime_source).
var _debug_overlay: NovaDebugOverlay
# The debug pick list shared with play-in-editor (click-picked while the
# overlay is up; snapshots embed it). Workspace-lifetime; PIE worlds are
# freed on stop, so stale cards just report stale in a later snapshot.
var _pick_list := NovaDebugPickList.new()

# Mission-only terrain/foliage inputs are scoped to activation because the
# Terrain and Mission workspaces share one TerrainEditor scene.
var _mission_preview_context_active := false


func _init(value: Node = null) -> void:
	terrain_editor = value
	_controller = MissionControllerScript.new(value)
	# The controller fires `changed` on load / clear / select / dirty / save; refresh
	# the shell title + action-button state (Save enables, `*` appears) on each.
	_controller.changed.connect(_sync_shell)
	_controller.changed.connect(_sync_mission_preview_context)
	# Transient action feedback (undo / delete / place / rejected edits) flows through
	# status_reported; relay it to the shell status bar. Open / save keep their own poll.
	_controller.status_reported.connect(_on_controller_status)


func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"MissionViewport", func() -> Control: return TerrainViewportScript.new())
	return _mount


func _ensure_play_mount() -> ViewportMount:
	if _play_mount == null:
		_play_mount = ViewportMount.new(&"MissionPlayViewport", func() -> Control:
			var play := MissionPlayControllerScript.new()
			play.stop_requested.connect(stop_play_mission)
			play.status_reported.connect(_on_controller_status)
			play.debug_overlay_requested.connect(toggle_debug_overlay)
			return play)
	return _play_mount


func _play_node():
	return _play_mount.get_viewport_node() if _play_mount != null else null


## The live play-in-editor controller, null unless play is mounted. Public read
## seam (ADR 0018): probes and diagnostics reach play through this, never the
## privates.
func play_controller():
	return _play_node()


func set_terrain_editor(value: Node) -> void:
	var previous := terrain_editor
	if _mission_preview_context_active and previous != null and previous != value \
			and previous.has_method("clear_mission_preview_context"):
		previous.clear_mission_preview_context()
	terrain_editor = value
	if _controller != null:
		_controller.set_terrain_editor(value)
	_sync_mission_preview_context()
	if _mount != null:
		var viewport := _mount.get_viewport_node()
		if viewport != null:
			viewport.set_terrain_editor(terrain_editor)


func bind_to_editor(value: Node) -> void:
	# The shell binds the app root; unwrap to the terrain domain editor (the
	# mission edit world lives under it). Bare TerrainEditor binds pass through.
	if value != null and value.has_method("get_terrain_editor"):
		value = value.get_terrain_editor()
	set_terrain_editor(value)


## Install the active mission's external terrain tile array (and preview
## time-of-day) onto the shared terrain preview.
func _sync_mission_preview_context() -> void:
	if not _mission_preview_context_active or terrain_editor == null \
			or not terrain_editor.has_method("set_mission_preview_context"):
		return
	var tile_info: NovaTerrainTileInfo = null
	if _controller != null and _controller.has_method("get_mission_tile_info"):
		tile_info = _controller.get_mission_tile_info()
	var preview_time_of_day := NAN
	if _controller != null and _controller.has_method("get_mission_preview_time_of_day"):
		preview_time_of_day = _controller.get_mission_preview_time_of_day()
	terrain_editor.set_mission_preview_context(tile_info, preview_time_of_day)


func _clear_mission_preview_context() -> void:
	if terrain_editor != null and terrain_editor.has_method("clear_mission_preview_context"):
		terrain_editor.clear_mission_preview_context()


# --- Identity -----------------------------------------------------------------

func get_workspace_id() -> String:
	return "mission"


func get_workspace_label() -> String:
	return "Mission"


func get_workspace_tooltip() -> String:
	return "Open a mission to load its world and view its placed objects."


func get_open_resource_kind() -> String:
	return "mission"


func get_project_title() -> String:
	if _controller == null:
		return "Mission"
	return _controller.get_mission_title()


func get_status_tool() -> String:
	return "Mission"


func get_status_context() -> String:
	if _controller != null and _controller.is_loaded():
		var stats: Dictionary = _controller.get_stats()
		return "%s, %d objects" % [_controller.get_mission_title(), int(stats.get("placed", 0))]
	return "Open a mission to load its world."


func shows_camera_status() -> bool:
	return true


# --- Lifecycle / viewport (shared, read-only terrain viewport) ----------------

func activate() -> void:
	_mission_preview_context_active = true
	_sync_mission_preview_context()
	if _controller != null:
		# Drop a loaded mission whose terrain was swapped out underneath from the
		# Terrain workspace before re-showing its objects (else they float over a new
		# world). On the SAME terrain, a non-zero return means height edits left that
		# many objects off the ground — offer the one-step re-ground.
		var drift: int = _controller.reconcile_with_terrain()
		_controller.set_objects_visible(true)
		if drift > 0:
			_prompt_reground(drift)
	if terrain_editor != null and _mount != null and _mount.is_mounted():
		terrain_editor.set_viewport_active(true, false)


# Terrain heights changed under the loaded mission (same .trn): offer the bulk
# re-ground. A transient ConfirmationDialog parented to the shell (the music
# workspace's deactivate prompt pattern), with the shell theme set EXPLICITLY —
# an embedded Window does not resolve the in-tree theme through the Control
# parent chain (see the shell's _ensure_unsaved_dialog comment) — and exclusive
# like the shell's own confirms. Escape/X emit `canceled`, so every dismissal
# lands on the decline path (acknowledge: quiet until the next height edit).
func _prompt_reground(count: int) -> void:
	if editor_shell == null:
		return  # headless host: the inspector's manual Re-ground button still covers it
	if _reground_dialog != null and is_instance_valid(_reground_dialog):
		return
	var dialog := ConfirmationDialog.new()
	dialog.name = "MissionRegroundDialog"
	dialog.title = "Terrain changed"
	dialog.dialog_text = "Terrain changed under %d object%s.\nRe-ground them to the new surface?" % [count, "" if count == 1 else "s"]
	dialog.exclusive = true
	if editor_shell is Control and (editor_shell as Control).theme != null:
		dialog.theme = (editor_shell as Control).theme
	dialog.get_ok_button().text = "Re-ground"
	dialog.get_cancel_button().text = "Leave as-is"
	dialog.confirmed.connect(func() -> void:
		# The is_loaded guard covers a stale confirm: a dialog that lost modality
		# (another exclusive sibling was up) can be answered after the mission was
		# cleared or swapped underneath it.
		if _controller.is_loaded():
			_controller.reground_drifted()  # reports "Re-grounded N..." via status_reported
		_reground_dialog = null
		dialog.queue_free())
	dialog.canceled.connect(func() -> void:
		_controller.acknowledge_terrain_drift()
		_reground_dialog = null
		dialog.queue_free())
	_reground_dialog = dialog
	_host_under_shell(dialog)
	dialog.popup_centered()


func deactivate() -> void:
	_mission_preview_context_active = false
	_clear_mission_preview_context()
	# Leaving the workspace dismisses the re-ground question without answering it:
	# the revision is NOT adopted, so the prompt re-poses on the next activate
	# (unlike the explicit "Leave as-is"). Also keeps the dialog from floating over
	# other workspaces or colliding with their own exclusive prompts.
	if _reground_dialog != null and is_instance_valid(_reground_dialog):
		_reground_dialog.queue_free()
	_reground_dialog = null
	# The debug overlay is mission-scoped UI; hide it (toggle pauses its refresh
	# timer too) so it never floats over another workspace. It re-summons in one
	# click and keeps its tab/filter state.
	if is_debug_overlay_open():
		_debug_overlay.toggle()
	if is_playing_mission():
		stop_play_mission()
	if _controller != null:
		# End any half-finished drag and drop the placement tool before leaving, so a
		# stray click after the user returns cannot resume either gesture.
		_controller.sim_stop()
		_controller.cancel_drag()
		_controller.disarm_placement()
		_controller.set_objects_visible(false)
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)


func mount_viewport(host: Control) -> void:
	if host == null or terrain_editor == null:
		return
	_viewport_host = host
	# A workspace re-mount while playing (shell relayout) keeps the play view up.
	if is_playing_mission():
		_ensure_play_mount().mount(host)
		return
	_sync_mission_preview_context()
	var viewport := _ensure_mount().mount(host)
	if viewport != null:
		viewport.set_terrain_editor(terrain_editor)
		# Terrain brush stays dormant; the controller handles picking / dragging via the
		# router's separate input_target instead.
		viewport.set_edit_input_enabled(false)
		viewport.set_input_target(_controller)


func unmount_viewport(_host: Control) -> void:
	if is_playing_mission():
		stop_play_mission()
	if _play_mount != null:
		_play_mount.unmount()
	_detach_input_target()
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	_mission_preview_context_active = false
	_clear_mission_preview_context()
	if is_playing_mission():
		stop_play_mission()
	if _play_mount != null:
		_play_mount.release()
	if _debug_overlay != null and is_instance_valid(_debug_overlay):
		_debug_overlay.queue_free()
	_debug_overlay = null
	_viewport_host = null
	_detach_input_target()
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.release()


func _detach_input_target() -> void:
	if _controller != null:
		_controller.cancel_drag()
		_controller.disarm_placement()
	if _mount == null:
		return
	var viewport := _mount.get_viewport_node()
	if viewport != null and viewport.has_method("set_input_target"):
		viewport.set_input_target(null)


func get_viewport_camera() -> Camera3D:
	if is_playing_mission():
		return _play_node().get_play_camera()
	if terrain_editor != null and terrain_editor.has_method("get_editor_camera"):
		return terrain_editor.get_editor_camera()
	return null


# --- Play-in-editor (PIE M4) ----------------------------------------------
# Play boots the REAL game loop (game_world.tscn + MissionRuntime at the game
# cadence) over the OPEN in-memory mission in a play viewport that replaces the
# edit viewport; the edit world survives unmounted. Structural input safety:
# while playing, the edit input router is out of the tree.

func is_playing_mission() -> bool:
	var play = _play_node()
	return play != null and play.is_playing()


func can_play_mission() -> bool:
	return _controller != null and _controller.is_loaded() and _viewport_host != null and editor_shell != null


func play_mission() -> Error:
	if is_playing_mission():
		return ERR_BUSY
	if not can_play_mission():
		_on_controller_status("Open a mission (with the editor docked) before playing.", true)
		return ERR_UNAVAILABLE
	var root: NovaResourceRoot = _resource_root()
	if root == null:
		_on_controller_status("No resource directory mounted to play from.", true)
		return ERR_UNCONFIGURED
	# End the in-place sim + any half-finished gesture, then swap viewports.
	_controller.sim_stop()
	_controller.cancel_drag()
	_controller.disarm_placement()
	_detach_input_target()
	if terrain_editor != null:
		terrain_editor.set_viewport_active(false, false)
	if _mount != null:
		_mount.unmount()
	var play = _ensure_play_mount().mount(_viewport_host)
	var bms_name: String = _controller.get_current_path().get_file()
	if bms_name.is_empty():
		bms_name = "untitled.bms"
	var err: Error = play.start(_controller.get_mission(), bms_name, root)
	if err != OK:
		# Failed boot: swap straight back so the editor never strands viewport-less.
		stop_play_mission()
		return err
	_sync_shell()
	return OK


func stop_play_mission() -> void:
	var play = _play_node()
	if play != null:
		play.stop()
	if _play_mount != null:
		_play_mount.unmount()
	# Remount the edit viewport exactly as mount_viewport does for a fresh switch.
	if _viewport_host != null and terrain_editor != null:
		var viewport := _ensure_mount().mount(_viewport_host)
		if viewport != null:
			viewport.set_terrain_editor(terrain_editor)
			viewport.set_edit_input_enabled(false)
			viewport.set_input_target(_controller)
		terrain_editor.set_viewport_active(true, false)
	_sync_shell()


# --- Mission debug overlay (C12) -------------------------------------------
# The same NovaDebugOverlay the game summons with F3, mounted over the editor
# shell with variable edits locked — summoned from the Simulate panel's debug
# button, or with F3 while playing (the play controller forwards the key). The
# runtime source is re-resolved on every overlay refresh, so Play/Stop/sim
# restarts need no rewiring here.

func toggle_debug_overlay() -> void:
	if editor_shell == null:
		return  # headless host: nothing to float the overlay over
	if _debug_overlay == null or not is_instance_valid(_debug_overlay):
		_debug_overlay = NovaDebugOverlay.new()
		_debug_overlay.name = "MissionDebugOverlay"
		# One-way lock BEFORE the first refresh can build rows: mission VARIABLES
		# stay read-only from the editor's overlay. The Sim-tab transport stays
		# live by design (the button tooltip advertises it) — its presses relay
		# through _on_overlay_transport so the controller stays in step.
		_debug_overlay.lock_writes("Editing is off while simulating from the editor.")
		_debug_overlay.set_runtime_source(Callable(self, "_debug_runtime_source"))
		_debug_overlay.set_view_context_source(Callable(self, "get_debug_view_context"))
		_debug_overlay.transport_used.connect(_on_overlay_transport)
		_debug_overlay.debug_option_changed.connect(_on_debug_option_changed)
		_debug_overlay.set_effect_world_source(Callable(self, "_debug_effect_world_source"))
		_debug_overlay.set_world_source(Callable(self, "_debug_world_source"))
		_debug_overlay.set_pick_list(_pick_list)
		_host_under_shell(_debug_overlay)
	_debug_overlay.toggle()
	# While the overlay is up during play, the play session frees the mouse so
	# the overlay takes clicks (the game shell gets this via its pause menu;
	# play-in-editor has none). The freed mouse also click-picks: install the
	# workspace's pick list on the PIE world and flip its click catcher with
	# the overlay (the SubViewportContainer forwards only play-view clicks).
	var play = _play_node()
	if play != null and play.has_method("set_capture_suspended"):
		play.set_capture_suspended(is_debug_overlay_open())
	if is_playing_mission():
		var world = _play_node().get_world()
		if world != null and world.has_method("set_pick_debug"):
			world.set_pick_debug(_pick_list)
			world.set_pick_click_enabled(is_debug_overlay_open())


# Debug options act on the PIE world / player host (game_world.tscn) the same
# way the game host does: the row's target/setter live in NovaDebugOptions, so
# this handler is the same generic shape as main_game's — the two hosts cannot
# drift. Only Play Mission has a GameWorld; the in-place Simulate driver has
# none, so options are inert there (consistent with the editor's read-only
# overlay stance).
func _on_debug_option_changed(id: StringName, value: Variant) -> void:
	if not is_playing_mission():
		return
	var option := NovaDebugOptions.find(id)
	if option.is_empty():
		return
	var play = _play_node()
	var target: Object = play.get_world() \
			if option["target"] == NovaDebugOptions.TARGET_WORLD \
			else play.get_player_host()
	if target != null and is_instance_valid(target) \
			and target.has_method(option["setter"]):
		target.callv(option["setter"], [value])


# The world-fed pages' data source (Stats counters and friends): only Play
# Mission has a GameWorld; the in-place Simulate driver renders none.
func _debug_world_source():
	if not is_playing_mission():
		return null
	return _play_node().get_world()


# The Particles tab's data source: only Play Mission has a GameWorld (and so
# an effect world); the in-place Simulate driver renders none.
func _debug_effect_world_source():
	if not is_playing_mission():
		return null
	var world = _play_node().get_world()
	return world.get_effect_world() if world != null else null


func is_debug_overlay_open() -> bool:
	return _debug_overlay != null and is_instance_valid(_debug_overlay) and _debug_overlay.visible


# The overlay's runtime supplier. PIE first: Play Mission boots the real game
# world (play_mission() sim_stops the in-place driver before swapping, so the
# two can never BOTH be live); otherwise the in-place Simulate driver, which is
# null while idle (the overlay shows its no-mission state).
func _debug_runtime_source():
	if is_playing_mission():
		var world = _play_node().get_world()
		return world.get_runtime() if world != null else null
	return _controller.get_sim_runtime() if _controller != null else null


## Exact PIE camera context consumed by the shared F3 debug snapshot.
func get_debug_view_context() -> DebugViewContext:
	if not is_playing_mission():
		return null
	var play = _play_node()
	if play == null:
		return null
	var context := DebugViewContext.new()
	var camera: Camera3D = play.get_play_camera()
	if camera != null and is_instance_valid(camera):
		context.camera = camera
	var player_host = play.get_player_host()
	if player_host != null and is_instance_valid(player_host):
		context.camera_mode_known = true
		context.third_person = bool(player_host.is_third_person())
	return context


## The live runtime an external surface (the MCP agent service) should read:
## the PIE world's when playing, else the in-place sim driver, else null.
## Public mirror of _debug_runtime_source, same resolution order.
func get_active_runtime():
	return _debug_runtime_source()


# The overlay drives the live runtime DIRECTLY (it is host-neutral and only
# knows a runtime), so its transport presses bypass MissionController's
# sim_play/sim_pause/sim_stop — whose `changed` signal is the only thing the
# editor sim bar refreshes on. Relay: Stop completes the editor-side stop
# (sim_stop frees the driver, unlocking edits and restoring the gizmo — the
# overlay's runtime.stop() alone left is_simulating() true over a visually
# stopped world); everything else just re-emits changed via the controller.
func _on_overlay_transport(action: String) -> void:
	if is_playing_mission():
		return  # PIE: the play world owns its transport; no editor sim bar involved
	if _controller == null:
		return
	if action == "stop":
		_controller.sim_stop()
	elif _controller.has_method("notify_sim_transport_changed"):
		_controller.notify_sim_transport_changed()


# --- New (create a mission from scratch) --------------------------------------
# The shell builds the New button on workspace switch when has_new_action() is true (base returns
# can_new()); visibility must not depend on a loaded mission, so can_new() needs only a bound
# terrain editor. The controller checks for a loaded terrain and reports if there is none.

func can_new() -> bool:
	return _controller != null and terrain_editor != null


func get_new_action_label() -> String:
	return "New Mission"


func new_current() -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.new_mission())
	var status: String = _controller.get_last_status()
	if not status.is_empty():
		if not _notify_status(status, &"info" if err == OK else &"error") and err != OK:
			push_warning("New mission: " + status)
	return err as Error


# --- Open ---------------------------------------------------------------------

func can_open() -> bool:
	return terrain_editor != null


func get_open_action_label() -> String:
	return "Open Mission..."


func get_open_dialog_title() -> String:
	return "Open mission (.bms, .mis)"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.bms ; Binary Mission", "*.mis ; Mission Metafile"])


func get_open_dialog_dir() -> String:
	return _controller.get_last_open_dir() if _controller != null else ""


func get_current_resource_path() -> String:
	return _controller.get_current_path() if _controller != null else ""


func open_file(path: String) -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.open_mission(path))
	# Surface the controller's detailed outcome (e.g. "missing dvxi5.trn", or the
	# placement summary). On failure the shell also shows a generic error code; the
	# success message is the one that lands for the user. Without a shell (headless),
	# a failure still goes to the log rather than vanishing.
	var status: String = _controller.get_last_status()
	if not status.is_empty():
		if not _notify_status(status, &"success" if err == OK else &"error") and err != OK:
			push_warning("Mission open: " + status)
	return err as Error


# The domain document the EditorWorkspace base derives undo/redo + dirty from.
func get_editor_document() -> Object:
	return _controller


# --- Save ---------------------------------------------------------------------
# The base EditorWorkspace exposes these hooks; implementing them lights up the Save /
# Save As buttons and the `*` title marker with no shell changes. save_current() saves
# in place; if there is no path it returns a non-OK and the shell routes to Save As.

# The shell builds action buttons only on workspace switch (before a mission is open),
# using has_save_action()/has_save_as_action() for visibility; it then refreshes only
# the disabled state on each change. So visibility must NOT depend on a loaded mission
# (else the button is never created), while enablement still does via can_save*().

func has_save_action() -> bool:
	return _controller != null


func has_save_as_action() -> bool:
	return _controller != null


func can_save() -> bool:
	return _controller != null and _controller.is_dirty() and not _controller.get_current_path().is_empty()


func get_save_action_label() -> String:
	return "Save Mission"


func can_save_as() -> bool:
	return _controller != null and _controller.is_loaded()


func get_save_as_action_label() -> String:
	return "Save Mission As..."


func save_current() -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.save_current())
	_report_save_status(err)
	return err as Error


func save_as(dir_path: String) -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.save_as(dir_path))
	_report_save_status(err)
	return err as Error


func uses_save_file_dialog() -> bool:
	return true


func get_save_file_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.bms ; Binary Mission", "*.mis ; Mission Metafile"])


func get_save_file_dialog_default_name() -> String:
	if _controller != null:
		var current: String = _controller.get_current_path().get_file()
		if not current.is_empty():
			return current
	return "mission.bms"


func save_as_file(path: String) -> Error:
	if _controller == null:
		return ERR_UNAVAILABLE
	var err := int(_controller.save_as_file(path))
	_report_save_status(err)
	return err as Error


func get_save_dialog_title() -> String:
	return "Choose where to save the mission"


func get_save_dialog_dir() -> String:
	return _controller.get_last_open_dir() if _controller != null else ""


func _report_save_status(err: int) -> void:
	var status: String = _controller.get_last_status() if _controller != null else ""
	if status.is_empty():
		return
	if not _notify_status(status, &"info" if err == OK else &"error") and err != OK:
		# No shell to show in (headless), but a failed save must not be silent.
		push_warning("Mission save: " + status)


# Surface a controller action's transient status in the shell status bar. Errors linger
# a little longer. No-op without a shell (headless tests read get_last_status instead).
func _on_controller_status(message: String, is_error: bool) -> void:
	if message.is_empty():
		return
	_notify_status(message, &"error" if is_error else &"success")


# --- Undo / redo --------------------------------------------------------------
# The base EditorWorkspace exposes these hooks; the controller drives the document's in-memory
# undo history. The live trigger is the viewport Ctrl+Z / Ctrl+Y (handled in MissionController);
# implementing the hooks makes undo/redo reachable for tests and a future shell toolbar with no
# shell change. Mirrors strings_workspace.gd.


# --- Inspector ----------------------------------------------------------------

func build_inspector(host: Control) -> void:
	_inspector = MissionInspectorScript.new()
	host.add_child(_inspector)
	# The dock host was forwarded just before this (set_asset_dock at shell line 585, build_inspector
	# at 592), so it is already cached: the fresh inspector builds its editor + Mission form straight
	# into the dock with no reparent.
	_inspector.setup(_controller, _detail_host)
	if _inspector.has_method("set_play_hooks"):
		_inspector.set_play_hooks(Callable(self, "play_mission"), Callable(self, "is_playing_mission"),
			Callable(self, "stop_play_mission"))
	if _inspector.has_method("set_debug_hooks"):
		_inspector.set_debug_hooks(Callable(self, "toggle_debug_overlay"),
			Callable(self, "is_debug_overlay_open"))
	if editor_shell != null and _inspector.has_method("set_reference_services"):
		_inspector.set_reference_services(ResourceRefWidget.services_from_shell(editor_shell))


# --- Asset dock (the right pane) ----------------------------------------------
# Opt into %AssetDock and host the per-selection editor + the Mission form there, keeping the left
# pane to just the mode tabs + the current mode's list/palette. The inspector owns the dock subtree
# and reparents it back under its own root on teardown (set_asset_dock(null)) before the shell frees
# the dock's children, so the editor widgets are never torn down.

func uses_asset_dock() -> bool:
	return true


func set_asset_dock(dock: Control) -> void:
	_detail_host = dock
	# is_instance_valid guards the gap between switch-away (old inspector freed) and switch-back
	# (set_asset_dock fires before build_inspector rebuilds it): skip the stale ref, just cache.
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.set_detail_host(dock)


func sync_asset_dock() -> void:
	if _inspector != null and is_instance_valid(_inspector) and _detail_host != null:
		_inspector.set_detail_host(_detail_host)
