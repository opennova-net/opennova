extends GutTest

# Exercises the NovaHudPos GDExtension binding over libs/def hudpos.def parsing.
# Values mirror tests/def/def_parse_hudpos_test.cpp against fixtures/def/hudpos.def.

const HUDPOS_PATH := "res://../fixtures/def/hudpos.def"


func _load() -> NovaHudPos:
	var hud := NovaHudPos.new()
	var abs := ProjectSettings.globalize_path(HUDPOS_PATH)
	var err := hud.load(abs)
	assert_eq(err, OK, "NovaHudPos.load should parse the hudpos.def fixture.")
	return hud


func test_loads_and_reports_state() -> void:
	var hud := _load()
	assert_true(hud.is_loaded(), "Fixture should report loaded.")
	assert_eq(hud.get_last_error(), "", "No error after a clean load.")


func test_design_space_constants() -> void:
	assert_eq(NovaHudPos.DESIGN_WIDTH, 1024, "Witnessed HUD design width.")
	assert_eq(NovaHudPos.DESIGN_HEIGHT, 768, "Witnessed HUD design height.")


func test_health_rect_from_corners() -> void:
	var hud := _load()
	# health[4] = {2,739,141,757} corners -> Rect2i(x1,y1,x2-x1,y2-y1).
	assert_eq(hud.get_health_rect(), Rect2i(2, 739, 139, 18), "Health rect from corners.")


func test_colors_normalized() -> void:
	var hud := _load()
	var colors := hud.get_colors()
	assert_true(colors.has("hud_textcolor"), "Colors dict exposes hud_textcolor.")
	var c: Color = colors["hud_textcolor"]
	# hud_textcolor 251,213,5 -> normalized.
	assert_almost_eq(c.r, 251.0 / 255.0, 0.005, "hud_textcolor red normalized.")
	assert_almost_eq(c.g, 213.0 / 255.0, 0.005, "hud_textcolor green normalized.")
	assert_almost_eq(c.b, 5.0 / 255.0, 0.005, "hud_textcolor blue normalized.")


func test_spinmap_bounds() -> void:
	var hud := _load()
	# spinmap x1=810,x2=1020,y1=552,y2=762 -> Rect2i(810,552,210,210).
	assert_eq(hud.get_spinmap_bounds(), Rect2i(810, 552, 210, 210), "Spinmap bounds rect.")


func test_stances() -> void:
	var hud := _load()
	var stances := hud.get_stances()
	assert_eq(stances.size(), 5, "Fixture defines 5 HUDSTANCE frames.")
	if stances.size() > 0:
		var s0: Dictionary = stances[0]
		assert_eq(int(s0["id"]), 0, "First stance id.")
		assert_eq(String(s0["texture"]), "stance_1.tga", "First stance texture.")
		assert_eq(String(s0["name"]), "STAND", "First stance name.")
		assert_true(s0["offset"] is Vector2i, "Stance offset is Vector2i.")


func test_to_dictionary_shape() -> void:
	var hud := _load()
	var d := hud.to_dictionary()
	for key in ["fonts", "rects", "positions", "colors", "spinmap", "stances", "static_frames"]:
		assert_true(d.has(key), "to_dictionary exposes '%s'." % key)
	var rects: Dictionary = d["rects"]
	assert_eq(rects["health"], Rect2i(2, 739, 139, 18), "to_dictionary health rect matches getter.")
	var positions: Dictionary = d["positions"]
	assert_true(positions["ammo_count"] is Vector4i, "[4] positions are Vector4i (x,y,hidden,align).")
	assert_true(positions["stance"] is Vector2i, "[2] positions are Vector2i.")
	# GAMEINFO 1013,430 (2 fields) -> hidden 0, align left.
	assert_eq(positions["game_info"], Vector4i(1013, 430, 0, 0), "game_info carries x,y,hidden,align.")
	var misc: Dictionary = d["misc"]
	# ALPHAFADE 30 50 3 raw file fields (base %, max %, seconds) — floats, since the
	# original's atof keeps fractions for the x2.55/x62 converts [orig: @0x5a0882].
	assert_eq(misc["alpha_fade"], Vector3(30, 50, 3), "alpha_fade raw triple exposed in misc.")


func test_not_loaded_is_safe() -> void:
	var hud := NovaHudPos.new()
	assert_false(hud.is_loaded(), "Fresh instance is not loaded.")
	assert_eq(hud.get_health_rect(), Rect2i(), "Unloaded getters return empty.")
	assert_eq(hud.get_stances().size(), 0, "Unloaded stances empty.")
	assert_eq(hud.to_dictionary().size(), 0, "Unloaded to_dictionary empty.")
