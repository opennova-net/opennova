extends Node

# Scratch (uncommitted): boots a PFF-mounted mission, parks a free camera over
# known painted foliage texels, and saves PNG captures of both tiers.
# Use (windowed, NOT headless):
#   "$GODOT_BIN" --path godot res://tests/foliage_shot_probe.tscn -- <install_dir> <mission.bms> <out_dir>


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var dir: String = args[0]
	var bms: String = args[1]
	var out_dir: String = args[2]

	var packed := load("res://game/main_game.tscn") as PackedScene
	var scene := packed.instantiate()
	get_tree().root.add_child(scene)
	var world: GameWorld = scene.get_node_or_null("World")
	world.load_mission(bms, dir)
	var data: NovaTerrainData = null
	for _i in range(240):
		await get_tree().process_frame
		data = world.get_terrain_data()
		if data != null and data.is_loaded():
			break
	if data == null or not data.is_loaded():
		print("[shot] TERRAIN NOT LOADED")
		get_tree().quit(1)
		return

	# Drive the SCENE camera (main_game ticks the world from it every frame);
	# a second camera would fight the shell's own tick and hide the far pool.
	# Freeze the player host so it stops steering the camera to the player eye.
	# The menu shell keeps the loaded World hidden (menu backdrop world shows
	# instead); force the real world on screen for the captures.
	world.visible = true
	for child in scene.get_children():
		if child is Node3D and child != world and String(child.name) != "Camera3D":
			print("[shot] hiding sibling 3D: ", child.name)
			(child as Node3D).visible = false
	var host := scene.get_node_or_null("LocalPlayerHost")
	if host != null:
		host.set_process(false)
		host.set_physics_process(false)
	# Render through our own root camera; steer the SCENE camera to the same
	# pose per frame so main_game's world tick collects around the same spot.
	var tick_cam: Camera3D = scene.get_node_or_null("Camera3D")
	if tick_cam != null:
		tick_cam.set_process(false)
		tick_cam.set_physics_process(false)
	var cam := Camera3D.new()
	cam.far = 4000.0
	get_tree().root.add_child(cam)
	cam.make_current()

	# Painted ground (foliage byte 254 at world (100, 100) on 00TRg) plus the
	# spawn rock for contrast.
	var spots := [
		{"name": "painted_low", "eye": Vector3(100, 0, 82), "look": Vector3(100, 0, 100), "h": 1.7},
		{"name": "painted_wide", "eye": Vector3(72, 0, 72), "look": Vector3(100, 0, 100), "h": 8.0},
		{"name": "spawn", "eye": Vector3(158.9, 0, -292), "look": Vector3(158.9, 0, -274), "h": 2.0},
	]
	for spot in spots:
		var eye: Vector3 = spot.eye
		var look: Vector3 = spot.look
		var ground_eye := data.get_height_world_bilinear(Vector3(eye.x, 0, eye.z))
		var ground_look := data.get_height_world_bilinear(Vector3(look.x, 0, look.z))
		cam.global_position = Vector3(eye.x, ground_eye + float(spot.h), eye.z)
		cam.look_at(Vector3(look.x, ground_look + 0.5, look.z), Vector3.UP)
		# Let the shell's own tick settle the dispatcher pool around the camera.
		for _j in range(60):
			if tick_cam != null:
				tick_cam.global_transform = cam.global_transform
			await get_tree().process_frame
		var img := get_viewport().get_texture().get_image()
		var path := out_dir.path_join("shot_%s.png" % spot.name)
		img.save_png(path)
		print("[shot] saved ", path)
		# A/B: same view with foliage hidden, for a pixel diff.
		var d0 := scene.get_node_or_null("World/NovaTerrain/FoliageDispatcher") as NovaFoliageDispatcher
		if d0 != null:
			d0.visible = false
			for _k in range(3):
				await get_tree().process_frame
			get_viewport().get_texture().get_image().save_png(
					out_dir.path_join("shot_%s_nofol.png" % spot.name))
			d0.visible = true
			for _k in range(3):
				await get_tree().process_frame
	var disp := scene.get_node_or_null("World/NovaTerrain/FoliageDispatcher") as NovaFoliageDispatcher
	print("[shot] stats: ", disp.get_dispatch_stats() if disp != null else "n/a")
	get_tree().quit(0)
