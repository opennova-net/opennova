extends GutTest

# KeyboardPass, the input frame's keyboard pass over the device's key queue
# (the native KeyEventQueue), over recording legs: one drain is the keyboard pass
# in key order (the open chat line first, then the special-key chain's inline
# legs and its queued deploy picks, then the binding scan's row events, the
# escape row's among them), the picks and the row events queued in key order for
# the update's drain, whose first input reset ends it (engine
# inmatch::Role::drain_input_events; ctest inmatch_tick_input_source pins it;
# engine world::pass_through_close_axis and controls::KeyEventQueue carry the
# witness).


# The shell's ring legs, recorded: the chat line takes every key while open (its
# Escape closes it), the special-key chain takes PgUp / PgDn inline and queues X
# and SPACE as picks (the default spawn 0 and the auto-team 0xFFFE), the scan's
# row events queue in the same order.
class RecordingLegs:
	extends RefCounted
	var chat_open := false
	var calls: Array[String] = []
	var release_passes := 0
	var release_pass_first := false

	# The release pass: counted, and whether it ran ahead of every other leg.
	func ring_release_pass() -> void:
		release_passes += 1
		release_pass_first = calls.is_empty()

	func ring_chat_key(key: InputEventKey) -> bool:
		if not chat_open:
			return false
		calls.append("chat %s" % _name(key.keycode))
		if key.keycode == KEY_ESCAPE:
			chat_open = false
		return true

	func ring_special_key(key: InputEventKey) -> bool:
		match key.keycode:
			KEY_PAGEUP, KEY_PAGEDOWN:
				calls.append("page %s" % _name(key.keycode))
				return true
			KEY_X:
				calls.append("pick 0")
				return true
			KEY_SPACE:
				calls.append("pick %d" % 0xFFFE)
				return true
		return false

	# The binding scan: Escape's VK fires the escape row (code 18), F6's huddetail
	# (code 19).
	func ring_binding_code(scan_code: int) -> int:
		if scan_code == 0x1B:
			return KeyboardPass.ESCAPE_CODE
		return 19 if scan_code == 0x75 else -1

	func ring_row_event(_key: InputEventKey, code: int, _scan_code: int) -> void:
		calls.append("row %d" % code)

	func _name(keycode: int) -> String:
		match keycode:
			KEY_X:
				return "x"
			KEY_ESCAPE:
				return "esc"
			KEY_PAGEUP:
				return "pgup"
			KEY_PAGEDOWN:
				return "pgdn"
		return str(keycode)


# The cases feed synthetic key events only: Godot's global Input holds no key.
func after_each() -> void:
	for keycode in [KEY_ESCAPE, KEY_X, KEY_SPACE, KEY_PAGEDOWN, KEY_CTRL, KEY_SHIFT]:
		assert_false(Input.is_key_pressed(keycode), "no key stays held: %d" % keycode)


func _ring(keys: Array) -> KeyboardPass:
	var ring := KeyboardPass.new()
	for keycode in keys:
		var key := InputEventKey.new()
		key.keycode = keycode
		key.pressed = true
		ring.push(key)
	return ring


func _drained(keys: Array, legs: RecordingLegs) -> Array[String]:
	var ring := _ring(keys)
	ring.drain(legs)
	assert_true(ring.is_empty(), "a drain takes every waiting key")
	return legs.calls


# Every key in key order: the escape row's event queues at its key, the special
# key behind it runs inline at its own.
# [orig: Input_ProcessKeyboardEvents @0x49d1f0 - Input_HandleSpecialKeys per
#  dequeued key (the call @0x49d2fb), the scan's Input_QueueEvent @0x49d42f /
#  @0x49d488]
func test_the_pass_takes_each_key_in_key_order() -> void:
	var calls := _drained([KEY_ESCAPE, KEY_PAGEDOWN], RecordingLegs.new())
	assert_eq(calls.size(), 2)
	assert_eq(calls[0] if calls.size() > 0 else "", "row 18")
	assert_eq(calls[1] if calls.size() > 1 else "", "page pgdn")


# The release pass runs once per drain, ahead of the first dequeue (its latched
# keys' releases queue before the pass's own events), and with no key waiting too.
# [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the release pass @0x49d22f..
#  0x49d2cc ahead of the first Input_DequeueKeyEvent @0x49d2d2]
func test_the_release_pass_runs_ahead_of_the_dequeue() -> void:
	var legs := RecordingLegs.new()
	var calls := _drained([KEY_PAGEDOWN, KEY_ESCAPE], legs)
	assert_eq(calls.size(), 2)
	assert_eq(legs.release_passes, 1, "one release pass per drain")
	assert_true(legs.release_pass_first, "ahead of the keys")
	var idle := RecordingLegs.new()
	KeyboardPass.new().drain(idle)
	assert_eq(idle.release_passes, 1, "an empty queue runs the release pass too")


# The deploy picks and the escape row's event queue in key order, every one of
# them: the drain dispatches them in that order, and the first whose case resets
# the input ends it (face (a)'s Escape NO: a pick ahead of the Escape drops it).
# [orig: Input_HandleSpecialKeys @0x49c5c0 - Input_QueueEvent(12, ...) @0x49ca14 /
#  @0x49ca73; Input_ProcessFrame @0x49d520 - the drain @0x49d599..0x49d5e7]
func test_the_picks_and_the_escape_queue_in_key_order() -> void:
	var calls := _drained([KEY_X, KEY_ESCAPE], RecordingLegs.new())
	assert_eq_deep(calls, ["pick 0", "row 18"])
	calls = _drained([KEY_ESCAPE, KEY_X], RecordingLegs.new())
	assert_eq_deep(calls, ["row 18", "pick 0"])
	calls = _drained([KEY_SPACE, KEY_X], RecordingLegs.new())
	assert_eq_deep(calls, ["pick 65534", "pick 0"])


# An open chat line takes the keys, its Escape closing it; the keys behind
# meet the special-key chain again.
func test_an_open_chat_line_takes_the_keys_until_it_closes() -> void:
	var legs := RecordingLegs.new()
	legs.chat_open = true
	var calls := _drained([KEY_X, KEY_ESCAPE, KEY_X], legs)
	assert_eq(calls.size(), 3)
	assert_eq(calls[0] if calls.size() > 0 else "", "chat x")
	assert_eq(calls[1] if calls.size() > 1 else "", "chat esc")
	assert_eq(calls[2] if calls.size() > 2 else "", "pick 0")


# A cleared ring (a screen's close) drains nothing.
func test_a_cleared_ring_drains_nothing() -> void:
	var legs := RecordingLegs.new()
	var ring := _ring([KEY_X, KEY_ESCAPE])
	assert_false(ring.is_empty())
	ring.clear()
	ring.drain(legs)
	assert_true(legs.calls.is_empty())


func _key_down(keycode: Key) -> InputEventKey:
	var key := InputEventKey.new()
	key.keycode = keycode
	key.pressed = true
	return key


# The native queue (engine controls::KeyEventQueue): each key-down keeps its event
# beside its slot, in order, and the binding scan's key is the VK, keypad Enter's
# 269 (the VK 13 slot with the extended bit).
func test_the_queue_keeps_each_event_and_its_scan_code() -> void:
	var keys := KeyEventQueue.new()
	var escape := _key_down(KEY_ESCAPE)
	var enter := _key_down(KEY_KP_ENTER)
	keys.push_key(escape, false, false)
	keys.push_key(enter, false, false)
	assert_eq(keys.pop_key(), escape, "the first key-down first")
	assert_eq(keys.last_scan_code(), 0x1B)
	assert_eq(keys.pop_key(), enter)
	assert_eq(keys.last_scan_code(), 269, "keypad Enter scans as 269")
	assert_null(keys.pop_key(), "the read index met the write index")
	assert_true(keys.is_empty())


# The ring has no full test: 128 unread key-downs read as an empty queue, and the
# next one alone reads from the read index on; the reset drops a waiting key.
func test_128_unread_key_downs_read_as_an_empty_queue() -> void:
	var keys := KeyEventQueue.new()
	for _i in range(128):
		keys.push_key(_key_down(KEY_A), false, false)
	assert_true(keys.is_empty(), "the write index wrapped onto the read index")
	var x := _key_down(KEY_X)
	keys.push_key(x, false, false)
	assert_eq(keys.pop_key(), x, "the newest key-down reads from the read index on")
	assert_true(keys.is_empty())
	keys.push_key(_key_down(KEY_X), false, false)
	keys.reset()
	assert_null(keys.pop_key(), "the reset drops the waiting key")


# The binding scan over a dequeued key (engine BindingSet::event_row): Escape fires
# the escape row (code 18), a bare F6 huddetail (code 19), under the default table.
# The code is the row id's, not the catalog index's: the port's catalog leaves row
# 66 out, so every row past it (escape 67, ShowFriendly 100, AudioEmote 101, NVG
# 104) sits one index below its id.
func test_the_binding_scan_names_the_row_a_key_fires() -> void:
	var model := ControlsModel.new()
	model.restore_defaults()
	assert_eq(model.event_code(0x1B), KeyboardPass.ESCAPE_CODE)
	assert_eq(model.event_code(0x75), 19, "a bare F6 fires huddetail")
	assert_eq(model.event_code(0x4B), 30, "K fires ShowFriendly")
	assert_eq(model.event_code(0x78), 33, "F9 fires AudioEmote")
	assert_eq(model.event_code(0x4E), 41, "N fires NVG")


# The input router owns one queue per process: the shell's key-downs join it and
# every input reset the shell runs (Input_ResetAllState's Input_ResetKeyQueue)
# empties it.
func test_the_player_presenter_owns_the_queue_its_input_reset_empties() -> void:
	var presenter: LocalPlayerPresenter = autofree(LocalPlayerPresenter.new())
	var keys := presenter.get_key_events()
	assert_not_null(keys)
	assert_eq(presenter.get_key_events(), keys, "one queue")
	presenter.push_key_event(_key_down(KEY_X))
	assert_false(keys.is_empty())
	presenter.reset_input_state()
	assert_true(keys.is_empty(), "the input reset resets the key queue")
	var x := _key_down(KEY_X)
	presenter.push_key_event(x)
	var pass_over := KeyboardPass.new(keys)
	assert_eq(pass_over.take_key(), x, "the shell's pass reads the presenter's queue")


# A key the chain leaves that the scan gives a row queues that row's event for the
# input router in the keyboard pass, once per key-down (a held key's OS repeat
# again), in key order with the escape row's.
func test_another_rows_key_queues_its_row_event_in_the_pass() -> void:
	var calls := _drained([KEY_ESCAPE, KEY_F6, KEY_F6], RecordingLegs.new())
	assert_eq(calls.size(), 3)
	assert_eq(calls[0] if calls.size() > 0 else "", "row 18")
	assert_eq(calls[1] if calls.size() > 1 else "", "row 19")
	assert_eq(calls[2] if calls.size() > 2 else "", "row 19", "the repeat fires the row again")
