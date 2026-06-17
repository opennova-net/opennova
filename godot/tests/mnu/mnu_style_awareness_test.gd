extends GutTest

# Stylesheet awareness in the Menus workspace: the AARRGGBB color helper fix,
# %VAR% swatch resolution through the loaded stylesheet, the "%" variable
# dropdowns that write tokens (preserved on save per ADR 0005), the "Edit
# style" jump, the resolved Fonts jump, and the unresolved-variable count the
# status bar surfaces.

const MnuPropertyInspectorScript = preload("res://modtools/mnu/mnu_property_inspector.gd")
const MnuEditorScript = preload("res://modtools/mnu/mnu_editor.gd")
const MnuEditorDocumentScript = preload("res://modtools/mnu/mnu_editor_document.gd")
const MnuUiHelpersScript = preload("res://modtools/mnu/mnu_ui_helpers.gd")

# widgets.mnu's root window authors FONT NAME=%DEF_FONTNAME% and
# DEFAULT_FG=%DEF_TEXT_FG% - real token-valued fields to inspect.
const FIXTURE := "res://../fixtures/mnu/widgets.mnu"


func _sheet() -> MnsStyleSheet:
	var sheet := MnsStyleSheet.new()
	sheet.set_source_text("DEF_FONTNAME\tGunpl22b.fnt\nDEF_TEXT_FG\tFFFFFFFF\nTRIM_COLOR\tFF3870BF\nDEF_IMAGE_DEFAULT_BG\tJO_EPIL.TGA\n")
	return sheet


func _doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _root_window(doc: NovaMnuDocument) -> int:
	return doc.get_screen_root_id(doc.get_screen_ids()[0])


# An inspector showing the fixture's root window with the stylesheet loaded.
func _inspector(doc: NovaMnuDocument, id: int, sheet: MnsStyleSheet) -> MnuPropertyInspector:
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	inspector.set_stylesheet(sheet)
	inspector.show_widget(doc, id)
	return inspector


func _find_button(node: Node, text: String) -> Button:
	if node is Button and not (node is MenuButton) and (node as Button).text == text:
		return node
	for child in node.get_children():
		var found := _find_button(child, text)
		if found != null:
			return found
	return null


func _find_menu_buttons(node: Node, out: Array) -> void:
	if node is MenuButton:
		out.append(node)
	for child in node.get_children():
		_find_menu_buttons(child, out)


# MNU colors are AARRGGBB; Color.html reads RRGGBBAA - the old helper rendered
# every 8-digit swatch wrong (FFFF0000 = opaque red came out transparent yellow).
func test_color_helper_parses_aarrggbb() -> void:
	var red = MnuUiHelpersScript.color_from_mnu("FFFF0000")
	assert_not_null(red, "8-digit hex parses")
	assert_almost_eq(red.a, 1.0, 0.005, "FF alpha is opaque")
	assert_almost_eq(red.r, 1.0, 0.005, "FF red channel")
	assert_almost_eq(red.g, 0.0, 0.005, "00 green channel")

	var selected = MnuUiHelpersScript.color_from_mnu("7f1E6DF3")
	assert_almost_eq(selected.a, 127.0 / 255.0, 0.005, "7f alpha is half-transparent")
	assert_almost_eq(selected.r, 30.0 / 255.0, 0.005, "1E red")
	assert_almost_eq(selected.b, 243.0 / 255.0, 0.005, "F3 blue")

	var rgb = MnuUiHelpersScript.color_from_mnu("FF8000")
	assert_almost_eq(rgb.a, 1.0, 0.005, "6-digit hex is opaque")
	assert_almost_eq(rgb.g, 128.0 / 255.0, 0.005, "80 green")

	assert_null(MnuUiHelpersScript.color_from_mnu("%TRIM_COLOR%"), "a bare token stays unresolved")
	assert_null(MnuUiHelpersScript.color_from_mnu("not-a-color"), "junk stays unresolved")


func test_color_to_mnu_round_trip() -> void:
	assert_eq(MnuUiHelpersScript.color_to_mnu(Color(1, 0, 0, 1)), "FFFF0000", "8-digit by default")
	assert_eq(MnuUiHelpersScript.color_to_mnu(Color(1, 0, 0, 1), false), "FF0000",
		"force_alpha=false keeps 6-digit-authored opaque values 6-digit")
	assert_eq(MnuUiHelpersScript.color_to_mnu(Color(0, 0, 0, 0.5), false), "80000000",
		"translucency always carries the alpha pair")
	var parsed = MnuUiHelpersScript.color_from_mnu(MnuUiHelpersScript.color_to_mnu(Color(0.2, 0.4, 0.6, 0.8)))
	assert_almost_eq(parsed.b, 0.6, 0.005, "format -> parse round-trips")


func test_swatch_resolves_variable_through_stylesheet() -> void:
	var swatch := ColorRect.new()
	add_child_autofree(swatch)
	MnuUiHelpersScript.refresh_swatch(swatch, "%TRIM_COLOR%", _sheet())
	assert_almost_eq(swatch.color.a, 1.0, 0.005, "the token resolved to its themed color")
	assert_almost_eq(swatch.color.b, 0xBF / 255.0, 0.005, "FF3870BF blue channel")
	MnuUiHelpersScript.refresh_swatch(swatch, "%TRIM_COLOR%", null)
	assert_almost_eq(swatch.color.a, 0.0, 0.005, "no stylesheet -> unresolved (transparent)")


func test_var_menu_lists_type_matching_variables_and_writes_token() -> void:
	var doc := _doc()
	var id := _root_window(doc)
	var inspector := _inspector(doc, id, _sheet())

	var menus: Array = []
	_find_menu_buttons(inspector, menus)
	assert_gt(menus.size(), 0, "the %% dropdown renders for stylesheet-typed rows")

	# The color row's menu offers only color-typed variables.
	var color_menu: MenuButton = null
	for menu in menus:
		var items := PackedStringArray()
		for i in (menu as MenuButton).get_popup().item_count:
			items.append((menu as MenuButton).get_popup().get_item_text(i))
		var joined := ", ".join(items)
		if joined.contains("TRIM_COLOR"):
			color_menu = menu
			assert_string_contains(joined, "DEF_TEXT_FG", "color menu lists color variables")
			assert_false(joined.contains("DEF_FONTNAME"), "color menu excludes font variables")
			break
	assert_not_null(color_menu, "a color row carries the variable menu")

	# Picking a variable writes the %TOKEN% through the normal edit path.
	watch_signals(inspector)
	var popup := color_menu.get_popup()
	var trim_index := -1
	for i in popup.item_count:
		if popup.get_item_text(i).contains("TRIM_COLOR"):
			trim_index = popup.get_item_id(i)
	popup.id_pressed.emit(trim_index)
	assert_signal_emitted(inspector, "edit_requested", "picking a variable commits an edit")
	var edit: Dictionary = get_signal_parameters(inspector, "edit_requested")[0]
	assert_eq(String(edit.get("value", "")), "%TRIM_COLOR%", "the edit carries the token, not the literal")
	assert_eq(String(edit.get("prop", "")), "color", "the edit targets the color slot")


func test_style_jump_signal_carries_variable() -> void:
	var doc := _doc()
	var inspector := _inspector(doc, _root_window(doc), _sheet())
	# Tree order puts the font row's jump first; its token is %DEF_FONTNAME%.
	var jump := _find_button(inspector, "Edit style")
	assert_not_null(jump, "a token-valued row offers the Menu Styles jump")
	watch_signals(inspector)
	jump.pressed.emit()
	assert_signal_emitted_with_parameters(inspector, "style_jump_requested", ["DEF_FONTNAME"])


func test_font_jump_resolves_variable_through_stylesheet() -> void:
	var doc := _doc()
	var inspector := _inspector(doc, _root_window(doc), _sheet())
	var jump := _find_button(inspector, "Open in Fonts")
	assert_not_null(jump, "the font row offers the Fonts jump")
	watch_signals(inspector)
	jump.pressed.emit()
	# %DEF_FONTNAME% -> Gunpl22b.fnt -> basename for the Fonts name resolution.
	assert_signal_emitted_with_parameters(inspector, "font_jump_requested", ["Gunpl22b"])


func test_unresolved_var_count_tracks_stylesheet() -> void:
	var editor = MnuEditorScript.new()
	add_child_autofree(editor)
	var document = MnuEditorDocumentScript.new()
	document.open_mnu(FIXTURE)
	editor.set_document(document)
	# No resource root -> no stylesheet: both fixture tokens are unresolved.
	assert_eq(editor.get_unresolved_var_count(), 2,
		"%DEF_FONTNAME% and %DEF_TEXT_FG% count once each without a stylesheet")
