extends GameProbe

## dialog_vs_ambient: the "enabling ambient sounds broke mission dialog"
## report at the MissionAudio seam, on the launch's mounted root. Runs the
## full mission-audio setup for `mission` (ambient marker voices on unless
## `ambient` is false), then plays the first `lines` resolvable .DBF dialog
## ids the way _route_mission_effects does, asserting for each line
##   spawn    play_dialog returned true and a dialog voice appeared
##   audible  the Voice bus peak rises above the silence floor while playing
##   advance  the voice finishes and the serialized queue moves on
## and reporting the Ambient/Master bus peaks while dialog plays (the masking
## metric). `sim_ticks` > 0 first steps a bare Simulation of the mission and
## reports the effect kinds its startup ticks fire (delay-gated dialog) plus
## the player start the listener parks at. Runs on a menu launch: a loaded
## world's own bed would share the buses.

const SILENCE_FLOOR_DB := -50.0
const LINE_TIMEOUT_SLACK_S := 3.0
const AMBIENT_SPIN_MS := 1200
const CULL_RANGE_U := 240.0
const NEAR_RANGE_U := 120.0
const VOICE_STATE_SAMPLE := 5
const DIALOG_TICK_LOG_CAP := 12
const UNMEASURED_DB := -200.0

var _ctx: ProbeContext
var _failures: PackedStringArray = []


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var lines := int(ctx.args.get("lines", 3))
	var ambient := bool(ctx.args.get("ambient", true))
	var mission_name := String(ctx.args.get("mission", "00TRa.bms"))
	var sim_ticks := int(ctx.args.get("sim_ticks", 0))
	var live_world := ctx.world()
	if live_world != null and live_world.is_loaded():
		return ProbeVerdict.failed(
				"run on a menu launch (no --mission): the loaded world's own audio would share the buses")
	var root := ctx.resource_root()
	if root == null:
		return ProbeVerdict.failed("the shell has no mounted resource root")
	var item_db := ItemDatabase.new()
	item_db.load_from_resource_root(root, "items.def")
	var mission := MissionData.new()
	if mission.open_from_resource_root(root, mission_name) != OK:
		return ProbeVerdict.failed("%s did not parse from the mounted root" % mission_name)
	ctx.log("mission %s ambient=%s lines=%d" % [mission_name, str(ambient), lines])

	var data := {"mission": mission_name, "ambient": ambient, "lines": []}
	var player_pos := Vector3.INF
	if sim_ticks > 0:
		player_pos = _sim_arm(mission, sim_ticks, data)

	var container := Node3D.new()
	container.name = "DialogVsAmbientProbe"
	ctx.tree.root.add_child(container)
	var audio := MissionAudio.new(root, item_db)
	ctx.defer_restore(func() -> void:
		audio.teardown()
		if is_instance_valid(container):
			container.queue_free())
	if not ambient:
		# Banks + .DBF still load, zero ambient candidates resolve: the
		# "ambient disabled" arm.
		audio.set_ambient_markers_enabled(false)
	var stats := audio.setup(mission, mission_name, container)
	ctx.log("setup: %s" % str(stats.to_dict()))
	data["setup"] = stats.to_dict()

	# The listener parks at the player start when the sim arm resolved one
	# (the play-test position), else at the origin. A current Camera3D is the
	# audio listener (a bare AudioListener3D in a script-built tree leaves 3D
	# voices mixing at zero).
	var listen_pos := player_pos if player_pos.is_finite() else Vector3.ZERO
	var previous_camera := ctx.camera()
	var listener := Camera3D.new()
	container.add_child(listener)
	listener.global_position = listen_pos
	listener.make_current()
	ctx.defer_restore(func() -> void:
		if is_instance_valid(previous_camera):
			previous_camera.make_current())
	# The game culls voices beyond 240 u from the camera each frame.
	audio.tick(listen_pos, 0.2)
	var marker_players: Array = container.find_children("*", "AudioStreamPlayer3D", true, false)
	var near := 0
	var near_close := 0
	for p in marker_players:
		var d := ((p as Node3D).global_position - listen_pos).length()
		if d <= CULL_RANGE_U:
			near += 1
		if d <= NEAR_RANGE_U:
			near_close += 1
	ctx.log("ambient candidates=%d physical voices=%d (within %.0fu of listener: %d, within %.0fu: %d) listener=%s" % [
			int(stats.ambient_candidates), marker_players.size(),
			CULL_RANGE_U, near, NEAR_RANGE_U, near_close, str(listen_pos)])
	data["physical_voices"] = marker_players.size()
	data["voices_within_cull"] = near
	data["voices_within_near"] = near_close
	await _log_nearest_voice_states(marker_players, listen_pos)

	var voice_bus := AudioServer.get_bus_index(&"Voice")
	var ambient_bus := AudioServer.get_bus_index(&"Ambient")
	var master_bus := AudioServer.get_bus_index(&"Master")
	if voice_bus < 0:
		_fail("Voice bus missing from the layout")

	# Let the ambient bed spin up, then sample its steady peak.
	var ambient_floor := UNMEASURED_DB
	var master_floor := UNMEASURED_DB
	var spin_until := Time.get_ticks_msec() + AMBIENT_SPIN_MS
	while Time.get_ticks_msec() < spin_until and not ctx.cancelled:
		await ctx.tree.process_frame
		if ambient_bus >= 0:
			ambient_floor = maxf(ambient_floor, AudioServer.get_bus_peak_volume_left_db(ambient_bus, 0))
		master_floor = maxf(master_floor, AudioServer.get_bus_peak_volume_left_db(master_bus, 0))
	ctx.log("pre-dialog peaks: ambient=%.1f dB master=%.1f dB" % [ambient_floor, master_floor])
	data["pre_dialog_peaks"] = {"ambient_db": ambient_floor, "master_db": master_floor}

	# The first N dialog ids that resolve to a loaded set.
	var dialog_count := int(stats.dialogs)
	var ids: Array[int] = []
	for i in range(1, dialog_count + 1):
		if ids.size() >= lines:
			break
		if not String(audio.resolve_dialog_set(i)).is_empty():
			ids.append(i)
	if ids.is_empty():
		_fail("no dialog id resolves to a loaded set (dbf dialogs=%d)" % dialog_count)

	for id in ids:
		if ctx.cancelled:
			break
		var line := await _play_line(audio, id, ambient, voice_bus, ambient_bus, master_bus)
		(data["lines"] as Array).append(line)

	data["failures"] = Array(_failures)
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled", data)
	if not _failures.is_empty():
		return ProbeVerdict.failed("%d failure(s): %s" % [_failures.size(), "; ".join(_failures)], data)
	return ProbeVerdict.passed("%d dialog line(s) spawned, audible and advanced (ambient=%s)" % [
			ids.size(), str(ambient)], data)


func _fail(message: String) -> void:
	_failures.append(message)
	_ctx.log("FAIL: %s" % message)


## Trigger-layer arm: step the bare sim and report which effect kinds fire on
## startup ticks (no player spawned -> zone-gated triggers stay quiet; this
## catches mission-start / delay-gated dialog). Returns the player start.
func _sim_arm(mission: MissionData, sim_ticks: int, data: Dictionary) -> Vector3:
	var player_pos := Vector3.INF
	var sim := Simulation.new()
	if sim.load_from_mission_data(mission):
		if sim.spawn_local_player_at_start():
			player_pos = sim.get_local_player_position()
			_ctx.log("player start = %s" % str(player_pos))
		var kinds: Dictionary = {}
		var dialog_ticks: PackedStringArray = []
		for t in range(sim_ticks):
			sim.step()
			for e in sim.drain_effects():
				var effect: MissionEffect = e
				var k := effect.kind
				kinds[k] = int(kinds.get(k, 0)) + 1
				if k == "dialog" or k == "dialog_wav":
					dialog_ticks.append("%s@t%d a=%d str=%s" % [k, t, effect.a, effect.text])
		_ctx.log("sim %d ticks, effect kinds: %s" % [sim_ticks, str(kinds)])
		for d in dialog_ticks.slice(0, DIALOG_TICK_LOG_CAP):
			_ctx.log("  %s" % d)
		data["sim_effect_kinds"] = kinds
		data["sim_dialog_ticks"] = dialog_ticks
	else:
		_ctx.log("sim load FAILED")
		data["sim_load_failed"] = true
	return player_pos


## Per-voice state for the nearest few: distinguishes "not playing" from
## "playing but mixed to nothing" (a headless 3D-audio artifact vs a bug).
func _log_nearest_voice_states(marker_players: Array, listen_pos: Vector3) -> void:
	var by_dist := marker_players.duplicate()
	by_dist.sort_custom(func(a: Node3D, b: Node3D) -> bool:
		return (a.global_position - listen_pos).length() < (b.global_position - listen_pos).length())
	for i in range(mini(VOICE_STATE_SAMPLE, by_dist.size())):
		var p := by_dist[i] as AudioStreamPlayer3D
		var d := (p.global_position - listen_pos).length()
		var pos0 := p.get_playback_position()
		await _ctx.wait_frames(2)
		if not is_instance_valid(p):
			return
		var pos1 := p.get_playback_position()
		var length := p.stream.get_length() if p.stream != null else -1.0
		_ctx.log("  voice d=%5.1fu playing=%s paused=%s pos %.3f->%.3f vol=%.1f dB unit=%.1f max=%.1f bus=%s stream_len=%.2f" % [
				d, str(p.playing), str(p.stream_paused), pos0, pos1, p.volume_db, p.unit_size,
				p.max_distance, p.bus, length])


func _play_line(audio: MissionAudio, id: int, ambient: bool, voice_bus: int,
		ambient_bus: int, master_bus: int) -> Dictionary:
	var set_name := String(audio.resolve_dialog_set(id))
	var line := {"id": id, "set": set_name, "spawned": false, "audible": false, "advanced": false}
	if not audio.play_dialog(id):
		_fail("dlg%03d (%s): play_dialog returned false" % [id, set_name])
		return line
	var voice: AudioStreamPlayer = audio.dialog_voice()
	if voice == null:
		_fail("dlg%03d (%s): queued but no voice spawned" % [id, set_name])
		return line
	line["spawned"] = true
	var length: float = voice.stream.get_length() if voice.stream != null else 0.0
	var loop_mode := -1
	if voice.stream is AudioStreamWAV:
		loop_mode = (voice.stream as AudioStreamWAV).loop_mode
	var deadline := Time.get_ticks_msec() + int((length + LINE_TIMEOUT_SLACK_S) * 1000.0)
	var peak_voice := UNMEASURED_DB
	var peak_ambient := UNMEASURED_DB
	var peak_master := UNMEASURED_DB
	while audio.dialog_voice() != null and not _ctx.cancelled:
		if Time.get_ticks_msec() > deadline:
			break
		if voice_bus >= 0:
			peak_voice = maxf(peak_voice, AudioServer.get_bus_peak_volume_left_db(voice_bus, 0))
		if ambient_bus >= 0:
			peak_ambient = maxf(peak_ambient, AudioServer.get_bus_peak_volume_left_db(ambient_bus, 0))
		peak_master = maxf(peak_master, AudioServer.get_bus_peak_volume_left_db(master_bus, 0))
		await _ctx.tree.process_frame
	var advanced: bool = audio.dialog_voice() == null
	var audible := peak_voice > SILENCE_FLOOR_DB
	_ctx.log("dlg%03d set=%s len=%.2fs loop_mode=%d -> spawn=OK audible=%s (voice peak %.1f dB) advance=%s | ambient peak %.1f dB master peak %.1f dB" % [
			id, set_name, length, loop_mode, str(audible), peak_voice, str(advanced),
			peak_ambient, peak_master])
	line["length_s"] = length
	line["loop_mode"] = loop_mode
	line["audible"] = audible
	line["advanced"] = advanced
	line["peak_voice_db"] = peak_voice
	line["peak_ambient_db"] = peak_ambient
	line["peak_master_db"] = peak_master
	if not audible:
		_fail("dlg%03d (%s): voice never rose above %.0f dB on the Voice bus" % [id, set_name, SILENCE_FLOOR_DB])
	if not advanced:
		_fail("dlg%03d (%s): voice never finished within len+%.0fs (queue jam)" % [id, set_name, LINE_TIMEOUT_SLACK_S])
	# Masking: the bug class this pins is the ambient bed BURYING the dialog
	# (pre-fix: +21.5 dB clipping wall vs -0.6 dB speech, inverted radius
	# semantics). The per-voice levels are IDA-witnessed
	# (SoundBank_CalcDistanceVolPan @ 0x75ca20), so the gate only asserts the
	# bed stays below the speech peak and below clipping. Only meaningful when
	# the 3D path actually mixed (a windowed run); headless mixes 3D voices to
	# nothing.
	if ambient and peak_ambient > UNMEASURED_DB + 20.0:
		if peak_ambient >= peak_voice:
			_fail("dlg%03d: ambient bed peak %.1f dB is above the dialog voice peak %.1f dB (dialog buried)" % [
					id, peak_ambient, peak_voice])
		elif peak_ambient >= 0.0:
			_fail("dlg%03d: ambient bed peak %.1f dB is clipping" % [id, peak_ambient])
	return line
