extends RefCounted

# Performance probes temporarily point the editor/runtime at a caller-selected
# resource mount. That setting lives in the user's shared editor config, and
# set_resource_dir() also rewrites the recent-directory list. Preserve the
# config byte-for-byte so an empty/unset mount and every incidental setting are
# restored on normal completion and MainLoop/scene teardown alike.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH

var _captured := false
var _restored := false
var _had_config := false
var _saved_config := PackedByteArray()


func capture() -> Error:
	if _captured and not _restored:
		return OK
	_had_config = FileAccess.file_exists(STATE_CONFIG_PATH)
	_saved_config = PackedByteArray()
	if _had_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.READ)
		if file == null:
			return FileAccess.get_open_error()
		_saved_config = file.get_buffer(file.get_length())
		file.close()
	_captured = true
	_restored = false
	return OK


func restore() -> Error:
	if not _captured or _restored:
		return OK
	var err := OK
	if _had_config:
		var file := FileAccess.open(STATE_CONFIG_PATH, FileAccess.WRITE)
		if file == null:
			return FileAccess.get_open_error()
		file.store_buffer(_saved_config)
		err = file.get_error()
		file.close()
	elif FileAccess.file_exists(STATE_CONFIG_PATH):
		err = DirAccess.remove_absolute(ProjectSettings.globalize_path(STATE_CONFIG_PATH))
	if err == OK:
		_restored = true
	return err
