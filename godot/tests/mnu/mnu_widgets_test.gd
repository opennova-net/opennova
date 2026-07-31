extends GutTest

# M9 gate (text + marker widgets): the formerly-placeholder MNU widget types build
# as real, runtime-interactive Godot controls. This suite covers M9.1: Edit,
# MultilineEdit, and the invisible Goto marker, plus the shared edit_mode-inert gate
# and the aggregate NovaMnuMenu.widget_value_changed relay. List/Multi/SpinList,
# Combo/Scroll/Table and the special views are covered in their own files.

const FIXTURE := "res://../fixtures/mnu/all_widgets.mnu"
const STYLE_FIXTURE := "res://../fixtures/mns/menu_style.mns"


func _load_doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _build_menu(edit_mode: bool = false) -> NovaMnuMenu:
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)  # in-tree so built widgets _ready (wire/inert) correctly
	menu.set_edit_mode(edit_mode)
	menu.menu = _load_doc()  # set_menu rebuilds because the node is in the tree
	return menu


# --- Edit -------------------------------------------------------------------

func test_edit_builds_and_seeds_text() -> void:
	var menu := _build_menu()
	var edit := menu.find_child("NameEdit", true, false)
	assert_not_null(edit, "NameEdit built")
	assert_true(edit is NovaMnuEdit, "edit becomes a NovaMnuEdit")
	assert_true(edit is LineEdit, "NovaMnuEdit is a LineEdit")
	assert_eq((edit as LineEdit).text, "Player", "STRING text seeded into the field")
	assert_true((edit as LineEdit).editable, "editable at runtime")
	assert_eq(edit.get_hotkey(), "", "no hotkey on this field")


# --- MultilineEdit ----------------------------------------------------------

func test_multiline_readonly_flag() -> void:
	var menu := _build_menu()
	var notes := menu.find_child("Notes", true, false)
	assert_true(notes is NovaMnuMultilineEdit, "multiline_edit becomes a NovaMnuMultilineEdit")
	assert_true(notes is TextEdit, "NovaMnuMultilineEdit is a TextEdit")
	assert_eq((notes as TextEdit).text, "First line", "STRING text seeded")
	assert_true(notes.get_readonly(), "READONLY flag parsed -> not editable")
	assert_false((notes as TextEdit).editable, "readonly maps to editable=false")
	notes.set_readonly(false)
	assert_true((notes as TextEdit).editable, "shell can clear readonly at runtime")


# --- Goto -------------------------------------------------------------------

func test_goto_is_invisible_marker() -> void:
	var menu := _build_menu()
	var go := menu.find_child("QuickExit", true, false)
	assert_true(go is NovaMnuGoto, "goto becomes a NovaMnuGoto")
	assert_false(go is TextureButton, "goto is not a button")
	assert_eq(go.get_action_count(), 1, "carries its single ACTION")
	assert_eq(go.get_hotkey(), "VK_ESCAPE", "hotkey stored")
	assert_false(go.get_fire_on_show(), "has a hotkey, so not an immediate goto")
	assert_eq((go as Control).size, Vector2.ZERO, "zero-size marker")
	assert_eq((go as Control).mouse_filter, Control.MOUSE_FILTER_IGNORE, "click-through")


func test_goto_trigger_navigates() -> void:
	var menu := _build_menu()
	assert_eq(menu.current_screen, "ALL", "starts on first screen")
	watch_signals(menu)
	var go := menu.find_child("QuickExit", true, false)
	go.trigger()
	assert_eq(menu.current_screen, "TARGET", "trigger dispatched the screen action")
	assert_signal_emitted(menu, "action_dispatched")


# --- Shared edit_mode-inert gate --------------------------------------------

func test_widgets_inert_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var edit := menu.find_child("NameEdit", true, false) as LineEdit
	var notes := menu.find_child("Notes", true, false) as TextEdit
	assert_false(edit.editable, "edit non-editable while authoring")
	assert_false(notes.editable, "multiline non-editable while authoring")

	# Goto must not navigate from the inert preview canvas.
	assert_eq(menu.current_screen, "ALL", "preview on first screen")
	var go := menu.find_child("QuickExit", true, false)
	go.trigger()
	assert_eq(menu.current_screen, "ALL", "goto trigger is a no-op in edit_mode")


# --- Aggregate widget_value_changed relay -----------------------------------

func test_aggregate_value_signal_at_runtime() -> void:
	var menu := _build_menu()
	watch_signals(menu)
	var edit := menu.find_child("NameEdit", true, false) as LineEdit
	# Simulate a submit; the runtime field relays the value through the menu.
	edit.emit_signal("text_submitted", "Hello")
	assert_signal_emitted_with_parameters(
		menu, "widget_value_changed", ["NameEdit", "edit", -1, "Hello"])


func test_aggregate_value_signal_suppressed_in_edit_mode() -> void:
	var menu := _build_menu(true)
	watch_signals(menu)
	var edit := menu.find_child("NameEdit", true, false) as LineEdit
	# In edit_mode nothing is connected, so the relay never fires.
	edit.emit_signal("text_submitted", "Hello")
	assert_signal_not_emitted(menu, "widget_value_changed")


# --- List -------------------------------------------------------------------

func test_list_builds_and_seeds() -> void:
	var menu := _build_menu()
	var list := menu.find_child("MissionList", true, false)
	assert_true(list is NovaMnuList, "list becomes a NovaMnuList")
	assert_true(list is ItemList, "NovaMnuList is an ItemList")
	assert_eq((list as ItemList).select_mode, ItemList.SELECT_SINGLE, "single-select")
	assert_eq((list as ItemList).item_count, 2, "two template ITEM rows seeded")
	assert_eq((list as ItemList).get_item_text(0), "MM_Alpha", "first item (raw id, no RTXT set)")


func test_list_runtime_data_binding_and_relay() -> void:
	var menu := _build_menu()
	var list := menu.find_child("MissionList", true, false) as ItemList
	# The runtime shell replaces the template rows with live data (mission list).
	list.set_items(["Alpha", "Bravo", "Charlie"])
	assert_eq(list.item_count, 3, "set_items replaced the seed")
	list.select(1)
	assert_eq(list.get_selected_index(), 1, "get_selected_index reports selection")

	watch_signals(menu)
	list.emit_signal("item_selected", 1)  # user selection at runtime
	assert_signal_emitted_with_parameters(
		menu, "widget_value_changed", ["MissionList", "list", 1, "Bravo"])


func test_list_inert_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var list := menu.find_child("MissionList", true, false) as ItemList
	assert_eq(list.focus_mode, Control.FOCUS_NONE, "not focusable while authoring")
	watch_signals(menu)
	list.emit_signal("item_selected", 0)
	assert_signal_not_emitted(menu, "widget_value_changed")


func test_list_honors_authored_items_alignment() -> void:
	var mnu_text := "<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>" + \
		"<WINDOW type=\"list\" name=\"ALIGNED_LIST\">" + \
		"<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>210</RIGHT><BOTTOM>110</BOTTOM></POSITION>" + \
		"<ITEMS justify=\"RIGHT\" vjustify=\"TOP\"><ITEM>Authored</ITEM></ITEMS>" + \
		"</WINDOW></WINDOW></SCREEN>"
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(mnu_text.to_utf8_buffer()), OK, "synthetic aligned list parses")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.menu = doc
	var list := menu.find_child("ALIGNED_LIST", true, false)
	assert_true(list is NovaMnuList, "aligned list builds")
	assert_eq(list.get_item_horizontal_alignment(), HORIZONTAL_ALIGNMENT_RIGHT,
		"ITEMS justify controls authored and runtime row text")
	assert_eq(list.get_item_vertical_alignment(), VERTICAL_ALIGNMENT_TOP,
		"ITEMS vjustify controls authored and runtime row text")


# --- Multi ------------------------------------------------------------------

func test_multi_builds_and_multiselect() -> void:
	var menu := _build_menu()
	var multi := menu.find_child("MapPicker", true, false)
	assert_true(multi is NovaMnuMulti, "multi becomes a NovaMnuMulti")
	assert_eq((multi as ItemList).select_mode, ItemList.SELECT_MULTI, "multi-select mode")
	assert_eq((multi as ItemList).item_count, 3, "three items seeded")

	var list := multi as ItemList
	list.select(0, false)
	list.select(1, false)  # additive
	assert_eq(list.get_selected_items(), PackedInt32Array([0, 1]), "two rows selected")

	watch_signals(menu)
	list.emit_signal("multi_selected", 1, true)
	assert_signal_emitted_with_parameters(
		menu, "widget_value_changed", ["MapPicker", "multi", 1, "Jungle"])


func test_multi_honors_authored_items_alignment() -> void:
	var mnu_text := "<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>" + \
		"<WINDOW type=\"multi\" name=\"ALIGNED_MULTI\">" + \
		"<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>210</RIGHT><BOTTOM>110</BOTTOM></POSITION>" + \
		"<ITEMS justify=\"CENTER\" vjustify=\"BOTTOM\"><ITEM>Authored</ITEM></ITEMS>" + \
		"</WINDOW></WINDOW></SCREEN>"
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(mnu_text.to_utf8_buffer()), OK, "synthetic aligned multi parses")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.menu = doc
	var multi := menu.find_child("ALIGNED_MULTI", true, false)
	assert_true(multi is NovaMnuMulti, "aligned multi builds")
	assert_eq(multi.get_item_horizontal_alignment(), HORIZONTAL_ALIGNMENT_CENTER,
		"ITEMS justify controls all multi-select row text")
	assert_eq(multi.get_item_vertical_alignment(), VERTICAL_ALIGNMENT_BOTTOM,
		"ITEMS vjustify controls all multi-select row text")


func test_edit_mode_text_families_render_authored_foreground() -> void:
	# Exact ONED regression: authoring makes Edit and MultilineEdit read-only,
	# while List/Multi redraw their own aligned glyphs. Inspect the effective
	# colors those four render paths consume; this remains deterministic under
	# CI's dummy headless renderer (which has no readable framebuffer).
	var stylesheet := MnsStyleSheet.new()
	assert_eq(stylesheet.load_from_bytes(
			FileAccess.get_file_as_bytes(STYLE_FIXTURE)), OK)
	stylesheet.set_variable("DEF_TEXT_FG", "FFFF00FF") # opaque magenta, AARRGGBB

	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.set_edit_mode(true)
	menu.stylesheet = stylesheet
	menu.menu = _load_doc()
	var expected := Color(1, 0, 1, 1)
	var edit := menu.find_child("NameEdit", true, false) as LineEdit
	var notes := menu.find_child("Notes", true, false) as TextEdit
	var list := menu.find_child("MissionList", true, false) as NovaMnuList
	var multi := menu.find_child("MapPicker", true, false) as NovaMnuMulti
	assert_eq(edit.get_theme_color("font_uneditable_color"), expected,
			"Edit keeps DEFAULT_FG when authoring makes it uneditable")
	assert_eq(notes.get_theme_color("font_readonly_color"), expected,
			"MultilineEdit keeps DEFAULT_FG when authoring makes it read-only")
	assert_eq(list.get_item_text_color(0), expected,
			"List ignores the opaque-black unset custom-foreground sentinel")
	assert_eq(multi.get_item_text_color(0), expected,
			"Multi ignores the opaque-black unset custom-foreground sentinel")


func test_list_text_colors_honor_authored_font_states() -> void:
	var source := """<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">
<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>640</RIGHT><BOTTOM>480</BOTTOM></POSITION>
<FONT><DEFAULT_FG>FF00FF00</DEFAULT_FG><MOUSEOVER_FG>FFFF0000</MOUSEOVER_FG>
<SELECTED_FG>FF0000FF</SELECTED_FG><DISABLED_FG>FFFFFF00</DISABLED_FG></FONT>
<WINDOW type="list" name="COLORS"><POSITION><LEFT>0</LEFT><TOP>0</TOP>
<RIGHT>200</RIGHT><BOTTOM>100</BOTTOM></POSITION>
<ITEMS justify="LEFT"><ITEM>Row</ITEM></ITEMS></WINDOW>
<WINDOW type="edit" name="FIELD"><POSITION><LEFT>0</LEFT><TOP>110</TOP>
<RIGHT>200</RIGHT><BOTTOM>134</BOTTOM></POSITION></WINDOW>
<WINDOW type="multiline_edit" name="NOTES"><POSITION><LEFT>0</LEFT><TOP>140</TOP>
<RIGHT>200</RIGHT><BOTTOM>200</BOTTOM></POSITION></WINDOW>
</WINDOW></SCREEN>"""
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(source.to_utf8_buffer()), OK)
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.menu = doc
	var list := menu.find_child("COLORS", true, false) as NovaMnuList
	assert_eq(list.get_item_text_color(0), Color8(0, 255, 0),
			"default row text uses DEFAULT_FG")
	assert_eq(list.get_item_text_color(0, true), Color8(255, 0, 0),
			"hovered row text uses MOUSEOVER_FG")
	var custom := Color8(12, 34, 56)
	list.set_item_custom_fg_color(0, custom)
	assert_eq(list.get_item_text_color(0), custom,
			"ordinary rows honor a non-sentinel per-item foreground")
	assert_eq(list.get_item_text_color(0, true), Color8(255, 0, 0),
			"hover state takes precedence over a per-item foreground")
	list.select(0)
	assert_eq(list.get_item_text_color(0), Color8(0, 0, 255),
			"selected row text uses SELECTED_FG")
	assert_eq(list.get_item_text_color(0, true), Color8(0, 0, 255),
			"selected rows keep SELECTED_FG while hovered")
	list.set_item_disabled(0, true)
	assert_eq(list.get_item_text_color(0), Color8(255, 255, 0),
			"disabled row text uses DISABLED_FG")
	list.set_item_disabled(0, false)
	list.deselect_all()
	list.set_item_custom_fg_color(0, Color())
	assert_true(menu.handle_window_action("COLORS", "DISABLE"))
	assert_eq(list.get_item_text_color(0), Color8(255, 255, 0),
			"runtime WINDOW DISABLE switches the whole list to DISABLED_FG")
	assert_true(menu.handle_window_action("COLORS", "ENABLE"))
	assert_eq(list.get_item_text_color(0), Color8(0, 255, 0),
			"runtime WINDOW ENABLE restores the list's normal foreground")

	var field := menu.find_child("FIELD", true, false) as NovaMnuEdit
	var notes := menu.find_child("NOTES", true, false) as NovaMnuMultilineEdit
	assert_true(menu.handle_window_action("FIELD", "DISABLE"))
	assert_true(menu.handle_window_action("NOTES", "DISABLE"))
	assert_eq(field.get_theme_color("font_uneditable_color"), Color8(255, 255, 0),
			"runtime-disabled Edit uses DISABLED_FG")
	assert_eq(notes.get_theme_color("font_readonly_color"), Color8(255, 255, 0),
			"runtime-disabled MultilineEdit uses DISABLED_FG")
	assert_true(menu.handle_window_action("FIELD", "ENABLE"))
	assert_true(menu.handle_window_action("NOTES", "ENABLE"))
	assert_eq(field.get_theme_color("font_uneditable_color"), Color8(0, 255, 0),
			"re-enabled Edit restores DEFAULT_FG")
	assert_eq(notes.get_theme_color("font_readonly_color"), Color8(0, 255, 0),
			"re-enabled MultilineEdit restores DEFAULT_FG")


# --- SpinList ---------------------------------------------------------------

func test_spinlist_structure_and_values() -> void:
	var menu := _build_menu()
	var spin := menu.find_child("Difficulty", true, false)
	assert_true(spin is NovaMnuSpinList, "spinlist becomes a NovaMnuSpinList")
	assert_true(spin is Control, "NovaMnuSpinList is a Control")
	assert_eq(spin.get_value_count(), 3, "three values seeded from ITEMS")
	assert_eq(spin.get_value(), "OPTION_LOW", "starts on first value")
	assert_not_null(spin.find_child("Value", true, false), "has a Value cell mount")
	var up := spin.find_child("SpinUp", true, false)
	var down := spin.find_child("SpinDown", true, false)
	assert_true(up is NovaMnuButton, "SpinUp built from SPINUP art")
	assert_true(down is NovaMnuButton, "SpinDown built from SPINDOWN art")


func test_spinlist_cycle_wraps_and_emits() -> void:
	var menu := _build_menu()
	var spin := menu.find_child("Difficulty", true, false)
	watch_signals(menu)
	watch_signals(spin)
	spin.cycle(1)
	assert_eq(spin.get_value_index(), 1, "advanced one step")
	assert_eq(spin.get_value(), "OPTION_MED", "value follows index")
	assert_signal_emitted_with_parameters(spin, "value_changed", [1, "OPTION_MED"])
	assert_signal_emitted_with_parameters(
		menu, "widget_value_changed", ["Difficulty", "spinlist", 1, "OPTION_MED"])
	# Wrap from the last value back to the first.
	spin.set_value_index(2)
	spin.cycle(1)
	assert_eq(spin.get_value_index(), 0, "wraps past the end to the start")
	# Text items render in the cell's CellLabel (image/color items use CellImage/CellSwatch).
	var label := spin.find_child("CellLabel", true, false) as Label
	assert_eq(label.text, "OPTION_LOW", "Value cell label tracks the current value")


func test_spinlist_color_items_render_swatches() -> void:
	# type="color" items draw as a full-rect color swatch; the color is the element-text
	# hex (RRGGBB), forced opaque [orig: CSpinListWnd_Render @ 0x64b220].
	var mnu_text := "<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>" + \
		"<WINDOW type=\"spinlist\" name=\"COLOR\">" + \
		"<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>60</RIGHT><BOTTOM>30</BOTTOM></POSITION>" + \
		"<ITEMS><ITEM type=\"color\" value=\"16711680\">FF0000</ITEM>" + \
		"<ITEM type=\"color\" value=\"65280\">00FF00</ITEM></ITEMS>" + \
		"</WINDOW></WINDOW></SCREEN>"
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(mnu_text.to_utf8_buffer()), OK, "synthetic color spinlist parses")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.menu = doc
	var spin := menu.find_child("COLOR", true, false)
	assert_true(spin is NovaMnuSpinList, "color spinlist builds")
	var swatch := spin.find_child("CellSwatch", true, false) as ColorRect
	assert_not_null(swatch, "color item renders a CellSwatch")
	if swatch == null:
		return
	assert_true(swatch.visible, "swatch shown for a color item")
	assert_eq(swatch.color, Color(1, 0, 0, 1), "first color is opaque red (FF0000)")
	var cell_label := spin.find_child("CellLabel", true, false) as Label
	assert_false(cell_label.visible, "the text label is hidden for a color item")
	spin.cycle(1)
	assert_eq(swatch.color, Color(0, 1, 0, 1), "swatch follows the selection (00FF00)")


func test_spinlist_get_value_attr_returns_value_not_label() -> void:
	# The shell reads the item's `value=` attribute (the semantic value the original reads with wcstoul
	# [orig: CUISpinList_ParseXMLDefinition @0x64bd10]) to map a selection to behavior — e.g. the host
	# screen's SERVERTYPE spinlist: HG_SERVEPLAY=0 (serve-and-play) vs HG_SERVEONLY=1 (dedicated). This
	# is distinct from get_value()'s localized DISPLAY text. (MpMenuCompanion._is_dedicated reads the attr.)
	var mnu_text := "<SCREEN><NAME>S</NAME><WINDOW type=\"window\" name=\"ROOT\">" + \
		"<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>" + \
		"<WINDOW type=\"spinlist\" name=\"SERVERTYPE\">" + \
		"<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>60</RIGHT><BOTTOM>30</BOTTOM></POSITION>" + \
		"<ITEMS><ITEM type=\"id\" value=\"0\">HG_SERVEPLAY</ITEM>" + \
		"<ITEM type=\"id\" value=\"1\">HG_SERVEONLY</ITEM></ITEMS>" + \
		"</WINDOW></WINDOW></SCREEN>"
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(mnu_text.to_utf8_buffer()), OK, "synthetic SERVERTYPE spinlist parses")
	var menu := NovaMnuMenu.new()
	menu.build_on_ready = false
	add_child_autofree(menu)
	menu.menu = doc
	var spin := menu.find_child("SERVERTYPE", true, false)
	assert_true(spin is NovaMnuSpinList, "spinlist builds")
	# get_value() is the display text; get_value_attr() is the `value=` attribute.
	assert_eq(spin.get_value(), "HG_SERVEPLAY", "get_value() starts on the first item's display text")
	assert_eq(spin.get_value_attr(), "0", "get_value_attr() returns the first item's value (serve-and-play)")
	spin.cycle(1)
	assert_eq(spin.get_value(), "HG_SERVEONLY", "get_value() advances to the second item's text")
	assert_eq(spin.get_value_attr(), "1", "get_value_attr() follows the selection (dedicated)")


func test_spinlist_button_press_cycles_at_runtime() -> void:
	var menu := _build_menu()
	var spin := menu.find_child("Difficulty", true, false)
	var up := spin.find_child("SpinUp", true, false)
	up.emit_signal("pressed")  # the spinlist wires the buttons at runtime
	assert_eq(spin.get_value_index(), 1, "SpinUp advanced the value")


func test_spinlist_inert_in_edit_mode() -> void:
	var menu := _build_menu(true)
	var spin := menu.find_child("Difficulty", true, false)
	var up := spin.find_child("SpinUp", true, false)
	assert_true((up as BaseButton).disabled, "spin button disabled while authoring")
	up.emit_signal("pressed")
	assert_eq(spin.get_value_index(), 0, "button not wired in edit_mode -> no cycle")
