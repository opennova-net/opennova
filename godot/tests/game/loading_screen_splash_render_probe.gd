extends Node

## Manual VISUAL regression probe (not collected by GUT — *_probe.gd): raises
## the SP start-mission splash over retail briefing art in a real window and
## captures the VIEWPORT (self-readback — no OS capture) at both blink
## phases, then synthesizes a key press and captures the dismissed state
## [orig: show_start_mission_splash @ 0x520820].
##
## Run windowed:
##   "$GODOT_BIN" --path godot res://tests/game/loading_screen_splash_render_probe.tscn
## Needs OPENNOVA_JO_DIR (or the default retail install path below). Writes
## splash_on.png / splash_off.png / splash_dismissed.png into
## SPLASH_CAPTURE_DIR (or user://). Quits by itself.

const DEFAULT_JO_DIR := "C:/Users/taylor/Desktop/Games/Joint Operations Combined Arms"
const MISSION := "00TRa.bms"

enum Stage { WARMUP, WAIT_ON, WAIT_OFF, DISMISS, WAIT_DISMISSED, DONE }

var _screen: LoadingScreen
var _stage := Stage.WARMUP
var _elapsed := 0.0
var _out_dir := ""


func _ready() -> void:
	var dir := OS.get_environment("OPENNOVA_JO_DIR")
	if dir.is_empty():
		dir = DEFAULT_JO_DIR
	_out_dir = OS.get_environment("SPLASH_CAPTURE_DIR")
	if _out_dir.is_empty():
		_out_dir = ProjectSettings.globalize_path("user://")
	var root := ResourceRoot.new()
	if root.mount_runtime(dir, "", false, "jo") != OK:
		push_error("splash_render_probe: mount failed: %s" % root.get_last_error())
		get_tree().quit(1)
		return
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(root.read_file("gametext.bin")) == OK:
		Strings.register_table("gametext", table)
	_screen = LoadingScreen.new()
	add_child(_screen)
	_screen.setup(root, {"mission_file": MISSION})
	_screen.size = _screen.get_viewport_rect().size
	if not _screen.has_custom_background():
		push_error("splash_render_probe: %s resolved no sidecar art" % MISSION)
		get_tree().quit(1)
		return
	if not _screen.begin_start_mission_splash(root):
		push_error("splash_render_probe: splash failed to raise")
		get_tree().quit(1)
		return
	if not _screen.has_splash_arrow():
		push_error("splash_render_probe: newarow1.tga did not decode")
		get_tree().quit(1)
		return


func _process(delta: float) -> void:
	_elapsed += delta
	if _elapsed < 0.8:
		return  # let the window settle and the first frames render
	var phase_on := (Time.get_ticks_msec() & HudPos.SPLASH_BLINK_MASK_MS) != 0
	match _stage:
		Stage.WARMUP:
			# Drive the arrow anchor through the real motion-event path so the
			# arrow leg is visible in the captures regardless of window focus.
			var mv := InputEventMouseMotion.new()
			mv.position = Vector2(420, 320)
			get_viewport().push_input(mv)
			_stage = Stage.WAIT_ON
		Stage.WAIT_ON:
			if phase_on and _screen.is_splash_active():
				await _capture("splash_on.png")
				_stage = Stage.WAIT_OFF
		Stage.WAIT_OFF:
			if not phase_on and _screen.is_splash_active():
				await _capture("splash_off.png")
				_stage = Stage.DISMISS
		Stage.DISMISS:
			var ev := InputEventKey.new()
			ev.keycode = KEY_SPACE
			ev.pressed = true
			get_viewport().push_input(ev)
			_elapsed = 0.0
			_stage = Stage.WAIT_DISMISSED
		Stage.WAIT_DISMISSED:
			if _elapsed > 0.5 and not _screen.is_splash_active():
				await _capture("splash_dismissed.png")
				_stage = Stage.DONE
				get_tree().quit()
			elif _elapsed > 3.0:
				push_error("splash_render_probe: dismissal never completed")
				get_tree().quit(1)
		Stage.DONE:
			pass


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	var path := _out_dir.path_join(name)
	if image.save_png(path) != OK:
		push_error("splash_render_probe: failed to save %s" % path)
