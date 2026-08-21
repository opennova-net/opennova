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
func _find_target(sim, eye: Vector3) -> Dictionary:
	var best := {}
	var best_dist := INF
	for pitch_deg in [0.0, -10.0, 10.0]:
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
			# The struck face sits on the ray, inside the entity's bound sphere.
			var radius := float(pick.get("bound_radius", 1.0))
			best["hit_godot"] = eye + dir * maxf(1.0, d - radius)
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

	var player: Vector3 = sim.get_local_player_position()
	var eye := player + Vector3.UP * 1.6
	var target := _find_target(sim, eye)
	if target.is_empty():
		_fail("no pickable entity face within %.0f u of the spawn" % PICK_RANGE)
		return
	var hit: Vector3 = target["hit_godot"]
	var dir: Vector3 = target["dir"]
	print("PROBE target: entity=%s at %s (%.1f u), blocked=%s" % [
			str(target.get("entity_handle", -1)), str(hit), eye.distance_to(hit),
			str(target.get("blocked", ""))])

	# The volley: jittered around the pick direction so the quads spread and
	# the ring wraps (more rounds than the 256-slot ring holds).
	var spawned := 0
	var rng := RandomNumberGenerator.new()
	rng.seed = 0x5CC830
	while spawned < ROUNDS:
		for _k in range(ROUNDS_PER_FRAME):
			if spawned >= ROUNDS:
				break
			var jitter := Vector3(rng.randf_range(-0.03, 0.03),
					rng.randf_range(-0.03, 0.03), rng.randf_range(-0.03, 0.03))
			if sim.debug_spawn_round(eye, (dir + jitter).normalized(), AMMO) >= 0:
				spawned += 1
		await process_frame
	await _wait_frames(120)

	var stats = world.get_scar_present_stats()
	if stats == null:
		_fail("the runtime owns no scar presentation pass")
		return
	print("PROBE scars: spawned=%d slots_live=%d slots_culled=%d rings_leased=%d batches=%d world_surfaces=%d entity_meshes=%d textures_missing=%d owners_unresolved=%d" % [
			spawned, stats.slots_live, stats.slots_culled, stats.rings_leased,
			stats.batches, stats.world_surfaces, stats.entity_meshes,
			stats.textures_missing, stats.owners_unresolved])
	var draw: Dictionary = sim.get_scar_draw_list(eye, 0.0, Color.WHITE)
	print("PROBE draw list: vertices=%d batches=%d" % [
			(draw.get("vertices", PackedVector3Array()) as PackedVector3Array).size(),
			(draw.get("batch_owner", PackedInt32Array()) as PackedInt32Array).size()])
	await _shot("overview", eye, hit)
	var back := (eye - hit).normalized()
	await _shot("close", hit + back * 1.6 + Vector3.UP * 0.3, hit)
	print("PROBE done")
	quit(0)
