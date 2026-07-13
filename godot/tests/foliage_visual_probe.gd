extends SceneTree

# Visual driver for the foliage two-tier port (D-FOLIAGE-4/-5): loads the
# runtime world from a resource dir, finds a sloped foliage-painted spot,
# parks the camera low over it, saves consecutive-frame PNGs (frame-to-frame
# diffs expose regen/churn flicker) plus side and wide angles, and prints a
# per-frame dispatcher-stats series to quantify instance churn. Not collected
# by GUT (probe suffix).
#
# Use (windowed - screenshots need a real rasterizer, NOT --headless):
#   "$GODOT_BIN" --path godot -s res://tests/foliage_visual_probe.gd -- <resource_dir> <out_dir> [prefix]

const SETTLE_FRAMES := 40
const CAPTURE_WAIT_FRAMES := 3
const STATS_FRAMES := 40


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2:
		push_error("foliage_visual_probe: usage -- <resource_dir> <out_dir> [prefix]")
		quit(1)
		return
	var resource_dir: String = args[0]
	var out_dir: String = args[1]
	var prefix: String = args[2] if args.size() >= 3 else "foliage"
	DirAccess.make_dir_recursive_absolute(out_dir)

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("foliage_visual_probe: failed to load main_game.tscn")
		quit(1)
		return
	var scene := packed.instantiate()
	root.add_child(scene)

	var world = scene.get_node_or_null("World")
	if world == null:
		push_error("foliage_visual_probe: no World node")
		quit(1)
		return
	world.load_failed.connect(func(reason: String) -> void:
		push_error("foliage_visual_probe: load_failed: " + reason))
	# Loose extract mounted editor-style (same as env_visual_baseline_probe).
	var loose_root := NovaResourceRoot.new()
	loose_root.set_root_dir(resource_dir)
	world.set_resource_root(loose_root)
	var load_err: int = world.load_world(resource_dir)
	print("foliage_visual_probe: load_world -> ", load_err)
	var menu_layer := scene.get_node_or_null("MenuLayer")
	if menu_layer != null:
		menu_layer.visible = false
	world.visible = true

	var camera: Camera3D = scene.get_node_or_null("Camera3D")
	var env: Node = null
	var data = null
	var dispatcher: Node = null
	for _i in range(900):
		await process_frame
		env = world.get_node_or_null("NovaEnvironment")
		data = world.get_terrain_data() if world.has_method("get_terrain_data") else null
		dispatcher = world.get_node_or_null("NovaTerrain/FoliageDispatcher")
		if data != null and data.is_loaded() and env != null and env.has_method("is_loaded") and env.is_loaded():
			break
	if data == null or not data.is_loaded():
		push_error("foliage_visual_probe: terrain never loaded")
		quit(1)
		return
	if env != null and env.has_method("set"):
		env.set("time_of_day", 1200.0)

	# Find a sloped, foliage-painted spot: coarse scan of the full terrain
	# extent for max height gradient where the foliage map has an index.
	var lo_x := float(data.get_origin_x() * 512)
	var lo_z := float(data.get_origin_y() * 512)
	var cx := lo_x + 8.0 * 512.0
	var cz := lo_z + 8.0 * 512.0
	var best_pos := Vector3(cx, 0.0, cz)
	var best_grad := -1.0
	for gz in range(4, 508, 8):
		for gx in range(4, 508, 8):
			var px := lo_x + float(gx) * 16.0
			var pz := lo_z + float(gz) * 16.0
			var idx := 0
			if data.has_method("get_foliage_index_world"):
				idx = data.get_foliage_index_world(px, pz)
			if idx <= 0:
				continue
			var h0: float = data.get_height_world_bilinear(Vector3(px, 0.0, pz))
			var hx: float = data.get_height_world_bilinear(Vector3(px + 8.0, 0.0, pz))
			var hz: float = data.get_height_world_bilinear(Vector3(px, 0.0, pz + 8.0))
			var grad: float = abs(hx - h0) + abs(hz - h0)
			if grad > best_grad:
				best_grad = grad
				best_pos = Vector3(px, h0, pz)
	print("foliage_visual_probe: spot=", best_pos, " grad=", best_grad)

	# Down-slope look direction from the local gradient.
	var h_px: float = data.get_height_world_bilinear(best_pos + Vector3(8, 0, 0))
	var h_pz: float = data.get_height_world_bilinear(best_pos + Vector3(0, 0, 8))
	var downhill := Vector3(h_px - best_pos.y, 0.0, h_pz - best_pos.y)
	if downhill.length() < 0.01:
		downhill = Vector3.FORWARD
	downhill = -downhill.normalized()

	var eye := best_pos + Vector3(0, 2.2, 0) - downhill * 4.0
	var target := best_pos + downhill * 14.0
	if camera != null:
		camera.global_position = eye
		camera.look_at(target, Vector3.UP)
	for _i in range(SETTLE_FRAMES):
		await process_frame

	# Per-frame stats series: instance-count oscillation = regen churn.
	var series: Array = []
	for _i in range(STATS_FRAMES):
		await process_frame
		var stats := {}
		if dispatcher != null and dispatcher.has_method("get_dispatch_stats"):
			stats = dispatcher.get_dispatch_stats()
		var total: int = dispatcher.get_total_instances() if dispatcher != null and dispatcher.has_method("get_total_instances") else -1
		var foliage_us: int = int(world.get("_perf_foliage_us")) if world.get("_perf_foliage_us") != null else -1
		series.append({"total": total, "fps": Engine.get_frames_per_second(), "foliage_us": foliage_us, "stats": stats})
	print("foliage_visual_probe: stats series (first/last 6):")
	for i in range(series.size()):
		if i < 6 or i >= series.size() - 6:
			print("  f%02d total=%s fps=%s foliage_us=%s stats=%s" % [i, str(series[i]["total"]),
				str(series[i]["fps"]), str(series[i]["foliage_us"]), str(series[i]["stats"])])
	var totals: Array = series.map(func(s): return int(s["total"]))
	var t_min: int = totals.min()
	var t_max: int = totals.max()
	print("foliage_visual_probe: total_instances min=%d max=%d churn=%d" % [t_min, t_max, t_max - t_min])

	var shots: Array[Dictionary] = []
	# Four consecutive frames from the same eye - flicker shows as diffs.
	for f in range(4):
		shots.append(await _capture(camera, eye, target,
				out_dir.path_join("%s_seq%d.png" % [prefix, f]), 1))
	# Side angle across the slope.
	var side := downhill.cross(Vector3.UP).normalized()
	shots.append(await _capture(camera, best_pos + side * 10.0 + Vector3(0, 3.0, 0),
			best_pos, out_dir.path_join("%s_side.png" % prefix), CAPTURE_WAIT_FRAMES))
	# Wide shot from above/behind.
	shots.append(await _capture(camera, best_pos - downhill * 20.0 + Vector3(0, 14.0, 0),
			best_pos + downhill * 20.0, out_dir.path_join("%s_wide.png" % prefix), CAPTURE_WAIT_FRAMES))

	var ok := shots.filter(func(s): return s["ok"]).size()
	print("foliage_visual_probe: shots ok=%d/%d" % [ok, shots.size()])
	for shot in shots:
		print("  shot: ", shot)
	scene.queue_free()
	await process_frame
	quit(0 if ok == shots.size() else 1)


func _capture(camera: Camera3D, eye: Vector3, target: Vector3, path: String, wait_frames: int) -> Dictionary:
	if camera != null:
		camera.global_position = eye
		camera.look_at(target, Vector3.UP)
	for _i in range(wait_frames):
		await process_frame
	var image := root.get_viewport().get_texture().get_image()
	var err := ERR_UNAVAILABLE
	if image != null:
		err = image.save_png(path)
	return {"ok": err == OK, "path": path}
