extends Node

## The process's named string tables for the shell's scripts (autoload "Strings"): the engine's
## registry (TextTables over engine/formats/rtxt/text_tables.h) holds the tables, follows the
## expansion override table and owns every lookup rule (the override table first, the miss
## marker, the menu tokens' chain); this autoload only names the tables and sections.

const TABLE_GAMETEXT := "gametext"
const TABLE_MISSION := "mission"
const SECTION_OVERLAYS := "Overlays"
const SECTION_WPNAMES := "WPNames"
const SECTION_CLIENT := "Client"
const SECTION_LOADING_TEXT := "LoadingText"
const TABLE_MENUTXT := "menutxt"
const TABLE_GAMEUI := "gameui"
const TABLE_KEYHELP := "keyhelp"
const TABLE_GAMEERR := "gameerr"
const SECTION_GENERIC_ERRORS := "Generic Error Strings"
const SECTION_MENU := "Menu"


func clear() -> void:
	TextTables.clear()


func register_table(name: String, table: RtxtStringFile) -> void:
	TextTables.register_table(name, table)


func get_table(name: String) -> RtxtStringFile:
	return TextTables.get_table(name)


func load_rtxt(root: ResourceRoot, file: String) -> RtxtStringFile:
	return TextTables.load_rtxt(root, file)


func track_expansion_override(root: ResourceRoot) -> void:
	TextTables.track_expansion_override(root)


func lookup(table_name: String, section: String, key: String) -> String:
	return TextTables.lookup(table_name, section, key)


func lookup_or(table_name: String, section: String, key: String, fallback: String) -> String:
	return TextTables.lookup_or(table_name, section, key, fallback)


func has_text(table_name: String, section: String, key: String) -> bool:
	return TextTables.has_text(table_name, section, key)


func menu_text(key: String, fallback: String) -> String:
	return TextTables.menu_text(key, fallback)


func lookup_display(table_name: String, section: String, key: String) -> String:
	return TextTables.lookup_display(table_name, section, key)
