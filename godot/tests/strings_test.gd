extends GutTest

## Strings runtime model tests: the named-table registry, the override
## table consulted before every lookup, and the engine-faithful section-scoped
## lookup with its visible ??section:key?? miss marker.
##
## Mirrors the original engine's table model: four global tables loaded at init
## [orig: Game_InitSubsystems @ 0x4A6CD0], one expansion override table
## [orig: TextResource_LoadOverrideTable @ 0x75D5C0], and miss formatting from
## the error path [orig: GameErr_GetString @ 0x4C2C60].

const TMP_TABLE := "user://nova_strings_table_test.bin"


func before_each() -> void:
	Strings.clear()


func after_all() -> void:
	Strings.clear()
	if FileAccess.file_exists(TMP_TABLE):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(TMP_TABLE))


func _make_table(section: String, key: String, text: String) -> RtxtStringFile:
	var t := RtxtStringFile.new()
	t.add_section(section)
	t.add_entry(key, text, 0, Vector2i())
	return t


func test_register_and_lookup_by_table_name() -> void:
	Strings.register_table("gametext", _make_table("Item Names", "STR_ITM0001", "M4 Carbine"))
	assert_not_null(Strings.get_table("GAMETEXT"), "table names are case-insensitive")
	assert_eq(Strings.lookup("gametext", "Item Names", "str_itm0001"), "M4 Carbine")

	Strings.register_table("gametext", null)
	assert_null(Strings.get_table("gametext"), "registering null unregisters")


func test_lookup_miss_returns_engine_marker() -> void:
	Strings.register_table("gameerr", _make_table("Errors", "ERR_NET", "Network failure"))
	assert_eq(Strings.lookup("gameerr", "Errors", "NOPE"), "??Errors:NOPE??",
		"a miss must produce the original engine's visible marker")
	assert_eq(Strings.lookup("gameerr", "Wrong Section", "ERR_NET"), "??Wrong Section:ERR_NET??",
		"section scoping is part of the lookup")
	assert_eq(Strings.lookup("unregistered", "s", "k"), "??s:k??",
		"an unknown table also resolves to the miss marker")


func test_override_table_wins_before_named_tables() -> void:
	Strings.register_table("gametext", _make_table("menu", "TITLE", "Base game title"))
	Strings.set_override_table(_make_table("menu", "TITLE", "Expansion title"))
	assert_eq(Strings.lookup("gametext", "menu", "TITLE"), "Expansion title",
		"the override table is consulted first, like the expansion text bin")

	Strings.set_override_table(null)
	assert_eq(Strings.lookup("gametext", "menu", "TITLE"), "Base game title",
		"clearing the override restores base lookups")


func test_override_only_shadows_keys_it_has() -> void:
	Strings.register_table("gametext", _make_table("menu", "TITLE", "Base"))
	Strings.set_override_table(_make_table("menu", "OTHER", "Override"))
	assert_eq(Strings.lookup("gametext", "menu", "TITLE"), "Base",
		"keys missing from the override fall through to the named table")


func test_lookup_display_strips_hotkey() -> void:
	Strings.register_table("menutxt", _make_table("menu", "BTN_OK", "{hot}OK"))
	assert_eq(Strings.lookup("menutxt", "menu", "BTN_OK"), "{hot}OK", "lookup returns raw text")
	assert_eq(Strings.lookup_display("menutxt", "menu", "BTN_OK"), "OK", "display strips the marker")
	assert_eq(Strings.lookup_display("menutxt", "menu", "MISSING"), "??menu:MISSING??",
		"the miss marker is never hotkey-stripped")


func test_legacy_single_table_api_unchanged() -> void:
	var t := _make_table("menu", "KEY", "value")
	assert_eq(t.save_to_path(TMP_TABLE), OK)
	assert_eq(Strings.load_table(TMP_TABLE), OK)
	assert_true(Strings.is_loaded())
	assert_eq(Strings.get_string("key"), "value")
	assert_eq(Strings.get_string("missing", "fallback"), "fallback")
	Strings.clear()
	assert_false(Strings.is_loaded())
	assert_null(Strings.get_override_table(), "clear() also resets the registry and override")
