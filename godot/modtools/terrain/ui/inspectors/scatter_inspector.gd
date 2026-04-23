class_name ScatterInspector
extends MarginContainer

const FOLIAGE_DEFS_LIMIT := 4
const _VEG_PICKER_SCENE := preload("res://modtools/terrain/ui/widgets/veg_picker.tscn")

@onready var _list: ItemList = %FoliageList
@onready var _count_label: Label = %CountLabel
@onready var _add_button: Button = %AddButton
@onready var _remove_button: Button = %RemoveButton
@onready var _detail_box: VBoxContainer = %DetailBox
@onready var _detail_empty: Label = %DetailEmpty
@onready var _graphic_button: Button = %GraphicButton
@onready var _graphic_name_label: Label = %GraphicNameLabel
@onready var _color_lower: OptionButton = %ColorLowerOption
@onready var _color_upper: OptionButton = %ColorUpperOption
@onready var _shadow_toggle: CheckBox = %ShadowToggle
@onready var _force_on_toggle: CheckBox = %ForceOnToggle
@onready var _brush: BrushControls = %BrushControls

var editor: TerrainEditor
var _selected_index: int = -1
var _syncing: bool = false
var _graphic_preview: VegPreview
var _picker: VegPicker


func _ready() -> void:
	_populate_color_options(_color_lower)
	_populate_color_options(_color_upper)
	_build_graphic_preview()

	_list.item_selected.connect(_on_list_selected)
	_add_button.pressed.connect(_on_add_pressed)
	_remove_button.pressed.connect(_on_remove_pressed)
	_graphic_button.pressed.connect(_on_graphic_button_pressed)
	_color_lower.item_selected.connect(_on_color_lower_selected)
	_color_upper.item_selected.connect(_on_color_upper_selected)
	_shadow_toggle.toggled.connect(_on_shadow_toggled)
	_force_on_toggle.toggled.connect(_on_force_on_toggled)

	_brush.radius_changed.connect(_on_brush_radius)
	_brush.strength_changed.connect(_on_brush_strength)
	_brush.hardness_changed.connect(_on_brush_hardness)


func _build_graphic_preview() -> void:
	_graphic_preview = VegPreview.new()
	_graphic_preview.anchor_right = 1.0
	_graphic_preview.anchor_bottom = 1.0
	_graphic_preview.offset_left = 4
	_graphic_preview.offset_top = 4
	_graphic_preview.offset_right = -4
	_graphic_preview.offset_bottom = -4
	_graphic_preview.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_graphic_button.add_child(_graphic_preview)


func set_editor(value: TerrainEditor) -> void:
	var callback := Callable(self, "_on_editor_ui_state_changed")
	if editor != null and editor.ui_state_changed.is_connected(callback):
		editor.ui_state_changed.disconnect(callback)
	editor = value
	if editor != null and not editor.ui_state_changed.is_connected(callback):
		editor.ui_state_changed.connect(callback)
	if editor and editor.current_tool != TerrainEditor.Tool.FOLIAGE_PAINT:
		editor.set_tool(TerrainEditor.Tool.FOLIAGE_PAINT)
	_sync_from_editor()


func _on_editor_ui_state_changed(_version: int) -> void:
	_sync_from_editor()


func _sync_from_editor() -> void:
	if editor == null:
		return
	_syncing = true
	_brush.set_values(editor.brush_radius, editor.brush_strength, editor.brush_hardness)
	var defs := editor.get_foliage_defs()
	_selected_index = clampi(editor.get_selected_foliage_def_index(), -1, defs.size() - 1)
	_refresh_list(defs)
	_refresh_detail(defs)
	_syncing = false


func _refresh_list(defs: Array) -> void:
	_count_label.text = "%d / %d" % [defs.size(), FOLIAGE_DEFS_LIMIT]
	_add_button.disabled = defs.size() >= FOLIAGE_DEFS_LIMIT

	var want_selected := clampi(_selected_index, -1, defs.size() - 1)
	_list.clear()
	for i in defs.size():
		var def = defs[i]
		var graphic := String(def.graphic) if def and def.graphic else "(unnamed)"
		_list.add_item("%d - %s" % [i + 1, graphic])
	if want_selected >= 0 and want_selected < defs.size():
		_list.select(want_selected)
		_selected_index = want_selected
	else:
		_selected_index = -1
	_remove_button.disabled = _selected_index < 0


func _refresh_detail(defs: Array) -> void:
	var has_selected := _selected_index >= 0 and _selected_index < defs.size()
	_detail_empty.visible = not has_selected
	_detail_box.visible = has_selected
	if not has_selected:
		return
	var def = defs[_selected_index]
	var graphic := String(def.graphic) if def and def.graphic else ""
	_graphic_preview.set_graphic(graphic)
	_graphic_name_label.text = graphic if not graphic.is_empty() else "(none)"
	_match_color_option(_color_lower, int(def.color_lower))
	_match_color_option(_color_upper, int(def.color_upper))
	_shadow_toggle.set_pressed_no_signal(bool(def.shadow))
	_force_on_toggle.set_pressed_no_signal(bool(def.force_on))


func _populate_color_options(option: OptionButton) -> void:
	option.clear()
	option.add_item("Match ground")
	option.set_item_metadata(0, NovaTerrainFoliageDef.COLOR_MATCH_GROUND)
	option.add_item("Blend 50%")
	option.set_item_metadata(1, NovaTerrainFoliageDef.COLOR_BLEND_50)
	option.add_item("Retain full color")
	option.set_item_metadata(2, NovaTerrainFoliageDef.COLOR_RETAIN_FULL)


func _match_color_option(option: OptionButton, value: int) -> void:
	for i in option.item_count:
		if int(option.get_item_metadata(i)) == value:
			option.select(i)
			return
	option.select(0)


func _on_list_selected(idx: int) -> void:
	if _syncing or editor == null:
		return
	editor.set_selected_foliage_def_index(idx)
	_sync_from_editor()


func _on_add_pressed() -> void:
	if editor == null:
		return
	editor.add_foliage_def()
	_sync_from_editor()


func _on_remove_pressed() -> void:
	if editor == null or _selected_index < 0:
		return
	editor.remove_foliage_def(_selected_index)
	_sync_from_editor()


func _on_graphic_button_pressed() -> void:
	if editor == null or _selected_index < 0:
		return
	_ensure_picker()
	_picker.refresh()
	_picker.open_picker()


func _ensure_picker() -> void:
	if _picker != null and is_instance_valid(_picker):
		return
	_picker = _VEG_PICKER_SCENE.instantiate()
	add_child(_picker)
	_picker.graphic_selected.connect(_on_picker_graphic_selected)


func _on_picker_graphic_selected(basename: String) -> void:
	_commit_graphic(basename)


func _commit_graphic(text: String) -> void:
	if editor == null or _selected_index < 0 or _syncing:
		return
	editor.set_foliage_def_field(_selected_index, "graphic", text)


func _on_color_lower_selected(idx: int) -> void:
	if editor == null or _selected_index < 0 or _syncing:
		return
	editor.set_foliage_def_field(_selected_index, "color_lower", int(_color_lower.get_item_metadata(idx)))


func _on_color_upper_selected(idx: int) -> void:
	if editor == null or _selected_index < 0 or _syncing:
		return
	editor.set_foliage_def_field(_selected_index, "color_upper", int(_color_upper.get_item_metadata(idx)))


func _on_shadow_toggled(pressed: bool) -> void:
	if editor == null or _selected_index < 0 or _syncing:
		return
	editor.set_foliage_def_field(_selected_index, "shadow", pressed)


func _on_force_on_toggled(pressed: bool) -> void:
	if editor == null or _selected_index < 0 or _syncing:
		return
	editor.set_foliage_def_field(_selected_index, "force_on", pressed)


func _on_brush_radius(v: float) -> void:
	if editor == null or _syncing:
		return
	editor.set_brush_radius_value(v)


func _on_brush_strength(v: float) -> void:
	if editor == null or _syncing:
		return
	editor.set_brush_strength_value(v)


func _on_brush_hardness(v: float) -> void:
	if editor == null or _syncing:
		return
	editor.set_brush_hardness_value(v)
