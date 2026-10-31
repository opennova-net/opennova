extends GutTest

## The Strings autoload over the engine's named string tables (TextTables): a registered table
## reads back as the object registered, a table the registry holds alone comes back as a binding
## over it, and clear() forgets the tables, the override table and the binding labels' table. The
## lookup rules themselves are the engine's (ctest rtxt_text_tables).


func before_each() -> void:
	Strings.clear()


func after_all() -> void:
	Strings.clear()


func _make_table(section: String, key: String, text: String) -> RtxtStringFile:
	var t := RtxtStringFile.new()
	t.add_section(section)
	t.add_entry(key, text, 0, Vector2i())
	return t


func test_a_registered_table_reads_back_as_itself() -> void:
	var table := _make_table("Item Names", "STR_ITM0001", "M4 Carbine")
	Strings.register_table("gametext", table)
	assert_same(Strings.get_table("GAMETEXT"), table, "names match without case")
	assert_eq(Strings.lookup("gametext", "Item Names", "str_itm0001"), "M4 Carbine")
	Strings.register_table("gametext", null)
	assert_null(Strings.get_table("gametext"), "registering null unregisters")


## An edit through the registered binding is the registry's table: the engine reads the same one.
func test_edits_reach_the_engine_registry() -> void:
	var table := _make_table("Overlays", "STROVER_A", "Before")
	Strings.register_table("gametext", table)
	table.set_entry_text(0, "After")
	assert_eq(Strings.lookup_or("gametext", "Overlays", "STROVER_A", "fb"), "After")


func test_clear_forgets_the_tables_and_the_override() -> void:
	Strings.register_table("gametext", _make_table("menu", "TITLE", "Base"))
	_make_table("menu", "OV", "Override").install_text_override()
	Strings.register_table(Strings.TABLE_KEYHELP, _make_table("Keys", "SPACE", "Space"))
	assert_true(RtxtStringFile.has_key_strings(), "keyhelp's registration installs the labels")
	Strings.clear()
	assert_null(Strings.get_table("gametext"))
	assert_eq(Strings.lookup("gametext", "menu", "OV"), "??menu:OV??",
		"clear() forgets the installed override")
	assert_false(RtxtStringFile.has_key_strings(), "clear() forgets the binding labels")
