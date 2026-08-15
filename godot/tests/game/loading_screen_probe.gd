extends Node

## Manual VISUAL probe (not collected by GUT — *_probe.gd): holds the mission
## loading screen on screen with the MP session text and a fixed progress so
## the composite can be eyeballed against retail, then flips to the SP
## start-mission splash phase (blinking centered LT_Continue + the cursor
## arrow tracking the mouse; any key or mouse button ends the splash phase)
## [orig: render_loading_screen @ 0x521d10 / LoadingScreen_UpdateAndPresent
## @ 0x586be0 / show_start_mission_splash @ 0x520820].
##
## Run windowed:
##   "$GODOT_BIN" --path godot res://tests/game/loading_screen_probe.tscn
## Needs OPENNOVA_JO_DIR pointing at a retail PFF install. Quits by itself.

const HOLD_SECONDS := 6.0
const SPLASH_MAX_SECONDS := 20.0
const MISSION := "00TRg.bms"

var _screen: LoadingScreen
var _root: ResourceRoot
var _elapsed := 0.0
var _splash_phase := false


func _ready() -> void:
	var dir := OS.get_environment("OPENNOVA_JO_DIR")
	if dir.is_empty():
		push_error("loading_screen_probe: set OPENNOVA_JO_DIR to a retail install")
		get_tree().quit(1)
		return
	_root = ResourceRoot.new()
	if _root.mount_runtime(dir, "", false, "jo") != OK:
		push_error("loading_screen_probe: mount failed: %s" % _root.get_last_error())
		get_tree().quit(1)
		return
	# The gametext table the game-type line resolves through (the shell's menu
	# normally registers it) [orig: g_TextGameText loads gametext.bin @ 0x4a6cd0].
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(_root.read_file("gametext.bin")) == OK:
		Strings.register_table("gametext", table)
	_screen = LoadingScreen.new()
	add_child(_screen)
	_screen.setup(_root, {
		"mission_file": MISSION,
		"in_session": true,
		"server_name": "OPENNOVA HOST",
		"mission_name": "Weapons Training: M203",
		"game_type": 0x10010,  # AAS -> LTGT_AAS
		"custom_text": "Welcome to the OpenNova test server. Play fair and have fun.",
	})
	_screen.size = _screen.get_viewport_rect().size
	_screen.set_progress(45)


func _process(delta: float) -> void:
	_elapsed += delta
	if _screen == null:
		return
	if not _splash_phase:
		_screen.present()  # throttled; the bar creeps toward reported + 10
		if _elapsed >= HOLD_SECONDS:
			_begin_splash_phase()
		return
	if _elapsed >= SPLASH_MAX_SECONDS:
		get_tree().quit()


# Retail's splash runs on the not-in-session screen; rebuild as the SP
# composite (image only) before raising it so the eyeball matches
# [orig: the splash draws over the SP background @ 0x5209ab].
func _begin_splash_phase() -> void:
	_splash_phase = true
	_elapsed = HOLD_SECONDS
	_screen.setup(_root, {"mission_file": MISSION})
	if not _screen.begin_start_mission_splash(_root):
		push_error("loading_screen_probe: splash failed to raise")
		get_tree().quit(1)
		return
	Input.set_mouse_mode(Input.MOUSE_MODE_HIDDEN)
	_screen.splash_dismissed.connect(func() -> void:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
		get_tree().quit())
