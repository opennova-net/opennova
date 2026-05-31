extends Node

## Runtime localized-string lookup. Autoload singleton wrapping a single
## RtxtStringFile (NovaLogic strings/*.bin). Game code calls
## NovaStrings.get_string("BTN_NEW_GAME"). The case-insensitive lookup and the
## {hot} marker handling live in libs/rtxt via the RtxtStringFile resource.

var _table: RtxtStringFile


## Loads a strings .bin from a res:// or absolute path. Returns OK on success.
func load_table(path: String) -> Error:
	var table := RtxtStringFile.new()
	var err := table.load_from_path(path)
	if err != OK:
		return err
	_table = table
	return OK


## Uses an already-loaded table directly (e.g. from the editor or ResourceLoader).
func set_table(table: RtxtStringFile) -> void:
	_table = table


func is_loaded() -> bool:
	return _table != null


func has_string(key: StringName) -> bool:
	return _table != null and _table.has_string(key)


## Returns the raw localized text (including any {hot} marker), or default.
func get_string(key: StringName, default: String = "") -> String:
	if _table == null or not _table.has_string(key):
		return default
	return _table.get_string(key)


## Returns the display text with the {hot} accelerator marker stripped.
func get_display_string(key: StringName, default: String = "") -> String:
	return RtxtStringFile.strip_hotkey(get_string(key, default))


func clear() -> void:
	_table = null
