extends GameProbe

## hud_pass_call_site: the live check of the HUD pass's call site (D-NET-NEW-L8-50). Over the
## loaded mission it reads, per state, the frame's verdict (GameWorld.get_last_frame_hud_pass_called,
## engine inmatch::FrameOutcome::hud_pass_called), the HUD draw list's element count and a PNG:
## live play in camera mode 0 (the pass is called), the F1 help held open through the real key
## path (not called), the debug on-foot chase view (camera mode 1 on foot: not called), live play
## again, then, with `kill`, the local player's death (the dead bit: not called). Retail's scene
## frame calls its HUD pass only past those gates (engine inmatch::FrameOutcome::hud_pass_called
## carries the witness). Needs a window.

const HUD_TIMEOUT_MS := 30_000
const SETTLE_MS := 3000
const STATE_FRAMES := 12


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var hud := await _wait_hud(ctx)
	if hud == null:
		return ProbeVerdict.failed("HudOverlay never appeared")
	await ctx.wait_ms(SETTLE_MS)
	var states: Array = []
	var problems := PackedStringArray()

	var live := await _sample(ctx, "live")
	states.append(live)
	if not bool(live.get("called", false)):
		problems.append("live play in camera mode 0 calls no HUD pass")

	var help_keys := ControlsBindings.model().godot_keys_for_token("help")
	if help_keys.is_empty():
		problems.append("the help row has no key")
	else:
		var help_key := help_keys[0] as Key
		await _tap(ctx, help_key)
		var helped := await _sample(ctx, "f1_help")
		states.append(helped)
		if not bool(helped.get("help_open", false)):
			problems.append("the F1 key opened no help")
		elif bool(helped.get("called", true)):
			problems.append("the F1 help still calls the HUD pass")
		await _tap(ctx, help_key)

	var sim := ctx.sim()
	if sim != null:
		sim.set_local_player_debug_third_person(true)
		var chased := await _sample(ctx, "chase_on_foot")
		states.append(chased)
		if int(chased.get("camera_mode", -1)) == 1 and bool(chased.get("called", true)):
			problems.append("the chase view on foot still calls the HUD pass")
		sim.set_local_player_debug_third_person(false)

	var back := await _sample(ctx, "live_again")
	states.append(back)
	if not bool(back.get("called", false)):
		problems.append("live play after the states calls no HUD pass")

	if bool(ctx.args.get("kill", false)) and sim != null:
		var err := sim.debug_kill_player_entity(sim.get_local_player_wire_handle())
		var dead := await _sample(ctx, "dead")
		dead["kill_error"] = err
		states.append(dead)
		if err == OK and bool(dead.get("local_dead", false)) and bool(dead.get("called", true)):
			problems.append("a dead local player still calls the HUD pass")

	var data := {"states": states}
	if not problems.is_empty():
		return ProbeVerdict.failed("; ".join(problems), data)
	return ProbeVerdict.passed("the HUD pass follows its call site in every state sampled", data)


## One state's read-out after it settles: the frame's verdict, the HUD's element count, the
## help word, the camera mode, the dead bit and a capture.
func _sample(ctx: ProbeContext, label: String) -> Dictionary:
	await ctx.wait_frames(STATE_FRAMES)
	var out := {"state": label}
	var world := ctx.world()
	out["called"] = world != null and world.get_last_frame_hud_pass_called()
	var presenter := ctx.hud_presenter()
	var hud: HudOverlay = presenter.get_game_hud() if presenter != null else null
	if hud != null:
		out["elements_drawn"] = hud.get_draw_list_stats().elements_drawn
	if presenter != null:
		out["help_open"] = presenter.toggles().is_help_open()
	var sim := ctx.sim()
	if sim != null:
		var view := sim.get_local_player_view()
		out["camera_mode"] = view.get_camera_mode() if view != null else -1
		out["local_dead"] = bool(sim.is_local_player_dead())
	var png: String = await ctx.capture_png("hud_pass_" + label)
	out["capture"] = png
	ctx.log("%s: %s" % [label, out])
	return out


## A held key over a few input frames, then its release: the binding sampler sees the edge.
func _tap(ctx: ProbeContext, key: Key) -> void:
	ProbeInput.hold(key, true)
	await ctx.wait_frames(3)
	ProbeInput.hold(key, false)
	await ctx.wait_frames(3)


## The HUD builds lazily on the first frame a mission has a local player.
func _wait_hud(ctx: ProbeContext) -> HudOverlay:
	var deadline := Time.get_ticks_msec() + HUD_TIMEOUT_MS
	while not ctx.cancelled and Time.get_ticks_msec() < deadline:
		var presenter := ctx.hud_presenter()
		if presenter != null:
			var hud := presenter.get_game_hud()
			if hud != null:
				return hud
		await ctx.tree.process_frame
	return null
