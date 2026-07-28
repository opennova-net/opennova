extends Node

## Runtime localized-string lookup. Autoload singleton over RtxtStringFile
## tables (NovaLogic .bin string tables), mirroring the original engine's
## table model:
##  - Named tables for the four globals Jointops.exe loads at init (gameerr,
##    gametext, vmacros, keyhelp) plus per-mission/menu tables
##    [orig: Game_InitSubsystems @ 0x4A6CD0].
##  - One override table consulted before every lookup — the original loads
##    the active expansion's text bin there (expansion\<exp>\<exp>.bin)
##    [orig: TextResource_LoadOverrideTable @ 0x75D5C0].
##  - lookup(table, section, key) misses resolve to "??section:key??", the
##    marker the original formats for its error table
##    [orig: GameErr_GetString @ 0x4C2C60].
## The case-insensitive matching, first-match-wins and {hot} marker handling
## live in libs/rtxt via the RtxtStringFile resource.
##
## The single-table convenience API (load_table/get_string/...) below predates
## the registry and keeps working unchanged; it reads the default table.

## Marker formatted on lookup() misses, after the original's "??%s:%s??".
const MISS_FORMAT := "??%s:%s??"

var _table: RtxtStringFile
var _tables: Dictionary = {}
var _override_table: RtxtStringFile


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
	_tables.clear()
	_override_table = null


## --- Named-table registry [orig: Game_InitSubsystems @ 0x4A6CD0] ---

## Registers a loaded table under a name ("gametext", "gameerr", ...). Passing
## null unregisters. Names are case-insensitive.
func register_table(name: String, table: RtxtStringFile) -> void:
	var id := name.to_lower()
	if table == null:
		_tables.erase(id)
	else:
		_tables[id] = table


func get_table(name: String) -> RtxtStringFile:
	return _tables.get(name.to_lower())


## --- Override table [orig: TextResource_LoadOverrideTable @ 0x75D5C0] ---

## Sets the table consulted before every lookup (the original engine loads the
## active expansion's text bin here). Pass null to clear.
func set_override_table(table: RtxtStringFile) -> void:
	_override_table = table


func get_override_table() -> RtxtStringFile:
	return _override_table


## --- Engine-faithful lookup [orig: TextResource_FindEntryBySectionAndKey
## @ 0x75D250 via GameErr_GetString @ 0x4C2C60] ---

## Section-scoped lookup against a named table, override-table-first. A miss
## returns "??section:key??", the visible marker the original engine produces,
## so missing strings are debuggable instead of silently blank.
func lookup(table_name: String, section: String, key: String) -> String:
	var entry := _lookup_raw(table_name, section, key)
	if entry == "":
		return MISS_FORMAT % [section, key]
	return entry


## lookup() with the {hot} accelerator marker stripped for display.
func lookup_display(table_name: String, section: String, key: String) -> String:
	var entry := _lookup_raw(table_name, section, key)
	if entry == "":
		return MISS_FORMAT % [section, key]
	return RtxtStringFile.strip_hotkey(entry)


func _lookup_raw(table_name: String, section: String, key: String) -> String:
	if _override_table != null and _override_table.has_string_in_section(section, key):
		return _override_table.get_string_in_section(section, key)
	var table: RtxtStringFile = _tables.get(table_name.to_lower())
	if table == null or not table.has_string_in_section(section, key):
		return ""
	return table.get_string_in_section(section, key)
