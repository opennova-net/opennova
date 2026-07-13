extends Node

## Manual VISUAL probe (not collected by GUT — *_probe.gd): holds the mission
## loading screen on screen with the MP session text and a fixed progress so
## the composite can be eyeballed against retail
## [orig: render_loading_screen @ 0x521d10 / LoadingScreen_UpdateAndPresent @ 0x586be0].
##
## Run windowed:
##   "$GODOT_BIN" --path godot res://tests/game/loading_screen_probe.tscn
## Needs OPENNOVA_JO_DIR pointing at a retail PFF install. Quits by itself.

const HOLD_SECONDS := 6.0
const MISSION := "00TRg.bms"

var _screen: NovaLoadingScreen
var _elapsed := 0.0


func _ready() -> void:
	var dir := OS.get_environment("OPENNOVA_JO_DIR")
	if dir.is_empty():
		push_error("loading_screen_probe: set OPENNOVA_JO_DIR to a retail install")
		get_tree().quit(1)
		return
	var root := NovaResourceRoot.new()
	if root.mount_runtime(dir, "", false, "jo") != OK:
		push_error("loading_screen_probe: mount failed: %s" % root.get_last_error())
		get_tree().quit(1)
		return
	# The gametext table the game-type line resolves through (the shell's menu
	# normally registers it) [orig: g_TextGameText loads gametext.bin @ 0x4a6cd0].
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(root.read_file("gametext.bin")) == OK:
		NovaStrings.register_table("gametext", table)
	_screen = NovaLoadingScreen.new()
	add_child(_screen)
	_screen.setup(root, {
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
	if _screen != null:
		_screen.present()  # throttled; the bar creeps toward reported + 10
	if _elapsed >= HOLD_SECONDS:
		get_tree().quit()
