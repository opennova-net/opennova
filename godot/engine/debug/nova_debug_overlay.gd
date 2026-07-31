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

## Fired after a fresh debug snapshot lands on disk — the local-player pose
## plus every picked entity's live state. The path is absolute so it can be
## pasted into an issue or opened directly.
signal debug_snapshot_dumped(path: String)

const REFRESH_INTERVAL := 0.25
const DEFAULT_PANEL_WIDTH := 560.0
const MIN_PANEL_WIDTH := 420.0
const PANEL_EDGE_MARGIN := 8.0
const SIDEBAR_WIDTH := 148.0
const RESIZE_HANDLE_WIDTH := 10.0
const COMPACT_NAV_PANEL_WIDTH := 480.0
const COPY_FEEDBACK_SECONDS := 1.5
const COPY_TOOLTIP := \
		"Copy a complete structured snapshot of the runtime and every debug control."
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
var _unlock_edits: CheckButton
var _copy_button: Button
var _copy_feedback_timer: Timer
var _page_list: ItemList
var _compact_page_picker: OptionButton
var _page_mount: ScrollContainer

var _pages: Array[NovaDebugPage] = []
var _active_page: NovaDebugPage = null
var _row_pages: Dictionary = {}  # sidebar row index -> NovaDebugPage
var _panel_width := DEFAULT_PANEL_WIDTH
var _applied_panel_width := DEFAULT_PANEL_WIDTH
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


func _ready() -> void:
	var viewport := get_viewport()
	if viewport != null and not viewport.size_changed.is_connected(
			_on_viewport_size_changed):
		viewport.size_changed.connect(_on_viewport_size_changed)
	_apply_panel_width()


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


## Convenience for owners holding one runtime instance directly.
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


## Supplier of the world owner (GameWorld or null) for world-fed pages (the
## Stats page's counters today).
func set_world_source(source: Callable) -> void:
	_ctx.world_source = source
	_session.set_target_source(NovaDebugCatalog.TARGET_WORLD,
			func(): return _ctx.world(), "No game world is loaded.")
	_session.set_target_source(NovaDebugCatalog.TARGET_TERRAIN,
			_resolve_terrain_target, "The current world has no terrain.")


## Supplier for LocalPlayerPresenter-owned presentation knobs.
func set_player_source(source: Callable) -> void:
	_session.set_target_source(NovaDebugCatalog.TARGET_PLAYER, source,
			"No local player presenter is active.")
	if visible:
		_refresh()


## Host authority supplier. A false result permanently rejects world-mutating
## edit actions for that observation; UI unlock and MCP confirmation never
## override joiner authority.
func set_authority_source(source: Callable) -> void:
	_session.set_authority_source(source)
	if visible:
		_refresh()


func get_debug_session() -> NovaDebugSession:
	return _session


## The shell-owned debug pick list (see NovaDebugPickList): the Entities page
## renders/curates it and snapshots embed it. Null detaches.
func set_pick_list(pick_list: NovaDebugPickList) -> void:
	_ctx.pick_list = pick_list
	if _entities_pane != null:
		_entities_pane.rebind_pick_list(pick_list)
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
		_focus_page_list()


func close() -> void:
	if visible:
		toggle()


## The shell-owned FrameStatsBoard feeding the Stats page (null detaches).
func set_frame_stats_board(board) -> void:
	_stats_pane.set_frame_stats_board(board)


## Append a page to the overlay (hosts and tests can add their own; the
## default set registers itself). The page is set up against the shared
## context and slots into its declared category.
func register_page(page: NovaDebugPage) -> void:
	page.setup(_ctx)
	page.visible = false
	page.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	page.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_pages.append(page)
	_page_mount.add_child(page)
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
## session, so the owning page's control re-syncs and the session's
## control_invoked fires exactly like a click.
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


## The complete payload used by Copy. Search is navigational and deliberately
## does not filter this snapshot.
func capture_clipboard_snapshot() -> Variant:
	var snapshot: Dictionary = _session.capture_snapshot()
	var picks: Array = _ctx.pick_list.get_picks() if _ctx.pick_list != null else []
	var mission_snapshot := DebugSnapshotWriter.capture(_ctx, picks)
	if not mission_snapshot.is_empty():
		snapshot["mission"] = mission_snapshot
	return snapshot


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
	_panel.offset_top = PANEL_EDGE_MARGIN
	_panel.offset_right = -PANEL_EDGE_MARGIN
	_panel.offset_bottom = -PANEL_EDGE_MARGIN
	_panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	var panel_surface := StyleBoxFlat.new()
	panel_surface.bg_color = Color(0.035, 0.04, 0.05, 0.94)
	panel_surface.border_color = Color(0.42, 0.47, 0.54, 0.72)
	panel_surface.border_width_left = 1
	panel_surface.border_width_top = 1
	panel_surface.border_width_right = 1
	panel_surface.border_width_bottom = 1
	panel_surface.corner_radius_top_left = 4
	panel_surface.corner_radius_top_right = 4
	panel_surface.corner_radius_bottom_left = 4
	panel_surface.corner_radius_bottom_right = 4
	panel_surface.content_margin_left = 4.0
	panel_surface.content_margin_top = 4.0
	panel_surface.content_margin_right = 4.0
	panel_surface.content_margin_bottom = 4.0
	_panel.add_theme_stylebox_override("panel", panel_surface)
	add_child(_panel)

	var frame := HBoxContainer.new()
	frame.name = "DebugFrame"
	frame.add_theme_constant_override("separation", 0)
	_panel.add_child(frame)

	var handle := VSeparator.new()
	handle.name = "DebugResizeHandle"
	handle.custom_minimum_size = Vector2(RESIZE_HANDLE_WIDTH, 0)
	handle.mouse_default_cursor_shape = Control.CURSOR_HSIZE
	handle.tooltip_text = "Drag to resize. Double-click to reset the dock width."
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
	title.text = "F3"
	title.tooltip_text = "Runtime debug cockpit"
	title.add_theme_font_size_override("font_size", 18)
	header.add_child(title)

	_runtime_status_label = Label.new()
	_runtime_status_label.name = "RuntimeStatus"
	_runtime_status_label.text = "NO MISSION"
	_runtime_status_label.tooltip_text = "The runtime currently inspected by this cockpit."
	_runtime_status_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_runtime_status_label.clip_text = true
	_runtime_status_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	_runtime_status_label.add_theme_font_size_override("font_size", 13)
	_runtime_status_label.add_theme_color_override(
			"font_color", Color(0.76, 0.8, 0.84))
	header.add_child(_runtime_status_label)

	_unlock_edits = CheckButton.new()
	_unlock_edits.name = "UnlockEdits"
	_unlock_edits.text = "Live edits"
	_unlock_edits.tooltip_text = \
			"Allow marked controls to change the running mission. The toggle changes " + \
			"nothing by itself; edits apply immediately, are not undoable, and remain " + \
			"host-only in multiplayer."
	_unlock_edits.toggled.connect(_on_unlock_edits_toggled)
	header.add_child(_unlock_edits)

	_copy_button = Button.new()
	_copy_button.name = "CopyDebugSnapshot"
	_copy_button.text = "Copy"
	_copy_button.tooltip_text = COPY_TOOLTIP
	_copy_button.pressed.connect(_on_copy_snapshot_pressed)
	header.add_child(_copy_button)

	var close_button := Button.new()
	close_button.name = "CloseDebug"
	close_button.text = "×"
	close_button.tooltip_text = "Close debug cockpit (Escape)"
	close_button.pressed.connect(close)
	header.add_child(close_button)

	_status_label = Label.new()
	_status_label.name = "DebugStatus"
	_status_label.text = "NO MISSION · Host-wide diagnostics remain available."
	_status_label.clip_text = true
	_status_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	_status_label.add_theme_font_size_override("font_size", 12)
	box.add_child(_status_label)

	var page_heading := HBoxContainer.new()
	page_heading.name = "ActivePageHeading"
	page_heading.add_theme_constant_override("separation", 8)
	box.add_child(page_heading)

	_page_title_label = Label.new()
	_page_title_label.name = "ActivePageTitle"
	_page_title_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_page_title_label.clip_text = true
	_page_title_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	_page_title_label.add_theme_font_size_override("font_size", 17)
	page_heading.add_child(_page_title_label)

	_page_help_label = Label.new()
	_page_help_label.name = "ActivePageHelp"
	_page_help_label.custom_minimum_size = Vector2(144, 0)
	_page_help_label.clip_text = true
	_page_help_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	_page_help_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_page_help_label.add_theme_font_size_override("font_size", 12)
	page_heading.add_child(_page_help_label)

	_compact_page_picker = OptionButton.new()
	_compact_page_picker.name = "CompactPagePicker"
	_compact_page_picker.fit_to_longest_item = false
	_compact_page_picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_compact_page_picker.tooltip_text = "Choose a debug page."
	_compact_page_picker.visible = false
	_compact_page_picker.item_selected.connect(_on_compact_page_selected)
	box.add_child(_compact_page_picker)

	var body := HBoxContainer.new()
	body.name = "DebugBody"
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_theme_constant_override("separation", 6)
	box.add_child(body)

	_page_list = ItemList.new()
	_page_list.name = "PageList"
	_page_list.custom_minimum_size = Vector2(SIDEBAR_WIDTH, 0)
	_page_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_page_list.focus_mode = Control.FOCUS_ALL
	_page_list.item_selected.connect(_on_page_row_selected)
	body.add_child(_page_list)

	_page_mount = ScrollContainer.new()
	_page_mount.name = "PageMount"
	_page_mount.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_page_mount.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# SHOW_NEVER is the width firewall: unlike DISABLED, it does not promote a
	# wide page child's minimum width into the dock's own minimum size.
	_page_mount.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_SHOW_NEVER
	_page_mount.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	_page_mount.follow_focus = true
	body.add_child(_page_mount)

	_copy_feedback_timer = Timer.new()
	_copy_feedback_timer.name = "CopyFeedbackTimer"
	_copy_feedback_timer.one_shot = true
	_copy_feedback_timer.wait_time = COPY_FEEDBACK_SECONDS
	_copy_feedback_timer.timeout.connect(_reset_copy_feedback)
	add_child(_copy_feedback_timer)


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
	_compact_page_picker.clear()
	_row_pages.clear()
	var categories: Array[StringName] = CATEGORY_ORDER.duplicate()
	for page in _pages:
		if not categories.has(page.page_category()):
			categories.append(page.page_category())
	for category in categories:
		var members: Array[NovaDebugPage] = []
		for page in _pages:
			if page.page_category() == category:
				members.append(page)
		if members.is_empty():
			continue
		var header := _page_list.add_item(String(category).to_upper(), null, false)
		_page_list.set_item_selectable(header, false)
		_page_list.set_item_custom_fg_color(header, Color(0.7, 0.73, 0.77))
		_page_list.set_item_custom_bg_color(header, Color(0.12, 0.13, 0.15, 0.86))
		for page in members:
			var row := _page_list.add_item(page.page_title())
			_row_pages[row] = page
			_page_list.set_item_tooltip(row, "%s · %s" % [
				page.page_title(), String(page.page_category())])
			_compact_page_picker.add_item(page.page_title())
			var picker_index := _compact_page_picker.item_count - 1
			_compact_page_picker.set_item_metadata(picker_index, page)
			_compact_page_picker.set_item_tooltip(picker_index, "%s · %s" % [
				page.page_title(), String(page.page_category())])
	_sync_page_list_selection()


func _sync_page_list_selection() -> void:
	var selected_row := -1
	for row in _row_pages:
		if _row_pages[row] == _active_page:
			selected_row = int(row)
			break
	if selected_row >= 0:
		_page_list.select(selected_row)
		_page_list.ensure_current_is_visible()
	else:
		_page_list.deselect_all()
	for index in range(_compact_page_picker.item_count):
		if _compact_page_picker.get_item_metadata(index) == _active_page:
			_compact_page_picker.select(index)
			_compact_page_picker.tooltip_text = \
					_compact_page_picker.get_item_tooltip(index)
			return
	if _compact_page_picker.item_count == 0:
		_compact_page_picker.tooltip_text = "No debug pages are available."


func _activate_page(page: NovaDebugPage, persist: bool) -> void:
	if _active_page == page:
		_sync_page_list_selection()
		_update_page_header()
		return
	if _active_page != null:
		_active_page.visible = false
		_active_page.set_capture_active(false)
	_page_mount.scroll_vertical = 0
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


func _on_compact_page_selected(index: int) -> void:
	var page := _compact_page_picker.get_item_metadata(index) as NovaDebugPage
	if page != null:
		_activate_page(page, true)


func _update_page_header() -> void:
	if _page_title_label == null or _page_help_label == null:
		return
	if _active_page == null:
		_page_title_label.text = "No debug page"
		_page_help_label.text = ""
		_page_help_label.tooltip_text = _page_help_label.text
		return
	_page_title_label.text = _active_page.page_title()
	if _active_page.page_title() == String(_active_page.page_category()):
		_page_help_label.text = ""
	else:
		_page_help_label.text = String(_active_page.page_category())
	_page_help_label.tooltip_text = _page_help_label.text


# --- Panel width + persistence ------------------------------------------------

func _set_panel_width(width: float) -> void:
	# Preserve the user's preferred width independently from the temporary
	# viewport fit. Growing the window restores the preference automatically.
	_panel_width = maxf(width, MIN_PANEL_WIDTH)
	_apply_panel_width()


func _apply_panel_width() -> void:
	var applied_width := _panel_width
	var viewport := get_viewport() if is_inside_tree() else null
	if viewport != null:
		var viewport_width := viewport.get_visible_rect().size.x
		applied_width = minf(applied_width,
				maxf(PANEL_EDGE_MARGIN, viewport_width - PANEL_EDGE_MARGIN))
	_applied_panel_width = applied_width
	if _panel != null:
		_panel.offset_left = -_applied_panel_width
	_update_responsive_navigation()


func _on_viewport_size_changed() -> void:
	_apply_panel_width()


func _update_responsive_navigation() -> void:
	if _page_list == null or _compact_page_picker == null:
		return
	var compact := _applied_panel_width < COMPACT_NAV_PANEL_WIDTH
	_page_list.visible = not compact
	_compact_page_picker.visible = compact


func _on_resize_handle_input(event: InputEvent) -> void:
	var button := event as InputEventMouseButton
	if button != null and button.button_index == MOUSE_BUTTON_LEFT:
		if button.pressed and button.double_click:
			_resizing = false
			_set_panel_width(DEFAULT_PANEL_WIDTH)
			NovaConfigStore.write(_config_path, CONFIG_SECTION, "panel_width",
					_panel_width)
			return
		_resizing = button.pressed
		if not button.pressed:
			NovaConfigStore.write(_config_path, CONFIG_SECTION, "panel_width",
					_panel_width)
		return
	var motion := event as InputEventMouseMotion
	if motion != null and _resizing:
		# The panel is right-anchored: dragging the handle left widens it.
		var width_basis := _panel_width if motion.relative.x < 0.0 \
				else minf(_panel_width, _applied_panel_width)
		_set_panel_width(width_basis - motion.relative.x)


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
	var has_authority := _session.has_host_authority()
	if has_sim and not has_authority and _session.is_edit_unlocked():
		# Authority loss is a safety edge, not a visual mask. Relock so returning
		# to a host/local role never silently restores mutation access.
		_session.set_edit_unlocked(false)
	var unlocked := _session.is_edit_unlocked()
	if not has_sim:
		_status_label.text = \
				"NO MISSION · Host-wide diagnostics remain available."
		_status_label.add_theme_color_override(
				"font_color", Color(0.68, 0.72, 0.76))
	elif not has_authority:
		_status_label.text = \
				"READ ONLY · Authoritative mission changes are host-only."
		_status_label.add_theme_color_override(
				"font_color", Color(0.68, 0.72, 0.76))
	elif unlocked:
		_status_label.text = \
				"LIVE EDITS · Changes apply immediately and are not undoable."
		_status_label.add_theme_color_override(
				"font_color", Color(1.0, 0.72, 0.34))
	else:
		_status_label.text = \
				"READ ONLY · Enable Live edits to change the running mission."
		_status_label.add_theme_color_override(
				"font_color", Color(0.68, 0.72, 0.76))
	_status_label.tooltip_text = _status_label.text
	_runtime_status_label.text = String(status.get("label", "No mission"))
	var runtime_detail := String(status.get("detail", ""))
	_runtime_status_label.tooltip_text = "%s%s" % [
		String(status.get("label", "No mission")),
		"\n" + runtime_detail if not runtime_detail.is_empty() else ""]
	if _unlock_edits != null:
		_unlock_edits.disabled = not has_sim or not has_authority
		_unlock_edits.set_pressed_no_signal(unlocked)
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
			"detail": "F3 remains available for process-wide diagnostics.",
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


func _on_unlock_edits_toggled(unlocked: bool) -> void:
	set_edit_unlocked(unlocked)


func _on_edit_unlock_changed(unlocked: bool) -> void:
	if _unlock_edits != null:
		_unlock_edits.set_pressed_no_signal(unlocked)
	if visible:
		_refresh()


func _on_copy_snapshot_pressed() -> void:
	var snapshot: Dictionary = capture_clipboard_snapshot()
	DisplayServer.clipboard_set(JSON.stringify(snapshot, "\t"))
	_copy_button.text = "Copied"
	_copy_button.tooltip_text = \
			"Copied the complete structured debug snapshot to the clipboard."
	_copy_feedback_timer.start()


func _reset_copy_feedback() -> void:
	if _copy_button == null:
		return
	_copy_button.text = "Copy"
	_copy_button.tooltip_text = COPY_TOOLTIP


func _focus_page_list() -> void:
	if _page_list == null or _row_pages.is_empty():
		return
	if _compact_page_picker.visible:
		_compact_page_picker.grab_focus()
		return
	_sync_page_list_selection()
	if _page_list.get_selected_items().is_empty():
		for row in _row_pages:
			_page_list.select(int(row))
			break
	_page_list.ensure_current_is_visible()
	_page_list.grab_focus()


func _cycle_page(direction: int) -> bool:
	if _row_pages.is_empty():
		return false
	var rows: Array = _row_pages.keys()
	rows.sort()
	var active_index := -1
	for index in range(rows.size()):
		if _row_pages[rows[index]] == _active_page:
			active_index = index
			break
	var next_index := 0 if active_index < 0 \
			else posmod(active_index + direction, rows.size())
	_activate_page(_row_pages[rows[next_index]], true)
	return true


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
	elif key.ctrl_pressed and key.keycode == KEY_PAGEUP:
		return _cycle_page(-1)
	elif key.ctrl_pressed and key.keycode == KEY_PAGEDOWN:
		return _cycle_page(1)
	return false


func _input(event: InputEvent) -> void:
	if handle_key_input(event):
		get_viewport().set_input_as_handled()
