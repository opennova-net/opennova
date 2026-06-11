class_name TextureRefWidget
extends VBoxContainer
## A texture reference with an always-on preview: a ResourceRefWidget row
## (kind "texture" by default; values keep their extension) above a
## TexturePreviewBox. Forwards the full bind_link contract, so it binds
## anywhere ResourceRefWidget does.

signal value_changed(value: String)

var ref_row: ResourceRefWidget
var preview: TexturePreviewBox


func _init() -> void:
	add_theme_constant_override("separation", 4)
	ref_row = ResourceRefWidget.new()
	ref_row.name = "TextureRefRow"
	# Texture values are filenames with extension (the candidate resolver needs it).
	ref_row.set_value_from_path(func(path: String) -> String: return path.get_file())
	ref_row.value_changed.connect(func(value: String) -> void:
		preview.show_name(value)
		value_changed.emit(value))
	add_child(ref_row)
	preview = TexturePreviewBox.new()
	preview.name = "TextureRefPreview"
	# The preview box is a MOUSE_FILTER_STOP panel; forward so drops over the
	# image land on the row's handlers instead of dying inside it.
	preview.set_drag_forwarding(Callable(), _can_drop_data, _drop_data)
	add_child(preview)


func configure(kind: String, display_label: String, services: Dictionary = {}) -> void:
	# No texture workspace exists to jump to; strip the jump affordance while
	# keeping resolve (badge) and pick (browse).
	var trimmed := services.duplicate()
	trimmed.erase("jump")
	ref_row.configure(kind, display_label, trimmed)


func set_value(text: String) -> void:
	ref_row.set_value(text)
	preview.show_name(text)


func get_value() -> String:
	return ref_row.get_value()


# Drops anywhere on the widget (including the preview box) land on the row's
# handlers, so a dropped payload commits exactly like a pick.
func _can_drop_data(at: Vector2, data: Variant) -> bool:
	return ref_row._can_drop_data(at, data)


func _drop_data(at: Vector2, data: Variant) -> void:
	ref_row._drop_data(at, data)


func set_preview_loader(loader: Callable) -> void:
	preview.set_loader(loader)


## Push feed for callers that already hold the rendered texture (e.g. the env
## file's resolved sky maps) — truthful preview, no second decode.
func set_preview_texture(tex: Texture2D) -> void:
	preview.set_texture(tex)


func refresh_preview() -> void:
	preview.show_name(get_value())
