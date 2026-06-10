class_name VegPicker
extends Window

## Popup that presents a grid of *veg*.3di previews. Emits `graphic_selected`
## with the chosen basename (no extension) when the user picks one.

signal graphic_selected(basename: String)

const VegAssets := preload("res://engine/terrain/veg_assets.gd")

const _CELL_SIZE := Vector2(128, 156)
const _PREVIEW_SIZE := Vector2(112, 112)

@onready var _grid: GridContainer = %Grid
@onready var _empty_label: Label = %EmptyLabel
@onready var _close_button: Button = %CloseButton

var _populated: bool = false
var _resource_root: NovaResourceRoot


func set_resource_root(value: NovaResourceRoot) -> void:
	if _resource_root == value:
		return
	_resource_root = value
	_populated = false


func _ready() -> void:
	close_requested.connect(_on_close_requested)
	if _close_button != null:
		_close_button.pressed.connect(_on_close_requested)


func open_picker() -> void:
	if not is_node_ready():
		call_deferred("open_picker")
		return
	if not _populated:
		_populate()
		_populated = true
	_resize_to_fit_parent()
	popup_centered()


func _resize_to_fit_parent() -> void:
	var root := get_tree().root
	if root == null:
		return
	var parent_size: Vector2i = root.size
	size = Vector2i(
		clampi(int(parent_size.x * 0.7), 590, 640),
		clampi(int(parent_size.y * 0.8), 400, 560),
	)


func refresh() -> void:
	if not is_node_ready():
		call_deferred("refresh")
		return
	_populate()
	_populated = true


func _populate() -> void:
	if _grid == null or _empty_label == null:
		return
	for child in _grid.get_children():
		child.queue_free()

	var graphics := VegAssets.list_graphics(_resource_root, true)
	_empty_label.visible = graphics.is_empty()
	_grid.visible = not graphics.is_empty()

	for entry in graphics:
		_grid.add_child(_make_cell(String(entry.basename)))


func _make_cell(basename: String) -> Control:
	var button := Button.new()
	button.flat = false
	button.clip_contents = true
	button.custom_minimum_size = _CELL_SIZE
	button.tooltip_text = basename
	button.focus_mode = Control.FOCUS_NONE
	button.pressed.connect(func(): _on_cell_pressed(basename))

	var vbox := VBoxContainer.new()
	vbox.anchor_right = 1.0
	vbox.anchor_bottom = 1.0
	vbox.offset_left = 6
	vbox.offset_top = 6
	vbox.offset_right = -6
	vbox.offset_bottom = -6
	vbox.mouse_filter = Control.MOUSE_FILTER_IGNORE
	vbox.add_theme_constant_override("separation", 4)
	button.add_child(vbox)

	var preview := VegPreview.new()
	preview.custom_minimum_size = _PREVIEW_SIZE
	preview.mouse_filter = Control.MOUSE_FILTER_IGNORE
	preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	preview.size_flags_vertical = Control.SIZE_EXPAND_FILL
	vbox.add_child(preview)
	preview.set_resource_root(_resource_root)
	preview.set_graphic(basename)

	var label := Label.new()
	label.text = basename
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.autowrap_mode = TextServer.AUTOWRAP_WORD
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	vbox.add_child(label)

	return button


func _on_cell_pressed(basename: String) -> void:
	graphic_selected.emit(basename)
	hide()


func _on_close_requested() -> void:
	hide()
