class_name NovaDebugOverlay
extends CanvasLayer
## The mission debug overlay: the real game's F3 cockpit over its live
## MissionRuntime. It stays engine/UI-only and duck-types public runtime
## surfaces so the debug catalog is also usable by runtime automation.
##
## The overlay is the shell: a right-docked, drag-resizable panel with a
## categorized page list on the left and one active NovaDebugPage on the
## right. Pages register through register_page(); each declares its identity
## and reads live state through the shared NovaDebugContext. Only the ACTIVE
## page refreshes on the low-Hz timer (plus immediately on selection), so
## idle pages cost nothing.
##
## The runtime is re-resolved through a Callable on EVERY refresh — mission
## reloads free and recreate the MissionRuntime, so a held reference would go
## stale. Panel width and the last-selected page persist per user via
## NovaConfigStore; live toggles deliberately do not.

## Compatibility notification after a NovaDebugOptions control has already
## been applied through the shared session's public target.
signal debug_option_changed(id: StringName, value: Variant)

## Fired after a fresh debug snapshot lands on disk — the local-player pose
## plus every picked entity's live state. The path is absolute so it can be
## pasted into an issue or opened directly.
signal debug_snapshot_dumped(path: String)

const REFRESH_INTERVAL := 0.25
const DEFAULT_PANEL_WIDTH := 560.0
const MIN_PANEL_WIDTH := 420.0
const SIDEBAR_WIDTH := 148.0
const RESIZE_HANDLE_WIDTH := 6.0
const DEFAULT_CONFIG_PATH := "user://debug_overlay.cfg"
const CONFIG_SECTION := "overlay"
const RenderingPageScript := preload(
		"res://engine/debug/pages/debug_rendering_page.gd")

## Sidebar section order; register_page categories outside this list append
## after, in first-seen order.
const CATEGORY_ORDER: Array[StringName] = [
	NovaDebugPage.CATEGORY_SIM,
	NovaDebugPage.CATEGORY_WORLD,
	NovaDebugPage.CATEGORY_PLAYER,
	NovaDebugPage.CATEGORY_DIAGNOSTICS,
]

var _config_path: String
var _ctx := NovaDebugContext.new()
var _session: NovaDebugSession
var _timer: Timer
var _panel: PanelContainer
var _status_label: Label
var _runtime_status_label: Label
var _page_title_label: Label
var _page_help_label: Label
var _search_edit: LineEdit
var _unlock_edits: CheckButton
var _page_list: ItemList
var _page_host: MarginContainer

var _pages: Array[NovaDebugPage] = []
var _active_page: NovaDebugPage = null
var _empty_search_restore_page: NovaDebugPage = null
var _row_pages: Dictionary = {}  # sidebar row index -> NovaDebugPage
var _panel_width := DEFAULT_PANEL_WIDTH
var _resizing := false

# Direct pane handles for the public delegates (also poked by tests).
var _stats_pane: DebugStatsPage
var _perf_pane: DebugPerfPage
var _vars_pane: DebugVarsPage
var _player_pane: DebugPlayerPage
var _entities_pane: DebugEntitiesPage


func _init(
		config_path: String = DEFAULT_CONFIG_PATH,
		shared_session: NovaDebugSession = null) -> void:
	layer = 90
	_config_path = config_path
	_session = shared_session if shared_session != null \
			else NovaDebugSession.new()
	_ctx.request_refresh = refresh_now
	_ctx.session = _session
	_ctx.options = NovaDebugOptionState.new()
	_ctx.options.changed.connect(
			func(id: StringName, value: Variant):
				_session.set_control_value(id, value))
	_ctx.dump_snapshot = _dump_snapshot_internal
	NovaDebugCatalog.install(_session)
	if shared_session == null:
		_bind_builtin_targets()
	_session.control_invoked.connect(_on_session_control_invoked)
	_build_panel()
	_session.edit_unlock_changed.connect(_on_edit_unlock_changed)
	_on_edit_unlock_changed(_session.is_edit_unlocked())
	_build_default_pages()
	_timer = Timer.new()
	_timer.wait_time = REFRESH_INTERVAL
	_timer.autostart = true
	_timer.timeout.connect(_refresh)
	add_child(_timer)
	visible = false
	_sync_timer()
	_restore_config()


## The runtime supplier: a Callable returning the current MissionRuntime (or
## null). Re-resolved every refresh because reloads recreate the runtime.
func set_runtime_source(source: Callable) -> void:
	_ctx.runtime_source = source
	_session.set_target_source(NovaDebugCatalog.TARGET_RUNTIME,
			func(): return _ctx.runtime(), "No mission runtime is active.")
	_session.set_target_source(NovaDebugCatalog.TARGET_SIM,
			func(): return _ctx.sim(), "No simulation is active.")
	if visible:
		_refresh()


## Optional supplier for the exact NovaDebugViewContext used to render and
## dispatch foliage. It is sampled with the player pose so a disk snapshot
## reproduces the visual viewpoint, not just the player root.
func set_view_context_source(source: Callable) -> void:
	_ctx.view_context_source = source


## Convenience for hosts holding one runtime instance directly.
func set_runtime(runtime) -> void:
	var ref: WeakRef = weakref(runtime)
	set_runtime_source(func(): return ref.get_ref())


## The effect-world supplier for the Particles page: a Callable returning the
## live NovaEffectWorld (or null). Re-resolved every refresh — mission loads
## free and rebuild the effect world.
func set_effect_world_source(source: Callable) -> void:
	_ctx.effect_world_source = source
	if visible:
		_refresh()


## Supplier of the world host (GameWorld or null) for world-fed pages (the
## Stats page's counters today).
func set_world_source(source: Callable) -> void:
	_ctx.world_source = source
	_session.set_target_source(NovaDebugCatalog.TARGET_WORLD,
			func(): return _ctx.world(), "No game world is loaded.")
	_session.set_target_source(NovaDebugCatalog.TARGET_TERRAIN,
			_resolve_terrain_target, "The current world has no terrain.")


## Supplier for LocalPlayerHost-owned presentation knobs.
func set_player_source(source: Callable) -> void:
	_session.set_target_source(NovaDebugCatalog.TARGET_PLAYER, source,
			"No local player presentation host is active.")
	if visible:
		_refresh()


## Host authority supplier. A false result permanently rejects world-mutating
## edit actions for that observation; UI unlock and MCP confirmation never
## override joiner authority.
func set_authority_source(source: Callable) -> void:
	_session.set_authority_source(source)


func get_debug_session() -> NovaDebugSession:
	return _session


## The host-owned debug pick list (see NovaDebugPickList): the Entities page
## renders/curates it and snapshots embed it. Null detaches.
func set_pick_list(pick_list: NovaDebugPickList) -> void:
	_ctx.pick_list = pick_list
	if visible:
		_refresh()


func toggle() -> void:
	visible = not visible
	_session.set_presented(visible)
	_sync_timer()
	# Controls under a hidden CanvasLayer don't observe the layer hide; tell
	# the active page explicitly so capture-owning pages (Stats) close their
	# window with the overlay.
	if _active_page != null:
		_active_page.set_capture_active(visible)
	if visible:
		_refresh()
		if _search_edit != null:
			_search_edit.grab_focus()


func close() -> void:
	if visible:
		toggle()


## The host-owned FrameStatsBoard feeding the Stats page (null detaches).
func set_frame_stats_board(board) -> void:
	_stats_pane.set_frame_stats_board(board)


## Append a page to the overlay (hosts and tests can add their own; the
## default set registers itself). The page is set up against the shared
## context and slots into its declared category.
func register_page(page: NovaDebugPage) -> void:
	page.setup(_ctx)
	page.visible = false
	_pages.append(page)
	_page_host.add_child(page)
	_rebuild_page_list()


## Select a page by its stable id. Returns false for unknown ids.
func select_page(page_id: StringName) -> bool:
	for page in _pages:
		if page.page_id() == page_id:
			_activate_page(page, true)
			return true
	return false


func get_active_page_id() -> StringName:
	return _active_page.page_id() if _active_page != null else &""


## Programmatic option write (tests, automation): routes through the shared
## state, so the owning page's control re-syncs and debug_option_changed fires
## exactly like a click.
func set_option(id: StringName, value: Variant) -> void:
	_session.set_control_value(id, value)


func get_option_value(id: StringName) -> Variant:
	return _session.get_control_state(id).value


func list_controls(
		page_id: StringName = &"",
		filter_text: String = "") -> Array[Dictionary]:
	return _session.list_controls(page_id, filter_text)


func capture_control_snapshot(filter_text: String = "") -> Variant:
	return _session.capture_snapshot(filter_text)


func is_stats_capturing() -> bool:
	return _stats_pane.is_capturing()


func get_stats_display_snapshot() -> Array[DebugStatsDisplayRow]:
	return _stats_pane.get_display_snapshot()


func set_edit_unlocked(unlocked: bool) -> void:
	_session.set_edit_unlocked(unlocked)


## Sample the live runtime now and write one exact JSON debug snapshot — the
## local-player pose plus every picked entity's live state. An optional
## target is useful for automation; the pages' buttons use the timestamped
## user-data location. Returns the absolute file path, or an empty string.
func dump_debug_snapshot(path_override: String = "") -> String:
	return String(_dump_snapshot_internal(path_override).get("path", ""))


## The page-facing dump seam (ctx.dump_snapshot): capture + write + announce.
## Every dump — a page button OR the programmatic API — pushes its outcome
## back onto both dump-hosting pages' status labels, so the newest result is
## always visible wherever the developer is looking.
func _dump_snapshot_internal(path_override: String) -> Dictionary:
	var picks: Array = _ctx.pick_list.get_picks() if _ctx.pick_list != null else []
	var snapshot := DebugSnapshotWriter.capture(_ctx, picks)
	var result: Dictionary
	if snapshot.is_empty():
		result = {"path": "",
				"error": "Start a playable mission to capture the local player."}
	else:
		var target := path_override
		if target.is_empty():
			target = DebugSnapshotWriter.default_path(snapshot)
		result = DebugSnapshotWriter.write(snapshot, target)
	_player_pane.show_dump_result(result)
	_entities_pane.show_dump_result(result)
	if not String(result.get("path", "")).is_empty():
		debug_snapshot_dumped.emit(result["path"])
	return result


func refresh_now() -> void:
	_refresh()


func _sync_timer() -> void:
	if _timer != null:
		_timer.paused = not visible


# --- Panel construction --------------------------------------------------

func _build_panel() -> void:
	_panel = PanelContainer.new()
	_panel.name = "DebugPanel"
	_panel.anchor_left = 1.0
	_panel.anchor_right = 1.0
	_panel.anchor_bottom = 1.0
	_panel.offset_left = -_panel_width
	_panel.offset_top = 8.0
	_panel.offset_right = -8.0
	_panel.offset_bottom = -8.0
	_panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	add_child(_panel)

	var frame := HBoxContainer.new()
	frame.name = "DebugFrame"
	frame.add_theme_constant_override("separation", 0)
	_panel.add_child(frame)

	var handle := Control.new()
	handle.name = "DebugResizeHandle"
	handle.custom_minimum_size = Vector2(RESIZE_HANDLE_WIDTH, 0)
	handle.mouse_default_cursor_shape = Control.CURSOR_HSIZE
	handle.tooltip_text = "Drag to resize"
	handle.gui_input.connect(_on_resize_handle_input)
	frame.add_child(handle)

	var box := VBoxContainer.new()
	box.name = "DebugContent"
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 6)
	frame.add_child(box)

	var header := HBoxContainer.new()
	header.name = "DebugHeader"
	header.add_theme_constant_override("separation", 4)
	box.add_child(header)

	var title := Label.new()
	title.name = "DebugTitle"
	title.text = "Debug cockpit"
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(title)

	_runtime_status_label = Label.new()
	_runtime_status_label.name = "RuntimeStatus"
	_runtime_status_label.text = "NO MISSION"
	_runtime_status_label.tooltip_text = "The runtime currently inspected by this cockpit."
	header.add_child(_runtime_status_label)

	_unlock_edits = CheckButton.new()
	_unlock_edits.name = "UnlockEdits"
	_unlock_edits.text = "Edit"
	_unlock_edits.tooltip_text = \
			"Unlock live mission edits. This never overrides multiplayer authority."
	_unlock_edits.toggled.connect(_on_unlock_edits_toggled)
	header.add_child(_unlock_edits)

	var copy_button := Button.new()
	copy_button.name = "CopyDebugSnapshot"
	copy_button.text = "Copy"
	copy_button.tooltip_text = \
			"Copy a structured snapshot of the runtime and every debug control."
	copy_button.focus_mode = Control.FOCUS_NONE
	copy_button.pressed.connect(_on_copy_snapshot_pressed.bind(copy_button))
	header.add_child(copy_button)

	var close_button := Button.new()
	close_button.name = "CloseDebug"
	close_button.text = "Close"
	close_button.tooltip_text = "Close debug cockpit (Escape)"
	close_button.focus_mode = Control.FOCUS_NONE
	close_button.pressed.connect(close)
	header.add_child(close_button)

	_search_edit = LineEdit.new()
	_search_edit.name = "DebugSearch"
	_search_edit.placeholder_text = "Filter pages and controls"
	_search_edit.clear_button_enabled = true
	_search_edit.tooltip_text = "Search page names and public debug controls (Ctrl+F)."
	_search_edit.text_changed.connect(_on_search_changed)
	box.add_child(_search_edit)

	_status_label = Label.new()
	_status_label.name = "DebugStatus"
	_status_label.text = "No mission running."
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_status_label)

	_page_title_label = Label.new()
	_page_title_label.name = "ActivePageTitle"
	box.add_child(_page_title_label)

	_page_help_label = Label.new()
	_page_help_label.name = "ActivePageHelp"
	_page_help_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_page_help_label)

	var body := HBoxContainer.new()
	body.name = "DebugBody"
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_theme_constant_override("separation", 6)
	box.add_child(body)

	_page_list = ItemList.new()
	_page_list.name = "PageList"
	_page_list.custom_minimum_size = Vector2(SIDEBAR_WIDTH, 0)
	_page_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_page_list.focus_mode = Control.FOCUS_NONE
	_page_list.item_selected.connect(_on_page_row_selected)
	body.add_child(_page_list)

	_page_host = MarginContainer.new()
	_page_host.name = "PageHost"
	_page_host.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_page_host.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(_page_host)


## The built-in page set, in registration order. Registry toggles wire
## themselves through ctx.options; only the two page-specific signals
## (transport, pose dump) are re-emitted here.
func _build_default_pages() -> void:
	_entities_pane = DebugEntitiesPage.new()
	register_page(_entities_pane)

	register_page(DebugSimPage.new())

	_vars_pane = DebugVarsPage.new()
	register_page(_vars_pane)

	register_page(DebugNetPage.new())
	register_page(DebugParticlesPage.new())
	register_page(DebugOcclusionPage.new())
	register_page(DebugRoundsPage.new())
	register_page(DebugTerrainPage.new())
	register_page(RenderingPageScript.new())
	register_page(DebugEnvironmentPage.new())
	register_page(DebugAudioPage.new())
	register_page(DebugAnimationPage.new())

	_player_pane = DebugPlayerPage.new()
	register_page(_player_pane)

	_stats_pane = DebugStatsPage.new()
	register_page(_stats_pane)

	_perf_pane = DebugPerfPage.new()
	register_page(_perf_pane)


# --- Sidebar -----------------------------------------------------------------

func _rebuild_page_list() -> void:
	_page_list.clear()
	_row_pages.clear()
	var filter_text := _search_edit.text.strip_edges() if _search_edit != null else ""
	var categories: Array[StringName] = CATEGORY_ORDER.duplicate()
	for page in _pages:
		if not categories.has(page.page_category()):
			categories.append(page.page_category())
	for category in categories:
		var members: Array[NovaDebugPage] = []
		for page in _pages:
			if page.page_category() == category and _page_matches(page, filter_text):
				members.append(page)
		if members.is_empty():
			continue
		var header := _page_list.add_item(String(category).to_upper(), null, false)
		_page_list.set_item_selectable(header, false)
		_page_list.set_item_disabled(header, true)
		_page_list.set_item_custom_fg_color(header, Color(0.62, 0.62, 0.62))
		for page in members:
			var row := _page_list.add_item("  " + page.page_title())
			_row_pages[row] = page
	_sync_page_list_selection()


func _sync_page_list_selection() -> void:
	for row in _row_pages:
		if _row_pages[row] == _active_page:
			_page_list.select(int(row))
			return
	_page_list.deselect_all()


func _activate_page(page: NovaDebugPage, persist: bool) -> void:
	if _active_page == page:
		_sync_page_list_selection()
		_update_page_header()
		return
	if _active_page != null:
		_active_page.visible = false
		_active_page.set_capture_active(false)
	_active_page = page
	if page == null:
		_update_page_header()
		_sync_page_list_selection()
		return
	page.visible = true
	_update_page_header()
	_sync_page_list_selection()
	page.set_capture_active(visible)
	if visible:
		page.refresh()
		page.refresh_debug_controls()
	if persist:
		NovaConfigStore.write(_config_path, CONFIG_SECTION, "last_page",
				String(page.page_id()))


func _on_page_row_selected(row: int) -> void:
	var page: NovaDebugPage = _row_pages.get(row)
	if page != null:
		_activate_page(page, true)


func _on_search_changed(_text: String) -> void:
	_rebuild_page_list()
	if _active_page != null and _row_pages.values().has(_active_page):
		_empty_search_restore_page = null
		return
	if _row_pages.is_empty():
		if _active_page != null:
			_empty_search_restore_page = _active_page
		_activate_page(null, false)
		return
	if _empty_search_restore_page != null \
			and _row_pages.values().has(_empty_search_restore_page):
		var restore_page := _empty_search_restore_page
		_empty_search_restore_page = null
		_activate_page(restore_page, false)
		return
	_empty_search_restore_page = null
	for row in _row_pages:
		_activate_page(_row_pages[row], false)
		return


func _page_matches(page: NovaDebugPage, filter_text: String) -> bool:
	var needle := filter_text.to_lower()
	if needle.is_empty():
		return true
	var page_text := ("%s %s %s" % [
			page.page_id(), page.page_title(), page.page_category()]).to_lower()
	if page_text.contains(needle):
		return true
	return not _session.list_controls(page.page_id(), needle).is_empty()


func _update_page_header() -> void:
	if _page_title_label == null or _page_help_label == null:
		return
	if _active_page == null:
		_page_title_label.text = "No matching page"
		_page_help_label.text = "Clear the filter to show every debug page."
		return
	_page_title_label.text = _active_page.page_title()
	var control_count := _session.list_controls(_active_page.page_id()).size()
	_page_help_label.text = "%s | %d public control%s" % [
		String(_active_page.page_category()), control_count,
		"" if control_count == 1 else "s"]


# --- Panel width + persistence ------------------------------------------------

func _set_panel_width(width: float) -> void:
	# Floor only: a too-narrow panel is unusable, while an over-wide one is
	# self-correcting (drag it back). No viewport-fraction cap — headless
	# viewports report unreliable sizes.
	var maximum := INF
	var viewport := get_viewport() if is_inside_tree() else null
	if viewport != null:
		var viewport_width := viewport.get_visible_rect().size.x
		if viewport_width > MIN_PANEL_WIDTH:
			maximum = maxf(MIN_PANEL_WIDTH, viewport_width - 16.0)
	_panel_width = clampf(width, MIN_PANEL_WIDTH, maximum)
	if _panel != null:
		_panel.offset_left = -_panel_width


func _on_resize_handle_input(event: InputEvent) -> void:
	var button := event as InputEventMouseButton
	if button != null and button.button_index == MOUSE_BUTTON_LEFT:
		_resizing = button.pressed
		if not button.pressed:
			NovaConfigStore.write(_config_path, CONFIG_SECTION, "panel_width",
					_panel_width)
		return
	var motion := event as InputEventMouseMotion
	if motion != null and _resizing:
		# The panel is right-anchored: dragging the handle left widens it.
		_set_panel_width(_panel_width - motion.relative.x)


func _restore_config() -> void:
	_set_panel_width(float(NovaConfigStore.read(
			_config_path, CONFIG_SECTION, "panel_width", DEFAULT_PANEL_WIDTH)))
	var last_page := StringName(String(NovaConfigStore.read(
			_config_path, CONFIG_SECTION, "last_page", "")))
	for page in _pages:
		if page.page_id() == last_page:
			_activate_page(page, false)
			return
	if not _pages.is_empty():
		_activate_page(_pages[0], false)


# --- Refresh ---------------------------------------------------------------

func _refresh() -> void:
	_session.sync()
	var status := _runtime_status()
	var has_sim := _ctx.sim() != null
	_status_label.visible = not has_sim
	_status_label.text = "No playable mission is running." if not has_sim else ""
	_runtime_status_label.text = String(status.get("label", "NO MISSION")).to_upper()
	_runtime_status_label.tooltip_text = String(status.get("detail", ""))
	if _unlock_edits != null:
		_unlock_edits.disabled = not has_sim
	if _active_page != null:
		_active_page.refresh()
		_active_page.refresh_debug_controls()


func _bind_builtin_targets() -> void:
	NovaDebugCatalog.bind_runtime_targets(
			_session,
			func(): return _ctx.runtime(),
			func(): return _ctx.world(),
			Callable(),
			_resolve_viewport_target,
			_resolve_scene_tree_target)
	_session.set_authority_source(_has_runtime_authority)
	_session.set_status_source(_runtime_status)


func _resolve_terrain_target() -> Object:
	var world := _ctx.world()
	if world == null or not world.has_method("get_terrain_node"):
		return null
	var terrain: Variant = world.get_terrain_node()
	return terrain if terrain is Object and is_instance_valid(terrain) else null


func _resolve_viewport_target() -> Object:
	return get_viewport() if is_inside_tree() else null


func _resolve_scene_tree_target() -> Object:
	return get_tree() if is_inside_tree() else null


func _has_runtime_authority() -> bool:
	var sim := _ctx.sim()
	if sim == null:
		return false
	return not bool(sim.is_joiner()) if sim.has_method("is_joiner") else true


func _runtime_status() -> Dictionary:
	var runtime := _ctx.runtime()
	var sim := _ctx.sim()
	if runtime == null or sim == null:
		return {
			"label": "No mission",
			"detail": "F3 remains available for host-wide diagnostics.",
		}
	var mission_name := ""
	if runtime.has_method("get_mission_name"):
		mission_name = String(runtime.get_mission_name())
	if mission_name.is_empty() and runtime.has_method("get_mission_file"):
		mission_name = String(runtime.get_mission_file()).get_basename().get_file()
	if mission_name.is_empty():
		mission_name = "Mission"
	var role := "local"
	if sim.has_method("is_joiner") and bool(sim.is_joiner()):
		role = "joiner"
	elif sim.has_method("is_host_listening") and bool(sim.is_host_listening()):
		role = "host"
	var playing := not runtime.has_method("is_playing") or bool(runtime.is_playing())
	return {
		"label": "%s | %s%s" % [
			mission_name, role, "" if playing else " | paused"],
		"detail": "Runtime targets are resolved live on every read and write.",
		"mission": mission_name,
		"role": role,
		"playing": playing,
	}


func _on_session_control_invoked(id: StringName, value: Variant) -> void:
	if not NovaDebugOptions.find(id).is_empty():
		debug_option_changed.emit(id, value)


func _on_unlock_edits_toggled(unlocked: bool) -> void:
	set_edit_unlocked(unlocked)


func _on_edit_unlock_changed(unlocked: bool) -> void:
	if _unlock_edits != null:
		_unlock_edits.set_pressed_no_signal(unlocked)
	if visible:
		_refresh()


func _on_copy_snapshot_pressed(button: Button) -> void:
	var snapshot: Dictionary = _session.capture_snapshot(
			_search_edit.text if _search_edit != null else "")
	var picks: Array = _ctx.pick_list.get_picks() if _ctx.pick_list != null else []
	var mission_snapshot := DebugSnapshotWriter.capture(_ctx, picks)
	if not mission_snapshot.is_empty():
		snapshot["mission"] = mission_snapshot
	DisplayServer.clipboard_set(JSON.stringify(snapshot, "\t"))
	button.text = "Copied"
	button.tooltip_text = "Copied the structured debug snapshot to the clipboard."


## Apply overlay-owned keyboard gestures. The engine callback delegates here;
## hosts and tests can exercise the same deterministic shortcut contract
## without invoking a private notification method.
func handle_key_input(event: InputEvent) -> bool:
	var key := event as InputEventKey
	if not visible or key == null or not key.pressed or key.echo:
		return false
	if key.keycode == KEY_ESCAPE:
		close()
		return true
	elif key.keycode == KEY_F and key.ctrl_pressed and _search_edit != null:
		_search_edit.grab_focus()
		_search_edit.select_all()
		return true
	return false


func _unhandled_key_input(event: InputEvent) -> void:
	if handle_key_input(event):
		get_viewport().set_input_as_handled()
