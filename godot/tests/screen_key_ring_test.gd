extends GutTest

# ScreenKeyRing, the key ring under a pass-through screen (death.mnu's DEATH,
# stat.mnu's STAT), over recording legs: one drain is the input frame's keyboard
# pass in key order (the open chat line first, then the special-key chain's
# inline legs and its queued deploy picks, then the binding scan's Escape event),
# then the event drain in queue order, which the first input reset ends (engine
# world::pass_through_close_axis carries the witness).


# The shell's ring legs, recorded: the chat line takes every key while open (its
# Escape closes it), the special-key chain takes PgUp / PgDn inline and queues X
# and SPACE as picks (the default spawn 0 and the auto-team 0xFFFE), every pick's
# case resets the input and the Escape action's does when `escape_resets`.
class RecordingLegs:
	extends RefCounted
	var chat_open := false
	var escape_resets := true
	var calls: Array[String] = []
	var _reset := false

	func ring_chat_key(key: InputEventKey) -> bool:
		if not chat_open:
			return false
		calls.append("chat %s" % _name(key.keycode))
		if key.keycode == KEY_ESCAPE:
			chat_open = false
		return true

	func ring_special_key(key: InputEventKey, events: Array[int]) -> bool:
		match key.keycode:
			KEY_PAGEUP, KEY_PAGEDOWN:
				calls.append("page %s" % _name(key.keycode))
				return true
			KEY_X:
				events.append(0)
				return true
			KEY_SPACE:
				events.append(0xFFFE)
				return true
		return false

	func ring_escape() -> void:
		calls.append("escape")
		_reset = escape_resets

	func ring_pick(param: int) -> void:
		calls.append("pick %d" % param)
		_reset = true

	func ring_reset_ran() -> bool:
		var ran := _reset
		_reset = false
		return ran

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


func _ring(keys: Array) -> ScreenKeyRing:
	var ring := ScreenKeyRing.new()
	for keycode in keys:
		var key := InputEventKey.new()
		key.keycode = keycode
		key.pressed = true
		ring.push(key)
	return ring


func _drained(keys: Array, legs: RecordingLegs) -> Array[String]:
	var ring := _ring(keys)
	ring.drain(legs)
	assert_eq(ring.size(), 0, "a drain takes every waiting key")
	return legs.calls


# The special keys run inline in the keyboard pass, ahead of every queued event,
# a key behind the Escape included.
func test_inline_special_keys_run_ahead_of_the_queued_escape() -> void:
	var calls := _drained([KEY_ESCAPE, KEY_PAGEDOWN], RecordingLegs.new())
	assert_eq(calls.size(), 2)
	assert_eq(calls[0] if calls.size() > 0 else "", "page pgdn")
	assert_eq(calls[1] if calls.size() > 1 else "", "escape")


# A deploy key waiting ahead of an Escape: its pick's reset drops the Escape
# event (face (a)'s Escape NO).
func test_a_pick_ahead_of_an_escape_drops_it() -> void:
	var calls := _drained([KEY_X, KEY_ESCAPE], RecordingLegs.new())
	assert_eq(calls.size(), 1)
	assert_eq(calls[0] if calls.size() > 0 else "", "pick 0")


# An Escape ahead of a deploy key: the Escape action's reset drops the pick.
func test_an_escape_ahead_of_a_pick_drops_it() -> void:
	var calls := _drained([KEY_ESCAPE, KEY_X], RecordingLegs.new())
	assert_eq(calls.size(), 1)
	assert_eq(calls[0] if calls.size() > 0 else "", "escape")


# Two Escapes run one action; with no local entity the action resets nothing
# and the drain goes on to the pick.
func test_the_escape_actions_reset_ends_the_drain_only_when_it_runs() -> void:
	var calls := _drained([KEY_ESCAPE, KEY_ESCAPE], RecordingLegs.new())
	assert_eq(calls.size(), 1, "the first Escape's reset drops the second")
	var no_entity := RecordingLegs.new()
	no_entity.escape_resets = false
	calls = _drained([KEY_ESCAPE, KEY_X], no_entity)
	assert_eq(calls.size(), 2)
	assert_eq(calls[1] if calls.size() > 1 else "", "pick 0",
			"a reset that did not run leaves the queue")


# Two deploy keys queue two picks; the first one's reset drops the second.
func test_the_first_pick_drops_the_next() -> void:
	var calls := _drained([KEY_SPACE, KEY_X], RecordingLegs.new())
	assert_eq(calls.size(), 1)
	assert_eq(calls[0] if calls.size() > 0 else "", "pick 65534")


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
	assert_eq(ring.size(), 2)
	ring.clear()
	ring.drain(legs)
	assert_true(legs.calls.is_empty())
