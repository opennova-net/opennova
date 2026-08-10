extends GutTest

# Corpus render harness (display-axis proof): every screen of every shipped
# revx02 menu must compile through the engine draw walk (MenuFrame over
# MenuFrameCompiler) without crashing - the runtime counterpart to the C++
# round-trip coverage test. Real game textures/fonts are not present, so assets
# degrade gracefully; the hard assertion is "parses + every screen configures
# and emits a non-empty draw list".

const MENUS := [
	"jo_main", "jo_sp", "jo_mp", "jo_options", "jo_game", "jo_player",
	"jo_weapon", "jo_loadout", "jo_color", "jo_cmap", "jo_stat", "jo_death",
	"jo_vehicle", "jo_item_db", "jo_splash",
]


func test_all_shipped_menu_screens_compile() -> void:
	for menu_name in MENUS:
		var path := "res://../fixtures/mnu/%s.mnu" % menu_name
		var bytes := FileAccess.get_file_as_bytes(path)
		assert_gt(bytes.size(), 0, "%s readable" % menu_name)
		if bytes.is_empty():
			continue

		var doc := MnuDocument.new()
		assert_eq(doc.load_from_bytes(bytes), OK, "%s parses" % menu_name)
		assert_gt(doc.get_screen_count(), 0, "%s has at least one screen" % menu_name)

		var frame := MenuFrame.new()
		add_child_autofree(frame)
		frame.size = Vector2(800, 600)
		for screen_id in doc.get_screen_ids():
			var screen_name := doc.get_screen_name(screen_id)
			assert_true(frame.configure(doc, screen_name, null, null, {}),
					"%s/%s configures" % [menu_name, screen_name])
			var stats: Dictionary = frame.get_draw_list_stats()
			assert_gt(int(stats.get("widgets_drawn", 0)), 0,
					"%s/%s draws widgets" % [menu_name, screen_name])
		gut.p("  %s: %d screens, %d unresolved assets" % [
				menu_name, doc.get_screen_count(),
				frame.get_unresolved_asset_count()])
