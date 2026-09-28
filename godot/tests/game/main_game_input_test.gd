extends GutTest

# The shell's gameplay key routing over the REAL stack (ADR 0043 rule 11): the
# packed shell (WorldFixture.boot_shell), the minimal mission started through
# the real front end, and every key delivered through the process input path
# (Input.parse_input_event -> MainGame._unhandled_key_input, plus the
# presenters' polled binding rows sampled in the shell frame). Effects are
# read back through the public presenter / world seams: the HUD presenter's
# friendly-tag mode, declutter level and color index, the sim's local view
# (NVG), and the shell's own state.

const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH
# The huddetail cycle persists its level here like retail's config token
# round trip (GameHudPresenter.HUD_COLOR_CONFIG_PATH): saved and restored
# around every case so a cycled level never leaks into the user's settings.
const HUD_CONFIG_PATH := GameHudPresenter.HUD_COLOR_CONFIG_PATH

var _config: TestFs.Snapshot
var _hud_config: TestFs.Snapshot
var _temp_dir := ""
var _shell: MainGame = null


func before_each() -> void:
	_config = TestFs.snapshot(STATE_CONFIG_PATH)
	_hud_config = TestFs.snapshot(HUD_CONFIG_PATH)
	# A shell booted here sees only the launch flags a case sets through the
	# override (the GUT process carries none; no sibling leftovers).
	LaunchFlags.set_args_override(PackedStringArray([]))
	ResourceDirSettings.set_expansion("")


func after_each() -> void:
	await WorldFixture.release_shell(self, _shell)
	_shell = null
	if not _temp_dir.is_empty():
		TestFs.remove_dir_recursive(_temp_dir)
		_temp_dir = ""
	LaunchFlags.clear_args_override()
	_config.restore()
	_hud_config.restore()


# The shell in the minimal mission with gameplay input live (null when the
# boot or the load failed; the case then returns after its own assertions).
func _booted_in_world() -> MainGame:
	_shell = await WorldFixture.boot_shell(self)
	_temp_dir = WorldFixture.last_shell_dir()
	if _shell == null:
		return null
	var loaded: bool = await WorldFixture.start_shell_mission(self, _shell)
	assert_true(loaded, "the minimal mission loads through the real front end")
	if not loaded:
		return null
	# One shell frame ticks the HUD presenter, which builds the lazy overlay the
	# HUD rows write through and arms the binding-row edge latches.
	await get_tree().process_frame
	assert_true(_shell.is_gameplay_input_active(), "gameplay input is live in the world")
	assert_true(_shell.get_player_presenter().has_player(),
			"the SP spawn gives the player router a local player")
	return _shell


func _key_event(keycode: Key, pressed: bool, shift_pressed := false) -> InputEventKey:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.pressed = pressed
	event.shift_pressed = shift_pressed
	return event


# One press-and-release of `keycode` through Input: the same device edge feeds
# MainGame._unhandled_key_input (the event) and the presenters' polled binding
# rows (Input's physical key state, sampled in the next shell frames).
func _tap(keycode: Key) -> void:
	Input.parse_input_event(_key_event(keycode, true))
	await get_tree().process_frame
	await get_tree().process_frame
	Input.parse_input_event(_key_event(keycode, false))
	await get_tree().process_frame
	await get_tree().process_frame


# The Shift chord as a real keyboard produces it: Shift down, the key down
# with the modifier flag, the key up, Shift up.
func _tap_with_shift(keycode: Key) -> void:
	Input.parse_input_event(_key_event(KEY_SHIFT, true, true))
	await get_tree().process_frame
	Input.parse_input_event(_key_event(keycode, true, true))
	await get_tree().process_frame
	await get_tree().process_frame
	Input.parse_input_event(_key_event(keycode, false, true))
	await get_tree().process_frame
	Input.parse_input_event(_key_event(KEY_SHIFT, false, false))
	await get_tree().process_frame
	await get_tree().process_frame


func test_friendly_tags_use_f_while_n_reaches_the_nvg_router() -> void:
	var shell := await _booted_in_world()
	if shell == null:
		return
	var hud: GameHudPresenter = shell.get_hud_presenter()
	var world: GameWorld = shell.get_world()
	var tags_before := hud.friendly_tag_mode()
	var nvg_before: bool = world.local_player_view().nvg_active

	await _tap(KEY_F)
	assert_eq(hud.friendly_tag_mode(), HudOverlay.next_friendly_tag_mode(tags_before),
			"F cycles the retail friendly-tag mode")
	# The shell consumes the friendly-tag key: the player router's own state
	# (the NVG view it toggles on N) is untouched.
	assert_eq(world.local_player_view().nvg_active, nvg_before,
			"the shell consumes the friendly-tag key")

	await _tap(KEY_N)
	assert_eq(hud.friendly_tag_mode(), HudOverlay.next_friendly_tag_mode(tags_before),
			"N does not cycle friendly tags")
	assert_ne(world.local_player_view().nvg_active, nvg_before,
			"N reaches the local-player router for NVG")


func test_h_is_not_a_hud_key_and_reaches_the_player_router() -> void:
	# Retail H is only the secondary `pause` binding (SP-only); there is no
	# HUD-visibility toggle and no H color mapping.
	# [orig: catalog row 70 vk2 0x48; case 25 @0x49b520]
	var shell := await _booted_in_world()
	if shell == null:
		return
	var hud: GameHudPresenter = shell.get_hud_presenter()
	var world: GameWorld = shell.get_world()
	var tags_before := hud.friendly_tag_mode()
	var detail_before := hud.hud_detail_level()
	var color_before := hud.hud_color_index()
	var nvg_before: bool = world.local_player_view().nvg_active

	await _tap(KEY_H)
	assert_eq(hud.friendly_tag_mode(), tags_before, "H drives no HUD presenter action")
	assert_eq(hud.hud_detail_level(), detail_before, "H is not a declutter key")
	assert_eq(hud.hud_color_index(), color_before, "H has no color mapping")
	# H falls through the shell to the player router, whose gameplay legs
	# (B/N/+/-/Z/X/C) carry no H arm: the observable consequence is that no
	# shell leg (pause, picker, tools, armory) claims the key either, so play
	# continues untouched.
	assert_eq(shell.shell_state_name(), "world",
			"H falls through the shell to the player router")
	assert_true(shell.is_gameplay_input_active(), "nothing on the shell pauses on H")
	assert_eq(world.local_player_view().nvg_active, nvg_before,
			"the router's NVG leg is N, not H")


func test_plain_f6_reaches_the_binding_rows_while_shift_f6_is_debug_pick() -> void:
	var shell := await _booted_in_world()
	if shell == null:
		return
	var hud: GameHudPresenter = shell.get_hud_presenter()
	var detail_before := hud.hud_detail_level()
	var color_before := hud.hud_color_index()

	await _tap(KEY_F6)
	assert_eq(hud.hud_detail_level(), HudOverlay.next_hud_detail_level(detail_before),
			"Plain F6 remains available to the gameplay HUDDETAIL router.")
	assert_eq(hud.get_game_hud().get_hud_detail_level(), hud.hud_detail_level(),
			"the cycle restamps the built overlay")
	assert_eq(hud.hud_color_index(), color_before,
			"hudcolor shares F6 and ships shadowed by the huddetail row (D-CTRL-4)")
	assert_null(shell.find_child("PickToast", true, false),
			"the bare key never reaches the debug picker")

	var detail_after := hud.hud_detail_level()
	await _tap_with_shift(KEY_F6)
	assert_eq(hud.hud_detail_level(), detail_after,
			"Shift+F6 is consumed by the shell's debug picker.")
	assert_eq(hud.hud_color_index(), color_before,
			"a chorded press never cycles a HUD row")
	assert_not_null(shell.find_child("PickToast", true, false),
			"the chord landed on the debug picker (every attempt confirms with a toast)")
