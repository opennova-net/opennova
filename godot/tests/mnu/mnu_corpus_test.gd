extends GutTest

# Corpus render harness (display-axis proof): every screen of every menu our
# base game ships (assets/*.mnu, ADR 0048) and of the authored every-widget
# fixtures must compile through the engine draw walk (MenuFrame over
# MenuFrameCompiler) without crashing - the runtime counterpart to the C++
# round-trip coverage test. No textures or fonts are mounted, so assets degrade
# gracefully; the hard assertion is "parses + every screen configures and emits
# a non-empty draw list". Together the menus carry every window kind the shipped
# JO menu set uses, so each construct the shipped screens draw is drawn here.

const MENUS := [
	"res://../assets/main.mnu", "res://../assets/sp.mnu", "res://../assets/mp.mnu",
	"res://../assets/options.mnu", "res://../assets/game.mnu",
	"res://../assets/player.mnu", "res://../assets/weapon.mnu",
	"res://../assets/pre.mnu", "res://../assets/cmap.mnu", "res://../assets/stat.mnu",
	"res://../assets/death.mnu", "res://../assets/vehicle.mnu",
	"res://../fixtures/mnu/all_widgets.mnu", "res://../fixtures/mnu/widgets.mnu",
]

# The window kinds the shipped revx02 menu set (jo_main ... jo_splash, the
# reference set's mnu/jo_*.mnu) authors, every one of them.
const SHIPPED_KINDS := [
	MnuDocument.TYPE_WINDOW, MnuDocument.TYPE_STATIC, MnuDocument.TYPE_BUTTON,
	MnuDocument.TYPE_EDIT, MnuDocument.TYPE_MULTILINE_EDIT, MnuDocument.TYPE_LIST,
	MnuDocument.TYPE_CHECKBOX, MnuDocument.TYPE_RADIO, MnuDocument.TYPE_COMBO,
	MnuDocument.TYPE_SCROLL, MnuDocument.TYPE_TABLE, MnuDocument.TYPE_SPINLIST,
	MnuDocument.TYPE_MARQUEE,
]


func _collect_kinds(doc: MnuDocument, id: int, kinds: Dictionary) -> void:
	kinds[doc.get_widget_type(id)] = true
	for child in doc.get_child_ids(id):
		_collect_kinds(doc, int(child), kinds)


func test_all_menu_screens_compile() -> void:
	var kinds := {}
	for path: String in MENUS:
		var menu_name := path.get_file()
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
			_collect_kinds(doc, doc.get_screen_root_id(screen_id), kinds)
			assert_true(frame.configure(doc, screen_name, null, null, null),
					"%s/%s configures" % [menu_name, screen_name])
			var stats := frame.get_draw_list_stats()
			assert_gt(stats.widgets_drawn, 0,
					"%s/%s draws widgets" % [menu_name, screen_name])
		gut.p("  %s: %d screens, %d unresolved assets" % [
				menu_name, doc.get_screen_count(),
				frame.get_unresolved_asset_count()])
	for kind in SHIPPED_KINDS:
		assert_true(kinds.has(kind), "a drawn screen carries window kind %d" % kind)
