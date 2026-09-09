class_name MenuInputDispatch
extends RefCounted
## The menu driver's input half: the raw-mouse and wheel samples the shell's
## _gui_input forwards, the click -> widget activation switch, the CTRL /
## double-click list rules, the single open combo popup, edit focus and the
## key routing (edit keys, VK hotkeys, character hotkeys). It reads the
## driver's document and frame through typed seams and never touches its
## state store directly; the witnessed policies moved here verbatim.

const DOUBLE_CLICK_MS := 400

## The driver owns this dispatcher; the reverse link must not keep its menu,
## document and native resources alive through process teardown.
var _driver_ref: WeakRef
var _driver: MenuDriver:
	get:
		return _driver_ref.get_ref() as MenuDriver if _driver_ref != null else null
var _focus_id := -1        # keyboard/edit focus [orig: g_ui_focus_wnd @ 0x31C16D4]
var _open_combo_id := -1   # single open dropdown [orig: g_ui_active_combo_wnd @ 0x31C16D0]
var _last_claim := -1
var _last_mouse := Vector2.ZERO
var _mouse_down := false
var _last_click_id := -1
var _last_click_row := -1
var _last_click_ms := 0

var _frame: MenuFrame:
	get:
		return _driver.get_frame() if _driver != null else null
var _doc: MnuDocument:
	get:
		return _driver.document() if _driver != null else null


func setup(driver: MenuDriver) -> void:
	_driver_ref = weakref(driver)


## A new document: no focus, no open popup, no claim.
func reset() -> void:
	_focus_id = -1
	_open_combo_id = -1
	_last_claim = -1


## A screen change drops the edit focus.
func reset_focus() -> void:
	_focus_id = -1


## The frame was (re)configured: back to the stock cursor.
func reset_cursor() -> void:
	_apply_cursor(null)


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
		var combo_index := _driver.frame_index(_open_combo_id)
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
								position / _driver.design_scale()):
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
		var combo_index := _driver.frame_index(_open_combo_id)
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
		var prev_id := _driver.id_at_index(previous)
		if prev_id >= 0:
			_driver.play_widget_state_sound(prev_id, "MOUSEOUT")
			_driver.widget_hover_changed.emit(prev_id, false)
	if current >= 0 and not _frame.is_widget_disabled(current):
		var id := _driver.id_at_index(current)
		if id >= 0:
			_driver.play_widget_state_sound(id, "MOUSEIN")
			_driver.widget_hover_changed.emit(id, true)





func _apply_cursor(texture: Texture2D) -> void:
	# The retail cursor rides the claim as the OS custom cursor — the ONE
	# live cursor (both drawn showed the compiled one trailing by a pump
	# frame; emit_cursor stays for surfaces without an OS cursor).
	Input.set_custom_mouse_cursor(texture, Input.CURSOR_ARROW)


func on_frame_widget_clicked(index: int) -> void:
	if _driver == null:
		return
	var id := _driver.id_at_index(index)
	if id < 0 or _frame.is_widget_disabled(index):
		return
	_activate_widget(id, index, _last_mouse)





func _activate_widget(id: int, index: int, position: Vector2) -> void:
	var kind := _driver.widget_kind_of(id)
	match kind:
		MnuDocument.TYPE_BUTTON, MnuDocument.TYPE_GOTO, MnuDocument.TYPE_STATIC, \
		MnuDocument.TYPE_LABEL:
			_driver.play_widget_state_sound(id, "SELECTED")
			_driver.activate(id)
		MnuDocument.TYPE_CHECKBOX:
			var next := not _driver.is_widget_checked(id)
			_driver.set_widget_checked(id, next)
			_driver.play_widget_state_sound(id, "SELECTED")
			_driver.activate(id)
		MnuDocument.TYPE_RADIO:
			_driver.select_radio(id)
			_driver.play_widget_state_sound(id, "SELECTED")
			_driver.activate(id)
		MnuDocument.TYPE_COMBO:
			_driver.play_widget_state_sound(id, "SELECTED")
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
				_driver.spin_cycle(id, 1)
			elif arrow == 2:
				_driver.spin_cycle(id, -1)
		MnuDocument.TYPE_EDIT:
			focus_edit(id)
		MnuDocument.TYPE_TABLE:
			var row := _frame.table_row_at(index, position)
			if row >= 0:
				_table_click(id, row)
		_:
			# Generic containers: actions still dispatch (authored WINDOW
			# widgets carry SCREEN jumps in shipped menus).
			if _doc.get_widget_actions(id).size() > 0:
				_driver.play_widget_state_sound(id, "SELECTED")
				_driver.activate(id)


func _list_click(id: int, kind: int, row: int) -> void:
	var now := Time.get_ticks_msec()
	var double := id == _last_click_id and row == _last_click_row \
			and now - _last_click_ms <= DOUBLE_CLICK_MS
	_last_click_id = id
	_last_click_row = row
	_last_click_ms = 0 if double else now
	if kind == MnuDocument.TYPE_MULTI:
		var additive := Input.is_key_pressed(KEY_CTRL)
		var selected := _driver.selected_set(id) if additive else PackedInt32Array()
		if selected.has(row):
			var kept := PackedInt32Array()
			for r in selected:
				if r != row:
					kept.append(r)
			selected = kept
		else:
			selected.append(row)
		_driver.set_selected_set(id, selected)
	_driver.select_row(id, row)  # emits the "list"/"multi" value change
	_driver.play_widget_state_sound(id, "SELECTED")
	if double:
		_driver.list_activated.emit(id, row)


func _table_click(id: int, row: int) -> void:
	var now := Time.get_ticks_msec()
	var double := id == _last_click_id and row == _last_click_row \
			and now - _last_click_ms <= DOUBLE_CLICK_MS
	_last_click_id = id
	_last_click_row = row
	_last_click_ms = 0 if double else now
	var multiselect := _doc.is_widget_multiselect(id)
	_driver.table_select_row(id, row, multiselect and Input.is_key_pressed(KEY_CTRL))
	_driver.play_widget_state_sound(id, "SELECTED")
	if double:
		_driver.list_activated.emit(id, row)





# --- Combo popups ---

func _open_combo_popup(id: int) -> void:
	# One dropdown per menu: opening one closes the previous
	# [orig: g_ui_active_combo_wnd @ 0x31C16D0, single-open toggle @ 0x65c210].
	close_active_combo_popup()
	_open_combo_id = id
	var index := _driver.frame_index(id)
	if index >= 0:
		_frame.set_widget_popup_open(index, true)


func close_active_combo_popup() -> void:
	if _open_combo_id < 0:
		return
	var index := _driver.frame_index(_open_combo_id)
	if index >= 0:
		_frame.set_widget_popup_open(index, false)
		_frame.set_widget_hover_item(index, -1)
	_open_combo_id = -1


func is_combo_popup_open(id: int) -> bool:
	return _open_combo_id == id


func _combo_select(id: int, row: int) -> void:
	_driver.select_row(id, row)
	_driver.play_widget_state_sound(id, "SELECTED")


# --- Spin lists ---







# --- Edit focus + keyboard ---

func focus_edit(id: int) -> void:
	# Click focuses unless read-only [orig: edit_widget_handle_input_event
	# @ 0x661510 — g_ui_focus_wnd = this unless widget[194]].
	if (_doc.get_widget_flags(id) & MnuDocument.FLAG_READONLY) != 0:
		return
	if _focus_id == id:
		return
	_clear_edit_focus()
	_focus_id = id
	var index := _driver.frame_index(id)
	if index >= 0:
		_frame.set_widget_focused(index, true)
		if _frame.get_widget_caret(index) < 0:
			_frame.set_widget_caret(index, _driver.get_widget_text(id).length())


func _clear_edit_focus() -> void:
	if _focus_id < 0:
		return
	var id := _focus_id
	_focus_id = -1
	var index := _driver.frame_index(id)
	if index >= 0:
		_frame.set_widget_focused(index, false)
		# Persist the edited text for cross-screen reads.
		_driver.remember_widget_text(id, _frame.get_widget_text(index))
	_driver.emit_edit_changed(id)


func get_focused_widget() -> int:
	return _focus_id





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
	var index := _driver.frame_index(_focus_id)
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
			_driver.play_widget_state_sound(id, "SELECTED")
		elif result == MenuFrame.EDIT_RESULT_CHANGED:
			_driver.emit_edit_changed(id)
		return true
	var unicode := event.get_unicode()
	if unicode > 0:
		if _frame.edit_char(index, unicode):
			_driver.emit_edit_changed(id)
		return true
	return false


func _trigger_hotkey_target(index: int) -> bool:
	# A disabled target consumes the key without firing (prevents a later
	# same-key widget firing through a disabled modal); an actionless match
	# is still consumed — actionless named controls are the retail Command
	# seam the shell wires by name.
	if _frame.is_widget_disabled(index):
		return true
	var id := _driver.id_at_index(index)
	if id < 0:
		return true
	if _driver.widget_kind_of(id) == MnuDocument.TYPE_EDIT:
		focus_edit(id)
		return true
	var rect := _frame.widget_rect(index)
	_activate_widget(id, index, (rect.position + rect.size * 0.5) * _driver.design_scale())
	return true
