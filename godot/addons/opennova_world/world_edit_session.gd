@tool
class_name WorldEditSession
extends RefCounted
## Owns native documents independently of the active scene tab and its preview.
## Undo stores native field values; dirty state compares them to the last save.

signal changed(session: WorldEditSession)

const Files := preload("res://tools/file_transaction.gd")
# These are the same-basename sidecars consumed by the normal mission loader.
const SIDECAR_EXTENSIONS := ["til", "wac", "bin", "pcx"]

var _source: WorldSource
var _root: ResourceRoot
var _mission: MissionData
var _terrain: TerrainData
var _environment: EnvFile
# Indexed by WorldField.Document: the BMS, its TRN, its base ENV.
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
	var directory := (local_directory if not source.local_install_name.is_empty() else source.data_directory).strip_edges()
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
	# WorldField.Document order: the BMS, its TRN, its base ENV.
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


func get_file_name(field: WorldField.Id) -> String:
	return _names[WorldField.spec(field).document]


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
		return "Editing base environment values in %s." % _names[WorldField.Document.ENVIRONMENT]
	return "Editing base values in %s. %s supplies the effective %s overrides." % [
			_names[WorldField.Document.ENVIRONMENT], _names[WorldField.Document.MISSION],
			" and ".join(overrides)]


func get_value(field: WorldField.Id, slot: int = 0) -> Variant:
	var spec := WorldField.spec(field)
	if not _has_slot(spec, slot):
		return null
	return spec.read.call(_mission, _terrain, _environment, slot)


func validate_edit(field: WorldField.Id, value: Variant, slot: int = 0) -> String:
	if not _editable:
		return "Create an editable copy to change this world."
	var spec := WorldField.spec(field)
	if not _has_slot(spec, slot):
		return "That foliage slot is unavailable."
	match spec.widget:
		WorldField.Widget.TIME_TEXT, WorldField.Widget.INT_SPIN:
			if not value is int or value < spec.min_value or value > spec.max_value:
				return spec.range_message
		WorldField.Widget.FLOAT_SPIN:
			if not (value is float or value is int) or not is_finite(float(value)) \
					or value < spec.min_value or value > spec.max_value:
				return spec.range_message
		WorldField.Widget.CHECK:
			if not value is bool:
				return spec.range_message
		WorldField.Widget.FILENAME_TEXT:
			if not value is String:
				return "Enter an asset filename."
			var filename := str(value)
			if not filename.is_empty():
				if filename != filename.get_file() or filename.contains(":") or filename != filename.strip_edges():
					return "Use a filename from this world's game data."
				if filename.get_extension().is_empty():
					filename += spec.default_extension
				if not _root.has_file(filename):
					return "%s is missing from this world's game data." % filename
	return ""


## Public undo/redo target. The caller validates a new user edit before creating
## its action. Undo may restore a stock value outside the current UI's ranges.
func apply_value(field: WorldField.Id, value: Variant, slot: int = 0) -> void:
	if not _editable:
		return
	var spec := WorldField.spec(field)
	if not _has_slot(spec, slot):
		return
	spec.write.call(_mission, _terrain, _environment, slot, value)
	_last_error = ""
	changed.emit(self)


func _has_slot(spec: WorldField, slot: int) -> bool:
	return not spec.slotted or (slot >= 0 and slot < get_foliage_count())


func load_preview(world: GameWorld) -> Error:
	return world.load_preview_documents(_root, _mission, _terrain, _environment)


func update_preview(world: GameWorld) -> Error:
	return world.update_preview_settings(_environment)


func save() -> Error:
	if not is_dirty():
		return OK
	if not _editable:
		return _fail(ERR_UNAUTHORIZED, "Create an editable copy before saving.")
	# Paths preserve actual on-disk case, while the native references may differ.
	var writers: Dictionary[String, Callable] = {}
	for filename in get_dirty_files():
		var document := _names.find(filename) as WorldField.Document
		writers[_paths[document].get_file()] = _writer(document)
	var reason: String = Files.write(get_directory(), writers, _save_preflight, true)
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
	var directory := parent_directory.path_join("%s-%d" % [
			_names[WorldField.Document.MISSION].get_basename(), Time.get_ticks_usec()])
	if DirAccess.make_dir_recursive_absolute(directory) != OK:
		return ""
	var writers: Dictionary[String, Callable] = {}
	for document in range(WorldField.Document.size()):
		writers[_names[document]] = _writer(document as WorldField.Document)
	var reason: String = Files.write(directory, writers, func() -> String: return "", false)
	if not reason.is_empty():
		return ""
	var note := "Pending native world edits. Original game data: %s\nMission: %s\n" % [
			get_directory(), _names[WorldField.Document.MISSION]]
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
	var writers: Dictionary[String, Callable] = {
		basename + ".bms": _write_copy_mission.bind(basename),
		basename + ".trn": _terrain.save_to_path,
		basename + ".env": _environment.save_to_path,
	}
	for extension: String in SIDECAR_EXTENSIONS:
		var original := _names[WorldField.Document.MISSION].get_basename() + "." + extension
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


## One value list per document, in WorldField.Document order, so a document is
## dirty exactly when one of its editable values differs from the last save.
func _snapshot() -> Array:
	if _mission == null or _environment == null or _terrain == null:
		return []
	var documents: Array = []
	for document in range(WorldField.Document.size()):
		var values: Array = []
		for spec in WorldField.for_document(document as WorldField.Document):
			var slots := get_foliage_count() if spec.slotted else 1
			for slot in range(slots):
				values.append(spec.read.call(_mission, _terrain, _environment, slot))
		documents.append(values)
	return documents


func _writer(document: WorldField.Document) -> Callable:
	match document:
		WorldField.Document.MISSION: return _mission.save_to_path
		WorldField.Document.TERRAIN: return _terrain.save_to_path
	return _environment.save_to_path


func _fail(error: Error, message: String) -> Error:
	_last_error = message
	return error
