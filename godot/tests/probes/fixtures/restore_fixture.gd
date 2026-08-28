extends GameProbe

# Test fixture: mutates through the guarded setters and defer_restore, writes
# one artifact, and passes; the runner's finish() must undo the mutations.


func run(ctx: ProbeContext) -> ProbeVerdict:
	ctx.set_time_scale(0.5)
	ctx.defer_restore(func() -> void: ctx.log("restore: custom undo ran"))
	var path := ctx.artifact_dir.path_join("note.txt")
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string("probe note\n")
	file.close()
	ctx.artifact("note", path, "txt")
	await ctx.wait_frames(2)
	return ProbeVerdict.passed("mutated", {"time_scale_during": Engine.time_scale})
