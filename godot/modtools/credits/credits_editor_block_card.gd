class_name CreditsEditorBlockCard
extends PanelContainer

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

# Per-type accent palette. The selection highlight (A1) reuses the same hues as the
# type chips so a selected card reads as its type at a glance. TEXT rides the
# editor accent (single-sourced in editor_theme.tres, EditorPalette/accent).
const CHIP_IMAGE_COLOR := Color(0.45, 0.72, 0.88, 1.0)
const SELECT_ACCENT_IMAGE := CHIP_IMAGE_COLOR
const SELECT_ACCENT_SPACE := Color(0.45, 0.48, 0.52, 1.0)
const SPACER_FILL := Color(0.0863, 0.0941, 0.1098, 0.45)
const SPACER_BORDER := Color(0.1804, 0.2, 0.2314, 0.8)
const WARN_COLOR := Color(0.9176, 0.702, 0.0314, 1.0)  # matches theme "Warn"

signal request_delete(card)
signal request_drag(card)
signal request_select(card)

@onready var _drag_handle: Label = %DragHandle
@onready var _type_chip: Label = %TypeChip
@onready var _delete_button: Button = %DeleteButton

# Text controls
@onready var _text_panel: Control = %TextPanel
@onready var _text_edit: LineEdit = %TextEdit
@onready var _font_ref_mount: Control = %FontRefMount
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
# The owning document, for undo bracketing (B2): typing/color/spin bursts use
# begin/flush, structural singles use push_undo_step. Optional — a card bound
# without a document edits directly.
var _document: CreditsEditorDocument
var _suppress := false
var _selected := false
var _selected_stylebox: StyleBoxFlat
var _spacer_stylebox: StyleBoxFlat
var _resource_root: NovaResourceRoot
var _font_ref: ResourceRefWidget
var _ref_services: Dictionary = {}

func bind(entry: CbinEntry, resource_root: Variant = null, services: Dictionary = {}) -> void:
	if _entry and _entry.changed.is_connected(_refresh):
		_entry.changed.disconnect(_refresh)
	_entry = entry
	_resource_root = _coerce_resource_root(resource_root)
	if _entry:
		_entry.changed.connect(_refresh)
	set_reference_services(services)
	_refresh()


## The shell's resolve/pick/jump trio for the font link row; arrives through
## the workspace -> editor -> block list chain (re-binds are idempotent).
func set_reference_services(services: Dictionary) -> void:
	_ref_services = services
	_configure_font_ref()

func get_entry() -> CbinEntry:
	return _entry


func set_document(value: CreditsEditorDocument) -> void:
	_document = value


# Typing/drag bursts: open once (idempotent while a session is open), fold on
# the control's focus/popup exit.
func _begin_burst() -> void:
	if _document != null:
		_document.begin_edit()


func _commit_burst() -> void:
	if _document != null:
		_document.flush_edit()


# One equal-gated undo step around a single structural field change.
func _push_single(mutation: Callable) -> void:
	if _document != null:
		_document.push_undo_step(mutation)
	else:
		mutation.call()

func _ready() -> void:
	_wire_button_groups()
	_configure_affordances()
	_delete_button.pressed.connect(func(): request_delete.emit(self))
	_drag_handle.gui_input.connect(_on_drag_handle_input)
	self.mouse_filter = Control.MOUSE_FILTER_PASS
	self.gui_input.connect(_on_panel_input)
	_text_edit.text_changed.connect(_on_text_changed)
	_text_edit.focus_exited.connect(_commit_burst)
	_font_ref = ResourceRefWidget.new()
	_font_ref.name = "FontRef"
	_font_ref.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_font_ref.value_changed.connect(_on_font_ref_changed)
	_font_ref_mount.add_child(_font_ref)
	_configure_font_ref()
	_color_picker_text.color_changed.connect(_on_text_color_changed)
	_color_picker_text.popup_closed.connect(_commit_burst)
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
	# Spin bursts fold on the embedded line edit's focus exit (mirrors
	# environment_inspector.gd); arrow-only edits fold on the next flush.
	_image_x_spin.get_line_edit().focus_exited.connect(_commit_burst)
	_image_y_spin.get_line_edit().focus_exited.connect(_commit_burst)
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
	_color_picker_text.tooltip_text = "Text color"
	_image_path_edit.tooltip_text = "Image filename in the resource directory"
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
		_type_chip.add_theme_color_override("font_color", _accent())
		var entry_text: String = _entry.get_text()
		if not _text_edit.has_focus() and _text_edit.text != entry_text:
			_text_edit.text = entry_text
		var entry_color: Color = _entry.get_color()
		if _color_picker_text.color != entry_color:
			_color_picker_text.color = entry_color
		var entry_font: String = _entry.get_font_name()
		if _font_ref != null and _font_ref.get_value() != entry_font:
			_font_ref.set_value(entry_font)  # silent (bind_link contract)
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

func _configure_font_ref() -> void:
	if _font_ref == null:
		return
	_font_ref.configure("font", "Font", _ref_services)
	# An empty value is legal: the game renders with its built-in default font.
	_font_ref.name_edit.placeholder_text = "(default font)"
	_font_ref.name_edit.tooltip_text = "Font for this line; leave empty for the game's default."

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
	_begin_burst()
	(_entry as CbinTextEntry).set_text(value)

func _on_font_ref_changed(value: String) -> void:
	if _suppress or not (_entry is CbinTextEntry):
		return
	var name := value.strip_edges()
	var text_entry := _entry as CbinTextEntry
	var font: Resource = _resolve_font(name) if not name.is_empty() else null
	_push_single(func() -> void:
		text_entry.set_font_name(name)
		if font != null:
			text_entry.set_font(font))


func _on_text_color_changed(color: Color) -> void:
	if _suppress or not (_entry is CbinTextEntry):
		return
	_begin_burst()
	(_entry as CbinTextEntry).set_color(color)

func _on_align(justify: int) -> void:
	if _suppress or not (_entry is CbinTextEntry):
		return
	var text_entry := _entry as CbinTextEntry
	_push_single(func() -> void: text_entry.set_justify(justify))
	_refresh_align_buttons(justify)

var _files: FileDialogHelper


func _on_image_pick_pressed() -> void:
	if _files == null:
		_files = FileDialogHelper.new(self)
	var dir: String = _resource_root.get_root_dir() if _resource_root != null else ""
	_files.open("Choose an image", PackedStringArray(["*.pcx,*.png,*.jpg,*.jpeg,*.tga ; Image"]), _on_image_pick_selected, dir)

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
	var tex := _resolve_texture(image_name)
	_push_single(func() -> void:
		image_entry.set_texture_name(image_name)
		image_entry.set_texture(tex))
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
	if image_name.is_empty() or _resource_root == null:
		return null
	return _resource_root.load_texture(image_name)

func _resolve_font(font_name: String) -> Resource:
	if font_name.is_empty() or _resource_root == null:
		return null
	return _resource_root.load_font(font_name)

func _coerce_resource_root(value: Variant) -> NovaResourceRoot:
	if value is NovaResourceRoot:
		return value
	var dir := String(value).strip_edges() if value != null else ""
	if dir.is_empty():
		dir = ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		return null
	var resources := NovaResourceRoot.new()
	return resources if resources.set_root_dir(dir) == OK else null

func _on_image_mode(advances_y: bool) -> void:
	if _suppress or not (_entry is CbinImageEntry):
		return
	var image_entry := _entry as CbinImageEntry
	_push_single(func() -> void: image_entry.set_advances_y(advances_y))
	_image_offsets.visible = not advances_y
	_refresh_image_mode_buttons(advances_y)

func _on_image_x_changed(value: float) -> void:
	if _suppress or not (_entry is CbinImageEntry):
		return
	_begin_burst()
	(_entry as CbinImageEntry).set_display_x(int(value))

func _on_image_y_changed(value: float) -> void:
	if _suppress or not (_entry is CbinImageEntry):
		return
	_begin_burst()
	(_entry as CbinImageEntry).set_display_y(int(value))

# The editor accent, read from the theme at use time (the card sits under the
# shell's themed tree; standalone owners fall back to the engine default).
func _accent() -> Color:
	return get_theme_color(&"accent", &"EditorPalette")

func _selection_accent() -> Color:
	if _entry is CbinImageEntry:
		return SELECT_ACCENT_IMAGE
	elif _entry is CbinNewlineEntry:
		return SELECT_ACCENT_SPACE
	return _accent()

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
