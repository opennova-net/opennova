extends Node

# Windowed (non-headless) capture probe for the 00TRg foliage report: loads the
# mission, teleports the camera to the player spawn turned ~180deg (the user's
# repro view toward the armory), lets the dispatcher settle, saves the viewport
# to PNG, and quits. Captures ONLY the engine viewport — never the desktop.
#
# Scene mode (autoloads must exist for game_world.gd to compile):
# godot --path godot res://tests/foliage_00trg_capture_probe.tscn -- <install_dir> <out.png> [mission.bms] [yaw_deg]


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2:
		push_error("usage: -- <install_dir> <out.png> [mission.bms] [yaw_deg]")
		get_tree().quit(2)
		return
	var dir := args[0]
	var out_png := args[1]
	var bms := args[2] if args.size() >= 3 else "00TRg.bms"
	var yaw_deg := float(args[3]) if args.size() >= 4 else 180.0
	var pitch_deg := float(args[4]) if args.size() >= 5 else 0.0

	var packed := load("res://game/main_game.tscn") as PackedScene
	var scene := packed.instantiate()
	get_tree().root.add_child(scene)
	var world: GameWorld = scene.get_node_or_null("World")
	var rc: int = world.load_mission(bms, dir)
	print("[capture] load_mission rc=", rc)

	var data: NovaTerrainData = null
	for _i in range(240):
		await get_tree().process_frame
		data = world.get_terrain_data()
		if data != null and data.is_loaded():
			break
	if data == null:
		push_error("[capture] terrain never loaded")
		get_tree().quit(1)
		return

	# The shell shows World on entering gameplay; the probe bypasses the shell
	# state machine, so mirror that here (the C++ terrain draws server-side
	# regardless, which masks the hide — the foliage MeshInstances honor it).
	if world is Node3D:
		(world as Node3D).visible = true
	var menu_layer: Node = scene.get_node_or_null("MenuLayer")
	if menu_layer != null and menu_layer is CanvasLayer:
		(menu_layer as CanvasLayer).visible = false
	# The player host re-snaps the camera every frame; drop it so the probe owns
	# the view (probe-only teardown).
	var player_host: Node = scene.get_node_or_null("LocalPlayerHost")
	if player_host != null:
		player_host.queue_free()
	await get_tree().process_frame

	var pos: Vector3 = world.local_player_position()
	# yaw_deg < -900 = "find a painted foliage cluster near the spawn and frame
	# it" (the FAR mask sampler is the fix under test).
	var camera: Camera3D = scene.get_node_or_null("Camera3D")
	if yaw_deg < -900.0:
		var best := Vector3.ZERO
		var best_score := 0
		for rz in range(-160, 161, 8):
			for rx in range(-160, 161, 8):
				var wx: float = pos.x + rx
				var wz: float = pos.z + rz
				var score := 0
				for oz in [-4, 0, 4]:
					for ox in [-4, 0, 4]:
						if data.get_foliage_far_mask_world(wx + ox, wz + oz) != 0:
							score += 1
				if score > best_score:
					best_score = score
					best = Vector3(wx, 0.0, wz)
		if best_score > 0:
			var ground: float = data.get_height_world_bilinear(best)
			best.y = ground
			print("[capture] painted cluster at ", best, " score ", best_score)
			if camera != null:
				camera.current = true
				camera.global_position = best + Vector3(-3.0, 1.3, -3.0)
				camera.look_at(best + Vector3(0, 0.4, 0))
		else:
			print("[capture] no painted cluster found near spawn")
	elif camera != null:
		camera.current = true
		camera.global_position = pos + Vector3(0, 1.7, 0)
		camera.rotation_degrees = Vector3(pitch_deg, yaw_deg, 0)
	print("[capture] camera at ", camera.global_position if camera != null else Vector3.ZERO, " yaw ", yaw_deg, " pitch ", pitch_deg)

	# Let streaming/foliage settle and report an FPS estimate over 120 frames.
	var t0 := Time.get_ticks_msec()
	for _i in range(120):
		await get_tree().process_frame
	var dt := (Time.get_ticks_msec() - t0) / 1000.0
	print("[capture] avg fps over 120 frames: %.1f" % (120.0 / dt))

	var terr: Node = scene.get_node_or_null("World/NovaTerrain")
	if terr != null:
		for c in terr.get_children():
			if c is NovaFoliageDispatcher:
				print("[capture] dispatch stats: ", c.get_dispatch_stats())
				print("[capture] dispatcher visible=", c.visible, " in_tree=", c.is_visible_in_tree(),
					" terr_visible=", (terr as Node3D).visible if terr is Node3D else "n/a")
				var walk: Node = c
				while walk != null:
					var v = walk.visible if (walk is Node3D or walk is CanvasItem) else "-"
					print("[capture] chain: ", walk.name, " (", walk.get_class(), ") visible=", v)
					walk = walk.get_parent()
				var first_cell: Node3D = null
				for fc0 in c.get_children():
					if fc0 is MeshInstance3D and String(fc0.name).begins_with("FarCell"):
						first_cell = fc0
						break
				if first_cell != null:
					print("[capture] cell visible=", first_cell.visible, " in_tree=", first_cell.is_visible_in_tree(),
						" layers=", first_cell.layers, " origin=", first_cell.global_transform.origin,
						" top_level=", first_cell.top_level, " cam_cull=", camera.cull_mask)
				print("[capture] slot meshes: ", c.get_slot_meshes())
				var cells: Array = []
				for fc in c.get_children():
					if fc is MeshInstance3D and fc.visible and String(fc.name).begins_with("FarCell"):
						var aabb: AABB = (fc as MeshInstance3D).get_aabb()
						var center := aabb.get_center()
						cells.append([camera.global_position.distance_to(center), fc.name, aabb])
				cells.sort()
				for i in mini(cells.size(), 6):
					print("[capture] far cell ", cells[i][1], " dist=%.1f" % cells[i][0], " aabb=", cells[i][2])
				print("[capture] fd textures: ", c.slot_fd_textures)
				for ti in c.slot_fd_textures.size():
					print("[capture] fd[", ti, "] ", _alpha_histogram(c.slot_fd_textures[ti]))
				if cells.size() > 0:
					var nearest := terr.get_node(String(cells[0][1])) as MeshInstance3D
					if nearest == null:
						nearest = c.get_node_or_null(NodePath(String(cells[0][1]))) as MeshInstance3D
					if nearest != null and nearest.mesh != null:
						var arr: Array = nearest.mesh.surface_get_arrays(0)
						var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
						var cols: PackedColorArray = arr[Mesh.ARRAY_COLOR]
						print("[capture] nearest mesh verts=", verts.size(),
							" v0=", verts[0] if verts.size() > 0 else Vector3.ZERO,
							" v1=", verts[1] if verts.size() > 1 else Vector3.ZERO,
							" c0=", cols[0] if cols.size() > 0 else Color.BLACK)
						var uvs: PackedVector2Array = arr[Mesh.ARRAY_TEX_UV]
						print("[capture] uvs n=", uvs.size(), " uv0=", uvs[0] if uvs.size() > 0 else Vector2.ZERO, " uv1=", uvs[1] if uvs.size() > 1 else Vector2.ZERO, " uv2=", uvs[2] if uvs.size() > 2 else Vector2.ZERO)
						print("[capture] material: ", nearest.material_override)
						var debug_mat := StandardMaterial3D.new()
						debug_mat.albedo_color = Color(1.0, 0.0, 0.0)
						debug_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
						debug_mat.cull_mode = BaseMaterial3D.CULL_DISABLED
						for fc2 in c.get_children():
							if fc2 is MeshInstance3D and String(fc2.name).begins_with("FarCell"):
								(fc2 as MeshInstance3D).material_override = debug_mat
						for _j in range(5):
							await get_tree().process_frame
						if nearest.material_override is ShaderMaterial:
							var sm := nearest.material_override as ShaderMaterial
							print("[capture] u_fd_texture=", sm.get_shader_parameter("u_fd_texture"),
								" fade=", nearest.get_instance_shader_parameter("u_cell_fade"),
								" ref=", nearest.get_instance_shader_parameter("u_cell_alpha_ref"))
				break

	var img: Image = get_tree().root.get_viewport().get_texture().get_image()
	var err := img.save_png(out_png)
	print("[capture] save_png -> ", err, " (", out_png, ")")
	get_tree().quit(0 if err == OK else 1)


static func _alpha_histogram(tex: Texture2D) -> String:
	if tex == null:
		return "null"
	var img := tex.get_image()
	if img == null:
		return "no image"
	var over_high := 0
	var over_low := 0
	var top_high := 0
	var bottom_high := 0
	var total := img.get_width() * img.get_height()
	var half := img.get_height() / 2
	for y in img.get_height():
		for x in img.get_width():
			var a := img.get_pixel(x, y).a
			if a > 180.0 / 255.0:
				over_high += 1
				if y < half:
					top_high += 1
				else:
					bottom_high += 1
			if a > 8.0 / 255.0:
				over_low += 1
	return "%dx%d alpha>180: %d/%d (%.1f%%; top %d bottom %d)  alpha>8: %d/%d (%.1f%%)" % [
		img.get_width(), img.get_height(), over_high, total, 100.0 * over_high / total,
		top_high, bottom_high,
		over_low, total, 100.0 * over_low / total]
