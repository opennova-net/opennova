extends GutTest

# Corpus render harness (display-axis proof): every shipped revx02 menu must build
# a live widget tree in-engine without crashing - the runtime counterpart to the
# C++ round-trip coverage test. Real game textures/fonts are not present, so assets
# degrade gracefully; the hard assertion is "parses + builds >= 1 screen and a
# widget tree". Per-menu widget and unresolved-asset counts are printed for
# visibility (a future builder instrumentation can turn the unresolved count into a
# per-construct skipped report).

const MENUS := [
	"jo_main", "jo_sp", "jo_mp", "jo_options", "jo_game", "jo_player",
	"jo_weapon", "jo_loadout", "jo_color", "jo_cmap", "jo_stat", "jo_death",
	"jo_vehicle", "jo_item_db", "jo_splash",
]


func _count_controls(node: Node) -> int:
	var n := 1 if node is Control else 0
	for c in node.get_children():
		n += _count_controls(c)
	return n


func test_all_shipped_menus_build() -> void:
	for menu_name in MENUS:
		var path := "res://../fixtures/mnu/%s.mnu" % menu_name
		var bytes := FileAccess.get_file_as_bytes(path)
		assert_gt(bytes.size(), 0, "%s readable" % menu_name)
		if bytes.is_empty():
			continue

		var doc := NovaMnuDocument.new()
		assert_eq(doc.load_from_bytes(bytes), OK, "%s parses" % menu_name)
		assert_gt(doc.get_screen_count(), 0, "%s has at least one screen" % menu_name)

		# Runtime build path (the running game), assets degrading gracefully.
		var menu := NovaMnuMenu.new()
		menu.build_on_ready = false
		add_child_autofree(menu)
		menu.set_edit_mode(false)
		menu.menu = doc

		var controls := _count_controls(menu)
		assert_gt(controls, doc.get_screen_count(),
				"%s built a widget tree without crashing" % menu_name)
		gut.p("  %s: %d controls, %d unresolved assets" % [
				menu_name, controls, menu.get_unresolved_asset_count()])
