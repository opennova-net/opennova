class_name GameProbe
extends RefCounted

## The base of every runtime probe (docs/mcp.md, ADR 0041). A probe is a
## script under res://probes/ registered in ProbeCatalog; game_probe op=run
## instantiates it and awaits run(). The probe reads its typed args from the
## context, observes and drives the live game through the context's seams,
## logs through ctx.log (never print), captures into ctx.artifact_dir, undoes
## every mutation through the context's guarded setters or defer_restore,
## and returns a ProbeVerdict. It must never read the environment and must
## check ctx.cancelled across long waits.


func run(_ctx: ProbeContext) -> ProbeVerdict:
	return ProbeVerdict.failed("this probe does not implement run()")
