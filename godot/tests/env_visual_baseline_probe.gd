extends SceneTree

# Visual A/B driver for the ENG-2 environment port: loads the runtime world
# from a resource dir, steps time-of-day through a fixed grid, and saves
# viewport PNGs (sun-facing horizon + water view per TOD, plus one underwater
# shot) for before/after comparison. Not collected by GUT (probe suffix).
#
# Use (windowed - screenshots need a real rasterizer, NOT --headless):
#   "$GODOT_BIN" --path godot -s res://tests/env_visual_baseline_probe.gd -- <resource_dir> <out_dir> [prefix]

const TOD_GRID: Array[float] = [550.0, 1200.0, 1845.0, 2200.0]
# The iris and scalar channels chase their targets over a 62 Hz tick window;
# keep captures out of that transient just like mission_visual_probe.
const SETTLE_FRAMES := 90
const CAPTURE_WAIT_FRAMES := 4


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2:
		push_error("env_visual_baseline_probe: usage -- <resource_dir> <out_dir> [prefix]")
		quit(1)
		return
	var resource_dir: String = args[0]
	var out_dir: String = args[1]
	var prefix: String = args[2] if args.size() >= 3 else "baseline"
	DirAccess.make_dir_recursive_absolute(out_dir)

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("env_visual_baseline_probe: failed to load main_game.tscn")
		quit(1)
		return
	var scene := packed.instantiate()
	root.add_child(scene)

	var world = scene.get_node_or_null("World")
	if world == null:
		push_error("env_visual_baseline_probe: no World node")
		quit(1)
		return
	world.load_failed.connect(func(reason: String) -> void:
		push_error("env_visual_baseline_probe: load_failed: " + reason))
	# JOX is a loose extract: mount it editor-style (set_root_dir) and inject,
	# since the runtime mount path expects the retail PFF name table (D-VFS-2).
	var loose_root := NovaResourceRoot.new()
	loose_root.set_root_dir(resource_dir)
	world.set_resource_root(loose_root)
	var load_err: int = world.load_world(resource_dir)
	print("env_visual_baseline_probe: load_world -> ", load_err)
	# The shell boots into the menu state with the world hidden; flip to the
	# in-game view like _on_start_requested does.
	var menu_layer := scene.get_node_or_null("MenuLayer")
	if menu_layer != null:
		menu_layer.visible = false
	world.visible = true

	var env: Node = null
	var water: Node = null
	var weather: Node = null
	var camera: Camera3D = scene.get_node_or_null("Camera3D")
	var data = null
	for _i in range(900):
		await process_frame
		env = world.get_node_or_null("NovaEnvironment")
		water = world.get_node_or_null("NovaWater")
		weather = world.get_node_or_null("NovaWeather")
		data = world.get_terrain_data() if world.has_method("get_terrain_data") else null
		if data != null and data.is_loaded() and env != null and env.has_method("is_loaded") and env.is_loaded():
			break

	if env == null or not env.is_loaded():
		push_error("env_visual_baseline_probe: environment never loaded")
		quit(1)
		return

	# A stable viewpoint over the terrain: its painted center region.
	var anchor := Vector3(4096.0, 40.0, 4096.0)
	if data != null and data.is_loaded():
		var cx := float((data.get_origin_x() + 8) * 512)
		var cz := float((data.get_origin_y() + 8) * 512)
		var cy: float = data.get_height_world_bilinear(Vector3(cx, 0.0, cz))
		anchor = Vector3(cx, cy + 30.0, cz)

	var water_height := 10.5
	if water != null:
		water_height = float(water.get("water_height"))

	var celestial = world.get_node_or_null("NovaCelestial")
	var shots: Array[Dictionary] = []

	for tod in TOD_GRID:
		env.set("time_of_day", tod)
		if weather != null and weather.has_method("resync_colors"):
			weather.resync_colors()
		for _i in range(SETTLE_FRAMES):
			await process_frame

		var sun_dir: Vector3 = env.get_sun_direction()
		var look_dir := Vector3(sun_dir.x, clampf(sun_dir.y, -0.05, 0.35), sun_dir.z)
		if look_dir.length() < 0.01:
			look_dir = Vector3.FORWARD
		shots.append(await _capture(camera, anchor, anchor + look_dir.normalized() * 100.0,
				out_dir.path_join("%s_t%04d_sun.png" % [prefix, int(tod)])))

		var water_eye := Vector3(anchor.x, water_height + 12.0, anchor.z)
		shots.append(await _capture(camera, water_eye,
				water_eye + Vector3(0.6, -0.45, 0.65).normalized() * 60.0,
				out_dir.path_join("%s_t%04d_water.png" % [prefix, int(tod)])))

	# One underwater shot at noon: the frame clear + fog switch to lit water.
	env.set("time_of_day", 1200.0)
	if weather != null and weather.has_method("resync_colors"):
		weather.resync_colors()
	for _i in range(SETTLE_FRAMES):
		await process_frame
	var under_eye := Vector3(anchor.x, water_height - 6.0, anchor.z)
	shots.append(await _capture(camera, under_eye, under_eye + Vector3(1, 0.05, 0),
			out_dir.path_join("%s_underwater.png" % prefix)))

	var summary := {
		"terrain_loaded": data != null and data.is_loaded(),
		"env_loaded": env.is_loaded(),
		"water_height": water_height,
		"celestial_bodies": celestial.get_child_count() if celestial != null else -1,
		"shots_ok": shots.filter(func(s): return s["ok"]).size(),
		"shots_total": shots.size(),
	}
	print("env_visual_baseline_probe summary: ", summary)
	for shot in shots:
		print("  shot: ", shot)

	scene.queue_free()
	await process_frame
	quit(0 if int(summary["shots_ok"]) == shots.size() else 1)


func _capture(camera: Camera3D, eye: Vector3, target: Vector3, path: String) -> Dictionary:
	if camera != null:
		camera.global_position = eye
		camera.look_at(target, Vector3.UP)
	for _i in range(CAPTURE_WAIT_FRAMES):
		await process_frame
	var image := root.get_viewport().get_texture().get_image()
	var err := ERR_UNAVAILABLE
	if image != null:
		err = image.save_png(path)
	return {"ok": err == OK, "path": path, "eye": eye}
