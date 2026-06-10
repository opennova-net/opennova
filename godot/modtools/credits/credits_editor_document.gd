class_name CreditsEditorDocument
extends EditorResourceDocument

# Credits (.kda) document: the shared EditorResourceDocument lifecycle over a
# CbinCreditsResource. Only the blank factory, the loader, and the file naming
# are domain code.


func _make_new_resource():
	return CbinCreditsResource.new()


func _default_basename() -> String:
	return "credits"


func _file_extension() -> String:
	return "kda"


func open_kda(path: String) -> Error:
	# CACHE_MODE_REPLACE: re-opening the same path must reload from disk, not
	# serve Godot's cached resource instance.
	var loaded := ResourceLoader.load(path, "CbinCreditsResource", ResourceLoader.CACHE_MODE_REPLACE) as CbinCreditsResource
	if loaded == null:
		return ERR_CANT_OPEN
	adopt_loaded(loaded, path)
	return OK
