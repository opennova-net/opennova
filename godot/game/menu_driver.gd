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
# push, and the value-changed relay. Widgets go by stable MnuDocument id,
# valid across screens; per-id runtime state is replayed at each screen
# configure.

signal screen_changed(screen_name: String)
signal menu_requested(file: String, target_screen: String)
signal quit_requested()
signal url_requested(url: String)
# The widget sound edge as resolved from the SOUND table (trigger, bank); the
# MenuAudio player plays it. Observable without banks, which the seam tests
# need (a script double never intercepts the typed native call).
signal sound_requested(file: String, trigger: String)
signal widget_value_changed(widget_name: String, kind: String, index: int, value: String)
# Click-level activation of a widget (button/goto/checkbox/radio...) — the
# named-control seam the shell and companions wire launch/quit policy to.
signal widget_activated(id: int, widget_name: String)
# List double-click (the ItemList item_activated equivalent).
signal list_activated(id: int, row: int)
# The pump's claim moved between widgets (hover edges; PLAYER_PREVIEW zoom).
signal widget_hover_changed(id: int, hovered: bool)

# Double-click window for list/table activation, matching Godot's default.
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
## One indexed document widget: its screen, authored name and kind.
class WidgetInfo extends RefCounted:
	var screen: String
	var name: String
	var kind: int

	func _init(p_screen: String, p_name: String, p_kind: int) -> void:
		screen = p_screen
		name = p_name
		kind = p_kind

var _id_info: Dictionary = {}         # doc id -> WidgetInfo
# Runtime widget state keyed by doc id (MenuWidgetState), replayed onto the
# frame at configure.
var _id_state: Dictionary = {}
# Current screen's id<->pre-order-index maps.
var _id_of_index: PackedInt64Array = []
var _index_of_id: Dictionary = {}
# Per-screen RTXT text tables (TEXT_RSRC), cached by lowercased filename.
var _text_rsrc_cache: Dictionary = {}
# CBIN credits scrollers mounted over marquee widgets (menu_credits_overlays.gd
# owns the mounts + the hidden-tab visibility gate).
var _credits := MenuCreditsOverlays.new()



func attach(frame: MenuFrame, audio: MenuAudio) -> void:
	_frame = frame
	_audio = audio
	input.setup(self)
	if not _frame.widget_clicked.is_connected(input.on_frame_widget_clicked):
		_frame.widget_clicked.connect(input.on_frame_widget_clicked)
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
	input.reset()
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
	_id_info[id] = WidgetInfo.new(screen_name, name, _doc.get_widget_type(id))
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
	input.reset_focus()
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
	input.reset_cursor()


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
		if widget_kind_of(int(id)) != MnuDocument.TYPE_MARQUEE:
			continue
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
		var index := frame_index(int(id))
		if index >= 0 and not text.is_empty():
			_frame.set_widget_marquee_lines(index,
					text.replace("\r\n", "\n").split("\n"))
	_credits.sync(_frame, frame_index)


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
	var info: WidgetInfo = _id_info.get(id)
	return info.name if info != null else ""


func widget_kind_of(id: int) -> int:
	var info: WidgetInfo = _id_info.get(id)
	return info.kind if info != null else -1


func widget_screen_of(id: int) -> String:
	var info: WidgetInfo = _id_info.get(id)
	return info.screen if info != null else ""


func has_widget(name: String) -> bool:
	return widget_id(name) >= 0


func frame_index(id: int) -> int:
	# Frameless (headless seam tests): every frame-dependent path takes its
	# state-store fallback.
	if _frame == null:
		return -1
	return int(_index_of_id.get(id, -1))


func _state_of(id: int) -> MenuWidgetState:
	# An absent widget (-1, the retail null CUIWidget_FindByName) has no
	# state: its writes land in a throwaway so the store never grows a -1 row.
	if id < 0:
		return MenuWidgetState.new()
	if not _id_state.has(id):
		_id_state[id] = MenuWidgetState.new()
	return _id_state[id]


## The saved state of a widget, or null when nothing was ever written.
func _saved_state(id: int) -> MenuWidgetState:
	return _id_state.get(id)


## The widget's rect in the frame Control's local coordinates (the design
## rect scaled by the frame's current size) — icon/preview mounts position by
## it. Zero rect when the widget is not on the configured screen.
func widget_frame_rect(id: int) -> Rect2:
	var index := frame_index(id)
	if index < 0 or _frame == null:
		return Rect2()
	var design := _frame.widget_rect(index)
	var scale := design_scale()
	return Rect2(design.position * scale, design.size * scale)


func design_scale() -> Vector2:
	# The fixed authoring design space (the witness lives at the engine home,
	# engine/runtime/menu menu_frame.h kMenuDesignWidth/Height).
	var size := _frame.get_size()
	if size.x > 1.0 and size.y > 1.0:
		return Vector2(size.x / float(MenuFrame.DESIGN_WIDTH),
				size.y / float(MenuFrame.DESIGN_HEIGHT))
	return Vector2.ONE


func set_widget_shown(id: int, shown: bool) -> void:
	var state := _state_of(id)
	state.shown = shown
	state.has_shown = true
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_shown_override(index, shown)
	_credits.sync(_frame, frame_index)


func is_widget_shown(id: int) -> bool:
	var state := _saved_state(id)
	if state != null and state.has_shown:
		return state.shown
	return (_doc.get_widget_flags(id) & MnuDocument.FLAG_HIDDEN) == 0


func set_widget_disabled(id: int, disabled: bool) -> void:
	var state := _state_of(id)
	state.disabled = disabled
	state.has_disabled = true
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_disabled(index, disabled)


func is_widget_disabled(id: int) -> bool:
	var state := _saved_state(id)
	if state != null and state.has_disabled:
		return state.disabled
	return (_doc.get_widget_flags(id) & MnuDocument.FLAG_DISABLED) != 0


func set_widget_checked(id: int, checked: bool) -> void:
	var state := _state_of(id)
	state.checked = checked
	state.has_checked = true
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_checked(index, checked)


func is_widget_checked(id: int) -> bool:
	var state := _saved_state(id)
	if state != null and state.has_checked:
		return state.checked
	return (_doc.get_widget_flags(id) & MnuDocument.FLAG_CHECKED) != 0


func set_widget_text(id: int, text: String) -> void:
	var state := _state_of(id)
	state.text = text
	state.has_text = true
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_text(index, text)


func get_widget_text(id: int) -> String:
	var index := frame_index(id)
	if index >= 0:
		return _frame.get_widget_text(index)
	var state := _saved_state(id)
	if state != null and state.has_text:
		return state.text
	return _doc.get_widget_text(id)


func set_widget_items(id: int, items: PackedStringArray) -> void:
	var state := _state_of(id)
	state.items = items
	state.has_items = true
	# Fresh rows reset the selection unless the caller re-selects (the
	# Control set_items semantics).
	state.selected_item = 0 if items.size() > 0 else -1
	state.has_selected_item = true
	state.scroll_row = 0
	state.has_scroll_row = true
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_items(index, items)
		_frame.set_widget_selection(index, state.selected_item, -1, 0)


func get_widget_items(id: int) -> PackedStringArray:
	var state := _saved_state(id)
	if state != null and state.has_items:
		return state.items
	var out := PackedStringArray()
	for i in range(_doc.get_item_count(id)):
		out.append(_doc.get_item_text(id, i))
	return out


func item_count(id: int) -> int:
	var state := _saved_state(id)
	if state != null and state.has_items:
		return state.items.size()
	var index := frame_index(id)
	if index >= 0:
		return _frame.item_count(index)
	return _doc.get_item_count(id)


func item_text(id: int, row: int) -> String:
	var state := _saved_state(id)
	if state != null and state.has_items:
		var items := state.items
		return items[row] if row >= 0 and row < items.size() else ""
	return _doc.get_item_text(id, row)


## The authored item `value=` attribute of a row (the semantic value the
## original reads — SERVERTYPE 0/1; distinct from the display text).
func item_value(id: int, row: int) -> String:
	return _doc.get_item_value(id, row)


## A widget's authored ACTION rows, as the document parsed them (each a row
## dispatch_action_row accepts).
func widget_actions(id: int) -> Array[MnuActionRow]:
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
	state.selected_item = row
	state.has_selected_item = true
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_selection(index, row, -1, state.scroll_row)
	if emit:
		_emit_value_changed_for(id, row)


func selected_row(id: int) -> int:
	var state := _saved_state(id)
	if state != null and state.has_selected_item:
		return state.selected_item
	return 0 if item_count(id) > 0 else -1


func selected_rows(id: int) -> PackedInt32Array:
	var state := _saved_state(id)
	if state != null and state.has_selected_set:
		return state.selected_set
	var out := PackedInt32Array()
	var row := selected_row(id)
	if row >= 0:
		out.append(row)
	return out


func set_scroll_row(id: int, row: int) -> void:
	var state := _state_of(id)
	state.scroll_row = maxi(row, 0)
	state.has_scroll_row = true
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_selection(index,
				state.selected_item if state.has_selected_item else -1, -1,
				state.scroll_row)


# The engine pump's CScrollWnd interaction result (already clamped and
# applied to the frame): mirror it into the saved-state store and relay the
# value change.
func _on_frame_scroll_value(index: int, value: int) -> void:
	var id := id_at_index(index)
	if id < 0:
		return
	if widget_kind_of(id) != MnuDocument.TYPE_SCROLL:
		set_scroll_row(id, value)
		return
	var saved := _saved_state(id)
	var scroll: MenuScrollRange = saved.scroll_range if saved != null else null
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
	_state_of(id).scroll_range = scroll
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_scroll_range(index, scroll.minimum, scroll.maximum,
				scroll.page, scroll.value)


## Current standalone scroll state, or null until seeded.
func get_widget_scroll_range(id: int) -> MenuScrollRange:
	var state := _saved_state(id)
	return state.scroll_range if state != null else null


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
	return MenuTableState.row_count(_saved_state(id))


func table_cell_text(id: int, row: int, col: int) -> String:
	return MenuTableState.cell_text(_saved_state(id), row, col)


func table_selected_rows(id: int) -> PackedInt32Array:
	var state := _saved_state(id)
	return state.table_selected if state != null else PackedInt32Array()


func table_select_row(id: int, row: int, additive := false) -> void:
	MenuTableState.select_row(_state_of(id), row, additive)
	MenuTableState.push_selection(_frame, frame_index(id), _saved_state(id))


func _push_table_rows(id: int) -> void:
	MenuTableState.push_rows(_frame, frame_index(id), _saved_state(id))


# --- Input dispatch seams ---------------------------------------------------------
# The mouse / key / popup / edit-focus half lives in MenuInputDispatch
# (menu_input_dispatch.gd); these are the typed seams it drives and the
# shell-facing forwarders the _gui_input owners call.

var input := MenuInputDispatch.new()


func document() -> MnuDocument:
	return _doc


## The doc id at a frame index, or -1.
func id_at_index(index: int) -> int:
	if index < 0 or index >= _id_of_index.size():
		return -1
	return int(_id_of_index[index])


# widget_activated fires BEFORE the scripted ACTION list. Retail runs ACTIONs
# first [orig: CUIWidget_HandleScriptedAction @0x6497f0: ACTION walk, then
# widget[63]->vtable+32] but keeps every screen alive; this shell replaces the
# document on a cross-.mnu jump, so observers (PLAYER_INFO ACCEPT) read their
# still-live controls first, and the dispatch is skipped when an observer
# swapped the document under the emit (game.mnu CONFIRM_YES -> main.mnu;
# menu-re.md).
func activate(id: int) -> void:
	var doc_at_emit := _doc
	widget_activated.emit(id, widget_name_of(id))
	if _doc == doc_at_emit:
		_dispatch_widget_actions(id)


## Check one radio and uncheck its GROUP siblings on the same screen.
func select_radio(id: int) -> void:
	set_widget_checked(id, true)
	var group := _doc.get_widget_group(id)
	# Group exclusivity within the widget's screen (the authored GROUP id).
	var screen := widget_screen_of(id)
	for other_id in _id_info:
		if other_id == id:
			continue
		var info: WidgetInfo = _id_info[other_id]
		if info.screen != screen:
			continue
		if info.kind != MnuDocument.TYPE_RADIO:
			continue
		if _doc.get_widget_group(int(other_id)) != group:
			continue
		set_widget_checked(int(other_id), false)


func emit_edit_changed(id: int) -> void:
	var kind := "multiline" if widget_kind_of(id) == MnuDocument.TYPE_MULTILINE_EDIT \
			else "edit"
	widget_value_changed.emit(widget_name_of(id), kind, -1, get_widget_text(id))



## The MULTI list's stored selection set (empty until a CTRL-select wrote one).
func selected_set(id: int) -> PackedInt32Array:
	return _state_of(id).selected_set


## Store a MULTI list's selection set and push it to the frame.
func set_selected_set(id: int, rows: PackedInt32Array) -> void:
	var state := _state_of(id)
	state.selected_set = rows
	state.has_selected_set = true
	var index := frame_index(id)
	if index >= 0:
		_frame.set_widget_selected_set(index, rows)


## Persist an edit widget's text for cross-screen reads.
func remember_widget_text(id: int, text: String) -> void:
	var state := _state_of(id)
	state.text = text
	state.has_text = true


func process_mouse(position: Vector2, button_down: bool) -> void:
	input.process_mouse(position, button_down)


func process_wheel(position: Vector2, steps: int) -> bool:
	return input.process_wheel(position, steps)


func handle_key_input(event: InputEventKey) -> bool:
	return input.handle_key_input(event)


func close_active_combo_popup() -> void:
	input.close_active_combo_popup()


func is_combo_popup_open(id: int) -> bool:
	return input.is_combo_popup_open(id)


func get_focused_widget() -> int:
	return input.get_focused_widget()


# --- Spin lists ---------------------------------------------------------------------

## Wrap-around cycle (the spin arrows' step); emits the value change.
func spin_cycle(id: int, delta: int) -> void:
	var count := item_count(id)
	if count <= 0:
		return
	var row := ((selected_row(id) + delta) % count + count) % count
	select_row(id, row)
	play_widget_state_sound(id, "SELECTED")


## The selected item's `value=` attribute (authored rows only; runtime rows
## carry labels alone).
func spin_value_attr(id: int) -> String:
	return item_value(id, selected_row(id))



# --- Actions --------------------------------------------------------------------

func _dispatch_widget_actions(id: int) -> void:
	for action: MnuActionRow in _doc.get_widget_actions(id):
		dispatch_action_row(action)


## One parsed ACTION row [orig: CUIWidget_HandleScriptedAction @ 0x6497f0].
## Returns true when the action was handled. The service verbs (FORM_POST,
## GLB_*, APPMSG, LAN_*, MNX; docs/mnu/menu-re.md) are not the driver's: they
## return false, and the shell wires that behavior by named control.
func dispatch_action_row(action: MnuActionRow) -> bool:
	var type := action.type.to_lower()
	var target := action.target
	match type:
		"window":
			return handle_window_action(target, action.state.to_lower(), action.toggle)
		"screen":
			# Same-file detection: shipped menus spell same-file jumps with
			# their own filename; empty file = same file.
			var file := action.file
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
				input.focus_edit(target_id)
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

func play_widget_state_sound(id: int, state_token: String) -> void:
	for sound: MnuSoundRow in _doc.get_widget_sounds(id):
		if sound.state.nocasecmp_to(state_token) != 0:
			continue
		play_widget_sound(sound.trigger, sound.file)
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
