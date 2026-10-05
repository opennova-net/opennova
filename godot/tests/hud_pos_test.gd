extends GutTest

# Exercises the HudPos GDExtension binding over engine/formats/def hudpos.def
# parsing: the load contract, the design-space constants and the VEHICLE_HUD
# block handoff. The parsed values themselves are pinned by the
# def_parse_hudpos and hud_layout ctests against the shipped fixture; this binding test uses an authored block.


func _load() -> HudPos:
	var hud := HudPos.new()
	var abs := DefFixture.directory().path_join("hudpos.def")
	var err := hud.load(abs)
	assert_eq(err, OK, "HudPos.load should parse the hudpos.def fixture.")
	return hud


func test_loads_and_reports_state() -> void:
	var hud := _load()
	assert_true(hud.is_loaded(), "Fixture should report loaded.")
	assert_eq(hud.get_last_error(), "", "No error after a clean load.")


func test_design_space_constants() -> void:
	assert_eq(HudPos.DESIGN_WIDTH, 1024, "Witnessed HUD design width.")
	assert_eq(HudPos.DESIGN_HEIGHT, 768, "Witnessed HUD design height.")


func test_not_loaded_is_safe() -> void:
	var hud := HudPos.new()
	assert_false(hud.is_loaded(), "Fresh instance is not loaded.")
	assert_null(hud.get_vehicle_hud("test_vehicle"), "An unloaded layout hands out no block.")


# The VEHICLE_HUD block the vehicle panel presenter takes by items.def sid.
func test_vehicle_hud_block() -> void:
	var hud := _load()
	var block := hud.get_vehicle_hud("test_vehicle")
	assert_not_null(block, "The fixture vehicle's VEHICLE_HUD block resolves by sid.")
	if block != null:
		assert_eq(block.interface_texture, "fixture_panel.tga")
	assert_null(hud.get_vehicle_hud("no_such_sid"), "An unknown sid resolves to no block.")


# Two blocks naming one sid: the later one is the item's, as each VEHICLE_HUD
# commit walks every item and overwrites its panel; a block with no sid is the
# panel of an item whose alias is empty (an item a nested `begin` or the
# file's end closed) [orig: HUD_ParseHudposToken's VEHICLE_END commit, the
# _stricmp of each item's alias against the block sid; hud-re VEHICLE_HUD].
func test_vehicle_hud_last_block_and_empty_sid() -> void:
	var dir := TestFs.cache_dir(self, "hudpos_vehicle_blocks")
	var path := dir.path_join("hudpos.def")
	TestFs.write_text(self, path, "\n".join(PackedStringArray([
		"VEHICLE_HUD",
		"  sid dup_vehicle",
		"  interface first.tga",
		"VEHICLE_END",
		"VEHICLE_HUD",
		"  sid dup_vehicle",
		"  interface second.tga",
		"VEHICLE_END",
		"VEHICLE_HUD",
		"  interface nameless.tga",
		"VEHICLE_END",
	])))
	var hud := HudPos.new()
	assert_eq(hud.load(path), OK, "the two-block layout loads")
	var dup := hud.get_vehicle_hud("dup_vehicle")
	assert_not_null(dup, "a repeated sid resolves")
	if dup != null:
		assert_eq(dup.interface_texture, "second.tga", "the later block wins")
	var nameless := hud.get_vehicle_hud("")
	assert_not_null(nameless, "an empty sid resolves to the sid-less block")
	if nameless != null:
		assert_eq(nameless.interface_texture, "nameless.tga")
	TestFs.remove_dir_recursive(dir)
