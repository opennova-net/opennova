class_name FntEditorDocument
extends EditorResourceDocument

# Font (.fnt) document: the shared EditorResourceDocument lifecycle over a
# NovaFntResource. The byte loader (VFS opens hand bytes straight in), the
# blank factory, and the rasterizer entry are domain code.


func _make_new_resource():
	var fresh := NovaFntResource.new()
	fresh.create_blank(1, 0)
	return fresh


func _default_basename() -> String:
	return "font"


func _file_extension() -> String:
	return "fnt"


# NovaFntResource writes itself (byte serializer), not ResourceSaver.
func _save_resource(path: String) -> Error:
	var bytes: PackedByteArray = resource.to_bytes()
	if bytes.is_empty():
		return ERR_CANT_CREATE
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_buffer(bytes)
	file.close()
	return OK


func open_fnt(path: String) -> Error:
	if not FileAccess.file_exists(path):
		return ERR_FILE_NOT_FOUND
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty():
		return FileAccess.get_open_error()
	return open_fnt_bytes(bytes, path)


func open_fnt_bytes(bytes: PackedByteArray, display_path: String) -> Error:
	var loaded := NovaFntResource.new()
	var err := loaded.load_from_bytes(bytes)
	if err != OK:
		return err
	adopt_loaded(loaded, display_path)
	return OK


## Rasterize a system font into a fresh document. Unlike adopt_loaded this
## adopts DIRTY with no path: the generated bitmap exists only in memory until
## the user saves it somewhere.
func generate_from_font(font: Font, px_size: int, flags: int) -> Error:
	var generated := FntRasterizer.rasterize(font, px_size, flags)
	if generated == null:
		return ERR_CANT_CREATE
	_replace_resource(generated)
	set_current_path("")
	mark_dirty()
	state_changed.emit()
	resource_loaded.emit(resource)
	return OK
