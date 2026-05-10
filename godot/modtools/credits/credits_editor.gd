class_name CreditsEditor
extends Control

const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")
const PREVIEW_WHEEL_PIXELS := 48.0

enum Mode { VISUAL, SOURCE }

@onready var _mode_buttons: HBoxContainer = %ModeButtons
@onready var _visual_button: Button = %VisualButton
@onready var _source_button: Button = %SourceButton
@onready var _scroll_rate_spin: SpinBox = %ScrollRateSpin
@onready var _vertical_space_spin: SpinBox = %VerticalSpaceSpin
@onready var _center_x_spin: SpinBox = %CenterXSpin
@onready var _content_stack: Control = %ContentStack
@onready var _block_list_host: Control = %BlockListHost
@onready var _source_view_host: Control = %SourceViewHost
@onready var _preview_host: Control = %PreviewHost
@onready var _warning_bar: Label = %WarningBar
@onready var _block_list: Control = %BlockList
@onready var _add_text_button: Button = %AddTextButton
@onready var _add_image_button: Button = %AddImageButton
@onready var _add_newline_button: Button = %AddNewlineButton
@onready var _block_scroll: ScrollContainer = %BlockScroll
@onready var _player: NovaCreditsPlayer = %Player

var _document: CreditsEditorDocument
var _mode: Mode = Mode.VISUAL
var _suppress_env_signals := false
var _sync_suppress := false

func set_document(value: CreditsEditorDocument) -> void:
	if _document == value:
		return
	if _document != null:
		_document.resource_loaded.disconnect(_on_resource_loaded)
		_document.resource_changed.disconnect(_on_resource_changed)
	_document = value
	if _document != null:
		_document.resource_loaded.connect(_on_resource_loaded)
		_document.resource_changed.connect(_on_resource_changed)
		_on_resource_loaded(_document.resource)

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
	_set_mode(Mode.VISUAL)
	_add_text_button.pressed.connect(func(): _block_list.add_text())
	_add_image_button.pressed.connect(func(): _block_list.add_image())
	_add_newline_button.pressed.connect(func(): _block_list.add_newline())
	_block_scroll.get_v_scroll_bar().value_changed.connect(_on_block_scroll)
	_player.scroll_offset_changed.connect(_on_player_scroll_offset_changed)
	_player.gui_input.connect(_on_player_gui_input)
	_block_list.selection_changed.connect(_on_block_list_selection_changed)
	_player.mouse_filter = Control.MOUSE_FILTER_STOP
	_visual_button.tooltip_text = "Edit credits as cards"
	_source_button.tooltip_text = "Edit credits as text"
	_add_text_button.tooltip_text = "Insert a text entry after the selected card"
	_add_image_button.tooltip_text = "Insert an image entry after the selected card"
	_add_newline_button.tooltip_text = "Insert a blank line after the selected card"

func _set_mode(value: Mode) -> void:
	_mode = value
	_block_list_host.visible = _mode == Mode.VISUAL
	_source_view_host.visible = _mode == Mode.SOURCE
	_visual_button.set_pressed_no_signal(_mode == Mode.VISUAL)
	_source_button.set_pressed_no_signal(_mode == Mode.SOURCE)

func _on_resource_loaded(resource: CbinCreditsResource) -> void:
	_refresh_env_bar(resource)
	_refresh_warning(resource)
	if _block_list and _block_list.has_method("set_resource"):
		_block_list.set_resource(resource)
	if _source_view_host and _source_view_host.has_method("set_resource"):
		_source_view_host.set_resource(resource)
	if _preview_host and _preview_host.has_method("set_resource"):
		_preview_host.set_resource(resource)

func _on_resource_changed() -> void:
	if _document == null:
		return
	_refresh_env_bar(_document.resource)
	_refresh_warning(_document.resource)
	if _block_list and _block_list.has_method("set_resource"):
		_block_list.set_resource(_document.resource)
	if _source_view_host and _source_view_host.has_method("set_resource"):
		_source_view_host.set_resource(_document.resource)
	if _preview_host and _preview_host.has_method("set_resource"):
		_preview_host.set_resource(_document.resource)

func _refresh_warning(resource: CbinCreditsResource) -> void:
	if _warning_bar == null:
		return
	if resource == null:
		_warning_bar.visible = false
		return
	var missing := 0
	for i in range(resource.get_entry_count()):
		var e := resource.get_entry(i)
		if e is CbinImageEntry:
			var img_entry := e as CbinImageEntry
			if img_entry.get_texture() == null and not img_entry.get_texture_path().is_empty():
				missing += 1
	if missing > 0:
		_warning_bar.text = "%d image(s) missing - place them at res://assets/textures/" % missing
		_warning_bar.visible = true
	else:
		_warning_bar.visible = false

func _refresh_env_bar(resource: CbinCreditsResource) -> void:
	if resource == null:
		return
	_suppress_env_signals = true
	_scroll_rate_spin.value = resource.get_scroll_rate()
	_vertical_space_spin.value = resource.get_vertical_space()
	_center_x_spin.value = resource.get_center_x()
	_suppress_env_signals = false

func _on_scroll_rate_changed(value: float) -> void:
	if _suppress_env_signals or _document == null or _document.resource == null:
		return
	_document.resource.set_scroll_rate(value)

func _on_vertical_space_changed(value: float) -> void:
	if _suppress_env_signals or _document == null or _document.resource == null:
		return
	_document.resource.set_vertical_space(int(value))

func _on_center_x_changed(value: float) -> void:
	if _suppress_env_signals or _document == null or _document.resource == null:
		return
	_document.resource.set_center_x(int(value))

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
	var content_y := _player.content_y_for_entry(idx)
	_sync_suppress = true
	_player.set_scroll_offset(content_y + _player.get_size().y * 0.5)
	_sync_suppress = false

func _on_player_scroll_offset_changed(_offset: float) -> void:
	if _sync_suppress:
		return
	if _document == null or _document.resource == null:
		return
	var idx := _player.entry_index_at_scroll_center()
	if idx < 0:
		return
	var entry := _document.resource.get_entry(idx)
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
	var content_y := _player.content_y_for_entry(idx)
	_sync_suppress = true
	_player.set_scroll_offset(content_y + _player.get_size().y * 0.5)
	_sync_suppress = false

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
	var last_idx := _document.resource.get_entry_count() - 1
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
		var entry := _document.resource.get_entry(i)
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
