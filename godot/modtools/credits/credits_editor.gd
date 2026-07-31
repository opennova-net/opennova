class_name CreditsEditor
extends Control

const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")
const PREVIEW_WHEEL_PIXELS := 48.0

enum Mode { VISUAL, SOURCE }

@onready var _visual_button: Button = %VisualButton
@onready var _source_button: Button = %SourceButton
@onready var _scroll_rate_spin: SpinBox = %ScrollRateSpin
@onready var _vertical_space_spin: SpinBox = %VerticalSpaceSpin
@onready var _center_x_spin: SpinBox = %CenterXSpin
@onready var _block_list_mount: Control = %BlockListMount
@onready var _source_view_mount: Control = %SourceViewMount
@onready var _block_list: Control = %BlockList
@onready var _add_text_button: Button = %AddTextButton
@onready var _add_image_button: Button = %AddImageButton
@onready var _add_newline_button: Button = %AddNewlineButton
@onready var _block_scroll: ScrollContainer = %BlockScroll
@onready var _player: NovaCreditsPlayer = %Player
@onready var _play_button: Button = %Play
@onready var _pause_button: Button = %Pause
@onready var _stop_button: Button = %Stop
@onready var _speed_spin: SpinBox = %Speed
@onready var _scrub_slider: HSlider = %Scrub
@onready var _empty_hint: Control = %EmptyHint

var _document: CreditsEditorDocument
var _mode: Mode = Mode.VISUAL
var _suppress_env_signals := false
var _sync_suppress := false
var _preview_resource: CbinCreditsResource
var _preview_paused := false
var _resource_root: NovaResourceRoot
var _ref_services: Dictionary = {}

func set_document(value: CreditsEditorDocument) -> void:
	if _document == value:
		return
	if _document != null:
		_document.resource_loaded.disconnect(_on_resource_loaded)
		_document.resource_changed.disconnect(_on_resource_changed)
	_document = value
	if _block_list != null and _block_list.has_method("set_document"):
		_block_list.set_document(_document)
	if _source_view_mount != null and _source_view_mount.has_method("set_document"):
		_source_view_mount.set_document(_document)
	if _document != null:
		_document.resource_loaded.connect(_on_resource_loaded)
		_document.resource_changed.connect(_on_resource_changed)
		_on_resource_loaded(_document.resource)

func set_resource_root_dir(path: String) -> void:
	var resources := NovaResourceRoot.new()
	_resource_root = resources if resources.set_root_dir(path) == OK else null
	if _block_list != null and _block_list.has_method("set_resource_root"):
		_block_list.set_resource_root(_resource_root)

func set_resource_root(value: NovaResourceRoot) -> void:
	_resource_root = value
	if _block_list != null and _block_list.has_method("set_resource_root"):
		_block_list.set_resource_root(_resource_root)

## The shell's resolve/pick/jump trio for the cards' font link rows. Safe to
## call before _ready (mount order); _ready re-applies the stored services.
func set_reference_services(services: Dictionary) -> void:
	_ref_services = services
	if _block_list != null and _block_list.has_method("set_reference_services"):
		_block_list.set_reference_services(services)

func flush_pending_edits() -> Error:
	if _mode == Mode.SOURCE and _source_view_mount != null and _source_view_mount.has_method("apply_pending"):
		return _source_view_mount.apply_pending()
	return OK

# Visual-mode keyboard editing. Runs before viewport focus navigation, but after
# _gui_input, so a focused text field still consumes its own keys; the focus-owner
# guard keeps Delete/Alt+arrows inert while the user is typing in a card field.
func _shortcut_input(event: InputEvent) -> void:
	if _mode != Mode.VISUAL or _block_list == null:
		return
	if not (event is InputEventKey):
		return
	var key_event := event as InputEventKey
	if not key_event.pressed or key_event.echo:
		return
	var focus_owner := get_viewport().gui_get_focus_owner()
	if focus_owner is LineEdit or focus_owner is TextEdit or focus_owner is SpinBox:
		return
	var handled = false
	match key_event.keycode:
		KEY_DELETE:
			handled = _block_list.delete_selected()
		KEY_UP:
			if key_event.alt_pressed:
				handled = _block_list.move_selected(-1)
		KEY_DOWN:
			if key_event.alt_pressed:
				handled = _block_list.move_selected(1)
	if handled:
		get_viewport().set_input_as_handled()

func _ready() -> void:
	var mode_group := ButtonGroup.new()
	mode_group.allow_unpress = false
	_visual_button.button_group = mode_group
	_source_button.button_group = mode_group
	_visual_button.toggled.connect(func(p): if p: _set_mode(Mode.VISUAL))
	_source_button.toggled.connect(func(p): if p: _set_mode(Mode.SOURCE))
	_scroll_rate_spin.value_changed.connect(_on_scroll_rate_changed)
	_vertical_space_spin.value_changed.connect(_on_vertical_space_changed)
	_center_x_spin.value_changed.connect(_on_center_x_changed)
	_play_button.pressed.connect(_on_play_pressed)
	_pause_button.pressed.connect(_on_pause_pressed)
	_stop_button.pressed.connect(_on_stop_pressed)
	_speed_spin.value_changed.connect(func(v): _player.speed_scale = v)
	_scrub_slider.value_changed.connect(_on_scrub_slider_changed)
	_set_mode(Mode.VISUAL)
	_add_text_button.pressed.connect(func(): _block_list.add_text())
	_add_image_button.pressed.connect(func(): _block_list.add_image())
	_add_newline_button.pressed.connect(func(): _block_list.add_newline())
	_block_scroll.get_v_scroll_bar().value_changed.connect(_on_block_scroll)
	_player.scroll_offset_changed.connect(_on_player_scroll_offset_changed)
	_player.gui_input.connect(_on_player_gui_input)
	_player.started.connect(_refresh_preview_toolbar_state)
	_player.finished.connect(_on_player_finished)
	_block_list.selection_changed.connect(_on_block_list_selection_changed)
	# Document may have arrived before _ready (mount order): re-apply now that
	# the nodes exist.
	if _block_list.has_method("set_document"):
		_block_list.set_document(_document)
	if _source_view_mount.has_method("set_document"):
		_source_view_mount.set_document(_document)
	# The env-bar spin bursts fold on their line edits' focus exit (mirrors the
	# card spins); arrow-only edits fold on the next flush.
	for env_spin: SpinBox in [_scroll_rate_spin, _vertical_space_spin, _center_x_spin]:
		env_spin.get_line_edit().focus_exited.connect(_flush_document_edit)
	if _block_list.has_method("set_resource_root"):
		_block_list.set_resource_root(_resource_root)
	if not _ref_services.is_empty() and _block_list.has_method("set_reference_services"):
		_block_list.set_reference_services(_ref_services)
	if _source_view_mount.has_signal("pending_edits_changed"):
		_source_view_mount.pending_edits_changed.connect(_on_source_pending_edits_changed)
	_player.mouse_filter = Control.MOUSE_FILTER_STOP
	_visual_button.tooltip_text = "Edit credits as cards"
	_source_button.tooltip_text = "Edit credits as text"
	_add_text_button.tooltip_text = "Insert a text entry after the selected card"
	_add_image_button.tooltip_text = "Insert an image entry after the selected card"
	_add_newline_button.tooltip_text = "Insert a blank line after the selected card"
	_play_button.tooltip_text = "Play credits preview"
	_pause_button.tooltip_text = "Pause credits preview"
	_stop_button.tooltip_text = "Stop and show the first entries"
	_speed_spin.tooltip_text = "Preview playback speed"
	_scrub_slider.tooltip_text = "Drag to move through the credits roll"
	_scroll_rate_spin.tooltip_text = "Pixels the credits scroll upward each frame"
	_vertical_space_spin.tooltip_text = "Extra blank pixels between entries"
	_center_x_spin.tooltip_text = "Horizontal center of the credits column, in game pixels"
	_refresh_preview_toolbar_state()
	_refresh_empty_hint()
	_refresh_add_buttons_enabled()

func _set_mode(value: Mode) -> void:
	if value == Mode.VISUAL and _mode == Mode.SOURCE:
		var err := _apply_pending_source_for_visual_mode()
		if err != OK:
			_apply_mode_state()
			return
	_mode = value
	if _mode == Mode.SOURCE and _source_view_mount != null and _source_view_mount.has_method("refresh_from_resource"):
		_source_view_mount.refresh_from_resource(true)
	_apply_mode_state()

func _apply_mode_state() -> void:
	_block_list_mount.visible = _mode == Mode.VISUAL
	_source_view_mount.visible = _mode == Mode.SOURCE
	_visual_button.set_pressed_no_signal(_mode == Mode.VISUAL)
	_source_button.set_pressed_no_signal(_mode == Mode.SOURCE)
	_refresh_empty_hint()

func _apply_pending_source_for_visual_mode() -> Error:
	if _source_view_mount == null or not _source_view_mount.has_method("apply_pending"):
		return OK
	if _source_view_mount.has_method("has_pending_edits") and not _source_view_mount.has_pending_edits():
		return OK
	return _source_view_mount.apply_pending()

func _on_resource_loaded(resource: CbinCreditsResource) -> void:
	_refresh_env_bar(resource)
	if _block_list and _block_list.has_method("set_resource"):
		_block_list.set_resource(resource)
	if _source_view_mount and _source_view_mount.has_method("set_resource"):
		_source_view_mount.set_resource(resource)
	_set_preview_resource(resource, true)
	_refresh_empty_hint()
	_refresh_add_buttons_enabled()

func _on_resource_changed() -> void:
	if _document == null:
		return
	_refresh_env_bar(_document.resource)
	if _block_list and _block_list.has_method("set_resource"):
		_block_list.set_resource(_document.resource)
	if _source_view_mount and _source_view_mount.has_method("set_resource"):
		_source_view_mount.set_resource(_document.resource)
	_set_preview_resource(_document.resource, false)
	_refresh_empty_hint()
	_refresh_add_buttons_enabled()

func _set_preview_resource(resource: CbinCreditsResource, reset_playback: bool) -> void:
	if _preview_resource != resource:
		if _preview_resource != null and _preview_resource.entries_structure_changed.is_connected(_refresh_empty_hint):
			_preview_resource.entries_structure_changed.disconnect(_refresh_empty_hint)
		if resource != null and not resource.entries_structure_changed.is_connected(_refresh_empty_hint):
			resource.entries_structure_changed.connect(_refresh_empty_hint)
	_preview_resource = resource
	_player.credits_resource = resource
	if reset_playback:
		_preview_paused = false
	_refresh_preview_toolbar_state()

func _refresh_add_buttons_enabled() -> void:
	var has_resource := _document != null and _document.resource != null
	_add_text_button.disabled = not has_resource
	_add_image_button.disabled = not has_resource
	_add_newline_button.disabled = not has_resource


func _refresh_empty_hint() -> void:
	if _empty_hint == null:
		return
	var resource: CbinCreditsResource = _document.resource if _document != null else null
	var is_empty := resource == null or resource.get_entry_count() == 0
	_empty_hint.visible = is_empty and _mode == Mode.VISUAL

func _refresh_env_bar(resource: CbinCreditsResource) -> void:
	if resource == null:
		return
	_suppress_env_signals = true
	_scroll_rate_spin.value = resource.get_scroll_rate()
	_vertical_space_spin.value = resource.get_vertical_space()
	_center_x_spin.value = resource.get_center_x()
	_suppress_env_signals = false

func _flush_document_edit() -> void:
	if _document != null:
		_document.flush_edit()

func _on_scroll_rate_changed(value: float) -> void:
	if _suppress_env_signals or _document == null or _document.resource == null:
		return
	_document.begin_edit()
	_document.resource.set_scroll_rate(value)

func _on_vertical_space_changed(value: float) -> void:
	if _suppress_env_signals or _document == null or _document.resource == null:
		return
	_document.begin_edit()
	_document.resource.set_vertical_space(int(value))

func _on_center_x_changed(value: float) -> void:
	if _suppress_env_signals or _document == null or _document.resource == null:
		return
	_document.begin_edit()
	_document.resource.set_center_x(int(value))

func _on_source_pending_edits_changed(has_pending_edits: bool) -> void:
	if not has_pending_edits or _document == null:
		return
	_document.mark_dirty()
	_document.state_changed.emit()

func _on_player_finished() -> void:
	_on_stop_pressed()

func _on_play_pressed() -> void:
	if _preview_resource == null:
		return
	if _preview_paused:
		_player.resume()
		_preview_paused = false
	else:
		_player.play()
	_refresh_preview_toolbar_state()

func _on_pause_pressed() -> void:
	if not _player.is_playing():
		return
	_player.pause()
	_preview_paused = true
	_refresh_preview_toolbar_state()

func _on_stop_pressed() -> void:
	_player.stop()
	_preview_paused = false
	_player.set_scroll_offset(_player.get_size().y)
	_refresh_preview_toolbar_state()

func _refresh_preview_toolbar_state() -> void:
	var has_resource := _preview_resource != null
	_play_button.disabled = not has_resource
	_pause_button.disabled = not _player.is_playing()
	_stop_button.disabled = not has_resource
	_play_button.text = "Resume" if _preview_paused else "Play"
	_scrub_slider.editable = has_resource
	_sync_scrub_thumb(_player.get_scroll_offset())


# The roll's extent changes on deferred player rebuilds (edits, image loads,
# resizes) with no completion signal to ride, so the transport polls the cheap
# extent each frame; the thumb itself only moves on scroll_offset_changed.
func _process(_delta: float) -> void:
	_sync_scrub_range()


func _sync_scrub_range() -> void:
	_scrub_slider.max_value = _max_preview_scroll_offset()


func _sync_scrub_thumb(offset: float) -> void:
	_sync_scrub_range()
	_scrub_slider.set_value_no_signal(clampf(offset, 0.0, float(_scrub_slider.max_value)))


func _on_scrub_slider_changed(value: float) -> void:
	_player.set_scroll_offset(value)

func _seek_player_to_entry(idx: int) -> void:
	var content_y := _player.content_y_for_entry(idx)
	_sync_suppress = true
	_player.set_scroll_offset(content_y + _player.get_size().y * 0.5)
	_sync_suppress = false

func _on_block_scroll(_value: float) -> void:
	if _sync_suppress:
		return
	if _document == null or _document.resource == null:
		return
	var centered := _center_visible_entry()
	if centered == null:
		return
	var idx := _index_of_entry(centered)
	if idx < 0:
		return
	_seek_player_to_entry(idx)

func _on_player_scroll_offset_changed(offset: float) -> void:
	# The scrub thumb mirrors the roll position unconditionally (playback,
	# wheel, seeks); only the list-selection sync below honors the suppress.
	_sync_scrub_thumb(offset)
	if _sync_suppress:
		return
	if _document == null or _document.resource == null:
		return
	var idx := _player.entry_index_at_scroll_center()
	if idx < 0:
		return
	var entry: CbinEntry = _document.resource.get_entry(idx)
	_sync_suppress = true
	_block_list.select_entry(entry)
	var card = _block_list.card_for_entry(entry)
	if card != null:
		_block_scroll.ensure_control_visible(card)
	_sync_suppress = false

func _on_block_list_selection_changed(entry) -> void:
	if _sync_suppress:
		return
	if entry == null or _document == null or _document.resource == null:
		return
	var idx := _index_of_entry(entry)
	if idx < 0:
		return
	_seek_player_to_entry(idx)


func _on_player_gui_input(event: InputEvent) -> void:
	if _document == null or _document.resource == null:
		return
	if not (event is InputEventMouseButton):
		return
	var mouse_event := event as InputEventMouseButton
	if not mouse_event.pressed:
		return
	if mouse_event.button_index == MOUSE_BUTTON_WHEEL_UP:
		_scrub_preview(-PREVIEW_WHEEL_PIXELS)
		get_viewport().set_input_as_handled()
	elif mouse_event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
		_scrub_preview(PREVIEW_WHEEL_PIXELS)
		get_viewport().set_input_as_handled()

func _scrub_preview(delta: float) -> void:
	var max_offset := _max_preview_scroll_offset()
	var next_offset: float = clampf(_player.get_scroll_offset() + delta, 0.0, max_offset)
	_player.set_scroll_offset(next_offset)

func _max_preview_scroll_offset() -> float:
	if _document == null or _document.resource == null or _document.resource.get_entry_count() == 0:
		return _player.get_size().y
	var last_idx: int = _document.resource.get_entry_count() - 1
	return maxf(_player.get_size().y, _player.content_y_for_entry(last_idx) + _player.get_size().y)

func _center_visible_entry() -> CbinEntry:
	if _block_list == null or _document == null or _document.resource == null:
		return null
	var scroll_top := _block_scroll.scroll_vertical
	var scroll_bottom := scroll_top + _block_scroll.size.y
	var scroll_center := scroll_top + _block_scroll.size.y * 0.5
	var best_entry: CbinEntry = null
	var best_distance := INF
	for i in range(_document.resource.get_entry_count()):
		var entry: CbinEntry = _document.resource.get_entry(i)
		var card = _block_list.card_for_entry(entry)
		if card == null:
			continue
		var card_top: float = card.position.y
		var card_bottom: float = card_top + card.size.y
		if card_bottom < scroll_top or card_top > scroll_bottom:
			continue
		var card_center: float = card_top + card.size.y * 0.5
		var distance: float = absf(card_center - scroll_center)
		if distance < best_distance:
			best_distance = distance
			best_entry = entry
	return best_entry

func _index_of_entry(entry) -> int:
	if _document == null or _document.resource == null or entry == null:
		return -1
	for i in range(_document.resource.get_entry_count()):
		if _document.resource.get_entry(i) == entry:
			return i
	return -1
