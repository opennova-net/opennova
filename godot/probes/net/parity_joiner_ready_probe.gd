extends GameProbe

## parity_joiner_ready: wait until this OpenNova joiner (a --lan-join launch)
## reaches the requested readiness and return its witness. `in_match` is a
## live local player in the protocol InMatch phase (with `auto_deploy` the
## default deployment pick is sent when the host asks and readiness also
## waits for it to clear); `deploy_hold` is the witnessed split of net-re
## 5.61: the pick pending on a granted self handle with the DEATH screen
## presented. The witness is published as progress every heartbeat while
## waiting and is the verdict's data when ready; the runner's readiness
## classifiers (scripts/net/lib.ps1) read it unchanged.

const HEARTBEAT_INTERVAL_MS := 500
const READY_TIMEOUT_MS := 240_000


func run(ctx: ProbeContext) -> ProbeVerdict:
	var readiness_mode := String(ctx.args.get("readiness_mode", "in_match"))
	var auto_deploy := bool(ctx.args.get("auto_deploy", false))
	var exercise_motion := bool(ctx.args.get("exercise_motion", false))
	var run_id := String(ctx.args.get("run_id", ""))
	var topology := String(ctx.args.get("topology", ""))
	if readiness_mode == "deploy_hold" and (auto_deploy or exercise_motion):
		return ProbeVerdict.failed("deploy_hold must not auto-deploy or exercise motion")
	ctx.log("run %s topology %s: waiting for %s readiness (auto_deploy=%s exercise_motion=%s)" % [
			run_id, topology, readiness_mode, str(auto_deploy), str(exercise_motion)])
	var deadline := Time.get_ticks_msec() + READY_TIMEOUT_MS
	var last_heartbeat := 0
	var sequence := 0
	var witness := ParityJoinerWitness.compose(ParityJoinerWitness.empty_state(),
			readiness_mode, auto_deploy, exercise_motion, sequence)
	while not ctx.cancelled:
		var now := Time.get_ticks_msec()
		var state := ParityJoinerWitness.read_state(ctx)
		if not state.is_empty():
			if auto_deploy and not ParityJoinerWitness.pick_sent and bool(state.pick_pending):
				var sim := ctx.sim()
				ParityJoinerWitness.pick_sent = sim != null and bool(sim.send_deployment_pick(0))
				ctx.log("default deployment pick sent=%s" % str(ParityJoinerWitness.pick_sent))
				state = ParityJoinerWitness.read_state(ctx)
			if not state.is_empty():
				var ready := ParityJoinerWitness.in_match_ready(state, readiness_mode, auto_deploy) \
						or ParityJoinerWitness.deploy_hold_ready(state, readiness_mode)
				if ready:
					sequence += 1
					witness = ParityJoinerWitness.compose(state, readiness_mode, auto_deploy,
							exercise_motion, sequence)
					ctx.progress(witness)
					ctx.log("joiner ready mode=%s self_handle=%d phase=%d" % [
							readiness_mode, int(state.self_handle), int(state.joiner_phase)])
					return ProbeVerdict.passed("joiner ready (%s)" % readiness_mode, witness)
		if now - last_heartbeat >= HEARTBEAT_INTERVAL_MS:
			sequence += 1
			witness = ParityJoinerWitness.compose(
					state if not state.is_empty() else ParityJoinerWitness.empty_state(),
					readiness_mode, auto_deploy, exercise_motion, sequence)
			ctx.progress(witness)
			last_heartbeat = now
		if now >= deadline:
			return ProbeVerdict.failed("timed out waiting for %s readiness" % readiness_mode, witness)
		await ctx.tree.process_frame
	return ProbeVerdict.failed("cancelled while waiting for %s readiness" % readiness_mode, witness)
