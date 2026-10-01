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
## live in engine/formats/rtxt via the RtxtStringFile resource.
##
## The single-table convenience API (load_table/get_string/...) below predates
## the registry and keeps working unchanged; it reads the default table.

# The lookup-miss marker and the override-first ordering are the engine's
# RtxtStringFile.format_miss_marker / lookup_with_override statics — the
# witnesses live at the engine home, engine/formats/rtxt rtxt.h.

## The named tables (the registry below carries the witness) and the RTXT
## sections the game reads by name; every caller spells them through these.
const TABLE_GAMETEXT := "gametext"
const TABLE_MISSION := "mission"
const SECTION_OVERLAYS := "Overlays"
const SECTION_WPNAMES := "WPNames"
const SECTION_CLIENT := "Client"
const SECTION_LOADING_TEXT := "LoadingText"
const TABLE_MENUTXT := "menutxt"
const TABLE_GAMEUI := "gameui"
## keyhelp.bin: the "Keys" table every key-binding label (modifier prefixes,
## the " or " separator, mouse and key names) resolves through. Registering it
## installs it process-wide for the engine's binding formatters
## (RtxtStringFile.install_key_strings -> controls::set_key_strings, the
## KeyHelp_GetStringWithFallback lookup over retail's g_TextKeyHelp).
const TABLE_KEYHELP := "keyhelp"
## gameerr.bin: the "Generic Error Strings" the post-mission error dialog shows
## (STRE_CDTROUBLE / STRE_SYSTEM / STRE_PIRATE / STRE_BADMISSION).
const TABLE_GAMEERR := "gameerr"
const SECTION_GENERIC_ERRORS := "Generic Error Strings"
const SECTION_MENU := "Menu"

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


func is_loaded() -> bool:
	return _table != null


## Returns the raw localized text (including any {hot} marker), or default.
func get_string(key: StringName, default: String = "") -> String:
	if _table == null or not _table.has_string(key):
		return default
	return _table.get_string(key)


func clear() -> void:
	_table = null
	_tables.clear()
	_override_table = null
	# The installed "Keys" table goes with the registry: every binding label
	# falls back to its literal until the next keyhelp registration.
	RtxtStringFile.clear_key_strings()


## --- Named-table registry [orig: Game_InitSubsystems @ 0x4A6CD0] ---

## Registers a loaded table under a name ("gametext", "gameerr", ...). Passing
## null unregisters. Names are case-insensitive.
func register_table(name: String, table: RtxtStringFile) -> void:
	var id := name.to_lower()
	if table == null:
		_tables.erase(id)
	else:
		_tables[id] = table
	# keyhelp.bin doubles as the engine's process-wide "Keys" binding-label
	# table (TABLE_KEYHELP): registering installs it, unregistering forgets it.
	if id == TABLE_KEYHELP:
		if table != null:
			table.install_key_strings()
		else:
			RtxtStringFile.clear_key_strings()


func get_table(name: String) -> RtxtStringFile:
	return _tables.get(name.to_lower())


## One RTXT .bin read off a mounted resource root; null when there is no root,
## the root carries no such file, or it does not parse.
func load_rtxt(root: ResourceRoot, file: String) -> RtxtStringFile:
	if root == null:
		return null
	var bytes := root.read_file(file)
	if bytes.is_empty():
		return null
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(bytes) != OK:
		return null
	return table


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
## so missing strings are debuggable instead of silently blank. The ordering
## and marker are the engine's RtxtStringFile.lookup_with_override; adopting
## it also adopts the engine's found-entry-with-empty-text-is-a-HIT behavior
## (returns "" — the earlier .gd version returned the miss marker for an
## authored empty string).
func lookup(table_name: String, section: String, key: String) -> String:
	return RtxtStringFile.lookup_with_override(_override_table,
			_tables.get(table_name.to_lower()), section, key)


## The named table's own entry, or `fallback` when the table is absent or
## lacks the key: no override table and no miss marker, the form the HUD
## overlay and label lookups take (a table-only has/get pair).
func lookup_or(table_name: String, section: String, key: String, fallback: String) -> String:
	var t: RtxtStringFile = get_table(table_name)
	if t != null and t.has_string_in_section(section, key):
		return t.get_string_in_section(section, key)
	return fallback


## A menu UI token: menutxt's "Menu" section, then gameui's, else the fallback
## (the armory, player-info and DEATH-screen tokens all resolve this way).
## [orig: the menu tokens resolve against the menu resource (game.bin) via
##  TextResource_GetStringWithFallback(resource, "Menu", key) @0x562ee0]
func menu_text(key: String, fallback: String) -> String:
	return lookup_or(TABLE_MENUTXT, SECTION_MENU, key,
			lookup_or(TABLE_GAMEUI, SECTION_MENU, key, fallback))


## lookup() with the {hot} accelerator marker stripped for display (the miss
## marker never carries a {hot} token, so stripping the result is exact).
func lookup_display(table_name: String, section: String, key: String) -> String:
	return RtxtStringFile.strip_hotkey(lookup(table_name, section, key))
