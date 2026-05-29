class_name CreditsEditorBlockCard
extends PanelContainer

const TEXTURE_BASE_PATH := "res://assets/textures"
const TEXTURE_EXTENSIONS := ["png", "pcx", "tga", "jpg", "jpeg", "bmp"]

# Per-type accent palette. The selection highlight (A1) reuses the same hues as the
# type chips so a selected card reads as its type at a glance.
const CHIP_TEXT_COLOR := Color(0.84, 0.55, 0.29, 1.0)
const CHIP_IMAGE_COLOR := Color(0.45, 0.72, 0.88, 1.0)
const SELECT_ACCENT_TEXT := CHIP_TEXT_COLOR
const SELECT_ACCENT_IMAGE := CHIP_IMAGE_COLOR
const SELECT_ACCENT_SPACE := Color(0.45, 0.48, 0.52, 1.0)
const SPACER_FILL := Color(0.0863, 0.0941, 0.1098, 0.45)
const SPACER_BORDER := Color(0.1804, 0.2, 0.2314, 0.8)
const WARN_COLOR := Color(0.9176, 0.702, 0.0314, 1.0)  # matches theme "Warn"

signal request_delete(card)
signal request_drag(card)
signal request_select(card)
signal request_edit_font(font_name)

@onready var _drag_handle: Label = %DragHandle
@onready var _type_chip: Label = %TypeChip
@onready var _delete_button: Button = %DeleteButton

# Text controls
@onready var _text_panel: Control = %TextPanel
@onready var _text_edit: LineEdit = %TextEdit
@onready var _font_picker: OptionButton = %FontPicker
@onready var _font_edit_button: Button = %FontEditButton
@onready var _color_picker_text: ColorPickerButton = %TextColorPicker
@onready var _align_left: Button = %AlignLeft
@onready var _align_center: Button = %AlignCenter
@onready var _align_right: Button = %AlignRight

@onready var _spacer_panel: Control = %SpacerPanel

# Image controls
@onready var _image_panel: Control = %ImagePanel
@onready var _image_thumb: TextureRect = %ImageThumb
@onready var _image_path_edit: LineEdit = %ImagePathEdit
@onready var _image_pick_button: Button = %ImagePickButton
@onready var _image_mode_scroll: Button = %ImageModeScroll
@onready var _image_mode_fixed: Button = %ImageModeFixed
@onready var _image_offsets: Control = %ImageOffsets
@onready var _image_x_spin: SpinBox = %ImageXSpin
@onready var _image_y_spin: SpinBox = %ImageYSpin

var _entry: CbinEntry
var _suppress := false
var _selected := false
var _selected_stylebox: StyleBoxFlat
var _spacer_stylebox: StyleBoxFlat

func bind(entry: CbinEntry, font_options: PackedStringArray) -> void:
	if _entry and _entry.changed.is_connected(_refresh):
		_entry.changed.disconnect(_refresh)
	_entry = entry
	if _entry:
		_entry.changed.connect(_refresh)
	_populate_font_options(font_options)
	_refresh()

func get_entry() -> CbinEntry:
	return _entry

func _ready() -> void:
	_wire_button_groups()
	_configure_affordances()
	_delete_button.pressed.connect(func(): request_delete.emit(self))
	_drag_handle.gui_input.connect(_on_drag_handle_input)
	self.mouse_filter = Control.MOUSE_FILTER_PASS
	self.gui_input.connect(_on_panel_input)
	_text_edit.text_changed.connect(_on_text_changed)
	_font_picker.item_selected.connect(_on_font_selected)
	if not _font_edit_button.pressed.is_connected(_on_font_edit_pressed):
		_font_edit_button.pressed.connect(_on_font_edit_pressed)
	_color_picker_text.color_changed.connect(_on_text_color_changed)
	_align_left.pressed.connect(func(): _on_align(CbinEntry.CBIN_JUSTIFY_LEFT))
	_align_center.pressed.connect(func(): _on_align(CbinEntry.CBIN_JUSTIFY_CENTER))
	_align_right.pressed.connect(func(): _on_align(CbinEntry.CBIN_JUSTIFY_RIGHT))
	_image_path_edit.text_submitted.connect(_on_image_path_submitted)
	_image_path_edit.focus_exited.connect(_apply_image_path_edit)
	_image_pick_button.pressed.connect(_on_image_pick_pressed)
	_image_mode_scroll.pressed.connect(func(): _on_image_mode(true))
	_image_mode_fixed.pressed.connect(func(): _on_image_mode(false))
	_image_x_spin.value_changed.connect(_on_image_x_changed)
	_image_y_spin.value_changed.connect(_on_image_y_changed)
	_wire_child_selection(self)

func _wire_button_groups() -> void:
	var align_group := ButtonGroup.new()
	_align_left.button_group = align_group
	_align_center.button_group = align_group
	_align_right.button_group = align_group

	var image_mode_group := ButtonGroup.new()
	_image_mode_scroll.button_group = image_mode_group
	_image_mode_fixed.button_group = image_mode_group

func _configure_affordances() -> void:
	_drag_handle.tooltip_text = "Drag to reorder"
	_delete_button.tooltip_text = "Delete entry"
	_image_pick_button.tooltip_text = "Browse credits images"
	_image_mode_scroll.tooltip_text = "Image scrolls with the credits"
	_image_mode_fixed.tooltip_text = "Image stays fixed and fades in/out"
	_text_edit.tooltip_text = "Credits line text"
	_font_picker.tooltip_text = "Font for this line"
	_font_edit_button.tooltip_text = "Open this font in the Fonts workspace"
	_color_picker_text.tooltip_text = "Text color"
	_image_path_edit.tooltip_text = "Image filename under assets/textures"
	mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND

func _refresh() -> void:
	if _entry == null:
		return
	_suppress = true
	var is_text := _entry is CbinTextEntry
	var is_newline := _entry is CbinNewlineEntry
	var is_image := _entry is CbinImageEntry
	_text_panel.visible = is_text
	_spacer_panel.visible = is_newline
	_image_panel.visible = is_image
	custom_minimum_size.y = 28.0 if is_newline else 0.0
	_type_chip.theme_type_variation = &""

	if is_text:
		_type_chip.text = "TEXT"
		_type_chip.add_theme_color_override("font_color", CHIP_TEXT_COLOR)
		var entry_text: String = _entry.get_text()
		if not _text_edit.has_focus() and _text_edit.text != entry_text:
			_text_edit.text = entry_text
		var entry_color: Color = _entry.get_color()
		if _color_picker_text.color != entry_color:
			_color_picker_text.color = entry_color
		var entry_font: String = _entry.get_font_name()
		_font_edit_button.disabled = entry_font.is_empty()
		var current_font_idx := _font_picker.selected
		var current_font := _font_picker.get_item_text(current_font_idx) if current_font_idx > 0 else ""
		if current_font != entry_font:
			_select_font_in_picker(entry_font)
		_refresh_align_buttons(_entry.get_justify())
	elif is_newline:
		_type_chip.text = "SPACE"
		_type_chip.theme_type_variation = &"Muted"
		_type_chip.remove_theme_color_override("font_color")
	elif is_image:
		_type_chip.text = "IMAGE"
		_type_chip.add_theme_color_override("font_color", CHIP_IMAGE_COLOR)
		var entry_path: String = _entry.get_texture_path()
		if not _image_path_edit.has_focus() and _image_path_edit.text != entry_path:
			_image_path_edit.text = entry_path
		var dx: int = _entry.get_display_x()
		if int(_image_x_spin.value) != dx:
			_image_x_spin.value = dx
		var dy: int = _entry.get_display_y()
		if int(_image_y_spin.value) != dy:
			_image_y_spin.value = dy
		_image_offsets.visible = not _entry.get_advances_y()
		_refresh_image_mode_buttons(_entry.get_advances_y())
		var tex := _entry.get_texture() as Texture2D
		if _image_thumb.texture != tex:
			_image_thumb.texture = tex
		_update_image_missing_cue(entry_path, tex)
	_apply_panel_style()
	_suppress = false

func _populate_font_options(names: PackedStringArray) -> void:
	_font_picker.clear()
	_font_picker.add_item("(default)")
	for n in names:
		_font_picker.add_item(n)

func _select_font_in_picker(name: String) -> void:
	if name.is_empty():
		_font_picker.select(0)
		return
	for i in range(1, _font_picker.item_count):
		if _font_picker.get_item_text(i) == name:
			_font_picker.select(i)
			return
	_font_picker.add_item(name)
	_font_picker.select(_font_picker.item_count - 1)

func _refresh_align_buttons(justify: int) -> void:
	_align_left.button_pressed = justify == CbinEntry.CBIN_JUSTIFY_LEFT
	_align_center.button_pressed = justify == CbinEntry.CBIN_JUSTIFY_CENTER
	_align_right.button_pressed = justify == CbinEntry.CBIN_JUSTIFY_RIGHT

func _refresh_image_mode_buttons(advances_y: bool) -> void:
	_image_mode_scroll.button_pressed = advances_y
	_image_mode_fixed.button_pressed = not advances_y

func _on_text_changed(value: String) -> void:
	if _suppress or not (_entry is CbinTextEntry):
		return
	(_entry as CbinTextEntry).set_text(value)

func _on_font_selected(index: int) -> void:
	if _suppress or not (_entry is CbinTextEntry):
		return
	var name := "" if index == 0 else _font_picker.get_item_text(index)
	(_entry as CbinTextEntry).set_font_name(name)
	_font_edit_button.disabled = name.is_empty()

func _on_font_edit_pressed() -> void:
	if not (_entry is CbinTextEntry):
		return
	var font_name := _selected_font_name()
	if font_name.is_empty():
		return
	request_edit_font.emit(font_name)


func _selected_font_name() -> String:
	if _font_picker.selected > 0:
		return _font_picker.get_item_text(_font_picker.selected).strip_edges()
	return (_entry as CbinTextEntry).get_font_name().strip_edges()


func _on_text_color_changed(color: Color) -> void:
	if _suppress or not (_entry is CbinTextEntry):
		return
	(_entry as CbinTextEntry).set_color(color)

func _on_align(justify: int) -> void:
	if _suppress or not (_entry is CbinTextEntry):
		return
	(_entry as CbinTextEntry).set_justify(justify)
	_refresh_align_buttons(justify)

func _on_image_pick_pressed() -> void:
	var dialog := FileDialog.new()
	dialog.access = FileDialog.ACCESS_RESOURCES
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	dialog.current_dir = "res://assets/textures/"
	dialog.add_filter("*.pcx,*.png,*.jpg,*.jpeg,*.tga ; Image")
	dialog.file_selected.connect(func(path: String) -> void:
		_on_image_pick_selected(path)
		dialog.queue_free()
	)
	dialog.canceled.connect(dialog.queue_free)
	dialog.close_requested.connect(dialog.queue_free)
	add_child(dialog)
	dialog.popup_centered_ratio(0.6)

func _on_image_pick_selected(path: String) -> void:
	var name := path.get_file()
	_image_path_edit.text = name
	_apply_image_path(name)

func _on_image_path_submitted(value: String) -> void:
	_apply_image_path(value)

func _apply_image_path_edit() -> void:
	_apply_image_path(_image_path_edit.text)

func _apply_image_path(value: String) -> void:
	if _suppress or not (_entry is CbinImageEntry):
		return
	var image_entry := _entry as CbinImageEntry
	var image_name := _normalized_image_name(value)
	if _image_path_edit.text != image_name:
		_image_path_edit.text = image_name
	image_entry.set_texture_name(image_name)
	var tex := _resolve_texture(image_name)
	image_entry.set_texture(tex)
	_image_thumb.texture = tex
	_update_image_missing_cue(image_name, tex)

# Flags an image row whose filename does not resolve to a texture: tints the path
# field with the warning color and explains it in the thumbnail tooltip.
func _update_image_missing_cue(image_name: String, tex: Texture2D) -> void:
	var missing := tex == null and not image_name.strip_edges().is_empty()
	if missing:
		_image_path_edit.add_theme_color_override("font_color", WARN_COLOR)
		_image_thumb.tooltip_text = "Image not found: %s" % image_name
	else:
		_image_path_edit.remove_theme_color_override("font_color")
		_image_thumb.tooltip_text = ""

func _normalized_image_name(value: String) -> String:
	return value.strip_edges().get_file()

func _resolve_texture(image_name: String) -> Texture2D:
	if image_name.is_empty():
		return null

	var exact_path := TEXTURE_BASE_PATH.path_join(image_name)
	if ResourceLoader.exists(exact_path):
		return ResourceLoader.load(exact_path) as Texture2D

	var dir := DirAccess.open(TEXTURE_BASE_PATH)
	if dir == null:
		return null

	var lower_name := image_name.to_lower()
	var matched := _scan_texture_dir(dir, lower_name)
	if not matched.is_empty():
		var matched_path := TEXTURE_BASE_PATH.path_join(matched)
		if ResourceLoader.exists(matched_path):
			return ResourceLoader.load(matched_path) as Texture2D

	var basename := image_name.get_basename()
	for ext in TEXTURE_EXTENSIONS:
		var alt := ("%s.%s" % [basename, ext]).to_lower()
		if alt == lower_name:
			continue
		matched = _scan_texture_dir(dir, alt)
		if not matched.is_empty():
			var alt_path := TEXTURE_BASE_PATH.path_join(matched)
			if ResourceLoader.exists(alt_path):
				return ResourceLoader.load(alt_path) as Texture2D
	return null

func _scan_texture_dir(dir: DirAccess, lower_name: String) -> String:
	dir.list_dir_begin()
	var filename := dir.get_next()
	while not filename.is_empty():
		if not dir.current_is_dir() and filename.to_lower() == lower_name:
			dir.list_dir_end()
			return filename
		filename = dir.get_next()
	dir.list_dir_end()
	return ""

func _on_image_mode(advances_y: bool) -> void:
	if _suppress or not (_entry is CbinImageEntry):
		return
	(_entry as CbinImageEntry).set_advances_y(advances_y)
	_image_offsets.visible = not advances_y
	_refresh_image_mode_buttons(advances_y)

func _on_image_x_changed(value: float) -> void:
	if _suppress or not (_entry is CbinImageEntry):
		return
	(_entry as CbinImageEntry).set_display_x(int(value))

func _on_image_y_changed(value: float) -> void:
	if _suppress or not (_entry is CbinImageEntry):
		return
	(_entry as CbinImageEntry).set_display_y(int(value))

func _selection_accent() -> Color:
	if _entry is CbinImageEntry:
		return SELECT_ACCENT_IMAGE
	elif _entry is CbinNewlineEntry:
		return SELECT_ACCENT_SPACE
	return SELECT_ACCENT_TEXT

func _ensure_selected_stylebox() -> StyleBoxFlat:
	if _selected_stylebox == null:
		var accent := _selection_accent()
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(accent.r, accent.g, accent.b, 0.08)
		sb.border_color = accent
		sb.border_width_left = 3
		sb.border_width_top = 1
		sb.border_width_right = 1
		sb.border_width_bottom = 1
		sb.corner_radius_top_left = 4
		sb.corner_radius_top_right = 4
		sb.corner_radius_bottom_right = 4
		sb.corner_radius_bottom_left = 4
		sb.content_margin_left = 12
		sb.content_margin_right = 12
		sb.content_margin_top = 12
		sb.content_margin_bottom = 12
		_selected_stylebox = sb
	return _selected_stylebox

func _ensure_spacer_stylebox() -> StyleBoxFlat:
	if _spacer_stylebox == null:
		var sb := StyleBoxFlat.new()
		sb.bg_color = SPACER_FILL
		sb.border_color = SPACER_BORDER
		sb.border_width_bottom = 1
		sb.corner_radius_top_left = 3
		sb.corner_radius_top_right = 3
		sb.corner_radius_bottom_right = 3
		sb.corner_radius_bottom_left = 3
		sb.content_margin_left = 12
		sb.content_margin_right = 12
		sb.content_margin_top = 2
		sb.content_margin_bottom = 2
		_spacer_stylebox = sb
	return _spacer_stylebox

func set_selected(value: bool) -> void:
	_selected = value
	_apply_panel_style()

func _apply_panel_style() -> void:
	if _selected:
		add_theme_stylebox_override("panel", _ensure_selected_stylebox())
	elif _entry is CbinNewlineEntry:
		add_theme_stylebox_override("panel", _ensure_spacer_stylebox())
	else:
		remove_theme_stylebox_override("panel")

func _on_panel_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		request_select.emit(self)

func _on_drag_handle_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		request_select.emit(self)
		var preview := Label.new()
		preview.text = _type_chip.text
		force_drag({"card": self}, preview)
		request_drag.emit(self)

func _wire_child_selection(node: Node) -> void:
	for child in node.get_children():
		if child is Control and child != self and child != _drag_handle:
			var control := child as Control
			if not control.focus_entered.is_connected(_on_child_focus_entered):
				control.focus_entered.connect(_on_child_focus_entered)
			if not control.gui_input.is_connected(_on_child_control_input):
				control.gui_input.connect(_on_child_control_input)
		_wire_child_selection(child)

func _on_child_focus_entered() -> void:
	request_select.emit(self)

func _on_child_control_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		request_select.emit(self)

func _get_drag_data(_pos: Vector2) -> Variant:
	var preview := Label.new()
	preview.text = _type_chip.text
	set_drag_preview(preview)
	return {"card": self}

func _exit_tree() -> void:
	if _entry and _entry.changed.is_connected(_refresh):
		_entry.changed.disconnect(_refresh)
