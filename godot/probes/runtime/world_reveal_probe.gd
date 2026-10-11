extends GameProbe

## Reveals a loaded mission the single-player start-mission splash holds, as a player's fresh key or click does:
## the shell's dismiss_start_mission_splash (the loading screen still draws its closing frame and emits its normal
## dismissal) until the shell says the world is revealed (is_world_loading false). For an automated run whose game
## window takes no input (the OpenNova Editor's mission e2e, scripts/mcp/editor_mission_e2e.py --live). A world
## already revealed passes at once; the verdict says whether a splash was dismissed.

func run(ctx: ProbeContext) -> ProbeVerdict:
	var shell := ctx.game()
	if shell == null:
		return ProbeVerdict.failed("no game shell")
	var held := shell.is_world_loading()
	var dismissed := false
	var deadline := Time.get_ticks_msec() + int(ctx.args["timeout_ms"])
	while shell.is_world_loading():
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled")
		if Time.get_ticks_msec() >= deadline:
			return ProbeVerdict.failed("the world was not revealed in time", {"held": held, "dismissed": dismissed})
		# False while the load is still running (no splash yet) or once the splash closes: asked again each frame.
		dismissed = shell.dismiss_start_mission_splash() or dismissed
		await ctx.wait_frames(1)
	return ProbeVerdict.passed(
			"the world is revealed" + (" (the start-mission splash dismissed)" if dismissed else ""),
			{"held": held, "dismissed": dismissed})
