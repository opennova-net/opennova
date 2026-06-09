extends GutTest

# M2 gate: load a fixture .mnu into NovaMnuDocument, assert the parsed tree, and
# verify a serialize round-trip plus the core mutation surface.

const FIXTURE := "res://../fixtures/mnu/widgets.mnu"
const JO_FIXTURE := "res://../fixtures/mnu/jo_main.mnu"


func _load_doc() -> NovaMnuDocument:
	# Read bytes directly so the test does not depend on the resource cache.
	var bytes := FileAccess.get_file_as_bytes(FIXTURE)
	assert_gt(bytes.size(), 0, "fixture bytes are non-empty")
	var doc := NovaMnuDocument.new()
	var err := doc.load_from_bytes(bytes)
	assert_eq(err, OK, "load_from_bytes succeeds")
	return doc


func test_menu_size_derived_from_content() -> void:
	# The design canvas is computed from the authored window extents on load, not
	# left at the 640x480 default. The hand-authored widgets.mnu is a 640x480 menu;
	# a real Joint Operations menu is 800x600. Deriving this is what lets the editor
	# preview letterbox-fit the menu to its pane instead of overflowing a fixed
	# 640x480 box (regression guard for the "preview doesn't fit" fix).
	var widgets := _load_doc()
	assert_eq(widgets.get_menu_size(), Vector2i(640, 480),
		"640x480 menu derives its own size")

	var jo_bytes := FileAccess.get_file_as_bytes(JO_FIXTURE)
	assert_gt(jo_bytes.size(), 0, "jo_main fixture bytes are non-empty")
	var jo := NovaMnuDocument.new()
	assert_eq(jo.load_from_bytes(jo_bytes), OK, "jo_main loads")
	assert_eq(jo.get_menu_size(), Vector2i(801, 600),
		"real JO menu derives its 800x600 canvas, not the 640x480 default")


func test_resource_loader_returns_document() -> void:
	var res := load(FIXTURE)
	assert_not_null(res, "loader returns non-null for " + FIXTURE)
	assert_true(res is NovaMnuDocument, "loader returns NovaMnuDocument")


func test_screen_structure() -> void:
	var doc := _load_doc()
	assert_eq(doc.get_screen_count(), 2, "two screens")
	var screens := doc.get_screen_ids()
	assert_eq(screens.size(), 2, "two screen ids")

	var main_id: int = screens[0]
	assert_eq(doc.get_screen_name(main_id), "MAIN", "screen 0 name")
	assert_eq(doc.get_screen_music_var(main_id), 3, "screen 0 music_var")
	assert_eq(doc.get_screen_text_rsrc(main_id), "menutxt.BIN", "screen 0 text_rsrc")
	assert_true(doc.is_screen(main_id), "screen id is a screen")


func test_root_window_and_children() -> void:
	var doc := _load_doc()
	var main_id: int = doc.get_screen_ids()[0]
	var root_id := doc.get_screen_root_id(main_id)
	assert_gt(root_id, 0, "root id valid")
	assert_eq(doc.get_widget_name(root_id), "ROOT", "root window name")
	assert_eq(doc.get_widget_type(root_id), NovaMnuDocument.TYPE_WINDOW, "root is a window")
	assert_eq(doc.get_parent_id(root_id), main_id, "root's parent is its screen")

	var children := doc.get_child_ids(root_id)
	assert_eq(children.size(), 5, "root has 5 child widgets")

	var names := []
	var types := []
	for cid in children:
		names.append(doc.get_widget_name(cid))
		types.append(doc.get_widget_type(cid))
	assert_eq(names, ["Title", "StartBtn", "SoundChk", "Difficulty", "Version"], "child names in order")
	assert_eq(types[0], NovaMnuDocument.TYPE_STATIC, "Title is static")
	assert_eq(types[1], NovaMnuDocument.TYPE_BUTTON, "StartBtn is button")
	assert_eq(types[2], NovaMnuDocument.TYPE_CHECKBOX, "SoundChk is checkbox")
	assert_eq(types[3], NovaMnuDocument.TYPE_SPINLIST, "Difficulty is spinlist")
	assert_eq(types[4], NovaMnuDocument.TYPE_LABEL, "Version is label")


func test_widget_rect_and_properties() -> void:
	var doc := _load_doc()
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id: int = doc.get_child_ids(root_id)[1]  # StartBtn

	var rect := doc.get_window_rect(start_id)
	assert_eq(rect, Rect2(270, 120, 100, 30), "StartBtn rect")
	assert_eq(doc.get_widget_text(start_id), "MM_Start", "StartBtn string value")
	assert_eq(doc.get_widget_string_type(start_id), "id", "StartBtn string is an id ref")
	assert_eq(doc.get_widget_texture(start_id, NovaMnuDocument.TEX_DEFAULT), "btn_up.tga", "default appearance")
	assert_eq(doc.get_widget_texture(start_id, NovaMnuDocument.TEX_MOUSEOVER), "btn_over.tga", "mouseover appearance")
	assert_eq(doc.get_widget_font(root_id), "%DEF_FONTNAME%", "font %VAR% preserved raw")
	assert_eq(doc.get_widget_color(root_id, NovaMnuDocument.COLOR_DEFAULT_FG), "%DEF_TEXT_FG%", "color %VAR% preserved raw")

	var chk_id: int = doc.get_child_ids(root_id)[2]  # SoundChk
	assert_true((doc.get_widget_flags(chk_id) & NovaMnuDocument.FLAG_CHECKED) != 0, "SoundChk is checked")


func test_serialize_roundtrip() -> void:
	var doc := _load_doc()
	var bytes := doc.to_byte_array()
	assert_gt(bytes.size(), 0, "serialize produces bytes")

	var doc2 := NovaMnuDocument.new()
	assert_eq(doc2.load_from_bytes(bytes), OK, "re-parse serialized bytes")
	assert_eq(doc2.get_screen_count(), 2, "round-trip keeps 2 screens")

	var root2 := doc2.get_screen_root_id(doc2.get_screen_ids()[0])
	assert_eq(doc2.get_child_ids(root2).size(), 5, "round-trip keeps 5 children")
	assert_eq(doc2.get_screen_music_var(doc2.get_screen_ids()[0]), 3, "round-trip keeps music_var")


func test_mutation_emits_changed_and_persists() -> void:
	var doc := _load_doc()
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var start_id: int = doc.get_child_ids(root_id)[1]

	watch_signals(doc)
	doc.set_widget_name(start_id, "PlayBtn")
	doc.set_window_rect(start_id, Rect2(10, 20, 80, 24))
	assert_signal_emitted(doc, "changed", "mutations emit Resource.changed")
	assert_eq(doc.get_widget_name(start_id), "PlayBtn", "name mutated")
	assert_eq(doc.get_window_rect(start_id), Rect2(10, 20, 80, 24), "rect mutated")

	# The id stays stable across a serialize round-trip is NOT guaranteed (ids are
	# per-instance), but the mutation must survive serialize -> parse by content.
	var doc2 := NovaMnuDocument.new()
	doc2.load_from_bytes(doc.to_byte_array())
	var root2 := doc2.get_screen_root_id(doc2.get_screen_ids()[0])
	assert_eq(doc2.get_widget_name(doc2.get_child_ids(root2)[1]), "PlayBtn", "renamed widget persisted")


func test_add_and_delete_widget() -> void:
	var doc := _load_doc()
	var root_id := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var before := doc.get_child_ids(root_id).size()

	var new_id := doc.add_widget(root_id, NovaMnuDocument.TYPE_BUTTON, Rect2(0, 0, 50, 20))
	assert_gt(new_id, 0, "add_widget returns a valid id")
	assert_eq(doc.get_child_ids(root_id).size(), before + 1, "child added")
	assert_eq(doc.get_parent_id(new_id), root_id, "new widget parented to root")
	assert_eq(doc.get_widget_type(new_id), NovaMnuDocument.TYPE_BUTTON, "new widget is a button")

	doc.delete_widget(new_id)
	assert_eq(doc.get_child_ids(root_id).size(), before, "child removed")
	assert_false(doc.widget_exists(new_id), "deleted id no longer exists")


func test_create_empty() -> void:
	var doc := NovaMnuDocument.new()
	doc.create_empty()
	assert_eq(doc.get_screen_count(), 1, "empty doc has one screen")
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	assert_eq(doc.get_widget_type(root), NovaMnuDocument.TYPE_WINDOW, "empty root is a window")


# --- M8b: snapshot (capture/apply) + reparent ----------------------------------

# Full pre-order id walk (screen id, then its root-window subtree depth-first) —
# the order rebuild_ids/collect_ids use, so it is stable for a given tree shape.
func _all_ids(doc: NovaMnuDocument) -> Array:
	var out: Array = []
	for sid in doc.get_screen_ids():
		out.append(sid)
		_walk_ids(doc, doc.get_screen_root_id(sid), out)
	return out


func _walk_ids(doc: NovaMnuDocument, id: int, out: Array) -> void:
	out.append(id)
	for c in doc.get_child_ids(id):
		_walk_ids(doc, c, out)


func test_capture_apply_preserves_ids() -> void:
	var doc := _load_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var before_ids := _all_ids(doc)
	var state := doc.capture_state()

	# Mutate the shape (advance next_id + change the tree) then restore.
	doc.add_widget(root, NovaMnuDocument.TYPE_BUTTON, Rect2(0, 0, 10, 10))
	doc.delete_widget(doc.get_child_ids(root)[0])  # delete Title
	doc.apply_state(state)

	assert_eq(_all_ids(doc), before_ids, "apply_state restores the exact id walk")
	# Root id is byte-identical, so the same lookups still hold.
	assert_eq(doc.get_widget_name(doc.get_child_ids(root)[1]), "StartBtn", "restored content")
	assert_eq(doc.get_child_ids(root).size(), 5, "restored child count")


func test_apply_state_restores_next_id_no_collision() -> void:
	var doc := _load_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var state := doc.capture_state()
	var first_add := doc.add_widget(root, NovaMnuDocument.TYPE_BUTTON, Rect2(0, 0, 10, 10))

	doc.apply_state(state)
	var second_add := doc.add_widget(root, NovaMnuDocument.TYPE_BUTTON, Rect2(0, 0, 10, 10))
	assert_eq(second_add, first_add, "an add after restore mints the same next id (deterministic, no collision)")
	# The restored ids do not contain the minted id before the second add.
	assert_eq(doc.get_child_ids(root).size(), 6, "exactly one widget added after restore")


func test_reparent_widget_basic_and_index() -> void:
	var doc := _load_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var children := doc.get_child_ids(root)
	var title_id: int = children[0]
	var start_id: int = children[1]

	assert_true(doc.reparent_widget(start_id, title_id, 0), "reparent StartBtn under Title")
	assert_eq(doc.get_parent_id(start_id), title_id, "StartBtn now parented to Title")
	assert_eq(doc.get_child_ids(title_id), PackedInt32Array([start_id]), "Title holds StartBtn")
	assert_eq(doc.get_child_ids(root).size(), 4, "root lost a child")
	assert_eq(doc.get_widget_name(start_id), "StartBtn", "moved subtree keeps its id + name")


func test_reparent_refuses_screen_root_and_cycle() -> void:
	var doc := _load_doc()
	var main_id: int = doc.get_screen_ids()[0]
	var root := doc.get_screen_root_id(main_id)
	var children := doc.get_child_ids(root)
	var start_id: int = children[1]

	assert_false(doc.reparent_widget(main_id, root, 0), "cannot reparent a screen")
	assert_false(doc.reparent_widget(root, children[0], 0), "cannot reparent a root window")
	assert_false(doc.reparent_widget(start_id, start_id, 0), "cannot reparent into self")

	var grand := doc.add_widget(start_id, NovaMnuDocument.TYPE_WINDOW, Rect2(0, 0, 10, 10))
	assert_false(doc.reparent_widget(start_id, grand, 0), "cannot reparent into own descendant (cycle)")
	assert_eq(doc.get_parent_id(start_id), root, "refused cycle leaves the parent unchanged")


func test_reparent_reorder_within_same_parent() -> void:
	var doc := _load_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var version: int = doc.get_child_ids(root)[4]  # last child

	assert_true(doc.reparent_widget(version, root, 0), "reorder Version to the front")
	assert_eq(doc.get_child_ids(root)[0], version, "Version moved to index 0")
	assert_eq(doc.get_child_ids(root).size(), 5, "same-parent reorder keeps every child")

	# Forward move (index 0 -> 3): erase shifts the vector, so the insert index is
	# adjusted down by one and the node lands at index 2.
	assert_true(doc.reparent_widget(version, root, 3), "move Version forward")
	assert_eq(doc.get_child_ids(root).find(version), 2, "forward same-parent move lands at the shifted index")


func test_reparent_noop_same_slot_returns_false() -> void:
	var doc := _load_doc()
	var root := doc.get_screen_root_id(doc.get_screen_ids()[0])
	var children := doc.get_child_ids(root)
	var last: int = children[children.size() - 1]  # Version (last child)

	# Dropping the last child back onto its parent (append index) lands it in the
	# same slot: a no-op, reported as false so the caller records no undo entry.
	assert_false(doc.reparent_widget(last, root, children.size()), "A reparent to the same slot is a no-op (false).")
	assert_eq(doc.get_child_ids(root), children, "The no-op reparent leaves the tree unchanged.")


# M9.7: datasource (marquee) + orientation (scroll/marquee) scalar accessors
# round-trip through the document and survive a serialize cycle.
func _walk_for_name(doc: NovaMnuDocument, id: int, wname: String) -> int:
	if doc.get_widget_name(id) == wname:
		return id
	for cid in doc.get_child_ids(id):
		var f := _walk_for_name(doc, cid, wname)
		if f != -1:
			return f
	return -1


func _find_widget(doc: NovaMnuDocument, wname: String) -> int:
	for sid in doc.get_screen_ids():
		var f := _walk_for_name(doc, doc.get_screen_root_id(sid), wname)
		if f != -1:
			return f
	return -1


func test_datasource_orientation_round_trip() -> void:
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(
		"res://../fixtures/mnu/all_widgets.mnu")), OK, "all_widgets loads")
	var marquee := _find_widget(doc, "Credits")
	var scroll := _find_widget(doc, "VolumeBar")
	assert_eq(doc.get_widget_type(marquee), NovaMnuDocument.TYPE_MARQUEE, "Credits is a marquee")
	assert_eq(doc.get_widget_datasource(marquee), "credits.txt", "parsed DATASOURCE")
	assert_eq(doc.get_widget_orientation(marquee), "VERTICAL", "parsed marquee ORIENTATION")
	assert_eq(doc.get_widget_orientation(scroll), "VERTICAL", "parsed scroll ORIENTATION")

	doc.set_widget_datasource(marquee, "newcredits.txt")
	doc.set_widget_orientation(scroll, "HORIZONTAL")
	var doc2 := NovaMnuDocument.new()
	doc2.load_from_bytes(doc.to_byte_array())
	assert_eq(doc2.get_widget_datasource(_find_widget(doc2, "Credits")), "newcredits.txt",
		"datasource survives serialize")
	assert_eq(doc2.get_widget_orientation(_find_widget(doc2, "VolumeBar")), "HORIZONTAL",
		"orientation survives serialize")


func test_mns_stylesheet() -> void:
	var bytes := FileAccess.get_file_as_bytes("res://../fixtures/mns/test_style.mns")
	assert_gt(bytes.size(), 0, "mns fixture non-empty")
	var sheet := MnsStyleSheet.new()
	assert_eq(sheet.load_from_bytes(bytes), OK, "mns parses")
	assert_eq(sheet.get_variable("DEF_FONTNAME"), "Gunpl22b.fnt", "variable lookup")
	assert_eq(sheet.get_variable("def_fontname"), "Gunpl22b.fnt", "lookup is case-insensitive")
	assert_eq(sheet.substitute("font is %DEF_FONTNAME% ok"), "font is Gunpl22b.fnt ok", "substitution")
	assert_eq(sheet.substitute("%UNKNOWN%"), "%UNKNOWN%", "unknown var left as-is")


# --- M10.1: item-row authoring (list / multi / spinlist / combo) ----------------

func _load_all_widgets() -> NovaMnuDocument:
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(
		"res://../fixtures/mnu/all_widgets.mnu")), OK, "all_widgets loads")
	return doc


func test_item_accessors_read_list() -> void:
	var doc := _load_all_widgets()
	var list := _find_widget(doc, "MissionList")
	assert_eq(doc.get_item_count(list), 2, "MissionList has 2 items")
	assert_eq(doc.get_item(list, 0), {"type": "id", "value": "0", "text": "MM_Alpha"}, "first row")
	assert_eq(doc.get_item(list, 1), {"type": "id", "value": "1", "text": "MM_Bravo"}, "second row")
	assert_eq(doc.get_items(list).size(), 2, "get_items returns both rows")
	# Out-of-range reads are empty, never a crash.
	assert_eq(doc.get_item(list, 5), {}, "out-of-range row is empty")


func test_item_accessors_non_list_widget() -> void:
	var doc := _load_all_widgets()
	var edit := _find_widget(doc, "NameEdit")
	assert_eq(doc.get_item_count(edit), 0, "a non-list widget has no items")
	assert_eq(doc.add_item(edit, {"text": "x"}), -1, "add_item on a non-list widget is rejected")


func test_item_add_move_remove_round_trip() -> void:
	var doc := _load_all_widgets()
	var list := _find_widget(doc, "MissionList")

	watch_signals(doc)
	var idx := doc.add_item(list, {"type": "id", "value": "2", "text": "MM_Charlie"})
	assert_eq(idx, 2, "new row appended at index 2")
	assert_signal_emitted(doc, "changed", "add_item emits changed")
	assert_eq(doc.get_item_count(list), 3, "list now has 3 rows")

	# Move the new row to the front; the others shift down.
	doc.move_item(list, 2, 0)
	assert_eq(doc.get_item(list, 0)["text"], "MM_Charlie", "moved row is first")
	assert_eq(doc.get_item(list, 1)["text"], "MM_Alpha", "MM_Alpha shifted down")

	# Edit a cell, preserving the typed field.
	doc.set_item(list, 1, {"type": "id", "value": "9", "text": "MM_Alpha2"})
	assert_eq(doc.get_item(list, 1), {"type": "id", "value": "9", "text": "MM_Alpha2"}, "cell edited")

	# Serialize -> reload: the row-bearing <ITEMS> must survive (the List serializer
	# previously split rows and selection into two <ITEMS> and dropped the rows).
	var doc2 := NovaMnuDocument.new()
	assert_eq(doc2.load_from_bytes(doc.to_byte_array()), OK, "re-parse serialized bytes")
	var list2 := _find_widget(doc2, "MissionList")
	assert_eq(doc2.get_item_count(list2), 3, "rows survive the round-trip")
	assert_eq(doc2.get_item(list2, 0)["text"], "MM_Charlie", "order preserved across round-trip")
	assert_eq(doc2.get_item(list2, 1), {"type": "id", "value": "9", "text": "MM_Alpha2"},
		"typed + value fields preserved verbatim")

	doc.remove_item(list, 0)
	assert_eq(doc.get_item_count(list), 2, "remove_item drops a row")
	assert_eq(doc.get_item(list, 0)["text"], "MM_Alpha2", "remaining rows reindex")


func test_item_combo_uses_list_box_and_round_trips() -> void:
	var doc := _load_all_widgets()
	var combo := _find_widget(doc, "ServerList")
	assert_eq(doc.get_widget_type(combo), NovaMnuDocument.TYPE_COMBO, "ServerList is a combo")
	assert_eq(doc.get_item_count(combo), 3, "combo reads its LIST_BOX rows")
	assert_eq(doc.get_item(combo, 2)["text"], "LAN Server", "third LIST_BOX row")

	doc.add_item(combo, {"value": "3", "text": "Co-op Server"})
	assert_eq(doc.get_item_count(combo), 4, "row added to the combo")

	var doc2 := NovaMnuDocument.new()
	assert_eq(doc2.load_from_bytes(doc.to_byte_array()), OK, "combo round-trip re-parses")
	var combo2 := _find_widget(doc2, "ServerList")
	assert_eq(doc2.get_item_count(combo2), 4, "combo LIST_BOX rows survive the round-trip")
	assert_eq(doc2.get_item(combo2, 3)["text"], "Co-op Server", "added combo row persisted")


# A combo can hold its rows in a top-level <ITEMS> while its <LIST_BOX> carries
# only styling (a present but item-less list box). The active container must be
# the window's own items (matching the runtime builder, which reads list_box.items
# only when non-empty), not the empty list box.
func test_item_combo_top_level_items_with_empty_list_box() -> void:
	var src := """
<SCREEN><NAME>S</NAME><WINDOW type="window" name="ROOT">
  <WINDOW type="combo" name="ModeBox">
    <ITEMS justify="LEFT">
      <ITEM value="0">Solo</ITEM>
      <ITEM value="1">Team</ITEM>
    </ITEMS>
    <LIST_BOX>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>40</BOTTOM></POSITION>
      <APPEARANCE state="default" type="color">101820</APPEARANCE>
    </LIST_BOX>
  </WINDOW>
</WINDOW></SCREEN>
"""
	var doc := NovaMnuDocument.new()
	assert_eq(doc.load_from_bytes(src.to_utf8_buffer()), OK, "inline combo parses")
	var combo := _find_widget(doc, "ModeBox")
	assert_eq(doc.get_item_count(combo), 2, "reads the top-level ITEMS, not the empty LIST_BOX")
	assert_eq(doc.get_item(combo, 1)["text"], "Team", "second top-level row")

	doc.add_item(combo, {"value": "2", "text": "Co-op"})
	var doc2 := NovaMnuDocument.new()
	assert_eq(doc2.load_from_bytes(doc.to_byte_array()), OK, "edited combo re-parses")
	var combo2 := _find_widget(doc2, "ModeBox")
	assert_eq(doc2.get_item_count(combo2), 3, "the added row survives the round-trip")
	assert_eq(doc2.get_item(combo2, 2)["text"], "Co-op", "added row content persisted")


func test_widget_sounds_read_edit_roundtrip() -> void:
	# StartBtn carries <SOUND state="mousein" trigger="MOUSE_OVER">menu.lwf</SOUND>.
	var doc := _load_doc()
	var start := _find_widget(doc, "StartBtn")
	var sounds := doc.get_widget_sounds(start)
	assert_eq(sounds.size(), 1, "StartBtn has one authored sound")
	assert_eq(String(sounds[0]["trigger"]), "MOUSE_OVER", "hover trigger read")
	assert_eq(String(sounds[0]["file"]), "menu.lwf", "sound file read")
	assert_eq(String(sounds[0]["state"]), "mousein", "sound state read")

	# Append a click sound and replace the whole list.
	sounds.append({"state": "selected", "trigger": "CLICK_SELECT", "file": "menu.lwf"})
	doc.set_widget_sounds(start, sounds)
	assert_eq(doc.get_widget_sounds(start).size(), 2, "click sound added in-place")

	# Both survive a serialize round-trip.
	var doc2 := NovaMnuDocument.new()
	assert_eq(doc2.load_from_bytes(doc.to_byte_array()), OK, "edited menu re-parses")
	var start2 := _find_widget(doc2, "StartBtn")
	var sounds2 := doc2.get_widget_sounds(start2)
	assert_eq(sounds2.size(), 2, "both sounds persisted through the round-trip")
	assert_eq(String(sounds2[1]["trigger"]), "CLICK_SELECT", "added click trigger persisted")
