class_name FntGenerateDialog
extends AcceptDialog

# Modal for "Generate from font": pick a TTF/OTF file or an installed system font,
# a pixel size, and Bold/Italic/Outline/Shadow flags. Emits the built Font + params;
# the actual rasterization happens in FntRasterizer (driven by the document).

signal generate_requested(font, px_size, flags)

var _use_file: CheckBox
var _system_option: OptionButton
var _file_edit: LineEdit
var _size_spin: SpinBox
var _bold: CheckBox
var _italic: CheckBox
var _outline: CheckBox
var _shadow: CheckBox
var _status: Label
var _files: FileDialogHelper


func _init() -> void:
	title = "Generate font from TTF / system font"
	ok_button_text = "Generate"
	min_size = Vector2i(480, 0)

	var margin := MarginContainer.new()
	for side in ["margin_left", "margin_right", "margin_top", "margin_bottom"]:
		margin.add_theme_constant_override(side, 10)
	add_child(margin)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	margin.add_child(box)

	var sys_lbl := Label.new()
	sys_lbl.text = "System font"
	sys_lbl.theme_type_variation = &"Muted"
	box.add_child(sys_lbl)
	_system_option = OptionButton.new()
	_system_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for fname in OS.get_system_fonts():
		_system_option.add_item(fname)
	box.add_child(_system_option)

	_use_file = CheckBox.new()
	_use_file.text = "Use a .ttf / .otf file instead"
	box.add_child(_use_file)
	var file_row := HBoxContainer.new()
	file_row.add_theme_constant_override("separation", 6)
	_file_edit = LineEdit.new()
	_file_edit.placeholder_text = "path/to/font.ttf"
	_file_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	file_row.add_child(_file_edit)
	var browse := Button.new()
	browse.text = "Browse…"
	browse.pressed.connect(_on_browse)
	file_row.add_child(browse)
	box.add_child(file_row)

	var size_row := HBoxContainer.new()
	size_row.add_theme_constant_override("separation", 6)
	var size_lbl := Label.new()
	size_lbl.text = "Size (px)"
	size_lbl.theme_type_variation = &"Muted"
	size_lbl.custom_minimum_size = Vector2(70, 0)
	size_row.add_child(size_lbl)
	_size_spin = SpinBox.new()
	_size_spin.min_value = 6
	_size_spin.max_value = 128
	_size_spin.step = 1
	_size_spin.value = 24
	_size_spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_row.add_child(_size_spin)
	box.add_child(size_row)

	var flag_row := HBoxContainer.new()
	flag_row.add_theme_constant_override("separation", 10)
	_bold = CheckBox.new()
	_bold.text = "Bold"
	_italic = CheckBox.new()
	_italic.text = "Italic"
	_outline = CheckBox.new()
	_outline.text = "Outline"
	_shadow = CheckBox.new()
	_shadow.text = "Shadow"
	for cb in [_bold, _italic, _outline, _shadow]:
		flag_row.add_child(cb)
	box.add_child(flag_row)

	_status = Label.new()
	_status.theme_type_variation = &"Muted"
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_status)

	confirmed.connect(_on_confirmed)


func _on_browse() -> void:
	if _files == null:
		_files = FileDialogHelper.new(self)
	_files.open("Choose a font file", PackedStringArray(["*.ttf,*.otf,*.ttc ; Fonts"]), _on_file_selected)


func _on_file_selected(path: String) -> void:
	_file_edit.text = path
	_use_file.button_pressed = true


func _on_confirmed() -> void:
	var font := _build_font()
	if font == null:
		_status.text = "Could not load the selected font."
		# Re-open so the user can correct the selection.
		call_deferred("popup_centered")
		return
	var flags := 0
	if _bold.button_pressed:
		flags |= 1
	if _italic.button_pressed:
		flags |= 2
	if _outline.button_pressed:
		flags |= 4
	if _shadow.button_pressed:
		flags |= 8
	generate_requested.emit(font, int(_size_spin.value), flags)


func _build_font() -> Font:
	if _use_file.button_pressed:
		var path := _file_edit.text.strip_edges()
		if path.is_empty() or not FileAccess.file_exists(path):
			return null
		var ff := FontFile.new()
		if ff.load_dynamic_font(path) != OK:
			return null
		return ff
	if _system_option.item_count == 0 or _system_option.selected < 0:
		return null
	var sf := SystemFont.new()
	sf.font_names = PackedStringArray([_system_option.get_item_text(_system_option.selected)])
	return sf
