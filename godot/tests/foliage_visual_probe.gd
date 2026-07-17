extends SceneTree

const VegAssets := preload("res://engine/terrain/veg_assets.gd")

# Visual driver for the fresh foliage two-tier port: loads the
# runtime world from a resource dir, finds a dense foliage-painted cell,
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
# Use a camera band where the committed non-retail model has frame-stable
# rendered output. Its unusually large source mesh gives it a smaller retail
# cache than actual vegetation, so this isolates visible flicker from fixture churn.
const PROBE_EYE_HEIGHT := 24.0


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
		scene.queue_free()
		await process_frame
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
		await _shutdown(scene, world, dispatcher)
		VegAssets.clear_cache()
		loose_root.clear()
		dispatcher = null
		world = null
		scene = null
		await process_frame
		quit(1)
		return
	if env != null and env.has_method("set"):
		env.set("time_of_day", 1200.0)

	var foliage_probe := _find_foliage_world_point(data)
	if not foliage_probe.has("position"):
		push_error("foliage_visual_probe: no native cell has a foliagemap value matching an authored definition")
		await _shutdown(scene, world, dispatcher)
		VegAssets.clear_cache()
		loose_root.clear()
		data = null
		dispatcher = null
		world = null
		scene = null
		await process_frame
		quit(1)
		return
	var best_pos: Vector3 = foliage_probe["position"]
	print("foliage_visual_probe: probe=", foliage_probe)

	# Down-slope look direction from the local gradient.
	var h_px: float = data.get_height_world_bilinear(best_pos + Vector3(8, 0, 0))
	var h_pz: float = data.get_height_world_bilinear(best_pos + Vector3(0, 0, 8))
	var downhill := Vector3(h_px - best_pos.y, 0.0, h_pz - best_pos.y)
	if downhill.length() < 0.01:
		downhill = Vector3.FORWARD
	downhill = -downhill.normalized()

	var eye := best_pos + Vector3(0, PROBE_EYE_HEIGHT, 0) - downhill * 4.0
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
		if dispatcher != null and dispatcher.has_method("get_frame_stats"):
			stats = dispatcher.get_frame_stats()
		var total: int = dispatcher.get_total_instances() if dispatcher != null and dispatcher.has_method("get_total_instances") else -1
		series.append({"total": total, "stats": stats})
	print("foliage_visual_probe: stats series (first/last 6):")
	for i in range(series.size()):
		if i < 6 or i >= series.size() - 6:
			print("  f%02d total=%s stats=%s" % [i, str(series[i]["total"]), str(series[i]["stats"])])
	var totals: Array = series.map(func(s): return int(s["total"]))
	var t_min: int = totals.min()
	var t_max: int = totals.max()
	print("foliage_visual_probe: total_instances min=%d max=%d churn=%d" % [t_min, t_max, t_max - t_min])
	var foliage_ok := dispatcher != null
	if t_min != t_max:
		foliage_ok = false
		push_error("foliage_visual_probe: settled instance count changed (visible foliage flicker/churn)")
	var required_stats := ["runtime_detail_intents", "detail_vertices", "render_batches"]
	for key in required_stats:
		var values: Array = series.map(func(s): return int(s["stats"].get(key, 0)))
		var minimum := int(values.min()) if not values.is_empty() else 0
		var maximum := int(values.max()) if not values.is_empty() else 0
		print("foliage_visual_probe: %s min=%d max=%d" % [key, minimum, maximum])
		if minimum <= 0:
			foliage_ok = false
			push_error("foliage_visual_probe: expected %s > 0 in every settled frame" % key)
		if minimum != maximum:
			foliage_ok = false
			push_error("foliage_visual_probe: settled %s changed between frames" % key)
	if t_min <= 0:
		foliage_ok = false
		push_error("foliage_visual_probe: expected resident foliage instances in every settled frame")

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
	var exit_code := 0 if ok == shots.size() and foliage_ok else 1
	# Drop script-held native resources before the rendering server begins its
	# exit sequence. The world scene owns the remaining references until the
	# orderly shutdown below frees it.
	data = null
	env = null
	await _shutdown(scene, world, dispatcher)
	VegAssets.clear_cache()
	loose_root.clear()
	dispatcher = null
	world = null
	camera = null
	scene = null
	packed = null
	loose_root = null
	await process_frame
	quit(exit_code)


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


func _shutdown(scene: Node, world: Node, dispatcher: Node) -> void:
	# Stop native cache activity before freeing the large terrain scene. Waiting
	# through both queued-free frames avoids tearing the GDExtension down while
	# render resources from the final screenshot are still in flight.
	if dispatcher != null and dispatcher.has_method("reset"):
		dispatcher.reset()
	if world != null and world.has_method("unload"):
		world.unload()
	await process_frame
	if is_instance_valid(scene):
		scene.queue_free()
	await process_frame
	await process_frame


func _find_foliage_world_point(data: NovaTerrainData) -> Dictionary:
	var defs: Array = data.get_foliage_defs()
	var grid := data.get_sector_grid()
	var origin_x := data.get_origin_x()
	var origin_y := data.get_origin_y()
	var best := {}
	var best_score := 0
	var best_gradient := -1.0

	# A 3x3 regular score selects a dense native cell without copying the
	# runtime's private random candidate sequence into this visual gate.
	for grid_y in range(16):
		for grid_x in range(16):
			var grid_index := grid_y * 16 + grid_x
			var sector_id := int(grid[grid_index]) if grid_index < grid.size() else 0
			if sector_id <= 0:
				continue
			var sector_x := float((origin_x + grid_x) * 512)
			var sector_z := float((origin_y + grid_y) * 512)
			for local_z in range(0, 512, 16):
				for local_x in range(0, 512, 16):
					var score := 0
					var painted := 0
					for offset_z in [2.0, 8.0, 14.0]:
						for offset_x in [2.0, 8.0, 14.0]:
							var sampled := int(data.get_detail_foliage_index_world(
								sector_x + float(local_x) + offset_x,
								sector_z + float(local_z) + offset_z
							))
							if _has_matching_foliage_def(defs, sampled):
								score += 1
								painted = sampled
					if score <= 0:
						continue
					if score < best_score:
						continue
					var world_x := sector_x + float(local_x) + 8.0
					var world_z := sector_z + float(local_z) + 8.0
					var center := Vector3(world_x, 0.0, world_z)
					var world_y: float = data.get_height_world_bilinear(center)
					var hx: float = data.get_height_world_bilinear(center + Vector3(8.0, 0.0, 0.0))
					var hz: float = data.get_height_world_bilinear(center + Vector3(0.0, 0.0, 8.0))
					var gradient: float = abs(hx - world_y) + abs(hz - world_y)
					if score == best_score and gradient <= best_gradient:
						continue
					best_score = score
					best_gradient = gradient
					best = {
						"position": Vector3(world_x, world_y, world_z),
						"painted": painted,
						"painted_samples": score,
						"gradient": gradient,
						"cell": Vector2i(int(sector_x) + local_x, int(sector_z) + local_z),
						"sector_id": sector_id,
						"grid_cell": Vector2i(grid_x, grid_y),
					}

	return best if best_score > 0 else {}


func _has_matching_foliage_def(defs: Array, painted: int) -> bool:
	for value in defs:
		if value is NovaTerrainFoliageDef and int((value as NovaTerrainFoliageDef).match) == painted:
			return true
	return false
