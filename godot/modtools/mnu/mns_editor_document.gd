class_name MnsEditorDocument
extends EditorResourceDocument

# Menu stylesheet (.mns) document: the shared EditorResourceDocument lifecycle
# over an MnsStyleSheet. All format behavior lives in the engine wrapper
# (document-backed, lossless save; ADR 0014); only the seeded template, the
# byte loader, and the self-writing save are domain code.

# A fresh stylesheet's header, in the artist's language (the real
# menu_style.mns opens with NovaLogic's own much longer spec comment).
const NEW_TEMPLATE := "// Menu stylesheet. Menus refer to these names as %NAME%.\r\n" \
		+ "// One entry per line: NAME, some space, then the value.\r\n" \
		+ "// Colors are AARRGGBB hex, fonts are .fnt files, pictures are .tga files.\r\n" \
		+ "// Save this file as menu_style.mns so the game finds it.\r\n"


func _make_new_resource():
	var fresh := MnsStyleSheet.new()
	fresh.set_source_text(NEW_TEMPLATE)
	return fresh


# Save As of a pathless document lands on the canonical name the engine looks
# for ("named menu_style.mns for the game to find it").
func _default_basename() -> String:
	return "menu_style"


func _file_extension() -> String:
	return "mns"


# MnsStyleSheet writes itself (lossless document serializer), not ResourceSaver.
func _save_resource(path: String) -> Error:
	return resource.save_to_path(path)


func open_mns(path: String) -> Error:
	if not FileAccess.file_exists(path):
		return ERR_FILE_NOT_FOUND
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty() and FileAccess.get_open_error() != OK:
		return FileAccess.get_open_error()
	return open_mns_bytes(bytes, path)


# VFS-opened stylesheets (a name picked from the resource browser) arrive as
# bytes; display_path is the mounted dir + bare name, like the other formats.
func open_mns_bytes(bytes: PackedByteArray, display_path: String) -> Error:
	var loaded := MnsStyleSheet.new()
	var err := loaded.load_from_bytes(bytes)
	if err != OK:
		return err
	adopt_loaded(loaded, display_path)
	return OK
