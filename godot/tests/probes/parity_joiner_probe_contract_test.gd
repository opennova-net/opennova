extends GutTest

# The parity joiner probes' contract with the parity runner
# (scripts/net/run_parity_topology.ps1 + lib.ps1): the three catalog entries
# and their required arguments, the witness key set the runner's readiness
# snapshot reads, and the readiness rules.

# lib.ps1 ConvertTo-OpenNovaJoinerReadinessSnapshot reads exactly these.
const RUNNER_SNAPSHOT_FIELDS := [
	"process_id", "ticks_msec", "heartbeat_sequence", "readiness_mode", "in_match",
	"local_player", "deploy_hold_ready", "deployment_pick_pending", "deployment_pick_sent",
	"auto_deploy", "self_handle", "join_error", "session_lost", "joiner_phase",
	"join_admission_stage", "deploy_presented", "motion_complete",
]


func after_each() -> void:
	ParityJoinerWitness.pick_sent = false
	ParityJoinerWitness.exercise = {}


func test_catalog_lists_the_three_joiner_probes_with_their_required_args() -> void:
	var expected := {
		"parity_joiner_ready": ["run_id", "topology"],
		"parity_joiner_motion": ["run_id", "topology", "steady_started_utc"],
		"parity_joiner_state": [],
	}
	for name in expected:
		var def := ProbeDef.definition(name)
		assert_not_null(def, "%s is in the catalog" % name)
		if def == null:
			continue
		assert_true(def.script_path.begins_with("res://probes/net/"), "%s lives in the net family" % name)
		assert_false(def.needs_window, "%s never needs a window" % name)
		var required: Array = def.input_schema.get("required", [])
		assert_eq(required, expected[name], "%s required args" % name)
		var props: Dictionary = def.input_schema.get("properties", {})
		if props.has("topology"):
			assert_eq(props["topology"].get("enum", []), ["RO", "OO"])
		if props.has("readiness_mode"):
			assert_eq(props["readiness_mode"].get("enum", []), ["in_match", "deploy_hold"])
	assert_true(ProbeDef.definition("parity_joiner_motion").needs_mission,
			"the motion exercise runs on the joined world")


func test_witness_carries_every_runner_snapshot_field_and_nothing_undeclared() -> void:
	var witness := ParityJoinerWitness.compose(ParityJoinerWitness.empty_state(), "in_match", false, false, 7)
	for field in RUNNER_SNAPSHOT_FIELDS:
		assert_true(witness.has(field), "the witness carries %s" % field)
		assert_true(field in ParityJoinerWitness.FIELDS, "FIELDS declares %s" % field)
	var keys := witness.keys()
	keys.sort()
	var declared := Array(ParityJoinerWitness.FIELDS)
	declared.sort()
	assert_eq(keys, declared, "compose publishes exactly the declared fields")
	assert_eq(witness.heartbeat_sequence, 7)
	assert_eq(witness.process_id, OS.get_process_id())
	assert_true(witness.motion_complete, "a non-exercised run reports motion complete")
	assert_false(witness.motion_gate_required)


func test_readiness_rules_match_the_witnessed_split() -> void:
	# The runtime halves (the pick owed on a bound handle, no error, the
	# session alive; the local player in the InMatch phase, the auto-deploy
	# pick cleared) are the engine's typed verdicts (the role_feeds ctest);
	# the witness gates them on the mode and, for the hold, the shell's
	# presented DEATH screen.
	var state := ParityJoinerWitness.empty_state()
	assert_false(ParityJoinerWitness.in_match_ready(state, "in_match", false))
	state.match_ready = true
	assert_true(ParityJoinerWitness.in_match_ready(state, "in_match", false))
	assert_false(ParityJoinerWitness.in_match_ready(state, "in_match", true),
			"auto-deploy readiness is the engine's auto-deploy verdict (the pick must clear)")
	state.match_ready_auto = true
	assert_true(ParityJoinerWitness.in_match_ready(state, "in_match", true))
	assert_false(ParityJoinerWitness.in_match_ready(state, "deploy_hold", false))

	var hold := ParityJoinerWitness.empty_state()
	hold.hold_ready = true
	hold.deploy_presented = true
	assert_true(ParityJoinerWitness.deploy_hold_ready(hold, "deploy_hold"))
	assert_false(ParityJoinerWitness.deploy_hold_ready(hold, "in_match"))
	hold.deploy_presented = false
	assert_false(ParityJoinerWitness.deploy_hold_ready(hold, "deploy_hold"),
			"the DEATH screen must be presented")
	hold.deploy_presented = true
	hold.hold_ready = false
	assert_false(ParityJoinerWitness.deploy_hold_ready(hold, "deploy_hold"))


func test_witness_reports_what_the_process_witnessed_across_runs() -> void:
	ParityJoinerWitness.pick_sent = true
	ParityJoinerWitness.exercise = {
		"run_id": "run-7", "topology": "OO", "steady_started_utc": "2026-08-27T10:00:00.0000000Z",
		"created_utc": "2026-08-27T10:00:01.0000000Z",
		"started_ticks_msec": 100, "completed_ticks_msec": 4500,
	}
	var witness := ParityJoinerWitness.compose(ParityJoinerWitness.empty_state(), "in_match", true, true, 1)
	assert_true(witness.deployment_pick_sent)
	assert_true(witness.motion_gate_required)
	assert_true(witness.motion_gate_observed)
	assert_true(witness.motion_complete)
	assert_eq(witness.motion_gate_run_id, "run-7")
	assert_eq(witness.motion_started_ticks_msec, 100)
	assert_eq(witness.motion_completed_ticks_msec, 4500)
	assert_eq(witness.motion_pre_roll_ms, ParityJoinerWitness.MOTION_PRE_ROLL_MS)
	assert_eq(witness.look_samples_per_phase, ParityJoinerWitness.LOOK_SAMPLES_PER_PHASE)
	ParityJoinerWitness.exercise = {}
	var pending := ParityJoinerWitness.compose(ParityJoinerWitness.empty_state(), "in_match", true, true, 2)
	assert_false(pending.motion_complete, "an exercised run is incomplete until the motion probe ran")
	assert_false(pending.motion_gate_observed)


func test_timestamps_take_the_runner_shape() -> void:
	var pattern := RegEx.new()
	assert_eq(pattern.compile("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{7}Z$"), OK)
	assert_not_null(pattern.search(ParityJoinerWitness.utc_now()),
			"utc_now keeps the runner's 100 ns UTC shape")
	assert_true(ParityJoinerWitness.is_utc_timestamp("2026-08-27T10:00:00.1234567Z"))
	assert_true(ParityJoinerWitness.is_utc_timestamp("2026-08-27T10:00:00Z"))
	assert_false(ParityJoinerWitness.is_utc_timestamp("2026-08-27 10:00:00"))
	assert_false(ParityJoinerWitness.is_utc_timestamp(""))
