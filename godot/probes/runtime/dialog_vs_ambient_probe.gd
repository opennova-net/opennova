extends GameProbe

## dialog_vs_ambient: the "enabling ambient sounds broke mission dialog"
## report at the MissionAudio seam, on the launch's mounted root. Runs the
## full mission-audio setup for `mission` (ambient marker voices on unless
## `ambient` is false), then plays the first `lines` resolvable .DBF dialog
## ids the way _route_mission_effects does, asserting for each line
##   spawn    play_dialog returned true and a dialog voice appeared
##   audible  the dialog channel opens at a volume above 0
##   advance  the voice finishes and the serialized queue moves on
## and reporting the bed's loudest emitter channel against the dialog channel (the
## masking metric) and the one stream's Master peak. `sim_ticks` > 0 first steps a bare Simulation of the mission and
## reports the effect kinds its startup ticks fire (delay-gated dialog) plus
## the player start the listener parks at. Runs on a menu launch: a loaded
## world's own bed would share the mixer.

# The longest a line may play before the probe calls the queue jammed, and the clock's scale.
const LINE_TIMEOUT_MS := 63000
const MS_PER_S := 1000.0
const AMBIENT_SPIN_MS := 1200
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
	var audio := MissionAudio.create(root, item_db)
	ctx.defer_restore(func() -> void:
		audio.teardown()
		if is_instance_valid(container):
			container.queue_free())
	if not ambient:
		# Banks + .DBF still load, zero ambient candidates resolve: the
		# "ambient disabled" arm.
		audio.set_ambient_markers_enabled(false)
	var stats := audio.setup(mission, mission_name, container)
	ctx.log("setup: %s" % str(stats.to_json_value()))
	data["setup"] = stats.to_json_value()

	# The listener parks at the player start when the sim arm resolved one
	# (the play-test position), else at the origin.
	var listen_pos := player_pos if player_pos.is_finite() else Vector3.ZERO
	audio.tick(listen_pos, 0.2)
	# The emitter channels the bed binds (the engine mixer's 4..11) and their volume bytes.
	var bed_volume := 0
	var bound := audio.active_ambient_candidate_ids()
	for id: Variant in bound:
		bed_volume = maxi(bed_volume, audio.channel_volume(audio.ambient_channel_for_candidate(int(id))))
	ctx.log("ambient candidates=%d bound channels=%d (loudest volume %d) listener=%s" % [
			int(stats.ambient_candidates), bound.size(), bed_volume, str(listen_pos)])
	data["bound_channels"] = bound.size()
	data["bed_volume"] = bed_volume

	# Let the ambient bed spin up, then sample the one stream's steady peak on Master.
	var master_bus := AudioServer.get_bus_index(&"Master")
	var master_floor := UNMEASURED_DB
	var spin_until := Time.get_ticks_msec() + AMBIENT_SPIN_MS
	while Time.get_ticks_msec() < spin_until and not ctx.cancelled:
		await ctx.tree.process_frame
		audio.tick(listen_pos, 0.0)
		master_floor = maxf(master_floor, AudioServer.get_bus_peak_volume_left_db(master_bus, 0))
	ctx.log("pre-dialog peak: master=%.1f dB" % master_floor)
	data["pre_dialog_peaks"] = {"master_db": master_floor}

	# The first N dialog ids whose first line resolves to a wave of the dialog bank.
	var dialog_count := int(stats.dialogs)
	var ids: Array[int] = []
	for i in range(1, dialog_count + 1):
		if ids.size() >= lines:
			break
		if not String(audio.resolve_dialog_wave(i)).is_empty():
			ids.append(i)
	if ids.is_empty():
		_fail("no dialog id resolves to a wave of the dialog bank (dbf dialogs=%d)" % dialog_count)

	for id in ids:
		if ctx.cancelled:
			break
		var line := await _play_line(audio, id, ambient, bed_volume, master_bus, listen_pos)
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


func _play_line(audio: MissionAudio, id: int, ambient: bool, bed_volume: int,
		master_bus: int, listen_pos: Vector3) -> Dictionary:
	var set_name := String(audio.resolve_dialog_wave(id))
	var line := {"id": id, "set": set_name, "spawned": false, "audible": false, "advanced": false}
	if not audio.play_dialog(id):
		_fail("dlg%03d (%s): play_dialog returned false" % [id, set_name])
		return line
	# The dialog's first line loads on the next dialog tick, which the world runs
	# once a logic tick; the probe has no world, so it runs that tick itself.
	audio.advance_dialog_tick()
	if not audio.is_dialog_voice_playing():
		_fail("dlg%03d (%s): queued but no voice spawned" % [id, set_name])
		return line
	line["spawned"] = true
	# The dialog channel (3): its volume byte, the line's through the dialogue option.
	var dialog_volume := audio.channel_volume(3)
	var started := Time.get_ticks_msec()
	var deadline := started + LINE_TIMEOUT_MS
	var peak_master := UNMEASURED_DB
	while audio.is_dialog_voice_playing() and not _ctx.cancelled:
		if Time.get_ticks_msec() > deadline:
			break
		peak_master = maxf(peak_master, AudioServer.get_bus_peak_volume_left_db(master_bus, 0))
		await _ctx.tree.process_frame
		audio.tick(listen_pos, 0.0)
	var advanced: bool = not audio.is_dialog_voice_playing()
	var audible := dialog_volume > 0
	var length := float(Time.get_ticks_msec() - started) / MS_PER_S
	_ctx.log("dlg%03d set=%s played %.2fs volume %d -> spawn=OK audible=%s advance=%s | bed volume %d master peak %.1f dB" % [
			id, set_name, length, dialog_volume, str(audible), str(advanced), bed_volume, peak_master])
	line["length_s"] = length
	line["audible"] = audible
	line["advanced"] = advanced
	line["dialog_volume"] = dialog_volume
	line["peak_master_db"] = peak_master
	if not audible:
		_fail("dlg%03d (%s): the dialog channel opened at volume 0" % [id, set_name])
	if not advanced:
		_fail("dlg%03d (%s): voice never finished in time (queue jam)" % [id, set_name])
	# Masking: the bug class this pins is the ambient bed BURYING the dialog (pre-fix: a
	# clipping wall against quiet speech, inverted radius semantics). The per-voice levels
	# are IDA-witnessed (SoundBank_CalcDistanceVolPan @ 0x75ca20), so the gate only asserts
	# the bed's loudest channel stays below the dialog channel.
	if ambient and bed_volume > dialog_volume:
		_fail("dlg%03d: the ambient bed's channel volume %d is above the dialog's %d (dialog buried)" % [
				id, bed_volume, dialog_volume])
	return line
