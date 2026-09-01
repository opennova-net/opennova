class_name MenuDriver
extends RefCounted

const MenuFrameStateReplay := preload("res://game/menu_frame_state_replay.gd")
const MenuScrollRange := preload("res://game/menu_scroll_range.gd")
const MenuTableState := preload("res://game/menu_table_state.gd")

# The compiled-menu interaction runtime: drives ONE MenuFrame (the engine
# draw-list/pump surface) over a parsed MnuDocument. The engine owns
# everything witnessed (engine/runtime/menu; record: docs/mnu/menu-re.md);
# this driver is the shell-side orchestration: navigation + the back stack,
# ACTION dispatch, popup/scroll/table lifecycle, sound edges, the music-var
# push, and the value-changed relay — its signal surface mirrors the deleted
# MnuMenu node. Widgets go by stable MnuDocument id, valid across screens;
# per-id runtime state is replayed at each screen configure.

signal screen_changed(screen_name: String)
signal music_changed(music_var: int)
signal menu_requested(file: String, target_screen: String)
signal quit_requested()
signal url_requested(url: String)
signal sound_requested(file: String, trigger: String)
signal action_dispatched(type: String, target: String)
signal shell_action_requested(type: String, action: Dictionary)
signal widget_value_changed(widget_name: String, kind: String, index: int, value: String)
# Click-level activation of a widget (button/goto/checkbox/radio...) — the
# named-control seam the shell and companions wire launch/quit policy to.
signal widget_activated(id: int, widget_name: String)
# List double-click (the ItemList item_activated equivalent).
signal list_activated(id: int, row: int)
# The pump's claim moved between widgets (hover edges; PLAYER_PREVIEW zoom).
signal widget_hover_changed(id: int, hovered: bool)

# Double-click window for list/table activation, matching Godot's default.
const DOUBLE_CLICK_MS := 400

var _frame: MenuFrame
var _audio: MenuAudio
var _doc: MnuDocument
var _root: ResourceRoot
var _style: MnsStyleSheet
var _text: RtxtStringFile
var _menu_file := ""
var _music_director: MusicDirector = null
var _music_var_index := 0

var _current_screen := ""
var _nav_stack: PackedStringArray = []

# Per-document caches.
var _screen_ids: Dictionary = {}      # screen name (upper) -> screen id
var _screen_order: PackedStringArray = []
var _name_to_id: Dictionary = {}      # widget NAME (upper) -> doc id (first)
var _id_info: Dictionary = {}         # doc id -> {screen:String, name:String, kind:int}
# Runtime widget state keyed by doc id, replayed onto the frame at configure.
var _id_state: Dictionary = {}
# Current screen's id<->pre-order-index maps.
var _id_of_index: PackedInt64Array = []
var _index_of_id: Dictionary = {}
# Per-screen RTXT text tables (TEXT_RSRC), cached by lowercased filename.
var _text_rsrc_cache: Dictionary = {}
# CBIN credits scrollers mounted over marquee widgets (menu_credits_overlays.gd
# owns the mounts + the hidden-tab visibility gate).
var _credits := MenuCreditsOverlays.new()

var _focus_id := -1        # keyboard/edit focus [orig: g_ui_focus_wnd @ 0x31C16D4]
var _open_combo_id := -1   # single open dropdown [orig: g_ui_active_combo_wnd @ 0x31C16D0]
var _last_claim := -1
var _last_mouse := Vector2.ZERO
var _mouse_down := false
var _last_click_id := -1
var _last_click_row := -1
var _last_click_ms := 0


func attach(frame: MenuFrame, audio: MenuAudio) -> void:
	_frame = frame
	_audio = audio
	if not _frame.widget_clicked.is_connected(_on_frame_widget_clicked):
		_frame.widget_clicked.connect(_on_frame_widget_clicked)
	if not _frame.scroll_value_changed.is_connected(_on_frame_scroll_value):
		_frame.scroll_value_changed.connect(_on_frame_scroll_value)


func set_music_director(director: MusicDirector) -> void:
	_music_director = director


func set_music_var_index(index: int) -> void:
	_music_var_index = index


func get_frame() -> MenuFrame:
	return _frame


func get_menu_file() -> String:
	return _menu_file


func get_current_screen() -> String:
	return _current_screen


# --- Document / screen lifecycle ----------------------------------------------

# Bind a parsed document and show `target_screen` (empty = the first screen).
# Rebuilds every per-document cache; runtime widget state is dropped (the
# Control tree rebuilt from scratch here too).
func open_document(doc: MnuDocument, root: ResourceRoot, style: MnsStyleSheet,
		text: RtxtStringFile, menu_file: String, target_screen := "") -> bool:
	_doc = doc
	_root = root
	_style = style
	_text = text
	_menu_file = menu_file
	_id_state.clear()
	_name_to_id.clear()
	_id_info.clear()
	_screen_ids.clear()
	_screen_order = []
	_text_rsrc_cache.clear()
	_nav_stack = []
	_focus_id = -1
	_open_combo_id = -1
	_last_claim = -1
	if _doc == null:
		return false
	for screen_id in _doc.get_screen_ids():
		var name := _doc.get_screen_name(screen_id)
		_screen_ids[name.to_upper()] = screen_id
		_screen_order.append(name)
		_index_document_screen(name, int(screen_id))
	if _screen_order.is_empty():
		return false
	var initial := target_screen
	if initial.is_empty() or not _screen_ids.has(initial.to_upper()):
		initial = _screen_order[0]
	return show_screen(initial)


func _index_document_screen(screen_name: String, screen_id: int) -> void:
	var root_id := _doc.get_screen_root_id(screen_id)
	_index_widget_subtree(screen_name, root_id)


func _index_widget_subtree(screen_name: String, id: int) -> void:
	var name := _doc.get_widget_name(id)
	_id_info[id] = {
		"screen": screen_name,
		"name": name,
		"kind": _doc.get_widget_type(id),
	}
	if not name.is_empty() and not _name_to_id.has(name.to_upper()):
		_name_to_id[name.to_upper()] = id
	for child_id in _doc.get_child_ids(id):
		_index_widget_subtree(screen_name, int(child_id))


func get_screen_names() -> PackedStringArray:
	return _screen_order


## Show a screen (no stack change). Unknown screen -> false. Closes the open
## dropdown first — every screen switch does [orig: CUIScene_SelectNodeByName
## @ 0x63b6b0 closes g_ui_active_combo_wnd].
func show_screen(name: String) -> bool:
	if _doc == null or not _screen_ids.has(name.to_upper()):
		return false
	close_active_combo_popup()
	_set_current_screen_internal(name)
	_on_screen_shown()
	return true


## The in-menu forward move (same-file SCREEN actions): pushes the current
## screen for pop_screen.
func navigate_to_screen(name: String) -> bool:
	var previous := _current_screen
	if not show_screen(name):
		return false
	if not previous.is_empty() and previous != _current_screen:
		_nav_stack.append(previous)
	return true


## Back within the file; popping past the root is the shell's back/quit.
func pop_screen() -> bool:
	if not _nav_stack.is_empty():
		var prev := _nav_stack[_nav_stack.size() - 1]
		_nav_stack.resize(_nav_stack.size() - 1)
		return show_screen(prev)
	quit_requested.emit()
	return false


func _set_current_screen_internal(name: String) -> void:
	_current_screen = name
	_focus_id = -1
	_configure_frame()


func _on_screen_shown() -> void:
	screen_changed.emit(_current_screen)
	# The MUSICVAR push fires on every screen event, repeats and zeros
	# included [orig: UI_DispatchScreenEvent @ 0x54e6a0 ->
	# AudioVM_SetVariable(index, value) @ 0x54eff4].
	var screen_id := int(_screen_ids.get(_current_screen.to_upper(), -1))
	var music_var := 0
	if screen_id >= 0 and _doc.get_screen_has_music_var(screen_id):
		music_var = _doc.get_screen_music_var(screen_id)
	music_changed.emit(music_var)
	if _music_director != null:
		_music_director.set_var(_music_var_index, music_var)


func _configure_frame() -> void:
	if _doc == null:
		return
	# The index maps drive the state store even frameless (headless seam
	# tests run companions without a render surface).
	_rebuild_index_maps()
	if _frame == null:
		return
	_frame.configure(_doc, _current_screen, _root, _style,
			_screen_text_lookup())
	MenuFrameStateReplay.apply(_frame, _index_of_id, _id_state)
	_seed_marquee_widgets()
	_apply_cursor(null)


# Marquee DATASOURCE routing: a marquee_wnd DATASOURCE is either a
# CBIN-encrypted credits config (ENV scroll settings + TEXT entries — routed
# to the dedicated CreditsPlayer scroller) or plain text fed to the compiled
# roll [orig: marquee_load_credits_from_ini — the Control-tree builder carried
# this resolve; the CBIN scroller is godot/src/cbin].
func _seed_marquee_widgets() -> void:
	_credits.clear()
	if _root == null:
		return
	for id in _index_of_id:
		if int(_id_info.get(id, {}).get("kind", -1)) != MnuDocument.TYPE_MARQUEE:
			continue
		if _id_state.get(id, {}).has("marquee_lines"):
			continue  # embedder-seeded content wins
		var datasource := _doc.get_widget_datasource(int(id))
		if datasource.is_empty():
			continue
		var bytes := _root.read_file(datasource.get_file())
		if bytes.is_empty():
			continue
		var credits := CbinCreditsResource.from_cbin_bytes(bytes)
		if credits != null:
			_credits.mount(_frame, int(id), widget_frame_rect(int(id)), credits, true)
			continue
		var text := bytes.get_string_from_ascii()
		var index := _frame_index(int(id))
		if index >= 0 and not text.is_empty():
			_frame.set_widget_marquee_lines(index,
					text.replace("\r\n", "\n").split("\n"))
	_credits.sync(_frame, _frame_index)


# The current screen's id<->pre-order-index maps: the frame's index space is
# the same pre-order DFS as the document walk (root first, children in
# authored order) — the documented seam contract on MenuFrameCompiler.
func _rebuild_index_maps() -> void:
	_id_of_index = []
	_index_of_id.clear()
	var screen_id := int(_screen_ids.get(_current_screen.to_upper(), -1))
	if screen_id < 0:
		return
	_map_widget_subtree(_doc.get_screen_root_id(screen_id))


func _map_widget_subtree(id: int) -> void:
	_index_of_id[id] = _id_of_index.size()
	_id_of_index.append(id)
	for child_id in _doc.get_child_ids(id):
		_map_widget_subtree(int(child_id))


# The id->text table for String/Item type=="id" lookups: the screen's own
# TEXT_RSRC (loaded through the VFS, cached) wins, else the shell-provided
# text resource — the same fallback the Control-tree build ran.
func _screen_text_lookup() -> Dictionary:
	var table := _text
	var screen_id := int(_screen_ids.get(_current_screen.to_upper(), -1))
	if screen_id >= 0:
		var rsrc := _doc.get_screen_text_rsrc(screen_id)
		if not rsrc.is_empty():
			var loaded := _load_text_rsrc(rsrc)
			if loaded != null:
				table = loaded
	var out := {}
	if table == null:
		return out
	# First-match-wins across sections (the engine-faithful flat lookup).
	for section in range(table.get_section_count()):
		for key in table.get_section_keys(section):
			var token := String(key)
			if not out.has(token):
				out[token] = table.get_string_in_section(
						table.get_section_name(section), token)
	return out


func _load_text_rsrc(file: String) -> RtxtStringFile:
	var key := file.to_lower()
	if _text_rsrc_cache.has(key):
		return _text_rsrc_cache[key]
	var loaded: RtxtStringFile = null
	if _root != null:
		var bytes := _root.read_file(file.get_file())
		if not bytes.is_empty():
			var t := RtxtStringFile.new()
			if t.load_from_byte_array(bytes) == OK:
				loaded = t
	_text_rsrc_cache[key] = loaded
	return loaded


# --- Widget addressing / state (doc-id keyed) ---------------------------------

## The document-wide name seam (case-insensitive, first match in document
## order — the find_child equivalent). -1 = absent.
func widget_id(name: String) -> int:
	return int(_name_to_id.get(name.to_upper(), -1))


func widget_name_of(id: int) -> String:
	return String(_id_info.get(id, {}).get("name", ""))


func widget_kind_of(id: int) -> int:
	return int(_id_info.get(id, {}).get("kind", -1))


func widget_screen_of(id: int) -> String:
	return String(_id_info.get(id, {}).get("screen", ""))


func has_widget(name: String) -> bool:
	return widget_id(name) >= 0


func _frame_index(id: int) -> int:
	# Frameless (headless seam tests): every frame-dependent path takes its
	# state-store fallback.
	if _frame == null:
		return -1
	return int(_index_of_id.get(id, -1))


func _state_of(id: int) -> Dictionary:
	if not _id_state.has(id):
		_id_state[id] = {}
	return _id_state[id]


## The widget's rect in the frame Control's local coordinates (the design
## rect scaled by the frame's current size) — icon/preview mounts position by
## it. Zero rect when the widget is not on the configured screen.
func widget_frame_rect(id: int) -> Rect2:
	var index := _frame_index(id)
	if index < 0 or _frame == null:
		return Rect2()
	var design := _frame.widget_rect(index)
	var scale := _design_scale()
	return Rect2(design.position * scale, design.size * scale)


func _design_scale() -> Vector2:
	# The fixed authoring design space (the witness lives at the engine home,
	# engine/runtime/menu menu_frame.h kMenuDesignWidth/Height).
	var size := _frame.get_size()
	if size.x > 1.0 and size.y > 1.0:
		return Vector2(size.x / float(MenuFrame.DESIGN_WIDTH),
				size.y / float(MenuFrame.DESIGN_HEIGHT))
	return Vector2.ONE


func set_widget_shown(id: int, shown: bool) -> void:
	_state_of(id)["shown"] = shown
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_shown_override(index, shown)
	_credits.sync(_frame, _frame_index)


func is_widget_shown(id: int) -> bool:
	var state: Dictionary = _id_state.get(id, {})
	if state.has("shown"):
		return bool(state["shown"])
	return (_doc.get_widget_flags(id) & MnuDocument.FLAG_HIDDEN) == 0


func set_widget_disabled(id: int, disabled: bool) -> void:
	_state_of(id)["disabled"] = disabled
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_disabled(index, disabled)


func is_widget_disabled(id: int) -> bool:
	var state: Dictionary = _id_state.get(id, {})
	if state.has("disabled"):
		return bool(state["disabled"])
	return (_doc.get_widget_flags(id) & MnuDocument.FLAG_DISABLED) != 0


func set_widget_checked(id: int, checked: bool) -> void:
	_state_of(id)["checked"] = checked
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_checked(index, checked)


func is_widget_checked(id: int) -> bool:
	var state: Dictionary = _id_state.get(id, {})
	if state.has("checked"):
		return bool(state["checked"])
	return (_doc.get_widget_flags(id) & MnuDocument.FLAG_CHECKED) != 0


func set_widget_text(id: int, text: String) -> void:
	_state_of(id)["text"] = text
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_text(index, text)


func get_widget_text(id: int) -> String:
	var index := _frame_index(id)
	if index >= 0:
		return _frame.get_widget_text(index)
	var state: Dictionary = _id_state.get(id, {})
	if state.has("text"):
		return String(state["text"])
	return _doc.get_widget_text(id)


func set_widget_items(id: int, items: PackedStringArray) -> void:
	var state := _state_of(id)
	state["items"] = items
	# Fresh rows reset the selection unless the caller re-selects (the
	# Control set_items semantics).
	state["selected_item"] = 0 if items.size() > 0 else -1
	state["scroll_row"] = 0
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_items(index, items)
		_frame.set_widget_selection(index, int(state["selected_item"]), -1, 0)


func get_widget_items(id: int) -> PackedStringArray:
	var state: Dictionary = _id_state.get(id, {})
	if state.has("items"):
		return state["items"]
	var out := PackedStringArray()
	for i in range(_doc.get_item_count(id)):
		out.append(String(_doc.get_item(id, i).get("text", "")))
	return out


func item_count(id: int) -> int:
	var state: Dictionary = _id_state.get(id, {})
	if state.has("items"):
		return (state["items"] as PackedStringArray).size()
	var index := _frame_index(id)
	if index >= 0:
		return _frame.item_count(index)
	return _doc.get_item_count(id)


func item_text(id: int, row: int) -> String:
	var state: Dictionary = _id_state.get(id, {})
	if state.has("items"):
		var items: PackedStringArray = state["items"]
		return items[row] if row >= 0 and row < items.size() else ""
	return String(_doc.get_item(id, row).get("text", ""))


## The authored item `value=` attribute of a row (the semantic value the
## original reads — SERVERTYPE 0/1; distinct from the display text).
func item_value(id: int, row: int) -> String:
	return String(_doc.get_item(id, row).get("value", ""))


## A widget's authored ACTION rows, as the document parsed them (each a
## Dictionary dispatch_action_row accepts).
func widget_actions(id: int) -> Array:
	return _doc.get_widget_actions(id)


## Retail's select-by-value seed (SpinList_SelectItemByValue; the lookup is
## the engine's through MnuDocument.find_item_row_by_value): the row whose
## authored `value=` equals `value`, row 0 on a miss.
func select_row_by_value(id: int, value: String, emit := true) -> void:
	var row := _doc.find_item_row_by_value(id, value)
	if row >= 0:
		select_row(id, row, emit)


func select_row(id: int, row: int, emit := true) -> void:
	var state := _state_of(id)
	state["selected_item"] = row
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_selection(index, row, -1,
				int(state.get("scroll_row", 0)))
	if emit:
		_emit_value_changed_for(id, row)


func selected_row(id: int) -> int:
	return int(_id_state.get(id, {}).get("selected_item",
			0 if item_count(id) > 0 else -1))


func selected_rows(id: int) -> PackedInt32Array:
	var state: Dictionary = _id_state.get(id, {})
	if state.has("selected_set"):
		return state["selected_set"]
	var out := PackedInt32Array()
	var row := selected_row(id)
	if row >= 0:
		out.append(row)
	return out


func set_scroll_row(id: int, row: int) -> void:
	var state := _state_of(id)
	state["scroll_row"] = maxi(row, 0)
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_selection(index,
				int(state.get("selected_item", -1)), -1,
				int(state["scroll_row"]))


# The engine pump's CScrollWnd interaction result (already clamped and
# applied to the frame): mirror it into the saved-state store and relay the
# value change.
func _on_frame_scroll_value(index: int, value: int) -> void:
	var id := _id_at_index(index)
	if id < 0:
		return
	if widget_kind_of(id) != MnuDocument.TYPE_SCROLL:
		set_scroll_row(id, value)
		return
	var scroll := _id_state.get(id, {}).get("scroll_range") as MenuScrollRange
	if scroll == null or value == scroll.value:
		return
	scroll.value = clampi(value, scroll.minimum, scroll.maximum)
	widget_value_changed.emit(widget_name_of(id), "scroll", scroll.value,
			str(scroll.value))


## The CScrollWnd min/max/inclusive-page/value render/state seam settings
## companions update (the interaction lives in the engine pump).
func set_widget_scroll_range(id: int, minimum: int, maximum: int,
		page: int, value: int) -> void:
	if minimum > maximum:
		minimum = 0
		maximum = 0
	value = clampi(value, minimum, maximum)
	var scroll := MenuScrollRange.new(minimum, maximum, page, value)
	_state_of(id)["scroll_range"] = scroll
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_scroll_range(index, scroll.minimum, scroll.maximum,
				scroll.page, scroll.value)


## Current standalone scroll state, or null until seeded.
func get_widget_scroll_range(id: int) -> MenuScrollRange:
	return _id_state.get(id, {}).get("scroll_range") as MenuScrollRange


func set_widget_marquee_lines(id: int, lines: PackedStringArray) -> void:
	_state_of(id)["marquee_lines"] = lines
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_marquee_lines(index, lines)


# --- Table state (menu_table_state.gd owns the shapes) --------------------------

func table_add_row(id: int, cells: PackedStringArray) -> void:
	MenuTableState.add_row(_state_of(id), cells)
	_push_table_rows(id)


func table_remove_row(id: int, row: int) -> void:
	if MenuTableState.remove_row(_state_of(id), row):
		_push_table_rows(id)


func table_clear_rows(id: int) -> void:
	MenuTableState.clear_rows(_state_of(id))
	_push_table_rows(id)


func table_row_count(id: int) -> int:
	return MenuTableState.row_count(_id_state.get(id, {}))


func table_cell_text(id: int, row: int, col: int) -> String:
	return MenuTableState.cell_text(_id_state.get(id, {}), row, col)


func table_selected_rows(id: int) -> PackedInt32Array:
	return _id_state.get(id, {}).get("table_selected", PackedInt32Array())


func table_select_row(id: int, row: int, additive := false) -> void:
	MenuTableState.select_row(_state_of(id), row, additive)
	MenuTableState.push_selection(_frame, _frame_index(id), _id_state.get(id, {}))


func _push_table_rows(id: int) -> void:
	MenuTableState.push_rows(_frame, _frame_index(id), _id_state.get(id, {}))


# --- Input: mouse ---------------------------------------------------------------

## One raw-mouse sample in frame-local coordinates. The shell's _gui_input
## forwards motion and left-button edges here.
func process_mouse(position: Vector2, button_down: bool) -> void:
	if _frame == null or not _frame.is_configured():
		return
	_last_mouse = position
	var down_edge := button_down and not _mouse_down
	_mouse_down = button_down

	# An open dropdown owns the mouse exclusively [orig: dispatch_mouse_event
	# @ 0x63ab00 g_ui_open_popup_wnd gate; combobox_handle_event @ 0x65c190,
	# outside check @ 0x65c290 — D-MNU-11/12]: a press picks a popup row or
	# dismisses (the dismissing click is consumed either way; a press on the
	# input-dead closed cell does nothing).
	if _open_combo_id >= 0:
		var combo_index := _frame_index(_open_combo_id)
		if combo_index < 0:
			_open_combo_id = -1
		else:
			_frame.set_cursor_state(false, position)
			# The popup's scrollbar child sees the sample ahead of row picking
			# [orig: CListWnd child walk @ 0x643f30 — the scrollbar child
			# claims first; parts = CScrollWnd_HandleEvent @ 0x64d050]. Its
			# scroll_row changes arrive on scroll_value_changed like the
			# main pump's.
			if _frame.process_popup_mouse(combo_index, position, button_down):
				_frame.set_widget_hover_item(combo_index, -1)
				return
			# The popup-exclusive pump hovers the row under the mouse (style 2)
			# [orig: the per-frame pump runs ONLY on the popup while open —
			# scene_end_frame @ 0x63e600 gate @ 0x63e691; the row mouseover
			# style = CListWnd_DrawItems @ 0x643f30].
			_frame.set_widget_hover_item(combo_index,
					_frame.combo_popup_row_at(combo_index, position))
			if down_edge:
				var row := _frame.combo_popup_row_at(combo_index, position)
				if row >= 0:
					_combo_select(_open_combo_id, row)
					close_active_combo_popup()
				elif not _frame.combo_popup_contains(combo_index, position) \
						and not _frame.widget_rect(combo_index).has_point(
								position / _design_scale()):
					close_active_combo_popup()
			return

	# The CScrollWnd interaction (arrows/track/shuttle drag) lives in the
	# engine pump; its value changes arrive on scroll_value_changed.
	var claim := _frame.process_mouse(position, button_down)
	_frame.set_cursor_state(false, position)
	if claim != _last_claim:
		_on_claim_changed(_last_claim, claim)
		_last_claim = claim
	_apply_cursor(_frame.get_cursor_texture())


## One wheel tick (steps: +1 rows-down, -1 rows-up) in frame-local
## coordinates. Deliberate divergence D-MNU-18 — retail ships no functioning
## menu wheel scroll (the witness lives at the engine pump); the open popup
## consumes the tick exclusively, else the front-most row owner under the
## point. Returns true when a scrollable target claimed it.
func process_wheel(position: Vector2, steps: int) -> bool:
	if _frame == null or not _frame.is_configured():
		return false
	if not _frame.process_mouse_wheel(position, steps):
		return false
	if _open_combo_id >= 0:
		var combo_index := _frame_index(_open_combo_id)
		if combo_index >= 0:
			# Keep the popup row hover matching the rows that just moved
			# under the still cursor.
			_frame.set_widget_hover_item(combo_index,
					_frame.combo_popup_row_at(combo_index, position))
	return true


func _on_claim_changed(previous: int, current: int) -> void:
	# The hover sound edges ride the visual-state transitions
	# [orig: widget_process_mouse_event @ 0x647a00 — MOUSEIN on entering
	# state 2/3, MOUSEOUT on leaving the widget].
	if previous >= 0:
		var prev_id := _id_at_index(previous)
		if prev_id >= 0:
			_play_widget_sound_state(prev_id, "MOUSEOUT")
			widget_hover_changed.emit(prev_id, false)
	if current >= 0 and not _frame.is_widget_disabled(current):
		var id := _id_at_index(current)
		if id >= 0:
			_play_widget_sound_state(id, "MOUSEIN")
			widget_hover_changed.emit(id, true)


func _id_at_index(index: int) -> int:
	if index < 0 or index >= _id_of_index.size():
		return -1
	return int(_id_of_index[index])


func _apply_cursor(texture: Texture2D) -> void:
	# The retail cursor rides the claim as the OS custom cursor — the ONE
	# live cursor (both drawn showed the compiled one trailing by a pump
	# frame; emit_cursor stays for surfaces without an OS cursor).
	Input.set_custom_mouse_cursor(texture, Input.CURSOR_ARROW)


func _on_frame_widget_clicked(index: int) -> void:
	var id := _id_at_index(index)
	if id < 0 or _frame.is_widget_disabled(index):
		return
	_activate_widget(id, index, _last_mouse)


# widget_activated fires BEFORE the scripted ACTION list. Retail runs ACTIONs
# first [orig: CUIWidget_HandleScriptedAction @0x6497f0: ACTION walk, then
# widget[63]->vtable+32] but keeps every screen alive; this shell replaces the
# document on a cross-.mnu jump, so observers (PLAYER_INFO ACCEPT) read their
# still-live controls first, and the dispatch is skipped when an observer
# swapped the document under the emit (game.mnu CONFIRM_YES -> main.mnu;
# menu-re.md).
func _emit_activated_then_dispatch(id: int) -> void:
	var doc_at_emit := _doc
	widget_activated.emit(id, widget_name_of(id))
	if _doc == doc_at_emit:
		_dispatch_widget_actions(id)


func _activate_widget(id: int, index: int, position: Vector2) -> void:
	var kind := widget_kind_of(id)
	match kind:
		MnuDocument.TYPE_BUTTON, MnuDocument.TYPE_GOTO, MnuDocument.TYPE_STATIC, \
		MnuDocument.TYPE_LABEL:
			_play_widget_sound_state(id, "SELECTED")
			_emit_activated_then_dispatch(id)
		MnuDocument.TYPE_CHECKBOX:
			var next := not is_widget_checked(id)
			set_widget_checked(id, next)
			_play_widget_sound_state(id, "SELECTED")
			_emit_activated_then_dispatch(id)
		MnuDocument.TYPE_RADIO:
			_select_radio(id)
			_play_widget_sound_state(id, "SELECTED")
			_emit_activated_then_dispatch(id)
		MnuDocument.TYPE_COMBO:
			_play_widget_sound_state(id, "SELECTED")
			if _open_combo_id == id:
				close_active_combo_popup()
			else:
				_open_combo_popup(id)
		MnuDocument.TYPE_LIST, MnuDocument.TYPE_MULTI, MnuDocument.TYPE_LAN_LIST:
			var row := _frame.list_row_at(index, position) if index >= 0 else -1
			if row >= 0:
				_list_click(id, kind, row)
		MnuDocument.TYPE_SPINLIST:
			var arrow := _frame.spin_arrow_at(index, position)
			if arrow == 1:
				spin_cycle(id, 1)
			elif arrow == 2:
				spin_cycle(id, -1)
		MnuDocument.TYPE_EDIT:
			_focus_edit(id)
		MnuDocument.TYPE_TABLE:
			var row := _frame.table_row_at(index, position)
			if row >= 0:
				_table_click(id, row)
		_:
			# Generic containers: actions still dispatch (authored WINDOW
			# widgets carry SCREEN jumps in shipped menus).
			if _doc.get_widget_actions(id).size() > 0:
				_play_widget_sound_state(id, "SELECTED")
				_emit_activated_then_dispatch(id)


func _list_click(id: int, kind: int, row: int) -> void:
	var now := Time.get_ticks_msec()
	var double := id == _last_click_id and row == _last_click_row \
			and now - _last_click_ms <= DOUBLE_CLICK_MS
	_last_click_id = id
	_last_click_row = row
	_last_click_ms = 0 if double else now
	if kind == MnuDocument.TYPE_MULTI:
		var additive := Input.is_key_pressed(KEY_CTRL)
		var state := _state_of(id)
		var selected: PackedInt32Array = state.get("selected_set",
				PackedInt32Array()) if additive else PackedInt32Array()
		if selected.has(row):
			var kept := PackedInt32Array()
			for r in selected:
				if r != row:
					kept.append(r)
			selected = kept
		else:
			selected.append(row)
		state["selected_set"] = selected
		var index := _frame_index(id)
		if index >= 0:
			_frame.set_widget_selected_set(index, selected)
	select_row(id, row)  # emits the "list"/"multi" value change
	_play_widget_sound_state(id, "SELECTED")
	if double:
		list_activated.emit(id, row)


func _table_click(id: int, row: int) -> void:
	var now := Time.get_ticks_msec()
	var double := id == _last_click_id and row == _last_click_row \
			and now - _last_click_ms <= DOUBLE_CLICK_MS
	_last_click_id = id
	_last_click_row = row
	_last_click_ms = 0 if double else now
	var multiselect := bool(_doc.get_widget_authoring_state(id).get("items", {})
			.get("multiselect", false))
	table_select_row(id, row, multiselect and Input.is_key_pressed(KEY_CTRL))
	_play_widget_sound_state(id, "SELECTED")
	if double:
		list_activated.emit(id, row)


func _select_radio(id: int) -> void:
	set_widget_checked(id, true)
	var group := _doc.get_widget_group(id)
	# Group exclusivity within the widget's screen (the authored GROUP id).
	var screen := widget_screen_of(id)
	for other_id in _id_info:
		if other_id == id:
			continue
		var info: Dictionary = _id_info[other_id]
		if String(info.get("screen", "")) != screen:
			continue
		if int(info.get("kind", -1)) != MnuDocument.TYPE_RADIO:
			continue
		if _doc.get_widget_group(int(other_id)) != group:
			continue
		set_widget_checked(int(other_id), false)


# --- Combo popups ---------------------------------------------------------------

func _open_combo_popup(id: int) -> void:
	# One dropdown per menu: opening one closes the previous
	# [orig: g_ui_active_combo_wnd @ 0x31C16D0, single-open toggle @ 0x65c210].
	close_active_combo_popup()
	_open_combo_id = id
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_popup_open(index, true)


func close_active_combo_popup() -> void:
	if _open_combo_id < 0:
		return
	var index := _frame_index(_open_combo_id)
	if index >= 0:
		_frame.set_widget_popup_open(index, false)
		_frame.set_widget_hover_item(index, -1)
	_open_combo_id = -1


func is_combo_popup_open(id: int) -> bool:
	return _open_combo_id == id


func _combo_select(id: int, row: int) -> void:
	select_row(id, row)
	_play_widget_sound_state(id, "SELECTED")


# --- Spin lists -----------------------------------------------------------------

## Wrap-around cycle (the spin arrows' step); emits the value change.
func spin_cycle(id: int, delta: int) -> void:
	var count := item_count(id)
	if count <= 0:
		return
	var row := ((selected_row(id) + delta) % count + count) % count
	select_row(id, row)
	_play_widget_sound_state(id, "SELECTED")


## The selected item's `value=` attribute (authored rows only; runtime rows
## carry labels alone).
func spin_value_attr(id: int) -> String:
	return item_value(id, selected_row(id))


# --- Edit focus + keyboard ------------------------------------------------------

func _focus_edit(id: int) -> void:
	# Click focuses unless read-only [orig: edit_widget_handle_input_event
	# @ 0x661510 — g_ui_focus_wnd = this unless widget[194]].
	if (_doc.get_widget_flags(id) & MnuDocument.FLAG_READONLY) != 0:
		return
	if _focus_id == id:
		return
	_clear_edit_focus()
	_focus_id = id
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_focused(index, true)
		if _frame.get_widget_caret(index) < 0:
			_frame.set_widget_caret(index, get_widget_text(id).length())


func _clear_edit_focus() -> void:
	if _focus_id < 0:
		return
	var id := _focus_id
	_focus_id = -1
	var index := _frame_index(id)
	if index >= 0:
		_frame.set_widget_focused(index, false)
		# Persist the edited text for cross-screen reads.
		_state_of(id)["text"] = _frame.get_widget_text(index)
	_emit_edit_changed(id)


func get_focused_widget() -> int:
	return _focus_id


func _emit_edit_changed(id: int) -> void:
	var kind := "multiline" if widget_kind_of(id) == MnuDocument.TYPE_MULTILINE_EDIT \
			else "edit"
	widget_value_changed.emit(widget_name_of(id), kind, -1, get_widget_text(id))


## Route one key event. Returns true when consumed (the shell then marks the
## input handled). Order matches the Control tree: focused edit first, then
## the virtual-key hotkey scan, then the character scan.
func handle_key_input(event: InputEventKey) -> bool:
	if event.is_echo() or not event.is_pressed():
		return false
	if _frame == null or not _frame.is_configured():
		return false
	if _focus_id >= 0:
		if _route_edit_key(event):
			return true
	var vk := ""
	match event.get_keycode():
		KEY_ESCAPE:
			vk = "VK_ESCAPE"
		KEY_ENTER, KEY_KP_ENTER:
			vk = "VK_RETURN"
	if not vk.is_empty():
		var target := _frame.hotkey_widget(vk, true)
		if target >= 0 and _trigger_hotkey_target(target):
			return true
	var unicode := event.get_unicode()
	if unicode == 0:
		var keycode := int(event.get_keycode())
		if keycode >= 0x20 and keycode <= 0x7E:
			unicode = keycode
	if unicode > 0:
		var target := _frame.hotkey_widget(String.chr(unicode), false)
		if target >= 0 and _trigger_hotkey_target(target):
			return true
	return false


func _route_edit_key(event: InputEventKey) -> bool:
	var index := _frame_index(_focus_id)
	if index < 0:
		_focus_id = -1
		return false
	var id := _focus_id
	# Godot key -> the engine's edit VK codes (the witness lives at the engine
	# home, engine/runtime/menu menu_edit.h kEditKey*).
	var vk := 0
	match event.get_keycode():
		KEY_BACKSPACE: vk = MenuFrame.EDIT_KEY_BACKSPACE
		KEY_ENTER, KEY_KP_ENTER: vk = MenuFrame.EDIT_KEY_ENTER
		KEY_END: vk = MenuFrame.EDIT_KEY_END
		KEY_HOME: vk = MenuFrame.EDIT_KEY_HOME
		KEY_LEFT: vk = MenuFrame.EDIT_KEY_LEFT
		KEY_RIGHT: vk = MenuFrame.EDIT_KEY_RIGHT
		KEY_DELETE: vk = MenuFrame.EDIT_KEY_DELETE
	if vk != 0:
		var result := _frame.edit_key(index, vk, event.is_shift_pressed())
		if result == MenuFrame.EDIT_RESULT_COMMIT:
			# Enter commits: the value fires and focus releases (the witness
			# lives at the engine home, engine/runtime/menu menu_edit.h
			# EditKeyResult::kCommit — clears g_ui_focus_wnd and fires the
			# commit event 0x7000002).
			_clear_edit_focus()
			_play_widget_sound_state(id, "SELECTED")
		elif result == MenuFrame.EDIT_RESULT_CHANGED:
			_emit_edit_changed(id)
		return true
	var unicode := event.get_unicode()
	if unicode > 0:
		if _frame.edit_char(index, unicode):
			_emit_edit_changed(id)
		return true
	return false


func _trigger_hotkey_target(index: int) -> bool:
	# A disabled target consumes the key without firing (prevents a later
	# same-key widget firing through a disabled modal); an actionless match
	# is still consumed — actionless named controls are the retail Command
	# seam the shell wires by name.
	if _frame.is_widget_disabled(index):
		return true
	var id := _id_at_index(index)
	if id < 0:
		return true
	if widget_kind_of(id) == MnuDocument.TYPE_EDIT:
		_focus_edit(id)
		return true
	var rect := _frame.widget_rect(index)
	_activate_widget(id, index, (rect.position + rect.size * 0.5) * _design_scale())
	return true


# --- Actions --------------------------------------------------------------------

# The shell-owned verbs the dispatcher reports without inventing effects.
const SHELL_ACTION_TYPES := ["form_post", "glb_load", "glb_loadandping",
	"glb_filter", "glb_filter_num", "glb_ping", "glb_join", "appmsg",
	"lan_search", "lan_join", "mnx"]


func _dispatch_widget_actions(id: int) -> void:
	for action in _doc.get_widget_actions(id):
		dispatch_action_row(action)


## One parsed ACTION row [orig: CUIWidget_HandleScriptedAction @ 0x6497f0].
## Returns true when the action was handled (or deliberately consumed).
func dispatch_action_row(action: Dictionary) -> bool:
	var type := String(action.get("type", "")).to_lower()
	var target := String(action.get("target", ""))
	action_dispatched.emit(type, target)
	match type:
		"window":
			return handle_window_action(target,
					String(action.get("state", "")).to_lower(),
					bool(action.get("toggle", false)))
		"screen":
			# Same-file detection: shipped menus spell same-file jumps with
			# their own filename; empty file = same file.
			var file := String(action.get("file", ""))
			if file.is_empty() or file.nocasecmp_to(_menu_file) == 0:
				return navigate_to_screen(target)
			menu_requested.emit(file, target)
			return true
		"pop", "pop_screen":
			pop_screen()
			return true
		"quit", "quit_game":
			quit_requested.emit()
			return true
		"url":
			url_requested.emit(target)
			shell_action_requested.emit(type, action)
			return true
		"tab":
			# TAB selects the named focus target; the compiled path focuses
			# edit targets (the only focus model the frame carries).
			var target_id := widget_id(target)
			if target_id < 0 or widget_screen_of(target_id) != _current_screen:
				return false
			if not is_widget_shown(target_id) or is_widget_disabled(target_id):
				return false
			if widget_kind_of(target_id) == MnuDocument.TYPE_EDIT:
				_focus_edit(target_id)
			return true
		_:
			if SHELL_ACTION_TYPES.has(type):
				shell_action_requested.emit(type, action)
				return true
	return false


## WINDOW action: show/hide/enable/disable a named widget of the CURRENT
## screen, with the retail TOGGLE flag inverting the current state.
func handle_window_action(target: String, state: String, toggle := false) -> bool:
	var id := widget_id(target)
	if id < 0 or widget_screen_of(id) != _current_screen:
		return false
	match state:
		"enable":
			set_widget_disabled(id,
					not is_widget_disabled(id) if toggle else false)
			return true
		"disable":
			set_widget_disabled(id,
					not is_widget_disabled(id) if toggle else true)
			return true
		"show":
			if toggle:
				set_widget_shown(id, not is_widget_shown(id))
			else:
				set_widget_shown(id, true)
			return true
		"hide", "toggle":
			if toggle or state == "toggle":
				set_widget_shown(id, not is_widget_shown(id))
			else:
				set_widget_shown(id, false)
			return true
	return false


# --- Sounds / value relay -------------------------------------------------------

func _play_widget_sound_state(id: int, state_token: String) -> void:
	for sound in _doc.get_widget_sounds(id):
		if String(sound.get("state", "")).nocasecmp_to(state_token) != 0:
			continue
		var trigger := String(sound.get("trigger", ""))
		var file := String(sound.get("file", ""))
		play_widget_sound(trigger, file)
		return


## Direct play seam (voice preview etc.); emits sound_requested always.
func play_widget_sound(trigger: String, file: String) -> void:
	sound_requested.emit(file, trigger)
	if _audio == null:
		return
	_audio.play_widget_sound(trigger, file)


func _emit_value_changed_for(id: int, row: int) -> void:
	var kind := "list"
	match widget_kind_of(id):
		MnuDocument.TYPE_COMBO:
			kind = "combo"
		MnuDocument.TYPE_MULTI:
			kind = "multi"
		MnuDocument.TYPE_SPINLIST:
			kind = "spinlist"
		MnuDocument.TYPE_TABLE:
			kind = "table"
	widget_value_changed.emit(widget_name_of(id), kind, row, item_text(id, row))


# --- Frame time -----------------------------------------------------------------

## Advance the blink/marquee clock; the shell's _process forwards its clock.
func tick(time_ms: int) -> void:
	if _frame != null:
		_frame.set_time_ms(time_ms)
