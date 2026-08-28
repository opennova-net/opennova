extends GameProbe

# Test fixture: runs for `frames` frames, logging every tenth, honoring
# cancellation.


func run(ctx: ProbeContext) -> ProbeVerdict:
	var frames := int(ctx.args.get("frames", 600))
	for frame in frames:
		if ctx.cancelled:
			ctx.log("slow: cancelled at frame %d" % frame)
			return ProbeVerdict.failed("cancelled", {"frame": frame})
		if frame % 10 == 0:
			ctx.log("slow: frame %d" % frame)
			ctx.progress({"frame": frame, "frames": frames})
		await ctx.tree.process_frame
	return ProbeVerdict.passed("slow finished", {"frames": frames})
