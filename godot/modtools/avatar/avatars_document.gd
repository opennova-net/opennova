class_name AvatarsDocument
extends EditorResourceDocument

# Avatars.def document: the shared EditorResourceDocument lifecycle over a
# NovaAvatarDatabase. All format behavior lives in the engine wrapper; only the
# blank factory, the loaders, and the self-writing save are domain code. The
# database emits "changed" on set_model()/create_empty(), which the base wires to
# the dirty/undo state.


func _make_new_resource():
	var fresh := NovaAvatarDatabase.new()
	fresh.create_empty()
	return fresh


func _default_basename() -> String:
	return "Avatars"


func _file_extension() -> String:
	return "def"


# NovaAvatarDatabase writes itself from scratch (docs/adr/0021), not ResourceSaver.
func _save_resource(path: String) -> Error:
	return resource.save_to_path(path)


# Open an Avatars.def from disk (the editor Open dialog).
func open_avatars(path: String) -> Error:
	if not FileAccess.file_exists(path):
		return ERR_FILE_NOT_FOUND
	var loaded := NovaAvatarDatabase.new()
	var err := loaded.load(path)
	if err != OK:
		return err
	adopt_loaded(loaded, path)
	return OK


# Open an Avatars.def by flat name through the mounted resource root (VFS/PFF).
func open_avatars_from_root(resource_root, name: String) -> Error:
	var loaded := NovaAvatarDatabase.new()
	var err := loaded.load_from_resource_root(resource_root, name)
	if err != OK:
		return err
	adopt_loaded(loaded, name)
	return OK
