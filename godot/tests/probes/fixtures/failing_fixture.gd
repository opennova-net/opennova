extends GameProbe

# Test fixture: a probe whose verdict is a fail.


func run(ctx: ProbeContext) -> ProbeVerdict:
	ctx.log("failing: measured 3, wanted 4")
	return ProbeVerdict.failed("measured 3, wanted 4", {"measured": 3, "wanted": 4})
