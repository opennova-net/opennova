extends GameProbe

# Test fixture: logs its args, publishes progress, passes with the args echoed.


func run(ctx: ProbeContext) -> ProbeVerdict:
	ctx.log("echo %s" % JSON.stringify(ctx.args))
	ctx.progress({"step": 1, "text": ctx.args.get("text", "")})
	await ctx.wait_frames(1)
	ctx.log("echo done")
	return ProbeVerdict.passed("echoed", {"args": ctx.args})
