class_name MnsPreview
extends VBoxContainer

# Live menu preview for the Menu Styles workspace: pick any menu from the
# resource folder and see it rendered through the stylesheet being edited.
# Read-only by construction (a NovaMnuMenu in edit_mode with input ignored,
# letterbox-fitted like the Menus canvas) - no gesture machinery. Stylesheet
# changes re-render on a short debounce so typing in a color field repaints
# without rebuilding per keystroke.

const MENU_BOARD_SIZE := Vector2(640, 480)
const REFRESH_DEBOUNCE_SEC := 0.25

var _root: NovaResourceRoot
var _list_menus: Callable = Callable()
var _sheet: MnsStyleSheet
var _doc: NovaMnuDocument
var _text_resource: RtxtStringFile

var _menu_pick: OptionButton
var _screen_pick: OptionButton
var _body: Control
var _menu_node: NovaMnuMenu
var _hint: Label
var _timer: Timer
var _menu_names := PackedStringArray()


func _init() -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_theme_constant_override("separation", 4)

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 6)
	add_child(header)
	var title := Label.new()
	title.theme_type_variation = &"Muted"
	title.text = "Preview"
	header.add_child(title)
	_menu_pick = OptionButton.new()
	_menu_pick.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_menu_pick.fit_to_longest_item = false
	_menu_pick.item_selected.connect(func(index: int) -> void:
		_load_menu_at(index))
	header.add_child(_menu_pick)
	_screen_pick = OptionButton.new()
	_screen_pick.fit_to_longest_item = false
	_screen_pick.item_selected.connect(func(_index: int) -> void:
		_apply_screen_visibility())
	header.add_child(_screen_pick)

	_body = Control.new()
	_body.clip_contents = true
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_body.resized.connect(_fit)
	add_child(_body)

	_hint = Label.new()
	_hint.theme_type_variation = &"Muted"
	_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_hint.text = "Pick a menu to preview it with this stylesheet."
	_body.add_child(_hint)

	_timer = Timer.new()
	_timer.one_shot = true
	_timer.wait_time = REFRESH_DEBOUNCE_SEC
	_timer.timeout.connect(_rebuild)
	add_child(_timer)


# The workspace injects the resource root and a menu-name lister (empty in
# headless tests; the preview degrades to its hint).
func configure(root: NovaResourceRoot, list_menus: Callable) -> void:
	_root = root
	_list_menus = list_menus
	_populate_menu_pick()


func set_stylesheet(sheet: MnsStyleSheet) -> void:
	if _sheet == sheet:
		_request_refresh()
		return
	if _sheet != null and _sheet.changed.is_connected(_request_refresh):
		_sheet.changed.disconnect(_request_refresh)
	_sheet = sheet
	if _sheet != null:
		_sheet.changed.connect(_request_refresh)
	_request_refresh()


func _populate_menu_pick() -> void:
	_menu_pick.clear()
	_menu_names = PackedStringArray()
	if _list_menus.is_valid():
		_menu_names = _list_menus.call()
	for name_value in _menu_names:
		_menu_pick.add_item(String(name_value).get_file())
	if _menu_names.is_empty():
		_show_hint("No menus in the resource folder to preview.")
		return
	# Default to the menu the game opens with.
	var pick := 0
	for i in _menu_names.size():
		var file := String(_menu_names[i]).get_file().to_lower()
		if file == "main.mnu" or file == "jo_main.mnu":
			pick = i
			break
	_menu_pick.select(pick)
	_load_menu_at(pick)


func _load_menu_at(index: int) -> void:
	_doc = null
	_text_resource = null
	if index < 0 or index >= _menu_names.size():
		_request_refresh()
		return
	var name := String(_menu_names[index])
	var bytes := PackedByteArray()
	if FileAccess.file_exists(name):
		bytes = FileAccess.get_file_as_bytes(name)
	elif _root != null:
		bytes = _root.read_file(name)
	if bytes.is_empty():
		_show_hint("Could not read %s." % name.get_file())
		return
	var doc := NovaMnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		_show_hint("Could not open %s." % name.get_file())
		return
	_doc = doc
	_resolve_text_resource()
	_populate_screen_pick()
	_request_refresh()


# Best-effort RTXT table so the preview shows real strings (the same first
# non-empty screen text_rsrc rule the Menus editor applies).
func _resolve_text_resource() -> void:
	_text_resource = null
	if _doc == null or _root == null or _root.get_root_dir().is_empty():
		return
	for screen_id in _doc.get_screen_ids():
		var rsrc := _doc.get_screen_text_rsrc(screen_id)
		if rsrc.is_empty():
			continue
		var path := _root.resolve_file(rsrc)
		if path.is_empty():
			continue
		var rtxt := RtxtStringFile.new()
		if rtxt.load_from_path(path) == OK:
			_text_resource = rtxt
			return


func _populate_screen_pick() -> void:
	_screen_pick.clear()
	if _doc == null:
		return
	for screen_id in _doc.get_screen_ids():
		_screen_pick.add_item(_doc.get_screen_name(screen_id))
	if _screen_pick.item_count > 0:
		_screen_pick.select(0)


func _request_refresh() -> void:
	if _timer != null and _timer.is_inside_tree():
		_timer.start()
	else:
		_rebuild()


func _rebuild() -> void:
	if _doc == null:
		_show_hint(_hint.text if _menu_names.is_empty() else "Pick a menu to preview it with this stylesheet.")
		return
	_hint.visible = false
	if _menu_node == null:
		_menu_node = NovaMnuMenu.new()
		_menu_node.name = "Preview"
		_menu_node.build_on_ready = false
		_menu_node.set_edit_mode(true)
		_menu_node.mouse_filter = Control.MOUSE_FILTER_IGNORE
		_body.add_child(_menu_node)
	_menu_node.set_resource_root(_root)
	_menu_node.set_text_resource(_text_resource)
	_menu_node.set_stylesheet(_sheet)
	# Assigning the menu rebuilds the widget tree (the Menus canvas idiom);
	# edit_mode builds every screen, so single-screen visibility re-applies.
	_menu_node.menu = _doc
	_apply_screen_visibility()
	_fit()


func _apply_screen_visibility() -> void:
	if _menu_node == null or _doc == null:
		return
	var target := ""
	if _screen_pick.selected >= 0 and _screen_pick.selected < _screen_pick.item_count:
		target = _screen_pick.get_item_text(_screen_pick.selected)
	for child in _menu_node.get_children():
		if child is NovaMnuScreen:
			child.visible = target.is_empty() or child.get_screen_name() == target


# Letterbox the 640x480 board into the body (the Menus canvas fit math).
func _fit() -> void:
	if _menu_node == null:
		return
	var area := _body.size
	if area.x <= 1.0 or area.y <= 1.0:
		return
	var fit_scale := minf(area.x / MENU_BOARD_SIZE.x, area.y / MENU_BOARD_SIZE.y)
	fit_scale = maxf(fit_scale, 0.01)
	_menu_node.position = (area - MENU_BOARD_SIZE * fit_scale) * 0.5
	_menu_node.scale = Vector2(fit_scale, fit_scale)


func _show_hint(text: String) -> void:
	_hint.text = text
	_hint.visible = true
	if _menu_node != null and is_instance_valid(_menu_node):
		_menu_node.queue_free()
		_menu_node = null


# Test hook: the live menu node (null until a menu renders).
func get_menu_node() -> NovaMnuMenu:
	return _menu_node
