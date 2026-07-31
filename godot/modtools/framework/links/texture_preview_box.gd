class_name TexturePreviewBox
extends PanelContainer
## A small always-on texture preview: checkerboard underlay, aspect-kept image,
## and a "not found" overlay when the name resolves to nothing. Two feeds:
## push (set_texture — the caller already holds the texture, e.g. the one the
## sky actually renders) or pull (set_loader + show_name — loads on demand,
## memoized by name because decode-on-load is NOT cached upstream).

const _CHECKER_CELL := 8

static var _checker_texture: Texture2D

var _preview: TextureRect
var _checker: TextureRect
var _missing_label: Label
var _loader := Callable()
var _shown_name := ""
var _loaded_for := ""


func _init() -> void:
	custom_minimum_size = Vector2(96, 96)
	_checker = TextureRect.new()
	_checker.name = "PreviewChecker"
	_checker.texture = _ensure_checker()
	_checker.stretch_mode = TextureRect.STRETCH_TILE
	# The display layers never take mouse events themselves; owners that accept
	# drops over the preview need the walk to reach the box (and beyond).
	_checker.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_checker)
	_preview = TextureRect.new()
	_preview.name = "PreviewImage"
	_preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	_preview.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_preview)
	_missing_label = Label.new()
	_missing_label.name = "PreviewMissing"
	_missing_label.text = "?"
	_missing_label.tooltip_text = "Not found in the resource folder."
	# PASS (not STOP) keeps the tooltip while letting drops fall through to the box.
	_missing_label.mouse_filter = Control.MOUSE_FILTER_PASS
	_missing_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_missing_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_missing_label.visible = false
	add_child(_missing_label)


func set_box_size(px: Vector2) -> void:
	custom_minimum_size = px


## Push feed: display exactly this texture (null shows the checker + missing mark
## when a name is set).
func set_texture(tex: Texture2D) -> void:
	_loaded_for = _shown_name
	_apply(tex)


## Pull feed: loader(name) -> Texture2D. show_name() drives loads.
func set_loader(loader: Callable) -> void:
	_loader = loader
	_loaded_for = ""
	if not _shown_name.is_empty():
		show_name(_shown_name)


## Loads (memoized by name) and displays the texture for `name`. An empty name
## clears to the bare checker.
func show_name(name: String) -> void:
	_shown_name = name
	if name.is_empty():
		_loaded_for = ""
		_preview.texture = null
		_missing_label.visible = false
		return
	if name == _loaded_for:
		return
	_loaded_for = name
	var tex: Texture2D = null
	if _loader.is_valid():
		tex = _loader.call(name)
	_apply(tex)


func get_texture() -> Texture2D:
	return _preview.texture


func _apply(tex: Texture2D) -> void:
	_preview.texture = tex
	_missing_label.visible = tex == null and not _shown_name.is_empty()


static func _ensure_checker() -> Texture2D:
	if _checker_texture != null:
		return _checker_texture
	var img := Image.create(_CHECKER_CELL * 2, _CHECKER_CELL * 2, false, Image.FORMAT_RGB8)
	var dark := Color(0.16, 0.16, 0.18)
	var light := Color(0.22, 0.22, 0.25)
	for y in _CHECKER_CELL * 2:
		for x in _CHECKER_CELL * 2:
			var odd := (x / _CHECKER_CELL + y / _CHECKER_CELL) % 2 == 1
			img.set_pixel(x, y, light if odd else dark)
	_checker_texture = ImageTexture.create_from_image(img)
	return _checker_texture
