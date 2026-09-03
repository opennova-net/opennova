extends GameProbe

## perf_fire: the full-auto game-shell performance probe. Equips a clip
## weapon, aims into nearby terrain (or an F3 pose dump), taps the trigger
## three times for the first-shot hitch split, then measures BASELINE /
## FIRING x2 (reload between) / COOLDOWN with vsync off and per-second counter
## rows, followed by the A/B legs that attribute the idle frame: HUD canvas,
## world tick, HUD tick, present transform/visibility/body channels,
## occlusion, the hard water reflection, the particle fixed tick, the fixed
## handlers, every other node's _process, and the placed models' _process.
## Fails on a firing regression (avg > 1.5x + 1 ms or p95 > 2x + 2 ms over
## baseline). `baseline_only` measures BASELINE and the world-tick leg only
## (the joiner-side MP-vs-SP A/B on a --lan-join launch). Needs a window:
## render cost is the question.

const WEAPON_CANDIDATES := ["WPN_M249", "WPN_M60", "WPN_AK47", "WPN_M16", "WPN_M4", "WPN_M4AUTO"]

var _ctx: ProbeContext
var _vprid := RID()
# Frame segmentation: [post_draw -> process_frame] = swap/present + OS pump +
# physics; [process_frame -> pre_draw] = the process step; [pre -> post] = draw.
var _seg_t_pf := 0
var _seg_t_pre := 0
var _seg_sum := [0, 0, 0]
var _seg_n := 0
var _seg_worst_draw := 0
var _phase := ""
var _sample_t0 := 0
var _world_skipped := false
var _hud_skipped := false


func run(ctx: ProbeContext) -> ProbeVerdict:
	# The launch lands under the start-mission splash with the world held
	# un-ticked; leave it and wait for the local player before measuring.
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	_ctx = ctx
	var pose_json := String(ctx.args.get("pose_json", ""))
	var look_dy := float(ctx.args.get("look_dy", 120.0))
	var fire_s := float(ctx.args.get("fire_seconds", 3.0))
	var baseline_only := bool(ctx.args.get("baseline_only", false))
	var forced_weapon := String(ctx.args.get("weapon", ""))
	var taps := bool(ctx.args.get("taps", true))
	var sim := ctx.sim()
	var world := ctx.world()
	var runtime := ctx.runtime()
	var shell := ctx.game()
	if sim == null or world == null or runtime == null or shell == null:
		return ProbeVerdict.failed("no loaded mission")
	await ctx.wait_ms(5000)
	ProbePerfSetup.enable_spans(ctx)
	ProbePerfSetup.uncap_frame_rate(ctx)
	_vprid = ProbePerfSetup.measure_render_time(ctx)
	_connect_segmentation(ctx)
	ctx.log("census: %s" % str(ProbePerfSetup.node_census(ctx.tree)))
	var data := {}

	# The spawn kit equips the KNIFE - holding fire on it measures knife swings,
	# not automatic fire. Switch to a clip-carrying weapon first so the FIRING
	# phases exercise the real full-auto path (rounds, tracers, impacts, sounds).
	data["equipped"] = await _equip_clip_weapon(forced_weapon)
	# A pose dump (the F3 Player-tab JSON) lands the probe at an exact recorded
	# position + aim before measuring; otherwise the look pitch walks the
	# impact point into nearby terrain so every round lands its impact effects.
	if not pose_json.is_empty() and _apply_pose_dump(pose_json):
		await ctx.wait_ms(800)
	else:
		ProbeInput.look(Vector2(0, look_dy))
		await ctx.wait_ms(1500)
	# First-shot hitch attribution: the very first live round pays every lazy
	# one-time (pipeline compiles, texture/sound resolves). Tap 1 fires with
	# the particle master switch hidden, tap 2 with particles live, tap 3 is
	# the repeat control (a flat tap 3 proves the cost is one-time). The taps
	# also consume the first-times, so the FIRING phases below measure clean
	# sustained cost.
	if taps and not baseline_only:
		world.set_particles_hidden(true)
		await ctx.wait_ms(400)
		var tap1 := await _tap_and_measure("tap1-particles-hidden")
		world.set_particles_hidden(false)
		await ctx.wait_ms(400)
		var tap2 := await _tap_and_measure("tap2-particles-live")
		await ctx.wait_ms(400)
		var tap3 := await _tap_and_measure("tap3-repeat")
		ctx.log("first-shot hitch: hidden=%.1fms live=%.1fms repeat=%.1fms" % [tap1, tap2, tap3])
		data["first_shot_hitch_ms"] = {"hidden": tap1, "live": tap2, "repeat": tap3}
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled")

	var base := await _measure("baseline", 5000)
	data["baseline"] = base
	if baseline_only:
		# One structural A/B: the whole world tick (sim + netsim pump + present
		# passes) off for 3 s. A session tolerates it (peer timeout is 120 s).
		var worldoff := await _leg_world_off()
		ctx.log(ProbeFrameSampler.report_line("BASELINE", base))
		if float(worldoff.avg) >= 0.0:
			ctx.log("WORLDOFF avg=%.2fms (world.tick share vs baseline: %+.2fms)" % [
					float(worldoff.avg), float(base.avg) - float(worldoff.avg)])
			data["worldoff"] = worldoff
		return ProbeVerdict.passed("baseline %.2f ms" % float(base.avg), data)

	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
	var fire1 := await _measure("firing1", int(fire_s * 1000.0))
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
	ProbeInput.hold(KEY_R, true)
	await ctx.wait_ms(120)
	ProbeInput.hold(KEY_R, false)
	await ctx.wait_ms(2600)
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
	var fire2 := await _measure("firing2", int(fire_s * 1000.0))
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
	await ctx.wait_ms(1500)
	var cool := await _measure("cooldown", 5000)
	data["firing1"] = fire1
	data["firing2"] = fire2
	data["cooldown"] = cool
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled")

	var legs := await _attribution_legs(shell, world, runtime)
	data["legs"] = legs

	ctx.log(ProbeFrameSampler.report_line("BASELINE", base))
	ctx.log(ProbeFrameSampler.report_line("FIRING1 ", fire1))
	ctx.log(ProbeFrameSampler.report_line("FIRING2 ", fire2))
	ctx.log(ProbeFrameSampler.report_line("COOLDOWN", cool))
	for leg in legs:
		var st: Dictionary = legs[leg]
		if float(st.avg) >= 0.0:
			ctx.log("%-12s avg=%.2fms (share vs cooldown: %+.2fms)" % [
					String(leg).to_upper(), float(st.avg), float(cool.avg) - float(st.avg)])
	var base_avg: float = base.avg
	var base_p95: float = base.p95
	var cool_avg: float = cool.avg
	var fire_avg: float = maxf(fire1.avg, fire2.avg)
	var fire_p95: float = maxf(fire1.p95, fire2.p95)
	var regressed: bool = (fire_avg > base_avg * 1.5 + 1.0) or (fire_p95 > base_p95 * 2.0 + 2.0)
	var leaky: bool = cool_avg > base_avg * 1.3 + 1.0
	ctx.log("deltas: fire_avg %.2fx base | fire_p95 %.2fx base | cooldown %.2fx base" % [
			fire_avg / maxf(base_avg, 0.001), fire_p95 / maxf(base_p95, 0.001),
			cool_avg / maxf(base_avg, 0.001)])
	if leaky:
		ctx.log("LEAK SUSPECT: cooldown never returned to baseline")
	data["regressed"] = regressed
	data["leak_suspect"] = leaky
	if regressed:
		return ProbeVerdict.failed("REGRESSION: firing is markedly slower than baseline", data)
	return ProbeVerdict.passed("firing within the baseline envelope", data)


# --- the attribution legs -----------------------------------------------------------

func _attribution_legs(shell: Node, world: GameWorld, runtime: MissionRoot) -> Dictionary:
	var ctx := _ctx
	var legs := {}
	# HUD-canvas A/B: hide the WHOLE HUD layer. Ticks still run - this
	# isolates the _draw/canvas side from the info-build side.
	var hud := ctx.hud_presenter()
	var hud_layer: Node = hud.get_ui_parent() if hud != null else null
	if hud_layer != null and "visible" in hud_layer:
		var was_visible: bool = hud_layer.get("visible")
		hud_layer.set("visible", false)
		legs["hudoff"] = await _measure("hudoff", 3000)
		hud_layer.set("visible", was_visible)
	# Split A/B: which half of the shell's frame callback drags the
	# out-of-process cost with it (deferred/RS-side work its calls generate)?
	legs["worldoff"] = await _leg_world_off()
	shell.get_perf_probe_switches().skip_hud = true
	_hud_skipped = true
	await ctx.wait_ms(500)
	legs["hudtickoff"] = await _measure("hudtickoff", 3000)
	shell.get_perf_probe_switches().skip_hud = false
	_hud_skipped = false
	# Existing presenter options provide state-safe A/Bs without a production
	# probe branch: freeze transform/visibility/body submission independently
	# while simulation, body posing and muzzle feedback continue normally.
	var present := runtime.get_entity_presenter()
	if present != null:
		var channels := present.get_output_channels()
		for leg in [["xformoff", EntityPresenter.OUTPUT_TRANSFORM], ["visoff", EntityPresenter.OUTPUT_VISIBILITY],
				["bodyoff", EntityPresenter.OUTPUT_BODY_ANIM]]:
			present.set_output_channels(channels & ~int(leg[1]))
			await ctx.wait_ms(500)
			legs[leg[0]] = await _measure(leg[0], 3000)
			present.set_output_channels(channels)
	# Occlusion legs A/B (visibility writes across the occluded set per frame).
	world.set_perf_probe_skip_occlusion(true)
	await ctx.wait_ms(500)
	legs["occloff"] = await _measure("occloff", 3000)
	world.set_perf_probe_skip_occlusion(false)
	# HARD reflection off: stop the water script first (it re-asserts the update
	# mode every frame), THEN disable the RTT.
	var water := world.get_water_node()
	var reflection: SubViewport = water.get_reflection_viewport() if water != null else null
	if water != null and reflection != null:
		await ctx.wait_ms(500)
		var was_mode := reflection.render_target_update_mode
		reflection.render_target_update_mode = SubViewport.UPDATE_DISABLED
		legs["reflhardoff"] = await _measure("reflhardoff", 3000)
		reflection.render_target_update_mode = was_mode
	# Particle fixed-tick A/B (advance_fixed_tick runs per 62 Hz tick - 8-9x per
	# frame at low FPS).
	world.set_perf_probe_skip_effect_tick(true)
	await ctx.wait_ms(500)
	legs["fxtickoff"] = await _measure("fxtickoff", 3000)
	world.set_perf_probe_skip_effect_tick(false)
	# The direct simulation/presentation timing counters remain observational;
	# skipping either bundle would mutate gameplay state and corrupt later legs.
	world.set_perf_probe_skip_fixed_handlers(true)
	await ctx.wait_ms(500)
	legs["handleroff"] = await _measure("handleroff", 3000)
	world.set_perf_probe_skip_fixed_handlers(false)
	# Residual bisect, LAST because it broadly mutates processing state: turn
	# off every OTHER node's _process (the shell keeps ticking the world). If
	# the frame collapses toward the measured shell spans, the process residual
	# is node _process work; if it barely moves, the residual is engine-internal.
	var others: Array = []
	var walk: Array = [ctx.tree.root]
	while not walk.is_empty():
		var walk_node: Node = walk.pop_back()
		for walk_child in walk_node.get_children():
			walk.push_back(walk_child)
		if walk_node == shell or walk_node == ctx.tree.root:
			continue
		if walk_node.is_processing():
			others.append(walk_node)
	ctx.log("otherprocoff: disabling _process on %d node(s)" % others.size())
	var other_states := ProbeProcessGuard.disable_processing(others)
	await ctx.wait_ms(500)
	legs["otherprocoff"] = await _measure("otherprocoff", 3000)
	ProbeProcessGuard.restore_processing(other_states)
	# Finer attribution of the remaining _process share: the placed-model set alone.
	var models := ProbePerfSetup.nodes_of_class(ctx.tree, "ObjectModel")
	var model_states := ProbeProcessGuard.disable_processing(models)
	await ctx.wait_ms(500)
	legs["modelprocoff"] = await _measure("modelprocoff", 3000)
	ProbeProcessGuard.restore_processing(model_states)
	return legs


func _leg_world_off() -> Dictionary:
	var shell := _ctx.game()
	if shell == null:
		return {avg = -1.0}
	shell.get_perf_probe_switches().skip_world = true
	_world_skipped = true
	await _ctx.wait_ms(300)
	var st := await _measure("worldoff", 3000)
	shell.get_perf_probe_switches().skip_world = false
	_world_skipped = false
	return st


# --- sampling ----------------------------------------------------------------------

func _connect_segmentation(ctx: ProbeContext) -> void:
	var tree := ctx.tree
	tree.process_frame.connect(_on_seg_process_frame)
	RenderingServer.frame_pre_draw.connect(_on_seg_pre_draw)
	RenderingServer.frame_post_draw.connect(_on_seg_post_draw)
	ctx.defer_restore(func() -> void:
		if tree.process_frame.is_connected(_on_seg_process_frame):
			tree.process_frame.disconnect(_on_seg_process_frame)
		if RenderingServer.frame_pre_draw.is_connected(_on_seg_pre_draw):
			RenderingServer.frame_pre_draw.disconnect(_on_seg_pre_draw)
		if RenderingServer.frame_post_draw.is_connected(_on_seg_post_draw):
			RenderingServer.frame_post_draw.disconnect(_on_seg_post_draw))


func _on_seg_process_frame() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pre > 0:
		_seg_sum[2] += now - _seg_t_pre  # post-draw -> next process_frame
		_seg_n += 1
	_seg_t_pf = now


func _on_seg_pre_draw() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pf > 0:
		_seg_sum[0] += now - _seg_t_pf  # process step
	_seg_t_pre = now


func _on_seg_post_draw() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pre > 0:
		_seg_sum[1] += now - _seg_t_pre  # draw
		_seg_worst_draw = maxi(_seg_worst_draw, now - _seg_t_pre)
	_seg_t_pre = now


func _measure(phase: String, duration_ms: int) -> Dictionary:
	_phase = phase
	_sample_t0 = Time.get_ticks_msec()
	_ctx.progress({"phase": phase})
	return await ProbeFrameSampler.measure(_ctx.tree, duration_ms, _counter_row, _ctx.log,
			func() -> bool: return _ctx.cancelled)


func _counter_row(sec_frames: int, sec_accum: float) -> String:
	var t := float(Time.get_ticks_msec() - _sample_t0) / 1000.0
	var fps := 0.0
	if sec_frames > 0 and sec_accum > 0.0:
		fps = float(sec_frames) / (sec_accum / 1000.0)
	var rt := "-"
	var runtime := _ctx.runtime()
	if runtime != null:
		var rc := runtime.get_perf_counters()
		rt = "sim=%.2f present=%.2f fx=%.2f ticks=%d" % [
				float(rc.sim_us) / 1000.0, float(rc.present_us) / 1000.0,
				float(rc.effects_us) / 1000.0, rc.ticks]
	var parts := "-"
	var effect_world := _ctx.effect_world()
	if effect_world != null:
		var groups: Array = effect_world.get_debug_group_report()
		var alive := 0
		for g_v in groups:
			for e_v in (g_v as EffectGroupReport).emitters:
				alive += (e_v as EffectEmitterReport).alive
		parts = "%d/%d" % [groups.size(), alive]
	var spans := ""
	var shell := _ctx.game()
	if shell != null:
		var mg: Dictionary = shell.get_perf_probe_switches().spans
		if not mg.is_empty():
			spans += " main{before=%.1f world=%.1f after=%.1f hud=%.1f}" % [
					float(mg.get("before", 0)) / 1000.0, float(mg.get("world", 0)) / 1000.0,
					float(mg.get("after", 0)) / 1000.0, float(mg.get("hud", 0)) / 1000.0]
	var world := _ctx.world()
	if world != null and not _world_skipped:
		# The per-leg world spans (occlusion, iris, weather, blink) are the F3
		# Stats board's WORLD_* rows now (the leg table banks them); the typed
		# tick counters stay on the world.
		var pc: RuntimePerfCounters = world.get_runtime_perf_counters()
		spans += " gwtick{total=%.1f foliage=%.1f runtime=%.1f audio=%.1f}" % [
				float(pc.tick_us) / 1000.0, float(pc.foliage_us) / 1000.0,
				float(pc.runtime_us) / 1000.0, float(pc.audio_us) / 1000.0]
		var sim := world.get_sim()
		if sim != null:
			var sc: Dictionary = sim.get_runtime_perf_counters()
			if int(sc.get("trace_calls", 0)) > 0:
				spans += " trace{n=%d ter=%.1f sta=%.1f dyn=%.1f per=%.1f surv=%d/%d/%d faces=%d/%d}" % [
						int(sc.get("trace_calls", 0)),
						float(sc.get("trace_terrain_us", 0)) / 1000.0,
						float(sc.get("trace_static_us", 0)) / 1000.0,
						float(sc.get("trace_dynamic_us", 0)) / 1000.0,
						float(sc.get("trace_person_us", 0)) / 1000.0,
						int(sc.get("trace_static_survivors", 0)),
						int(sc.get("trace_dynamic_survivors", 0)),
						int(sc.get("trace_person_survivors", 0)),
						int(sc.get("trace_static_faces", 0)),
						int(sc.get("trace_dynamic_faces", 0))]
	var rmeas := ""
	if _vprid.is_valid():
		rmeas = " rcpu=%.1f rgpu=%.1f" % [
				RenderingServer.viewport_get_measured_render_time_cpu(_vprid),
				RenderingServer.viewport_get_measured_render_time_gpu(_vprid)]
	if _seg_n > 0:
		rmeas += " seg{proc=%.1f draw=%.1f post=%.1f}" % [
				float(_seg_sum[0]) / 1000.0 / _seg_n,
				float(_seg_sum[1]) / 1000.0 / _seg_n,
				float(_seg_sum[2]) / 1000.0 / _seg_n]
		_seg_sum = [0, 0, 0]
		_seg_n = 0
	var trails := "-"
	var live_sim := _ctx.sim()
	if live_sim != null:
		var rows: PackedFloat32Array = live_sim.get_tracer_trails()
		var channels := 0
		var i := 0
		while i + 2 < rows.size():
			var count := int(rows[i + 2])
			i += 3
			if count < 0 or i + count * 4 > rows.size():
				break
			channels += 1
			i += count * 4
		trails = "%d(%df)" % [channels, rows.size()]
	return ("%s t+%4.1fs fps=%6.1f proc=%5.2fms phys=%5.2fms %s draws=%5d objs=%5d prims=%8d nodes=%5d orphans=%4d parts=%s trails=%s%s" % [
			_phase, t, fps,
			Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
			Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0,
			rt,
			int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
			int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)),
			int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)),
			int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)),
			int(Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT)),
			parts, trails, spans + rmeas])


# --- setup helpers --------------------------------------------------------------------

# The direct spawn kit carries no gun at all (knife + grenades + claymore), so
# holding fire would measure knife swings / claymore throws. Apply a rifle kit
# through the same seam the deploy screen uses (apply_local_player_loadout +
# the inventory->presentation sync), candidates in table order - the first
# name the mission's weapon table resolves wins. Returns the equipped name.
func _equip_clip_weapon(forced: String) -> String:
	var ctx := _ctx
	var sim := ctx.sim()
	var world := ctx.world()
	if sim == null or world == null:
		return ""
	ctx.log("spawn inventory: %s" % str(sim.get_local_player_inventory().slots))
	var candidates: Array = WEAPON_CANDIDATES.duplicate()
	if not forced.is_empty():
		candidates.push_front(forced)
	for weapon_name in candidates:
		var kit: Array[WeaponKitEntry] = [WeaponKitEntry.make(weapon_name)]
		if not bool(sim.apply_local_player_loadout(kit, 0)):
			continue
		var inventory := sim.get_local_player_inventory()
		var equipped := inventory.equipped_name
		if equipped.is_empty():
			continue
		# Mirror the armory's post-apply install exactly: world weapon THEN the
		# player presenter's viewmodel refresh, or the first-person arms keep the
		# knife while the sim fires the rifle.
		if world.set_local_player_weapon_by_name(equipped):
			var presenter := ctx.presenter()
			if presenter != null:
				presenter.refresh_viewmodel()
		await ctx.wait_ms(1500)  # draw anim settles before the baseline
		var view := world.local_player_weapon_view()
		ctx.log("equipped: %s (clip %d, reserve %d)" % [equipped,
				int(view.clip) if view != null else -1,
				int(view.reserve) if view != null else -1])
		return equipped
	ctx.log("WARNING: no rifle kit applied; firing whatever is equipped")
	return ""


# Apply an F3 debug snapshot (opennova.debug_snapshot.v1): teleport the local
# player to its mission position + yaw/pitch via the sim's debug seam.
func _apply_pose_dump(path: String) -> bool:
	var ctx := _ctx
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		ctx.log("pose dump unreadable: %s" % path)
		return false
	var parsed: Variant = JSON.parse_string(text)
	if not (parsed is Dictionary):
		ctx.log("pose dump is not valid JSON: %s" % path)
		return false
	var player: Dictionary = (parsed as Dictionary).get("player", {})
	var bms: Dictionary = player.get("position_bms", {})
	var orientation: Dictionary = player.get("orientation_mission_deg", {})
	var sim := ctx.sim()
	if bms.is_empty() or sim == null:
		return false
	var pos := Vector3(float(bms.get("x", 0.0)), float(bms.get("y", 0.0)), float(bms.get("z", 0.0)))
	var yaw := float(orientation.get("yaw", 0.0))
	var pitch := float(orientation.get("pitch", 0.0))
	sim.debug_teleport_local_player(pos, yaw, pitch)
	ctx.log("pose applied: bms(%.1f, %.1f, %.1f) yaw %.1f pitch %.1f (%s)" % [
			pos.x, pos.y, pos.z, yaw, pitch, path.get_file()])
	return true


# One ~150 ms trigger tap; returns the worst frame time observed over the
# 600 ms window around it (the hitch detector).
func _tap_and_measure(label: String) -> float:
	var ctx := _ctx
	var worst := 0.0
	var last := Time.get_ticks_usec()
	var start := Time.get_ticks_msec()
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
	while Time.get_ticks_msec() - start < 600 and not ctx.cancelled:
		if Time.get_ticks_msec() - start >= 150:
			ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
		await ctx.tree.process_frame
		var now := Time.get_ticks_usec()
		worst = maxf(worst, float(now - last) / 1000.0)
		last = now
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
	# Split the worst frame: a big draw share = pipeline compile at first draw;
	# a small one = CPU-side cost (spawn/texture decode) in the process step.
	ctx.log("%s worst frame %.1fms (worst draw seg %.1fms)" % [label, worst, float(_seg_worst_draw) / 1000.0])
	_seg_worst_draw = 0
	return worst
