class_name MnuEditorDocument
extends EditorResourceDocument

# Menu (.mnu) document: the shared EditorResourceDocument lifecycle over a
# NovaMnuDocument. All format behavior lives in the engine wrapper; only the
# blank factory, the byte loader, and the self-writing save are domain code.


func _make_new_resource():
	var fresh := NovaMnuDocument.new()
	fresh.create_empty()
	return fresh


func _default_basename() -> String:
	return "menu"


func _file_extension() -> String:
	return "mnu"


# NovaMnuDocument writes itself (byte-faithful serializer), not ResourceSaver.
func _save_resource(path: String) -> Error:
	return resource.save_to_path(path)


func open_mnu(path: String) -> Error:
	if not FileAccess.file_exists(path):
		return ERR_FILE_NOT_FOUND
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty():
		return FileAccess.get_open_error()
	var loaded := NovaMnuDocument.new()
	var err := loaded.load_from_bytes(bytes)
	if err != OK:
		return err
	adopt_loaded(loaded, path)
	return OK
