extends SceneTree

# Impact-scar in-game probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Boots a mission in the real
# standalone game, finds the nearest struck-able entity face from the spawn,
# fires a volley of rounds at it through the REAL RoundSim (debug_spawn_round),
# and reports the scar presentation counters — the ring wrap shows as
# slots_live capped by the 256-slot ring while more rounds than that landed
# [orig: Scar_AdvanceRingCursor @0x5CC1D0; Scar_RenderCache @0x5CD830].
#
#   NW_SP_MISSION=00TRa.bms NW_RESOURCE_DIR=<retail install> \
#   NW_PROBE_SHOTS=<dir> "$GODOT_BIN" --path godot -s res://tests/scar_wall_probe.gd
#
# A NON-headless run with NW_PROBE_SHOTS saves scar_overview.png (from the
# firing eye) and scar_close.png (a probe camera 1.6 u off the struck face).
# godot-pos <-> mission-pos: mission = (x, -z, y) of the godot vector.

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const ROUNDS := 300
const ROUNDS_PER_FRAME := 10
const AMMO := "AMMO_CAR15_556MM"
const PICK_RANGE := 60.0

var _shots_dir := ""
var _shot_vp: SubViewport = null
var _shot_cam: Camera3D = null


func _initialize() -> void:
	_shots_dir = OS.get_environment("NW_PROBE_SHOTS")
	call_deferred("_run")


func _fail(msg: String) -> void:
	print("PROBE FAIL: %s" % msg)
	quit(1)


func _wait_frames(n: int) -> void:
	for _i in range(n):
		await process_frame


func _shot(name: String, from: Vector3, at: Vector3) -> void:
	if _shots_dir.is_empty():
		return
	if _shot_cam == null:
		_shot_vp = SubViewport.new()
		_shot_vp.size = Vector2i(1280, 720)
		_shot_vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		root.add_child(_shot_vp)
		_shot_vp.world_3d = root.world_3d
		_shot_cam = Camera3D.new()
		_shot_cam.fov = 55.0
		_shot_vp.add_child(_shot_cam)
	_shot_cam.position = from
	_shot_cam.look_at(at)
	_shot_cam.make_current()
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var img := _shot_vp.get_texture().get_image()
	if img == null:
		return
	var path := _shots_dir.path_join("scar_%s.png" % name)
	img.save_png(path)
	print("PROBE shot saved: %s" % path)


# The nearest pickable STATIC entity (a building/prop wall) from the eye over
# 24 yaws at three pitches. The picker reports the entity origin + bound
# radius, not the struck point: the volley flies along the pick ray, which by
# construction crosses one of that entity's faces.
func _find_target(sim, eye: Vector3, pitches: Array) -> Dictionary:
	var best := {}
	var best_dist := INF
	for pitch_deg in pitches:
		for i in range(24):
			var yaw := TAU * float(i) / 24.0
			var dir := Vector3(cos(yaw), 0.0, sin(yaw)).rotated(
					Vector3(-sin(yaw), 0.0, cos(yaw)), deg_to_rad(pitch_deg)).normalized()
			var pick: Dictionary = sim.debug_pick_entity(eye, dir, PICK_RANGE)
			if not bool(pick.get("hit", false)):
				continue
			if String(pick.get("hit_class", "")) == "person":
				continue
			var origin_v: Variant = pick.get("position_godot", null)
			if not (origin_v is Vector3):
				continue
			var d: float = eye.distance_to(origin_v)
			if d < 2.0 or d >= best_dist:
				continue
			best_dist = d
			best = pick.duplicate()
			best["dir"] = dir
			# The struck face from the projectile trace itself.
			best["hit_godot"] = pick.get("hit_position_godot", eye + dir)
	return best


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("scar_wall_probe: set NW_SP_MISSION=00TRa.bms")
		quit(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR")
	if not res_dir.is_empty():
		var settings := load("res://game/resource_index/resource_dir_settings.gd")
		settings.set_resource_dir(res_dir)
		settings.set_expansion("")
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		_fail("failed to load main_game.tscn")
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		_fail("main_game lacks a World child")
		return
	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			_fail("player never spawned (mission load stalled?)")
			return
	var sim = world.get_sim()
	await _wait_frames(90)
	# The SP start-mission splash holds the world UN-TICKED until its dismissal
	# edge (main_game.gd: `if _world_load.is_splash_active(): return` before the
	# tick; retail has not returned from Game_StartMission @0x525d42 yet). A
	# volley fired under the splash spawns rounds that never fly — zero stops,
	# zero scars, and the splash layer owns the viewport (black captures). Enter
	# gameplay through the production seam, then prove the world ticks.
	game.dismiss_start_mission_splash()
	await _wait_frames(30)
	var tick_before := int(sim.get_round_debug().get("tick", 0))
	await _wait_frames(10)
	var tick_probe := int(sim.get_round_debug().get("tick", 0))
	if tick_probe <= tick_before:
		_fail("the world is not ticking (logic tick %d -> %d); the start-mission splash or a pause still holds it" % [tick_before, tick_probe])
		return
	print("PROBE world ticking: logic tick %d -> %d over 10 frames" % [tick_before, tick_probe])

	var player: Vector3 = sim.get_local_player_position()
	var eye := player + Vector3.UP * 1.6
	# A level pick first (a wall face, not a lintel underside), then the
	# pitched sweeps.
	var target := _find_target(sim, eye, [0.0])
	if target.is_empty():
		target = _find_target(sim, eye, [-10.0, 10.0])
	if target.is_empty():
		_fail("no pickable entity face within %.0f u of the spawn" % PICK_RANGE)
		return
	var hit: Vector3 = target["hit_godot"]
	var dir: Vector3 = target["dir"]
	print("PROBE target: entity=%s %s '%s' at %s (%.1f u) section=%s face=%s surface=%s material_flags=%s" % [
			str(target.get("entity_handle", -1)), str(target.get("hit_class", "")),
			str(target.get("name", "")), str(hit), eye.distance_to(hit),
			str(target.get("section", -1)), str(target.get("face", -1)),
			str(target.get("surface_type", -1)), str(target.get("material_flags", 0))])

	# The volley: jittered around the pick direction so the quads spread and
	# the ring wraps (more rounds than the 256-slot ring holds).
	var spawned := 0
	var rng := RandomNumberGenerator.new()
	rng.seed = 0x5CC830
	while spawned < ROUNDS:
		for _k in range(ROUNDS_PER_FRAME):
			if spawned >= ROUNDS:
				break
			var jitter := Vector3(rng.randf_range(-0.08, 0.08),
					rng.randf_range(-0.08, 0.08), rng.randf_range(-0.08, 0.08))
			if sim.debug_spawn_round(eye, (dir + jitter).normalized(), AMMO) >= 0:
				spawned += 1
		await process_frame
	await _wait_frames(120)

	# Where the rounds stopped, from the sim's round debug trail.
	var trail: Dictionary = sim.get_round_debug()
	print("PROBE logic tick after the volley: %d" % int(trail.get("tick", 0)))
	var by_kind := {}
	var samples := []
	var stop_sum := Vector3.ZERO
	var stop_count := 0
	for ev in trail.get("events", []):
		var kind := String(ev.get("kind_name", "?"))
		by_kind[kind] = int(by_kind.get(kind, 0)) + 1
		if kind == "item face":
			var hit_v: Variant = ev.get("hit", null)
			if hit_v is Vector3:
				stop_sum += hit_v
				stop_count += 1
			if samples.size() < 3:
				samples.append("%s s%s f%s m%s @%s" % [str(ev.get("entity_name", "")),
						str(ev.get("section", -1)), str(ev.get("face", -1)),
						str(ev.get("material", -1)), str(ev.get("hit", Vector3()))])
	print("PROBE round stops: %s samples=%s" % [str(by_kind), str(samples)])
	# The captures aim at where the rounds actually stopped (the struck face may
	# not be the picker's face): the volley's stop centroid.
	if stop_count > 0:
		hit = stop_sum / float(stop_count)
		print("PROBE stop centroid (Godot): %s (%.2f u from the eye)" % [str(hit), eye.distance_to(hit)])
	var stats = world.get_scar_present_stats()
	if stats == null:
		_fail("the runtime owns no scar presentation pass")
		return
	print("PROBE scars: spawned=%d slots_live=%d slots_culled=%d rings_leased=%d batches=%d world_surfaces=%d entity_meshes=%d textures_missing=%d owners_unresolved=%d" % [
			spawned, stats.slots_live, stats.slots_culled, stats.rings_leased,
			stats.batches, stats.world_surfaces, stats.entity_meshes,
			stats.textures_missing, stats.owners_unresolved])
	var draw: Dictionary = sim.get_scar_draw_list(eye, 0.0, Color.WHITE)
	var verts: PackedVector3Array = draw.get("vertices", PackedVector3Array())
	var batch_flags: PackedInt32Array = draw.get("batch_flags", PackedInt32Array())
	var batch_first: PackedInt32Array = draw.get("batch_first", PackedInt32Array())
	var batch_count: PackedInt32Array = draw.get("batch_count", PackedInt32Array())
	print("PROBE draw list: vertices=%d batches=%d" % [
			verts.size(), (draw.get("batch_owner", PackedInt32Array()) as PackedInt32Array).size()])
	# The winding fold: Godot's front face is CLOCKWISE in a right-handed frame,
	# i.e. a triangle whose coordinate cross product points AWAY from the viewer
	# is front-facing. A shared-ring quad must front-face the shooter (the
	# struck face's side) so the scorch shader's cull_back culls the back
	# exactly as retail's CCW cull does [orig: Scar_RenderCache @0x5CD830 under
	# Math_FixedPointToFloat3_YNegated; the drawer's mode word 0x120651].
	# Entity-ring batches are section-local and skipped here.
	var facing := 0
	var away := 0
	var front_sum := Vector3.ZERO
	for b in range(batch_first.size()):
		if (batch_flags[b] & 1) != 0:
			continue
		var t := batch_first[b]
		while t + 2 < batch_first[b] + batch_count[b]:
			var n := (verts[t + 1] - verts[t]).cross(verts[t + 2] - verts[t])
			var centroid := (verts[t] + verts[t + 1] + verts[t + 2]) / 3.0
			if n.dot(eye - centroid) < 0.0:
				facing += 1
			else:
				away += 1
			if n.length() > 0.0:
				front_sum -= n.normalized()
			t += 3
	print("PROBE winding (shared ring, Godot space, clockwise front): %d triangles front-face the firing eye, %d face away" % [facing, away])
	# The captures look straight along the struck face's normal (the quads'
	# front), from 1 u off the face; the behind shot from 1 u inside it.
	var back := (eye - hit).normalized()
	var face_n := front_sum.normalized() if front_sum.length() > 0.0 else back
	print("PROBE struck face front (Godot): %s" % str(face_n))
	await _shot("overview", eye, hit)
	await _shot("close", hit + face_n * 1.0, hit)
	# From BEHIND the struck face: retail's scorch state culls the back (CCW),
	# so nothing of the volley may show through the wall here.
	await _shot("behind", hit - face_n * 1.0, hit)
	print("PROBE done")
	quit(0)
