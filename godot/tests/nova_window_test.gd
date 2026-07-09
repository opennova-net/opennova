extends GutTest

## NovaWindow — the core-engine fullscreen concept. Pure/no-display-server logic:
## the F11 event predicate and the mode read/set/toggle against a bare Window.


func _key(keycode: Key, pressed: bool, echo := false) -> InputEventKey:
	var ev := InputEventKey.new()
	ev.keycode = keycode
	ev.physical_keycode = keycode
	ev.pressed = pressed
	ev.echo = echo
	return ev


func test_is_toggle_event_matches_f11_press_only() -> void:
	assert_true(NovaWindow.is_toggle_event(_key(KEY_F11, true)), "F11 pressed is the toggle")
	assert_false(NovaWindow.is_toggle_event(_key(KEY_F11, false)), "F11 release is not")
	assert_false(NovaWindow.is_toggle_event(_key(KEY_F11, true, true)), "auto-repeat echo is not")
	assert_false(NovaWindow.is_toggle_event(_key(KEY_F10, true)), "a different key is not")
	assert_false(NovaWindow.is_toggle_event(InputEventMouseMotion.new()), "a non-key event is not")


func test_null_window_is_safe() -> void:
	assert_false(NovaWindow.is_fullscreen(null), "null window reads as not-fullscreen")
	NovaWindow.set_fullscreen(null, true)  # must not crash
	assert_false(NovaWindow.toggle_fullscreen(null), "toggling null returns false")


func test_mode_read_and_toggle() -> void:
	var w := Window.new()
	w.mode = Window.MODE_WINDOWED
	assert_false(NovaWindow.is_fullscreen(w), "windowed reads false")

	var now := NovaWindow.toggle_fullscreen(w)
	assert_true(now, "toggle from windowed returns true")
	assert_eq(w.mode, Window.MODE_FULLSCREEN, "toggle set the fullscreen mode")

	assert_false(NovaWindow.toggle_fullscreen(w), "toggle back returns false")
	assert_eq(w.mode, Window.MODE_WINDOWED, "toggle back set windowed")

	# Idempotent set: asking for the state it is already in is a no-op.
	w.mode = Window.MODE_EXCLUSIVE_FULLSCREEN
	assert_true(NovaWindow.is_fullscreen(w), "exclusive fullscreen also reads true")
	NovaWindow.set_fullscreen(w, true)
	assert_eq(w.mode, Window.MODE_EXCLUSIVE_FULLSCREEN, "already-fullscreen set left the mode alone")

	w.free()
