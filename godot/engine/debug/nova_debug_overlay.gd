class_name NovaDebugOverlay
extends CanvasLayer
## The mission debug overlay: a window into the live runtime, summonable over
## ANY MissionRuntime host — F3 in the game, mountable over the editor's
## mission preview. Host-neutral on purpose: engine/ui primitives + a
## duck-typed runtime only, no editor-shell imports, so the shipped game
## carries it.
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

## Fired after a Sim-page transport press (play/pause/step/stop) or the script
## pause toggle acted on the runtime. Hosts whose own UI mirrors the runtime's
## transport state (the editor sim bar) listen and re-read; hosts without one
## (the game's F3 overlay) ignore it.
signal transport_used(action: String)

## Fired when any NovaDebugOptions registry option changes value — every
## host-actionable debug toggle (world debug views, foliage/particle hiding,
## the FP viewmodel experiments) rides this ONE channel. The overlay is
## host-neutral (no reach into the 3D scene), so it only emits intent; each
## host resolves the row's `target` to its own object (GameWorld /
## LocalPlayerHost) and calls the row's `setter` — one generic handler per
## host, wiring that cannot drift between the game and the editor.
signal debug_option_changed(id: StringName, value: Variant)

## Fired after the Player page writes a fresh local-player pose snapshot. The
## path is absolute so it can be pasted into an issue or opened directly.
signal local_player_pose_dumped(path: String)

const REFRESH_INTERVAL := 0.25
const DEFAULT_PANEL_WIDTH := 560.0
const MIN_PANEL_WIDTH := 500.0
const SIDEBAR_WIDTH := 128.0
const RESIZE_HANDLE_WIDTH := 6.0
const DEFAULT_CONFIG_PATH := "user://debug_overlay.cfg"
const CONFIG_SECTION := "overlay"

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
var _timer: Timer
var _panel: PanelContainer
var _status_label: Label
var _page_list: ItemList
var _page_host: MarginContainer

var _pages: Array[NovaDebugPage] = []
var _active_page: NovaDebugPage = null
var _row_pages: Dictionary = {}  # sidebar row index -> NovaDebugPage
var _panel_width := DEFAULT_PANEL_WIDTH
var _resizing := false

# Direct pane handles for the public delegates (also poked by tests).
var _stats_pane: DebugStatsPage
var _perf_pane: DebugPerfPage
var _vars_pane: DebugVarsPage
var _player_pane: DebugPlayerPage


func _init(config_path: String = DEFAULT_CONFIG_PATH) -> void:
	layer = 90
	_config_path = config_path
	_ctx.request_refresh = refresh_now
	_ctx.options = NovaDebugOptionState.new()
	_ctx.options.changed.connect(
			func(id: StringName, value: Variant): debug_option_changed.emit(id, value))
	_build_panel()
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


func toggle() -> void:
	visible = not visible
	_sync_timer()
	# Controls under a hidden CanvasLayer don't observe the layer hide; tell
	# the active page explicitly so capture-owning pages (Stats) close their
	# window with the overlay.
	if _active_page != null:
		_active_page.set_capture_active(visible)
	if visible:
		_refresh()


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
	_ctx.options.set_value(id, value)


func get_option_value(id: StringName) -> Variant:
	return _ctx.options.value(id)


func is_stats_capturing() -> bool:
	return _stats_pane.is_capturing()


func get_stats_display_snapshot() -> Array[DebugStatsDisplayRow]:
	return _stats_pane.get_display_snapshot()


## One-way lock on the variable-edit toggle, for hosts that must not let the
## overlay mutate the live sim (the editor summons it over a mission preview).
## `reason` is the caller's artist-facing tooltip copy. Deliberately no
## unlock: a locked overlay stays read-only for its whole life, so a host
## mode change can never silently re-arm edits.
func lock_writes(reason: String) -> void:
	_vars_pane.lock_writes(reason)
	if visible:
		_refresh()


## Sample the live runtime now and write one exact JSON pose snapshot. An
## optional target is useful for automation; the button uses the timestamped
## user-data location. Returns the absolute file path, or an empty string.
func dump_local_player_pose(path_override: String = "") -> String:
	return _player_pane.dump_local_player_pose(path_override)


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

	var title := Label.new()
	title.name = "DebugTitle"
	title.text = "Mission debug"
	box.add_child(title)

	_status_label = Label.new()
	_status_label.name = "DebugStatus"
	_status_label.text = "No mission running."
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_status_label)

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
	register_page(DebugEntitiesPage.new())

	var sim_page := DebugSimPage.new()
	sim_page.transport_used.connect(
			func(action: String): transport_used.emit(action))
	register_page(sim_page)

	_vars_pane = DebugVarsPage.new()
	register_page(_vars_pane)

	register_page(DebugNetPage.new())
	register_page(DebugParticlesPage.new())
	register_page(DebugOcclusionPage.new())
	register_page(DebugRoundsPage.new())
	register_page(DebugTerrainPage.new())
	register_page(DebugViewPage.new())

	_player_pane = DebugPlayerPage.new()
	_player_pane.local_player_pose_dumped.connect(
			func(path: String): local_player_pose_dumped.emit(path))
	register_page(_player_pane)

	_stats_pane = DebugStatsPage.new()
	register_page(_stats_pane)

	_perf_pane = DebugPerfPage.new()
	register_page(_perf_pane)


# --- Sidebar -----------------------------------------------------------------

func _rebuild_page_list() -> void:
	_page_list.clear()
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
		return
	if _active_page != null:
		_active_page.visible = false
		_active_page.set_capture_active(false)
	_active_page = page
	page.visible = true
	_sync_page_list_selection()
	page.set_capture_active(visible)
	if visible:
		page.refresh()
	if persist:
		NovaConfigStore.write(_config_path, CONFIG_SECTION, "last_page",
				String(page.page_id()))


func _on_page_row_selected(row: int) -> void:
	var page: NovaDebugPage = _row_pages.get(row)
	if page != null:
		_activate_page(page, true)


# --- Panel width + persistence ------------------------------------------------

func _set_panel_width(width: float) -> void:
	# Floor only: a too-narrow panel is unusable, while an over-wide one is
	# self-correcting (drag it back). No viewport-fraction cap — headless
	# viewports report unreliable sizes.
	_panel_width = maxf(width, MIN_PANEL_WIDTH)
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
	_status_label.visible = _ctx.sim() == null
	if _active_page != null:
		_active_page.refresh()
