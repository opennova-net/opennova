extends GameProbe

## loading_screen_stage: the mission loading screen and the SP start-mission
## splash on a probe stage, captured from the stage itself (self-readback, no
## OS capture). Two modes:
##   session_hold    the MP session composite (server name, mission, game-type
##                   line, custom text) held at a fixed progress for
##                   `hold_seconds`, captured, then the SP splash raised over
##                   the same mission and captured
##                   [orig: render_loading_screen @ 0x521d10 /
##                   LoadingScreen_UpdateAndPresent @ 0x586be0 /
##                   show_start_mission_splash @ 0x520820]
##   splash_capture  the splash over retail briefing art at both blink
##                   phases, then a synthesized key press and the dismissed
##                   state [orig: show_start_mission_splash @ 0x520820]
## The mounted root (the launch's) must carry the mission's sidecar art; the
## shell's menu has registered gametext already. Needs a window.

const STAGE_SIZE := Vector2i(960, 540)
const SESSION_MISSION := "00TRg.bms"
const SPLASH_MISSION := "00TRa.bms"
const SPLASH_SETTLE_SECONDS := 0.8
const DISMISS_TIMEOUT_SECONDS := 3.0

var _ctx: ProbeContext
var _stage: ProbeStage
var _screen: LoadingScreen
var _out_dir := ""


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var mode := String(ctx.args.get("mode", "session_hold")).strip_edges().to_lower()
	_out_dir = ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	var root := ctx.resource_root()
	if root == null:
		return ProbeVerdict.failed("the shell has no mounted resource root")
	_stage = ProbeStage.create(ctx, STAGE_SIZE, ctx.viewport(), true)
	_screen = LoadingScreen.new()
	_stage.add_child(_screen)
	_screen.size = Vector2(STAGE_SIZE)
	# The schema fills `mission` with "" when absent; each mode has its own
	# retail default.
	var mission := String(ctx.args.get("mission", "")).strip_edges()
	match mode:
		"session_hold":
			return await _session_hold(root, float(ctx.args.get("hold_seconds", 6.0)),
					mission if not mission.is_empty() else SESSION_MISSION)
		"splash_capture":
			return await _splash_capture(root,
					mission if not mission.is_empty() else SPLASH_MISSION)
	return ProbeVerdict.failed("unknown mode %s" % mode)


func _session_hold(root: ResourceRoot, hold_seconds: float, mission: String) -> ProbeVerdict:
	_screen.setup(root, LoadingScreenInfo.make(mission, true, "OPENNOVA HOST",
			"Weapons Training: M203", 0x10010,  # AAS -> LTGT_AAS
			"Welcome to the OpenNova test server. Play fair and have fun."))
	_screen.set_progress(45)
	var deadline := Time.get_ticks_msec() + int(hold_seconds * 1000.0)
	while Time.get_ticks_msec() < deadline and not _ctx.cancelled:
		_screen.present()  # throttled; the bar creeps toward reported + 10
		await _ctx.tree.process_frame
	var hold_path := await _capture("session_hold.png")
	if hold_path.is_empty():
		return ProbeVerdict.failed("the session composite produced no image")

	# Retail's splash runs on the not-in-session screen; rebuild as the SP
	# composite (image only) before raising it so the eyeball matches
	# [orig: the splash draws over the SP background @ 0x5209ab].
	_screen.setup(root, LoadingScreenInfo.for_mission(mission))
	if not _screen.begin_start_mission_splash(root):
		return ProbeVerdict.failed("the start-mission splash failed to raise")
	await _ctx.wait_ms(int(SPLASH_SETTLE_SECONDS * 1000.0))
	var splash_path := await _capture("splash.png")
	if splash_path.is_empty():
		return ProbeVerdict.failed("the splash produced no image")
	return ProbeVerdict.passed("session composite held %.1f s, splash raised" % hold_seconds,
			{"session_hold": hold_path, "splash": splash_path, "splash_active": _screen.is_splash_active()})


func _splash_capture(root: ResourceRoot, mission: String) -> ProbeVerdict:
	_screen.setup(root, LoadingScreenInfo.for_mission(mission))
	if not _screen.has_custom_background():
		return ProbeVerdict.failed("%s resolved no sidecar art" % mission)
	if not _screen.begin_start_mission_splash(root):
		return ProbeVerdict.failed("the start-mission splash failed to raise")
	if not _screen.has_splash_arrow():
		return ProbeVerdict.failed("newarow1.tga did not decode")
	# Let the first frames render, then drive the arrow anchor through the
	# real motion-event path so the arrow leg is visible regardless of focus.
	await _ctx.wait_ms(int(SPLASH_SETTLE_SECONDS * 1000.0))
	var motion := InputEventMouseMotion.new()
	motion.position = Vector2(420, 320)
	_stage.push_input(motion)

	var on_path := await _capture_at_blink_phase(true, "splash_on.png")
	if on_path.is_empty():
		return ProbeVerdict.failed("the blink-on phase produced no capture")
	var off_path := await _capture_at_blink_phase(false, "splash_off.png")
	if off_path.is_empty():
		return ProbeVerdict.failed("the blink-off phase produced no capture")

	var press := InputEventKey.new()
	press.keycode = KEY_SPACE
	press.pressed = true
	_stage.push_input(press)
	var deadline := Time.get_ticks_msec() + int(DISMISS_TIMEOUT_SECONDS * 1000.0)
	var settle_until := Time.get_ticks_msec() + 500
	while not _ctx.cancelled:
		await _ctx.tree.process_frame
		if Time.get_ticks_msec() >= settle_until and not _screen.is_splash_active():
			break
		if Time.get_ticks_msec() >= deadline:
			return ProbeVerdict.failed("dismissal never completed")
	var dismissed_path := await _capture("splash_dismissed.png")
	if dismissed_path.is_empty():
		return ProbeVerdict.failed("the dismissed state produced no image")
	return ProbeVerdict.passed("splash on/off and dismissed captured", {
		"splash_on": on_path, "splash_off": off_path, "splash_dismissed": dismissed_path})


## Wait for the blink phase the mask names while the splash is up, then capture.
func _capture_at_blink_phase(want_on: bool, file_name: String) -> String:
	var deadline := Time.get_ticks_msec() + int(DISMISS_TIMEOUT_SECONDS * 1000.0)
	while not _ctx.cancelled and Time.get_ticks_msec() < deadline:
		var phase_on := (Time.get_ticks_msec() & HudPos.SPLASH_BLINK_MASK_MS) != 0
		if phase_on == want_on and _screen.is_splash_active():
			return await _capture(file_name)
		await _ctx.tree.process_frame
	return ""


func _capture(file_name: String) -> String:
	var image := await _stage.capture_image(_ctx.tree)
	if image == null:
		return ""
	var path := _out_dir.path_join(file_name)
	if image.save_png(path) != OK:
		return ""
	_ctx.artifact(file_name.get_basename(), path, "png")
	return path
