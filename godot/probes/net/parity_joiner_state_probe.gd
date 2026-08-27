extends GameProbe

## parity_joiner_state: this joiner's readiness witness right now, one
## synchronous read through the live seams (the runner's initial/final
## snapshots around a steady window). Reports the deployment pick and the
## motion exercise the process itself witnessed in earlier probe runs.


func run(ctx: ProbeContext) -> ProbeVerdict:
	var readiness_mode := String(ctx.args.get("readiness_mode", "in_match"))
	var auto_deploy := bool(ctx.args.get("auto_deploy", false))
	var exercise_motion := bool(ctx.args.get("exercise_motion", false))
	var state := ParityJoinerWitness.read_state(ctx)
	var witness := ParityJoinerWitness.compose(
			state if not state.is_empty() else ParityJoinerWitness.empty_state(),
			readiness_mode, auto_deploy, exercise_motion, 1)
	ctx.progress(witness)
	if state.is_empty():
		return ProbeVerdict.failed("no simulation to witness", witness)
	return ProbeVerdict.passed("in_match=%s local_player=%s phase=%d stage=%s" % [
			str(witness.in_match), str(witness.local_player), int(witness.joiner_phase),
			String(witness.join_admission_stage)], witness)
