extends GameProbe

# Test fixture: ignores cancellation for a long while (the watchdog's reap
# case). Bounded so the coroutine does not outlive the test process.

const FRAMES := 3000


func run(ctx: ProbeContext) -> ProbeVerdict:
	ctx.log("stubborn: ignoring cancel")
	for _frame in FRAMES:
		await ctx.tree.process_frame
	return ProbeVerdict.passed("finally")
