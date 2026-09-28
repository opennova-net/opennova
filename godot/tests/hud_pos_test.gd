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
