class_name ProbeRunner
extends Node

## Runs registered probes one at a time for the game_probe tool (docs/mcp.md,
## ADR 0041): looks the name up in ProbeCatalog, checks its preconditions,
## validates the typed args, gives it an artifact directory and a
## ProbeContext, and drives run() as a fire-and-forget coroutine so the MCP
## transport stays responsive while op=status polls. A watchdog cancels a
## run past its timeout and reaps one that ignores the cancellation. The
## last few runs stay readable; leaving the tree cancels the active one.

const KEEP_RUNS := 8
const DEFAULT_UNRESPONSIVE_GRACE_MS := 5000
const RUNS_DIR := "user://probe-runs"

## The shell seams handed to every context (null in pure tests).
var seams: ProbeShellSeams = null
## How long after cancellation a run may keep running before it is reaped.
var unresponsive_grace_ms := DEFAULT_UNRESPONSIVE_GRACE_MS

var _definitions: Array[ProbeDef] = []
var _use_catalog := true
var _runs: Array[ProbeRun] = []
var _active: ProbeRun = null
var _active_ctx: ProbeContext = null
var _active_def: ProbeDef = null
var _cancel_started_ms := 0


func _init() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS


## Test seam: a fixed definition list instead of ProbeCatalog.
func set_definitions(defs: Array[ProbeDef]) -> void:
	_definitions = defs
	_use_catalog = false


func definitions() -> Array[ProbeDef]:
	return ProbeCatalog.definitions() if _use_catalog else _definitions


func definition(name: String) -> ProbeDef:
	for def in definitions():
		if def.name == name:
			return def
	return null


func is_running() -> bool:
	return _active != null


## game_probe op=list.
func list() -> Dictionary:
	var probes: Array = []
	for def in definitions():
		probes.append(def.to_list_entry(ProbeCatalog.is_available(def)))
	return { "probes": probes, "active_run_id": _active.run_id if _active != null else "" }


## game_probe op=run: {run_id, name, started_at, artifact_dir} or {refused[, details]}.
func start(name: String, raw_args: Variant) -> Dictionary:
	var def := definition(name)
	if def == null:
		return { "refused": "Unknown probe '%s'; game_probe op=list names the catalog." % name }
	if _active != null:
		return { "refused": "Probe '%s' is still running as %s; wait for it or cancel it first." % [
				_active.name, _active.run_id] }
	if def.needs_window and DisplayServer.get_name() == "headless":
		return { "refused": "Probe '%s' needs a window; launch the game without --headless." % name }
	if def.needs_mission:
		var world: GameWorld = _live_world()
		if world == null or not world.is_loaded():
			return { "refused": "Probe '%s' needs a loaded mission; start one first (--mission, game_menu, or a probe verb)." % name }
	var validated := ProbeSchema.validate(def.input_schema, raw_args)
	if not validated.ok:
		return { "refused": "Probe '%s' arguments are invalid." % name, "details": Array(validated.errors) }
	if not ProbeCatalog.is_available(def):
		return { "refused": "Probe '%s' is not shipped in this build (its script is source-only)." % name }
	var probe := ProbeCatalog.load_probe(def)
	if probe == null:
		return { "refused": "Probe '%s' failed to load as a GameProbe." % name }

	var run := ProbeRun.new()
	run.name = name
	run.started_ticks_ms = Time.get_ticks_msec()
	run.started_at = Time.get_datetime_string_from_system(true, true)
	run.run_id = "%s-%s-%04x" % [
		name, run.started_at.replace("-", "").replace(":", "").replace("T", "-"), randi() & 0xFFFF]
	run.artifact_dir = ProjectSettings.globalize_path(RUNS_DIR.path_join(run.run_id))
	DirAccess.make_dir_recursive_absolute(run.artifact_dir)

	var ctx := ProbeContext.new()
	ctx.run_id = run.run_id
	ctx.name = name
	ctx.args = validated.values
	ctx.artifact_dir = run.artifact_dir
	ctx.tree = get_tree()
	ctx.seams = seams
	ctx.set_line_sink(run.add_line)

	_runs.append(run)
	while _runs.size() > KEEP_RUNS:
		_runs.pop_front()
	_active = run
	_active_ctx = ctx
	_active_def = def
	_cancel_started_ms = 0
	run.add_line("probe %s started" % name)
	_drive(run, ctx, probe)
	return {
		"run_id": run.run_id,
		"name": name,
		"started_at": run.started_at,
		"artifact_dir": run.artifact_dir,
	}


## game_probe op=status: the run's status with lines after `cursor`; when
## the run is live and nothing is new, long-polls up to wait_ms.
func status(run_id: String, cursor: int, wait_ms: int) -> Dictionary:
	var run := _find(run_id)
	if run == null:
		return { "refused": "No probe run %s." % run_id if not run_id.is_empty() else "No probe has run yet." }
	var deadline := Time.get_ticks_msec() + wait_ms
	while run.is_running() and run.last_seq() <= cursor \
			and Time.get_ticks_msec() < deadline and is_inside_tree():
		await get_tree().process_frame
	return run.to_status(cursor)


## game_probe op=cancel: asks the run to stop; the state flips when run()
## returns (or the watchdog reaps it).
func cancel(run_id: String) -> Dictionary:
	var run := _find(run_id)
	if run == null:
		return { "refused": "No probe run %s." % run_id if not run_id.is_empty() else "No probe has run yet." }
	if run.is_running() and not run.cancel_requested:
		_request_cancel(run, "cancel requested")
	return { "run_id": run.run_id, "state": run.state, "cancel_requested": run.cancel_requested }


## Cancel the active run and wait up to `timeout_ms` for it to settle
## (game_control quit calls this before the shell tears down).
func cancel_and_wait(timeout_ms: int) -> void:
	if _active == null:
		return
	var run := _active
	if not run.cancel_requested:
		_request_cancel(run, "cancel requested (shutdown)")
	var deadline := Time.get_ticks_msec() + timeout_ms
	while _active == run and Time.get_ticks_msec() < deadline and is_inside_tree():
		await get_tree().process_frame
	if _active == run:
		_reap(run, "did not stop within %d ms of the shutdown cancel" % timeout_ms)


func _process(_delta: float) -> void:
	if _active == null:
		return
	var now := Time.get_ticks_msec()
	if not _active.cancel_requested and now - _active.started_ticks_ms > _active_def.timeout_ms:
		_request_cancel(_active, "watchdog: timeout after %d ms" % _active_def.timeout_ms)
	elif _active.cancel_requested and now - _cancel_started_ms > unresponsive_grace_ms:
		_reap(_active, "unresponsive: still running %d ms after cancellation" % unresponsive_grace_ms)


func _exit_tree() -> void:
	if _active != null:
		_request_cancel(_active, "cancel requested (runner leaving the tree)")
		_reap(_active, "runner left the tree")


func _drive(run: ProbeRun, ctx: ProbeContext, probe: GameProbe) -> void:
	var verdict: Variant = await probe.run(ctx)
	if _active != run:
		return  # already reaped
	_settle(run, ctx, verdict)


func _settle(run: ProbeRun, ctx: ProbeContext, verdict: Variant) -> void:
	ctx.finish()
	run.artifacts = ctx.artifacts()
	run.progress = ctx.progress_value()
	if verdict is ProbeVerdict:
		run.verdict = verdict
	if run.cancel_requested:
		run.state = ProbeRun.STATE_CANCELLED
	elif run.verdict == null:
		run.state = ProbeRun.STATE_ERROR
		run.error = "the probe returned no verdict (a script error aborted run()? see game_logs)"
	else:
		run.state = ProbeRun.STATE_PASSED if run.verdict.ok else ProbeRun.STATE_FAILED
	run.finished_ticks_ms = Time.get_ticks_msec()
	run.add_line("probe %s %s%s" % [run.name, run.state,
			": " + run.verdict.summary if run.verdict != null else ""])
	_clear_active(run)


func _request_cancel(run: ProbeRun, reason: String) -> void:
	run.cancel_requested = true
	if _active == run and _active_ctx != null:
		_active_ctx.cancelled = true
	_cancel_started_ms = Time.get_ticks_msec()
	run.add_line(reason)


func _reap(run: ProbeRun, reason: String) -> void:
	if _active != run:
		return
	if _active_ctx != null:
		_active_ctx.finish()
		run.artifacts = _active_ctx.artifacts()
		run.progress = _active_ctx.progress_value()
	run.state = ProbeRun.STATE_ERROR
	run.error = reason
	run.finished_ticks_ms = Time.get_ticks_msec()
	run.add_line("probe %s error: %s" % [run.name, reason])
	_clear_active(run)


func _clear_active(run: ProbeRun) -> void:
	if _active == run:
		_active = null
		_active_ctx = null
		_active_def = null


func _find(run_id: String) -> ProbeRun:
	if run_id.is_empty():
		return _active if _active != null else (_runs.back() if not _runs.is_empty() else null)
	for run in _runs:
		if run.run_id == run_id:
			return run
	return null


func _live_world() -> GameWorld:
	if seams == null or not seams.world_source.is_valid():
		return null
	var value: Variant = seams.world_source.call()
	return value as GameWorld if value is Object and is_instance_valid(value) else null
