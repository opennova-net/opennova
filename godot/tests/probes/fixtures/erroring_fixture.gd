extends GameProbe

# Test fixture: returns no verdict at all (what an aborted run() looks like
# to the runner).


func run(ctx: ProbeContext) -> ProbeVerdict:
	ctx.log("erroring: bailing without a verdict")
	return null
