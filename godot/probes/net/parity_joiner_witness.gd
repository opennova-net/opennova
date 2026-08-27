class_name ParityJoinerWitness
extends RefCounted

## The OpenNova joiner's readiness witness the parity runner classifies
## (scripts/net/lib.ps1 ConvertTo-OpenNovaJoinerReadinessSnapshot /
## Get-OpenNovaJoinerReadinessClass): every field is the observed value read
## through the live seams in one synchronous pass. The deploy hold is the
## WITNESSED split (net-re 5.61): the client enters its protocol InMatch
## phase and owns a live motor after the first loadout grant so its pre-pick
## C2S 0x0C flows, while the player-paced DEATH screen (deploy_presented)
## holds presentation. Nothing here rewrites transport state into a
## synthesized presentation claim; classification is the verifier's.
##
## The parity_joiner_ready / _motion / _state probes share this reader. The
## two statics carry what the game process itself witnessed across probe
## runs (a sent deployment pick, a completed motion exercise), so a later
## snapshot reports them without the runner asserting them.

const DEPLOY_HOLD_ADMISSION_STAGE := "awaiting the player's deployment pick"
const MOTION_PRE_ROLL_MS := 250
const MOTION_INTER_PHASE_GAP_MS := 250
const LOOK_SAMPLES_PER_PHASE := 20
## The walk/strafe/turn exercise: key, duration, total look pixels.
const MOTION_PHASES := [
	{"name": "forward", "key": KEY_W, "duration_ms": 1800, "look": 320},
	{"name": "strafe", "key": KEY_D, "duration_ms": 1200, "look": -320},
	{"name": "return", "key": KEY_A, "duration_ms": 900, "look": 160},
]
## The witness keys, in publication order (the contract test pins them
## against the runner's snapshot).
const FIELDS: PackedStringArray = [
	"readiness_mode", "in_match", "local_player", "deploy_hold_ready", "auto_deploy",
	"deployment_pick_sent", "deployment_pick_pending", "deploy_presented", "joiner_phase",
	"join_admission_stage", "self_handle", "join_error", "session_lost", "exercise_motion",
	"motion_complete", "motion_gate_required", "motion_gate_observed", "motion_gate_run_id",
	"motion_gate_created_utc", "motion_gate_steady_started_utc", "motion_started_ticks_msec",
	"motion_completed_ticks_msec", "motion_pre_roll_ms", "motion_inter_phase_gap_ms",
	"look_samples_per_phase", "heartbeat_sequence", "ticks_msec", "process_id",
]

## The process-wide heartbeat count: every witness any of the three probes
## publishes takes the next value, so the runner's initial/final snapshots
## (different probe runs) still see a monotonic sequence from one process.
static var heartbeat := 0
## Set by parity_joiner_ready when it sent the default deployment pick.
static var pick_sent := false


static func next_heartbeat() -> int:
	heartbeat += 1
	return heartbeat
## Set by parity_joiner_motion when the exercise completed: run_id, topology,
## steady_started_utc, created_utc, started_ticks_msec, completed_ticks_msec.
static var exercise: Dictionary = {}


## The raw joiner state through the live seams ({} without a simulation).
static func read_state(ctx: ProbeContext) -> Dictionary:
	var sim := ctx.sim()
	if sim == null:
		return {}
	var deploy := ctx.deploy_presenter()
	return {
		"local_player": bool(sim.has_local_player()),
		"in_match": bool(sim.is_joined_in_match()),
		"pick_pending": bool(sim.is_join_deploy_pick_pending()),
		"deploy_presented": deploy != null and deploy.is_open(),
		"self_handle": int(sim.get_joiner_self_handle()),
		"join_error": String(sim.get_join_error()),
		"session_lost": bool(sim.is_session_lost()),
		"joiner_phase": int(sim.get_joiner_phase()),
		"join_admission_stage": String(sim.get_join_admission_stage()),
	}


static func empty_state() -> Dictionary:
	return {
		"local_player": false,
		"in_match": false,
		"pick_pending": false,
		"deploy_presented": false,
		"self_handle": 0,
		"join_error": "",
		"session_lost": false,
		"joiner_phase": -1,
		"join_admission_stage": "",
	}


static func deploy_hold_ready(state: Dictionary, readiness_mode: String) -> bool:
	return readiness_mode == "deploy_hold" \
			and bool(state.pick_pending) and int(state.self_handle) > 0 \
			and bool(state.deploy_presented) \
			and String(state.join_admission_stage) == DEPLOY_HOLD_ADMISSION_STAGE \
			and String(state.join_error).is_empty() and not bool(state.session_lost)


static func in_match_ready(state: Dictionary, readiness_mode: String, auto_deploy: bool) -> bool:
	return readiness_mode == "in_match" \
			and bool(state.local_player) and bool(state.in_match) \
			and (not auto_deploy or not bool(state.pick_pending))


## The witness for one heartbeat: the raw state plus the run's configuration
## and what the process has witnessed so far.
static func compose(state: Dictionary, readiness_mode: String, auto_deploy: bool,
		exercise_motion: bool, heartbeat_sequence: int) -> Dictionary:
	var done := not exercise.is_empty()
	return {
		"readiness_mode": readiness_mode,
		"in_match": bool(state.in_match),
		"local_player": bool(state.local_player),
		"deploy_hold_ready": deploy_hold_ready(state, readiness_mode),
		"auto_deploy": auto_deploy,
		"deployment_pick_sent": pick_sent,
		"deployment_pick_pending": bool(state.pick_pending),
		"deploy_presented": bool(state.deploy_presented),
		"joiner_phase": int(state.joiner_phase),
		"join_admission_stage": String(state.join_admission_stage),
		"self_handle": int(state.self_handle),
		"join_error": String(state.join_error),
		"session_lost": bool(state.session_lost),
		"exercise_motion": exercise_motion,
		"motion_complete": done if exercise_motion else true,
		"motion_gate_required": exercise_motion,
		"motion_gate_observed": done,
		"motion_gate_run_id": String(exercise.get("run_id", "")),
		"motion_gate_created_utc": String(exercise.get("created_utc", "")),
		"motion_gate_steady_started_utc": String(exercise.get("steady_started_utc", "")),
		"motion_started_ticks_msec": int(exercise.get("started_ticks_msec", 0)),
		"motion_completed_ticks_msec": int(exercise.get("completed_ticks_msec", 0)),
		"motion_pre_roll_ms": MOTION_PRE_ROLL_MS,
		"motion_inter_phase_gap_ms": MOTION_INTER_PHASE_GAP_MS,
		"look_samples_per_phase": LOOK_SAMPLES_PER_PHASE,
		"heartbeat_sequence": heartbeat_sequence,
		"ticks_msec": Time.get_ticks_msec(),
		"process_id": OS.get_process_id(),
	}


## The runner's fixed-width UTC form (100 ns precision).
static func utc_now() -> String:
	var unix_time := Time.get_unix_time_from_system()
	var whole_seconds := int(floor(unix_time))
	var fraction_100ns := int(floor((unix_time - float(whole_seconds)) * 10_000_000.0))
	var parts := Time.get_datetime_dict_from_unix_time(whole_seconds)
	return "%04d-%02d-%02dT%02d:%02d:%02d.%07dZ" % [
		int(parts.year), int(parts.month), int(parts.day),
		int(parts.hour), int(parts.minute), int(parts.second), fraction_100ns,
	]


static func is_utc_timestamp(value: String) -> bool:
	var pattern := RegEx.new()
	if pattern.compile(
			"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:"
			+ "[0-9]{2}(?:\\.[0-9]{1,7})?Z$") != OK:
		return false
	return pattern.search(value) != null
