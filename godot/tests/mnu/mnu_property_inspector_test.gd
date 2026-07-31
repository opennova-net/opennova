extends GutTest

# Phase 5: the property inspector's deep string integration. For a widget whose text
# is a string-table key (type="id"), the inspector resolves the display text, flags an
# unknown key, lets the user pick a key from the table (committing through the normal
# edit path), and offers one-click jumps to the Strings / Fonts workspaces. Also covers
# the standalone MnuStringPicker filtering + pick.

const MnuPropertyInspectorScript = preload("res://modtools/mnu/mnu_property_inspector.gd")
const MnuStringPickerScript = preload("res://modtools/mnu/mnu_string_picker.gd")
const FIXTURE := "res://../fixtures/mnu/widgets.mnu"


# A string table with two known entries (keys uppercased to stay deterministic).
func _make_table() -> RtxtStringFile:
	var t := RtxtStringFile.new()
	t.reset_empty()
	var sec := t.add_section("default")
	t.add_entry("ALPHA", "Alpha", sec, Vector2i())
	t.add_entry("BRAVO", "Bravo", sec, Vector2i())
	return t


# A fixture document with one extra string-id widget under MAIN's root. Returns
# [doc, widget_id].
func _doc_with_id_widget(key: String) -> Array:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var w := doc.add_widget(root, NovaMnuDocument.TYPE_STATIC, Rect2(10, 10, 100, 30))
	doc.set_widget_string_type(w, "id")
	doc.set_widget_text(w, key)
	return [doc, w]


func _all_text(node: Node) -> String:
	var out := ""
	if node is Label:
		out += (node as Label).text + "\n"
	elif node is LineEdit:
		out += (node as LineEdit).text + "\n"
	for c in node.get_children():
		out += _all_text(c)
	return out


func _find_button(node: Node, text: String) -> Button:
	if node is Button and (node as Button).text == text:
		return node
	for c in node.get_children():
		var found := _find_button(c, text)
		if found != null:
			return found
	return null


func _find_row_button_with_text(node: Node, row_text: String, button_text: String) -> Button:
	if node is HBoxContainer:
		var has_row_text := false
		var button: Button = null
		for child in node.get_children():
			if _subtree_has_line_text(child, row_text):
				has_row_text = true
			elif child is Button and (child as Button).text == button_text:
				button = child
		if has_row_text and button != null:
			return button
	for c in node.get_children():
		var found := _find_row_button_with_text(c, row_text, button_text)
		if found != null:
			return found
	return null


func _subtree_has_line_text(node: Node, text: String) -> bool:
	if node is LineEdit and (node as LineEdit).text == text:
		return true
	for child in node.get_children():
		if _subtree_has_line_text(child, text):
			return true
	return false


func _inspector_for(doc: NovaMnuDocument, id: int, text_res: RtxtStringFile):
	var inspector = MnuPropertyInspectorScript.new()
	add_child_autofree(inspector)
	await get_tree().process_frame
	inspector.show_widget(doc, id, text_res)
	await get_tree().process_frame
	return inspector


func test_inspector_shows_resolved_text_for_string_id() -> void:
	var arr := _doc_with_id_widget("ALPHA")
	var inspector = await _inspector_for(arr[0], arr[1], _make_table())
	assert_string_contains(_all_text(inspector), "Alpha",
		"The resolved display text is shown for a string id.")


func test_inspector_flags_unresolved_key() -> void:
	var arr := _doc_with_id_widget("MISSING_KEY")
	var inspector = await _inspector_for(arr[0], arr[1], _make_table())
	assert_string_contains(_all_text(inspector), "Not in string table",
		"An unknown string id is flagged.")


func _text_ref_widget(inspector: Node) -> StringRefWidget:
	return inspector.find_child("MnuWidgetTextRef", true, false) as StringRefWidget


func test_inspector_string_helpers_hidden_without_table() -> void:
	# No table loaded (no resource root): the key widget degrades to a plain
	# field - no badge, picker, jump, or resolved preview.
	var arr := _doc_with_id_widget("ALPHA")
	var inspector = await _inspector_for(arr[0], arr[1], null)
	var widget := _text_ref_widget(inspector)
	assert_not_null(widget, "The string-id text row is the link widget.")
	assert_false(widget.ref_row.badge.visible, "The badge is hidden when no string table is loaded.")
	assert_false(widget.ref_row.browse_button.visible, "The picker is hidden when no string table is loaded.")
	assert_false(widget.ref_row.jump_button.visible, "The jump is hidden when no string table is loaded.")
	assert_false(widget.preview.visible, "No resolved line without a table.")
	assert_eq(widget.get_value(), "ALPHA", "The raw key stays editable.")


func test_inspector_string_jump_emits_key() -> void:
	var arr := _doc_with_id_widget("ALPHA")
	var inspector = await _inspector_for(arr[0], arr[1], _make_table())
	var widget := _text_ref_widget(inspector)
	assert_not_null(widget, "The string-id text row is the link widget.")
	assert_true(widget.ref_row.jump_button.visible, "The Strings jump is offered for a string id.")
	watch_signals(inspector)
	widget.ref_row.jump_button.pressed.emit()
	assert_signal_emitted(inspector, "string_jump_requested", "Pressing jumps to Strings.")
	assert_eq(get_signal_parameters(inspector, "string_jump_requested", 0)[0], "ALPHA",
		"The jump carries the widget's string id.")


func test_inspector_pick_commits_text_edit() -> void:
	var arr := _doc_with_id_widget("ALPHA")
	var inspector = await _inspector_for(arr[0], arr[1], _make_table())
	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))
	var widget := _text_ref_widget(inspector)
	# Browse routes through the pick service into the real picker; drive the
	# pick result directly (the popup itself is covered by the picker test).
	widget.ref_row.browse_button.pressed.emit()
	inspector._on_string_picked("BRAVO")
	assert_eq(captured.size(), 1, "Picking commits exactly one edit.")
	if captured.size() == 1:
		var e: Dictionary = captured[0]
		assert_eq(e.get("prop"), "text", "The edit sets the widget text.")
		assert_eq(e.get("value"), "BRAVO", "The edit carries the chosen key.")
		assert_eq(e.get("id"), arr[1], "The edit targets the selected widget.")
	assert_eq(widget.get_value(), "BRAVO", "The widget shows the picked key without a rebuild.")


func test_screen_text_rsrc_is_a_link_widget() -> void:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	var screen_id: int = doc.get_screen_ids()[0]
	var inspector = await _inspector_for(doc, screen_id, null)
	var widget := inspector.find_child("MnuScreenTextRsrc", true, false) as ResourceRefWidget
	assert_not_null(widget, "The screen's Text resource row is a file link widget.")
	assert_eq(widget.get_value(), doc.get_screen_text_rsrc(screen_id), "The row reads the screen value.")
	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))
	widget.name_edit.text = "other.BIN"
	widget.name_edit.text_submitted.emit("other.BIN")
	assert_eq(captured.size(), 1, "Editing the table name commits one edit.")
	if captured.size() == 1:
		assert_eq(captured[0].get("prop"), "text_rsrc", "The edit targets the screen's text_rsrc.")


func test_inspector_font_jump_emits_font() -> void:
	var arr := _doc_with_id_widget("ALPHA")
	var doc: NovaMnuDocument = arr[0]
	var w: int = arr[1]
	doc.set_widget_font(w, "Gunpl22b.fnt")
	var inspector = await _inspector_for(doc, w, null)
	var btn := _find_button(inspector, "Open in Fonts")
	assert_not_null(btn, "The Open in Fonts button shows when a font is set.")
	watch_signals(inspector)
	btn.pressed.emit()
	assert_signal_emitted(inspector, "font_jump_requested", "Pressing jumps to Fonts.")
	assert_eq(get_signal_parameters(inspector, "font_jump_requested", 0)[0], "Gunpl22b",
		"The jump carries the font's bare name — the .fnt is stripped (intentional, #156) so the Fonts workspace name resolution lands.")


# --- Sounds section ------------------------------------------------------------

# widgets.mnu MAIN root children: [Title, StartBtn, SoundChk, Difficulty, Version].
# StartBtn carries a MOUSE_OVER sound; Title has none.
func _widgets_doc() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	doc.load_from_bytes(FileAccess.get_file_as_bytes(FIXTURE))
	return doc


func _main_child(doc: NovaMnuDocument, index: int) -> int:
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	return doc.get_child_ids(root)[index]


func _find_option_with_item(node: Node, item_text: String) -> OptionButton:
	if node is OptionButton:
		var opt := node as OptionButton
		for i in range(opt.item_count):
			if opt.get_item_text(i) == item_text:
				return opt
	for c in node.get_children():
		var found := _find_option_with_item(c, item_text)
		if found != null:
			return found
	return null


func _find_check_box(node: Node, text: String) -> CheckBox:
	if node is CheckBox and (node as CheckBox).text == text:
		return node
	for c in node.get_children():
		var found := _find_check_box(c, text)
		if found != null:
			return found
	return null


func _find_color_picker(node: Node) -> ColorPickerButton:
	if node is ColorPickerButton:
		return node as ColorPickerButton
	for c in node.get_children():
		var found := _find_color_picker(c)
		if found != null:
			return found
	return null


func test_widget_color_has_picker_and_preserves_raw_token_until_changed() -> void:
	var doc := _widgets_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	assert_true(doc.get_widget_color(root, NovaMnuDocument.COLOR_DEFAULT_FG).begins_with("%"),
		"fixture starts with a raw style-variable color")
	var inspector = await _inspector_for(doc, root, null)
	var picker := _find_color_picker(inspector)
	assert_not_null(picker, "literal widget colors have a purpose-built color picker")
	if picker == null:
		return
	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))

	# Looking at a picker must not silently materialize its preview over %VAR%.
	picker.popup_closed.emit()
	assert_eq(captured.size(), 0, "closing an untouched picker preserves the raw style token")

	picker.color_changed.emit(Color(1, 0, 0, 1))
	assert_eq(captured.size(), 0, "dragging previews locally instead of creating undo spam")
	picker.popup_closed.emit()
	assert_eq(captured.size(), 1, "the completed color gesture commits once")
	if captured.size() == 1:
		assert_eq(captured[0].get("prop"), "color", "picker edits the typed color slot")
		assert_eq(captured[0].get("value"), "FFFF0000", "picker emits retail AARRGGBB")


func test_inspector_shows_sound_rows() -> void:
	var doc := _widgets_doc()
	var inspector = await _inspector_for(doc, _main_child(doc, 1), null)  # StartBtn
	var text := _all_text(inspector)
	assert_string_contains(text, "Sounds", "the Sounds section heading shows")
	assert_string_contains(text, "MOUSE_OVER", "the hover trigger shows")
	assert_string_contains(text, "menu.lwf", "the sound file shows")


func test_inspector_action_screen_target_is_pickable() -> void:
	var doc := _widgets_doc()
	var inspector = await _inspector_for(doc, _main_child(doc, 1), null)  # StartBtn -> OPTIONS
	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))
	var opt := _find_option_with_item(inspector, "MAIN")
	assert_not_null(opt, "same-menu screen action target is a picker over screen names")
	if opt == null:
		return
	var idx := -1
	for i in range(opt.item_count):
		if opt.get_item_text(i) == "MAIN":
			idx = i
	assert_gte(idx, 0, "MAIN is a selectable screen target")
	opt.select(idx)
	opt.item_selected.emit(idx)
	assert_eq(captured.size(), 1, "changing the target commits one edit")
	if captured.size() == 1:
		assert_eq(captured[0].get("prop"), "actions", "the edit targets the action list")
		var actions: Array = captured[0].get("value")
		assert_eq(String(actions[0]["target"]), "MAIN", "the action target changes to the picked screen")


func test_inspector_url_action_external_browser_is_editable() -> void:
	var doc := _widgets_doc()
	var start := _main_child(doc, 1)
	doc.set_widget_actions(start, [{
		"type": "url",
		"target": "www.novalogic.com",
		"state": "",
		"file": "",
		"external_browser": false,
	}])
	var inspector = await _inspector_for(doc, start, null)
	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))
	var external := _find_check_box(inspector, "External browser")
	assert_not_null(external, "URL actions expose the preserved EXTERNAL_BROWSER flag")
	if external == null:
		return
	external.button_pressed = true  # emits toggled
	assert_eq(captured.size(), 1, "toggling EXTERNAL_BROWSER commits one action edit")
	if captured.size() == 1:
		assert_eq(captured[0].get("prop"), "actions", "the edit targets the action list")
		var actions: Array = captured[0].get("value")
		assert_true(bool(actions[0].get("external_browser", false)), "the flag is set in the emitted row")


func test_inspector_url_action_controls_fit_side_panel() -> void:
	var doc := _widgets_doc()
	var start := _main_child(doc, 1)
	doc.set_widget_actions(start, [{
		"type": "url",
		"target": "www.novalogic.com",
		"state": "",
		"file": "",
		"external_browser": true,
	}])

	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(260, 600)
	panel.size = Vector2(260, 600)
	add_child_autofree(panel)
	var inspector = MnuPropertyInspectorScript.new()
	panel.add_child(inspector)
	await get_tree().process_frame
	inspector.show_widget(doc, start, null)
	await get_tree().process_frame
	await get_tree().process_frame

	var external := _find_check_box(inspector, "External browser")
	assert_not_null(external, "URL action checkbox is rendered in the side panel")
	if external == null:
		return
	var panel_rect := panel.get_global_rect()
	var row := external.get_parent()
	assert_true(row is Control, "URL action controls live in a measurable row")
	if not (row is Control):
		return
	for child in row.get_children():
		if child is Control and (child as Control).visible:
			var child_rect := (child as Control).get_global_rect()
			assert_lte(child_rect.position.x + child_rect.size.x, panel_rect.position.x + panel_rect.size.x,
				"URL action row control '%s' stays inside the inspector side panel" % child.name)


func test_inspector_cross_menu_action_jump_emits_menu_target() -> void:
	var doc := _widgets_doc()
	var start := _main_child(doc, 1)
	doc.set_widget_actions(start, [{
		"type": "screen",
		"target": "SINGLE_PLAYER",
		"state": "",
		"file": "sp.mnu",
		"external_browser": false,
	}])
	var inspector = await _inspector_for(doc, start, null)
	var jump := _find_button(inspector, "Open menu")
	assert_not_null(jump, "cross-menu screen actions expose an Open menu jump")
	if jump == null:
		return
	watch_signals(inspector)
	jump.pressed.emit()
	assert_signal_emitted(inspector, "menu_jump_requested", "pressing jumps to the target menu")
	var args: Array = get_signal_parameters(inspector, "menu_jump_requested", 0)
	assert_eq(args, ["sp.mnu", "SINGLE_PLAYER"], "the jump carries the menu file and target screen")


func test_inspector_add_sound_emits_sounds_prop() -> void:
	var doc := _widgets_doc()
	var title := _main_child(doc, 0)  # Title: a static with no sounds
	var inspector = await _inspector_for(doc, title, null)
	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))
	var add_btn := _find_button(inspector, "Add sound")
	assert_not_null(add_btn, "an Add sound button is present")
	add_btn.pressed.emit()
	assert_eq(captured.size(), 1, "adding commits exactly one edit")
	if captured.size() == 1:
		assert_eq(captured[0].get("prop"), "sounds", "the edit targets the sounds list")
		assert_eq((captured[0].get("value") as Array).size(), 1, "one sound was appended")


func test_inspector_remove_sound_emits_shorter_list() -> void:
	var doc := _widgets_doc()
	var inspector = await _inspector_for(doc, _main_child(doc, 1), null)  # StartBtn (1 sound)
	var captured: Array = []
	inspector.edit_requested.connect(func(e: Dictionary) -> void: captured.append(e))
	var rm := _find_row_button_with_text(inspector, "menu.lwf", "✕")
	assert_not_null(rm, "a remove button is present for the existing sound")
	rm.pressed.emit()
	assert_eq(captured.size(), 1, "removing commits one edit")
	if captured.size() == 1:
		assert_eq(captured[0].get("prop"), "sounds", "the edit targets the sounds list")
		assert_eq((captured[0].get("value") as Array).size(), 0, "the sound was removed")


func test_combo_exposes_closed_and_dropdown_items_separately() -> void:
	var src := """
<SCREEN><NAME>MAIN</NAME><WINDOW type="window" name="ROOT">
  <WINDOW type="combo" name="Mode">
    <ITEMS><ITEM value="closed">Closed</ITEM></ITEMS>
    <LIST_BOX><ITEMS><ITEM value="popup">Popup</ITEM></ITEMS></LIST_BOX>
  </WINDOW>
</WINDOW></SCREEN>
"""
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(src.to_utf8_buffer()), OK)
	var root := int(doc.get_screen_root_id(doc.get_screen_ids()[0]))
	var combo := int(doc.get_child_ids(root)[0])
	var inspector = await _inspector_for(doc, combo, null)
	var text := _all_text(inspector)
	assert_true(text.contains("Closed / fallback items"),
		"top-level combo ITEMS has a clearly labeled independent editor")
	assert_true(text.contains("Dropdown items"),
		"LIST_BOX/ITEMS has its own clearly labeled editor")
	assert_true(text.contains("Closed"))
	assert_true(text.contains("Popup"))


func test_nested_rows_are_labeled_cards_with_color_pickers() -> void:
	var src := """
<SCREEN><NAME>MAIN</NAME><WINDOW type="window" name="ROOT">
  <WINDOW type="button" name="Styled">
    <APPEARANCE state="default" type="color" map_state="0" height="20">112233</APPEARANCE>
  </WINDOW>
</WINDOW></SCREEN>
"""
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(src.to_utf8_buffer()), OK)
	var root := int(doc.get_screen_root_id(doc.get_screen_ids()[0]))
	var styled := int(doc.get_child_ids(root)[0])
	var inspector = await _inspector_for(doc, styled, null)
	var text := _all_text(inspector)
	for label in ["Row 1", "State", "Type", "Picture / color", "Use map",
			"Map", "Use height", "Height"]:
		assert_true(text.contains(label), "nested rows label '%s'" % label)
	var picker := inspector.find_child("MnuNestedColorPicker", true, false)
	assert_not_null(picker, "nested color/outline rows keep a real picker")


func test_inspector_trigger_dropdown_from_profile_sets() -> void:
	# When the workspace supplies the profile's set names, the trigger field becomes
	# a dropdown over them (so the author picks a real trigger, not free text).
	var doc := _widgets_doc()
	var inspector = await _inspector_for(doc, _main_child(doc, 1), null)  # StartBtn
	inspector.set_sound_sets(PackedStringArray(["MOUSE_OVER", "CLICK_SELECT", "CLICK_VALUE"]))
	await get_tree().process_frame
	var opt := _find_option_with_item(inspector, "CLICK_SELECT")
	assert_not_null(opt, "the trigger field is a dropdown over the profile's sets")


func test_inspector_sound_preview_emits_request() -> void:
	var doc := _widgets_doc()
	var inspector = await _inspector_for(doc, _main_child(doc, 1), null)  # StartBtn
	watch_signals(inspector)
	var play := _find_button(inspector, "▶")
	assert_not_null(play, "a preview button is present for the existing sound")
	play.pressed.emit()
	assert_signal_emitted(inspector, "sound_preview_requested", "preview asks the workspace to play")
	assert_eq(get_signal_parameters(inspector, "sound_preview_requested", 0)[0], "MOUSE_OVER",
		"the preview carries the sound's trigger")


func test_string_picker_filters_and_picks() -> void:
	var picker = MnuStringPickerScript.new()
	add_child_autofree(picker)
	await get_tree().process_frame
	picker.set_table(_make_table(), "")
	assert_eq(picker.row_count(), 2, "All keys are listed initially.")
	picker.filter("brav")
	assert_eq(picker.row_count(), 1, "Filter narrows by key/text (case-insensitive).")
	assert_eq(picker.key_at(0), "BRAVO", "The remaining row is the match.")
	watch_signals(picker)
	picker.choose(0)
	assert_signal_emitted(picker, "picked", "Choosing a row emits picked.")
	assert_eq(get_signal_parameters(picker, "picked", 0)[0], "BRAVO", "picked carries the chosen key.")
