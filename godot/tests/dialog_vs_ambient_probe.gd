extends SceneTree

# Headless DIALOG-vs-AMBIENCE probe (NOT collected by GUT): reproduces the
# "enabling ambient sounds broke mission dialog" report at the NovaMissionAudio
# seam against a mounted install. Runs the full mission-audio setup (ambient
# marker voices ON unless --no-ambient), then plays the first N resolvable .DBF
# dialog ids the way _route_mission_effects does, asserting for each line:
#   spawn    - play_dialog returned true and a dialog voice appeared
#   audible  - the Voice bus peak rises above the silence floor while playing
#   advance  - the voice finishes and the serialized queue moves on
# Also reports the Ambient/Master bus peaks while dialog plays (masking metric).
# Prints [dlgprobe] lines + a final PASS/FAIL verdict line.
#
# Use: godot --headless --path godot -s res://tests/dialog_vs_ambient_probe.gd \
#        -- <dir> <expansion> <mission.bms> [--no-ambient] [--lines N]

const NovaMissionAudioScript = preload("res://engine/world/nova_mission_audio.gd")

const SILENCE_FLOOR_DB := -50.0
const LINE_TIMEOUT_SLACK_S := 3.0

var _fail := false


func _initialize() -> void:
	call_deferred("_run")


func _fail_note(msg: String) -> void:
	_fail = true
	print("[dlgprobe] FAIL: %s" % msg)


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 3:
		print("[dlgprobe] usage: -- <dir> <expansion> <mission.bms> [--no-ambient] [--lines N]")
		quit(1)
		return
	var dir := args[0]
	var expansion := args[1]
	var mission_name := args[2]
	var ambient := true
	var lines := 3
	var sim_ticks := 0
	for i in range(3, args.size()):
		if args[i] == "--no-ambient":
			ambient = false
		elif args[i] == "--lines" and i + 1 < args.size():
			lines = int(args[i + 1])
		elif args[i] == "--sim" and i + 1 < args.size():
			sim_ticks = int(args[i + 1])

	var root := NovaResourceRoot.new()
	var err: int = root.mount_runtime(dir, expansion, false, "jo")
	print("[dlgprobe] mount %s exp='%s' -> %d  ambient=%s" % [dir, expansion, err, str(ambient)])
	if err != OK:
		quit(1)
		return
	var item_db := NovaItemDatabase.new()
	item_db.load_from_resource_root(root, "items.def")

	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(root, mission_name) != OK:
		print("[dlgprobe] %s PARSE FAILED" % mission_name)
		quit(1)
		return

	# Trigger-layer arm: step the bare sim and report which effect kinds fire on
	# startup ticks (no player spawned -> zone-gated triggers stay quiet; this
	# catches mission-start / delay-gated dialog). Also grabs the player-start
	# position so the audio arm can park the listener where the play test stood.
	var player_pos := Vector3.INF
	if sim_ticks > 0:
		var sim := NovaSimulation.new()
		if sim.load_from_mission_data(mission):
			if sim.has_method("spawn_local_player_at_start") and sim.spawn_local_player_at_start():
				player_pos = sim.get_local_player_position()
				print("[dlgprobe] player start = %s" % str(player_pos))
			var kinds: Dictionary = {}
			var dialog_ticks: Array = []
			for t in range(sim_ticks):
				sim.step()
				for e in sim.drain_effects():
					var k := String((e as Dictionary).get("kind", ""))
					kinds[k] = int(kinds.get(k, 0)) + 1
					if k == "dialog" or k == "dialog_wav":
						dialog_ticks.append("%s@t%d a=%d str=%s" % [k, t, int((e as Dictionary).get("a", 0)), String((e as Dictionary).get("str", ""))])
			print("[dlgprobe] sim %d ticks, effect kinds: %s" % [sim_ticks, str(kinds)])
			for d in dialog_ticks.slice(0, 12):
				print("[dlgprobe]   %s" % d)
		else:
			print("[dlgprobe] sim load FAILED")
		sim.free()

	var container := Node3D.new()
	get_root().add_child(container)
	var audio = NovaMissionAudioScript.new(root, item_db)
	if not ambient:
		# STRATEGY_TARGET_ID resolves no marker names -> banks + .DBF still load,
		# zero ambient candidates resolve. This is the "ambient sounds disabled" arm.
		audio.set_resolution_strategy(NovaMissionAudioScript.STRATEGY_TARGET_ID)
	var stats: Dictionary = audio.setup(mission, mission_name, container)
	print("[dlgprobe] setup: %s" % str(stats))

	# Listener parked at the player start when the sim arm resolved one (the real
	# play-test position), else at the resolved-marker centroid. (Headless has no
	# camera; without this the 3D voices attenuate against the origin.)
	var listen_pos := player_pos
	if not (listen_pos.is_finite() and listen_pos != Vector3.INF):
		listen_pos = Vector3.ZERO
	# A current Camera3D is the audio listener (a bare AudioListener3D in a
	# script-built tree leaves 3D voices mixing at zero — loop_matrix_probe.gd).
	var listener := Camera3D.new()
	container.add_child(listener)
	listener.global_position = listen_pos
	listener.make_current()
	# The real game culls voices beyond 240u from the camera each frame.
	audio.tick(listen_pos, 0.2)
	var marker_players: Array = container.find_children(
		"*", "AudioStreamPlayer3D", true, false)
	var near := 0
	var near_120 := 0
	for p in marker_players:
		var d := ((p as Node3D).global_position - listen_pos).length()
		if d <= 240.0:
			near += 1
		if d <= 120.0:
			near_120 += 1
	print("[dlgprobe] ambient candidates=%d physical voices=%d (within 240u of listener: %d, within 120u: %d) listener=%s" % [
		int(stats.get("ambient_candidates", 0)), marker_players.size(),
		near, near_120, str(listen_pos)])

	# Per-voice state for the nearest few: distinguishes "not playing" from
	# "playing but mixed to nothing" (headless 3D-audio artifact vs real bug).
	var by_dist := marker_players.duplicate()
	by_dist.sort_custom(func(a, b):
		return ((a as Node3D).global_position - listen_pos).length() < ((b as Node3D).global_position - listen_pos).length())
	for i in range(mini(5, by_dist.size())):
		var p := by_dist[i] as AudioStreamPlayer3D
		var d := (p.global_position - listen_pos).length()
		var pos0 := p.get_playback_position()
		await process_frame
		await process_frame
		var pos1 := p.get_playback_position()
		print("[dlgprobe]   voice d=%5.1fu playing=%s paused=%s pos %.3f->%.3f vol=%.1f dB unit=%.1f max=%.1f bus=%s stream_len=%.2f" % [
			d, str(p.playing), str(p.stream_paused), pos0, pos1, p.volume_db, p.unit_size, p.max_distance, p.bus, p.stream.get_length() if p.stream != null else -1.0])

	var voice_bus := AudioServer.get_bus_index(&"Voice")
	var ambient_bus := AudioServer.get_bus_index(&"Ambient")
	var master_bus := AudioServer.get_bus_index(&"Master")
	if voice_bus < 0:
		_fail_note("Voice bus missing from the layout")

	# Let the ambient bed spin up, then sample its steady peak over ~1s.
	var ambient_floor := -200.0
	var master_floor := -200.0
	var spin_until := Time.get_ticks_msec() + 1200
	while Time.get_ticks_msec() < spin_until:
		await process_frame
		if ambient_bus >= 0:
			ambient_floor = maxf(ambient_floor, AudioServer.get_bus_peak_volume_left_db(ambient_bus, 0))
		master_floor = maxf(master_floor, AudioServer.get_bus_peak_volume_left_db(master_bus, 0))
	print("[dlgprobe] pre-dialog peaks: ambient=%.1f dB master=%.1f dB" % [ambient_floor, master_floor])

	# First N dialog ids that resolve to a loaded set.
	var dialog_count := int(stats.get("dialogs", 0))
	var ids: Array[int] = []
	for i in range(1, dialog_count + 1):
		if ids.size() >= lines:
			break
		if String(audio.resolve_dialog_set(i)) != "":
			ids.append(i)
	if ids.is_empty():
		_fail_note("no dialog id resolves to a loaded set (dbf dialogs=%d)" % dialog_count)

	for id in ids:
		var set_name := String(audio.resolve_dialog_set(id))
		var ok: bool = audio.play_dialog(id)
		if not ok:
			_fail_note("dlg%03d (%s): play_dialog returned false" % [id, set_name])
			continue
		var voice: AudioStreamPlayer = audio.dialog_voice()
		if voice == null:
			_fail_note("dlg%03d (%s): queued but no voice spawned" % [id, set_name])
			continue
		var length: float = voice.stream.get_length() if voice.stream != null else 0.0
		var loop_mode := -1
		if voice.stream is AudioStreamWAV:
			loop_mode = (voice.stream as AudioStreamWAV).loop_mode
		var deadline := Time.get_ticks_msec() + int((length + LINE_TIMEOUT_SLACK_S) * 1000.0)
		var peak_voice := -200.0
		var peak_ambient := -200.0
		var peak_master := -200.0
		while audio.dialog_voice() != null:
			if Time.get_ticks_msec() > deadline:
				break
			if voice_bus >= 0:
				peak_voice = maxf(peak_voice, AudioServer.get_bus_peak_volume_left_db(voice_bus, 0))
			if ambient_bus >= 0:
				peak_ambient = maxf(peak_ambient, AudioServer.get_bus_peak_volume_left_db(ambient_bus, 0))
			peak_master = maxf(peak_master, AudioServer.get_bus_peak_volume_left_db(master_bus, 0))
			await process_frame
		var advanced: bool = audio.dialog_voice() == null
		var audible := peak_voice > SILENCE_FLOOR_DB
		print("[dlgprobe] dlg%03d set=%s len=%.2fs loop_mode=%d -> spawn=OK audible=%s (voice peak %.1f dB) advance=%s | ambient peak %.1f dB master peak %.1f dB" % [
			id, set_name, length, loop_mode, str(audible), peak_voice, str(advanced), peak_ambient, peak_master])
		if not audible:
			_fail_note("dlg%03d (%s): voice never rose above %.0f dB on the Voice bus" % [id, set_name, SILENCE_FLOOR_DB])
		if not advanced:
			_fail_note("dlg%03d (%s): voice never finished within len+%.0fs (queue jam)" % [id, set_name, LINE_TIMEOUT_SLACK_S])
		# Masking: the bug class this probe pins is the ambient bed BURYING the
		# dialog (pre-fix: +21.5 dB clipping wall vs -0.6 dB speech — inverted
		# radius semantics). The per-voice levels are IDA-witnessed
		# (SoundBank_CalcDistanceVolPan @ 0x75ca20), so the gate only asserts
		# the bed stays below the speech peak and below clipping. Only
		# meaningful when the 3D path actually mixed (windowed run); headless
		# mixes 3D voices to nothing.
		if ambient and peak_ambient > -180.0:
			if peak_ambient >= peak_voice:
				_fail_note("dlg%03d: ambient bed peak %.1f dB is above the dialog voice peak %.1f dB (dialog buried)" % [id, peak_ambient, peak_voice])
			elif peak_ambient >= 0.0:
				_fail_note("dlg%03d: ambient bed peak %.1f dB is clipping" % [id, peak_ambient])

	audio.teardown()
	print("[dlgprobe] VERDICT: %s (ambient=%s)" % ["FAIL" if _fail else "PASS", str(ambient)])
	quit(1 if _fail else 0)
