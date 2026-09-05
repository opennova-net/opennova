@tool
class_name WorldEditSession
extends RefCounted
## Owns native documents independently of the active scene tab and its preview.
## Undo stores native field values; dirty state compares them to the last save.

signal changed(session: WorldEditSession)

const Files := preload("res://addons/opennova_world/world_file_transaction.gd")
# These are the same-basename sidecars consumed by the normal mission loader.
const SIDECAR_EXTENSIONS := ["til", "wac", "bin", "pcx"]
enum Field { START_TIME, SKY_MAP_1, SKY_MAP_2, SKY_HEIGHT, FOLIAGE_GRAPHIC, FOLIAGE_MATCH, FOLIAGE_SHADOW }

var _source: WorldSource
var _root: ResourceRoot
var _mission: MissionData
var _terrain: TerrainData
var _environment: EnvFile
var _names: PackedStringArray = []
var _paths: PackedStringArray = []
var _disk_versions: PackedStringArray = []
var _saved: Array = []
var _last_error := ""
var _editable := false
var _key := ""
var _recovery_path := ""
var _recovery_state: Array = []


static func selection_key(source: WorldSource, local_directory: String = "") -> String:
	if source == null:
		return ""
	var directory := (local_directory if not source.install_key.is_empty() else source.data_directory).strip_edges()
	if not directory.is_absolute_path():
		directory = "res://".path_join(directory)
	directory = ProjectSettings.globalize_path(directory).simplify_path().replace("\\", "/")
	if OS.get_name() == "Windows":
		directory = directory.to_lower()
	return JSON.stringify([directory, source.source_kind, source.mission_name.strip_edges().to_lower(),
			source.game_code.strip_edges().to_lower(), source.expansion.strip_edges().to_lower()])


func open(source: WorldSource, local_directory: String = "") -> Error:
	if is_dirty():
		return _fail(ERR_BUSY, "Save or discard this world's pending edits before reopening it.")
	return _open_documents(source, local_directory)


## The editor calls this only after the user chooses to discard pending edits.
## The same session remains the undo target, including after a disk reload.
func reload_from_disk() -> Error:
	return _open_documents(_source, get_directory())


func _open_documents(source: WorldSource, local_directory: String) -> Error:
	if source == null:
		return _fail(ERR_UNCONFIGURED, "Assign a WorldSource in the Inspector.")
	var selected := source.duplicate() as WorldSource
	selected.mission_name = selected.mission_name.strip_edges()
	selected.game_code = selected.game_code.strip_edges().to_lower()
	selected.expansion = selected.expansion.strip_edges()
	var root := selected.open_root(local_directory)
	if root == null:
		return _fail(selected.get_last_error_code(), selected.get_last_error())
	var mission := selected.open_mission(root)
	if mission == null:
		return _fail(selected.get_last_error_code(), selected.get_last_error())
	var names := PackedStringArray([selected.mission_name, mission.get_terrain_ref() + ".trn",
			mission.get_environment_ref() + ".env"])
	for filename in names:
		if filename != filename.get_file() or not root.has_file(filename):
			return _fail(ERR_FILE_NOT_FOUND, "%s requires %s." % [selected.mission_name, filename])
	var terrain := TerrainData.new()
	var error := terrain.load_from_resource_root(root, names[1])
	if error != OK:
		return _fail(error, "Could not load %s." % names[1])
	var environment := EnvFile.new()
	error = environment.load_from_resource_root(root, names[2])
	if error != OK:
		return _fail(error, "Could not load %s." % names[2])
	_source = selected
	_key = selection_key(source, local_directory)
	_root = root
	_mission = mission
	_terrain = terrain
	_environment = environment
	_names = names
	_paths.clear()
	_editable = _source.source_kind != WorldSource.RETAIL_INSTALL
	for filename in _names:
		var path := _root.resolve_file(filename)
		_paths.append(path)
		_editable = _editable and not path.is_empty()
	_saved = _snapshot()
	_remember_disk_versions()
	_last_error = ""
	return OK


func get_key() -> String:
	return _key


func get_source() -> WorldSource:
	return _source.duplicate() as WorldSource if _source != null else null


func get_directory() -> String:
	return _root.get_root_dir() if _root != null else ""


func get_last_error() -> String:
	return _last_error


func is_editable() -> bool:
	return _editable


func is_dirty() -> bool:
	return not _saved.is_empty() and _snapshot() != _saved


func get_dirty_files() -> PackedStringArray:
	var files := PackedStringArray()
	if not _saved.is_empty():
		var current := _snapshot()
		for i in range(_names.size()):
			if current[i] != _saved[i]:
				files.append(_names[i])
	return files


func get_file_name(field: Field) -> String:
	return _names[_document_index(field)]


func get_foliage_count() -> int:
	return _terrain.get_foliage_defs().size()


func get_environment_note() -> String:
	var info := _mission.get_info()
	var overrides := PackedStringArray()
	if info.has_water_override:
		overrides.append("water")
	if info.has_fog_distance_override or info.has_fog_color_override:
		overrides.append("fog")
	if overrides.is_empty():
		return "Editing base environment values in %s." % _names[2]
	return "Editing base values in %s. %s supplies the effective %s overrides." % [
			_names[2], _names[0], " and ".join(overrides)]


func get_value(field: Field, slot: int = 0) -> Variant:
	match field:
		Field.START_TIME: return _mission.get_info().start_time
		Field.SKY_MAP_1: return _environment.sky_map1
		Field.SKY_MAP_2: return _environment.sky_map2
		Field.SKY_HEIGHT: return _environment.sky_height
	var defs := _terrain.get_foliage_defs()
	if slot < 0 or slot >= defs.size():
		return null
	var definition := defs[slot] as TerrainFoliageDef
	match field:
		Field.FOLIAGE_GRAPHIC: return definition.graphic
		Field.FOLIAGE_MATCH: return definition.match
		Field.FOLIAGE_SHADOW: return definition.shadow
	return null


func validate_edit(field: Field, value: Variant, slot: int = 0) -> String:
	if not _editable:
		return "Create an editable copy to change this world."
	if field >= Field.FOLIAGE_GRAPHIC and (slot < 0 or slot >= get_foliage_count()):
		return "That foliage slot is unavailable."
	match field:
		Field.START_TIME:
			if not value is int or value < 0 or value >= 24 * 256:
				return "Start time must be between 00:00 and 23:59."
		Field.SKY_HEIGHT:
			if not (value is float or value is int) or not is_finite(float(value)) or value < 10 or value > 500:
				return "Sky height must be between 10 and 500."
		Field.FOLIAGE_MATCH:
			if not value is int or value < -1 or value > 255:
				return "Map match must be between -1 and 255."
		Field.FOLIAGE_SHADOW:
			if not value is bool:
				return "Shadow must be enabled or disabled."
		Field.SKY_MAP_1, Field.SKY_MAP_2, Field.FOLIAGE_GRAPHIC:
			if not value is String:
				return "Enter an asset filename."
			var filename := str(value)
			if not filename.is_empty():
				if filename != filename.get_file() or filename.contains(":") or filename != filename.strip_edges():
					return "Use a filename from this world's game data."
				if field == Field.FOLIAGE_GRAPHIC and filename.get_extension().is_empty():
					filename += ".3di"
				if not _root.has_file(filename):
					return "%s is missing from this world's game data." % filename
	return ""


## Public undo/redo target. The caller validates a new user edit before creating
## its action. Undo may restore a stock value outside the current UI's ranges.
func apply_value(field: Field, value: Variant, slot: int = 0) -> void:
	if not _editable:
		return
	match field:
		Field.START_TIME: _mission.set_header_int("start_time", int(value))
		Field.SKY_MAP_1: _environment.sky_map1 = str(value)
		Field.SKY_MAP_2: _environment.sky_map2 = str(value)
		Field.SKY_HEIGHT: _environment.sky_height = float(value)
		_:
			var defs := _terrain.get_foliage_defs()
			if slot < 0 or slot >= defs.size():
				return
			var definition := defs[slot] as TerrainFoliageDef
			match field:
				Field.FOLIAGE_GRAPHIC: definition.graphic = str(value)
				Field.FOLIAGE_MATCH: definition.match = int(value)
				Field.FOLIAGE_SHADOW: definition.shadow = bool(value)
			# Fresh native records preserve the unexposed color and attribute fields.
			_terrain.set_foliage_defs(defs)
	_last_error = ""
	changed.emit(self)


func load_preview(world: GameWorld) -> Error:
	return world.load_preview_documents(_root, _mission, _terrain, _environment)


func update_preview(world: GameWorld) -> Error:
	return world.update_preview_settings(_environment)


func save() -> Error:
	if not is_dirty():
		return OK
	if not _editable:
		return _fail(ERR_UNAUTHORIZED, "Create an editable copy before saving.")
	var writers: Dictionary = {}
	for filename in get_dirty_files():
		match _names.find(filename):
			0: writers[filename] = _mission.save_to_path
			1: writers[filename] = _terrain.save_to_path
			2: writers[filename] = _environment.save_to_path
	# Paths preserve actual on-disk case, while the native references may differ.
	var disk_writers: Dictionary = {}
	for filename: String in writers:
		disk_writers[_paths[_names.find(filename)].get_file()] = writers[filename]
	var reason: String = Files.write(get_directory(), disk_writers, _save_preflight, true)
	if not reason.is_empty():
		return _fail(ERR_FILE_CANT_WRITE, reason)
	_saved = _snapshot()
	_remember_disk_versions()
	ResourceRoot.bump_cache_epoch()
	_last_error = ""
	changed.emit(self)
	return OK


func disk_conflict() -> String:
	return _save_preflight() if _editable else ""


## Godot's external-save callback cannot veto an editor exit. On a failed
## save the plugin preserves pending documents as ordinary native recovery
## files, in a writable location independent of the selected game folder.
func write_recovery(parent_directory: String) -> String:
	if _recovery_state == _snapshot() and DirAccess.dir_exists_absolute(_recovery_path):
		return _recovery_path
	var directory := parent_directory.path_join("%s-%d" % [_names[0].get_basename(), Time.get_ticks_usec()])
	if DirAccess.make_dir_recursive_absolute(directory) != OK:
		return ""
	var reason: String = Files.write(directory, {
		_names[0]: _mission.save_to_path,
		_names[1]: _terrain.save_to_path,
		_names[2]: _environment.save_to_path,
	}, func() -> String: return "", false)
	if not reason.is_empty():
		return ""
	var note := "Pending native world edits. Original game data: %s\nMission: %s\n" % [get_directory(), _names[0]]
	note += "These BMS, TRN and ENV files use the original game's supporting assets.\n"
	note += "To recover, back up the destination files, copy these documents into the original data folder, then Reload in Godot.\n"
	_write_bytes(directory.path_join("RECOVERY.txt"), note.to_utf8_buffer())
	_recovery_path = directory
	_recovery_state = _snapshot()
	return directory


## Clone through the native writers; shared supporting assets stay on the root.
## Existing targets, including archive entries, are never overwritten.
func create_editable_copy(basename: String) -> WorldSource:
	var regex := RegEx.create_from_string("^[A-Za-z0-9_][A-Za-z0-9_-]{0,11}$")
	if regex.search(basename) == null:
		_fail(ERR_INVALID_PARAMETER, "Use 1-12 letters, digits, underscores or hyphens for the world name.")
		return null
	var writers: Dictionary = {
		basename + ".bms": _write_copy_mission.bind(basename),
		basename + ".trn": _terrain.save_to_path,
		basename + ".env": _environment.save_to_path,
	}
	for extension: String in SIDECAR_EXTENSIONS:
		var original := _names[0].get_basename() + "." + extension
		if _root.has_file(original):
			writers[basename + "." + extension] = _write_bytes.bind(_root.read_file(original))
	var names: Array = writers.keys()
	var reason: String = Files.write(get_directory(), writers, _copy_preflight.bind(names), false)
	if not reason.is_empty():
		_fail(ERR_FILE_CANT_WRITE, reason)
		return null
	var source := get_source()
	source.mission_name = basename + ".bms"
	if source.source_kind != WorldSource.LOOSE_SOURCE:
		source.source_kind = WorldSource.EDITABLE_GAME_DATA
	ResourceRoot.bump_cache_epoch()
	_last_error = ""
	return source


func _write_copy_mission(path: String, basename: String) -> Error:
	var error := _mission.save_to_path(path)
	if error != OK:
		return error
	var copy := MissionData.new()
	error = copy.open_file(path)
	if error != OK:
		return error
	if not copy.set_header_string("terrain", basename) or not copy.set_header_string("environment", basename):
		return ERR_INVALID_DATA
	return copy.save_as(path)


static func _write_bytes(path: String, bytes: PackedByteArray) -> Error:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_buffer(bytes)
	file.flush()
	var error := file.get_error()
	file.close()
	return error


func _save_preflight() -> String:
	for i in range(_paths.size()):
		if not FileAccess.file_exists(_paths[i]) or FileAccess.get_sha256(_paths[i]) != _disk_versions[i]:
			return "%s changed on disk. Your edits are still open; reload to use the disk version, or create a copy to keep your edits." % _names[i]
	return ""


func _copy_preflight(names: Array) -> String:
	var existing: PackedStringArray = []
	for filename in DirAccess.get_files_at(get_directory()):
		existing.append(filename.to_lower())
	for filename in DirAccess.get_directories_at(get_directory()):
		existing.append(filename.to_lower())
	for filename: String in names:
		if existing.has(filename.to_lower()) or _root.has_file(filename):
			return "%s already exists. Choose a different world name." % filename
	return ""


func _remember_disk_versions() -> void:
	_disk_versions.clear()
	for path in _paths:
		_disk_versions.append(FileAccess.get_sha256(path) if not path.is_empty() else "")


func _snapshot() -> Array:
	if _mission == null or _environment == null or _terrain == null:
		return []
	var foliage: Array = []
	for definition: TerrainFoliageDef in _terrain.get_foliage_defs():
		foliage.append([definition.graphic, definition.match, definition.attrib_flags])
	return [[_mission.get_info().start_time],
			foliage, [_environment.sky_map1, _environment.sky_map2, _environment.sky_height]]


static func _document_index(field: Field) -> int:
	if field == Field.START_TIME:
		return 0
	return 1 if field >= Field.FOLIAGE_GRAPHIC else 2


func _fail(error: Error, message: String) -> Error:
	_last_error = message
	return error
