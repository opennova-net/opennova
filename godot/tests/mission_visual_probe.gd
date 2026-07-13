extends SceneTree

# Mission-workspace visual A/B — the T3 retail-comparison substrate: opens the
# ONED mission workspace on a retail mission, drops the camera onto a painted
# foliage point at eye height (the default aerial editor camera renders the
# known aerial black-ground cosmetic), steps time-of-day (noon vs night entity
# lighting on the placed buildings), then adds a noon ground-foliage shot;
# saves viewport PNGs for side-by-side comparison against retail captures.
# Not collected by GUT (probe suffix).
#
# Use (windowed - screenshots need a real rasterizer, NOT --headless):
#   "$GODOT_BIN" --path godot -s res://tests/mission_visual_probe.gd -- <asset_dir> <out_dir> [mission_file] [prefix]
# defaults: mission_file "CP15.bms", prefix "mission".

const TOD_GRID: Array[float] = [1200.0, 2200.0]
# 90, not the env probe's 24: the weather color smoother chases the TOD
# register over a 62-tick window and we want the settled value, not the chase.
const SETTLE_FRAMES := 90
const CAPTURE_WAIT_FRAMES := 4
# Mission loads are seconds-long; a fixed generous wait is fine for a probe.
const MISSION_LOAD_WAIT_FRAMES := 180
const INVALID_CELL := Vector2i(-9999, -9999)


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() < 2:
		push_error("mission_visual_probe: usage -- <asset_dir> <out_dir> [mission_file] [prefix]")
		quit(1)
		return
	var asset_dir: String = args[0]
	var out_dir: String = args[1]
	var mission_file: String = args[2] if args.size() >= 3 else "CP15.bms"
	var prefix: String = args[3] if args.size() >= 4 else "mission"
	DirAccess.make_dir_recursive_absolute(out_dir)

	var scene := load("res://modtools/editor/editor_main.tscn") as PackedScene
	if scene == null:
		push_error("mission_visual_probe: failed to load editor_main.tscn")
		quit(1)
		return
	var editor = scene.instantiate()
	root.add_child(editor)
	await process_frame
	var ws = editor if editor.has_method("open_in_workspace") else editor.find_child("EditorWorkstation", true, false)
	if ws == null:
		for child in editor.get_children():
			if child.has_method("open_in_workspace"):
				ws = child
				break
	if ws == null:
		push_error("mission_visual_probe: editor main scene exposes no open_in_workspace shell.")
		quit(1)
		return

	ws.set_resource_root_dir(asset_dir, false, true)
	ws.open_in_workspace("mission", asset_dir + "/" + mission_file)
	print("mission_visual_probe: opened %s — waiting %d frames for the mission load" % [mission_file, MISSION_LOAD_WAIT_FRAMES])
	for i in range(MISSION_LOAD_WAIT_FRAMES):
		await process_frame
		if (i + 1) % 60 == 0:
			print("mission_visual_probe: load wait %d/%d frames" % [i + 1, MISSION_LOAD_WAIT_FRAMES])

	# Camera first: besides framing the ground shots it disambiguates WHICH
	# env/weather pair to drive — the shell furnishes one preview world per 3D
	# workspace (the object preview owns a NovaEnvironment too); the mission
	# view's pair lives in the same viewport world as the workspace camera.
	var camera: Camera3D = _find_workspace_camera(ws)
	var env_nodes: Array = []
	var weather_nodes: Array = []
	for node in root.find_children("*", "", true, false):
		if node.has_method("get_sky_ambient") and "time_of_day" in node:
			env_nodes.append(node)
		if node.has_method("resync_colors"):
			weather_nodes.append(node)
	var env = _pick_in_camera_world(env_nodes, camera)
	var weather = _pick_in_camera_world(weather_nodes, camera)
	if env == null:
		print("mission_visual_probe: no environment node (time_of_day + get_sky_ambient) — capturing as-is")
	if weather == null:
		print("mission_visual_probe: no weather node (resync_colors) — TOD sets without a resync snap")

	# Ground the camera on a painted foliage point BEFORE any capture: the
	# default aerial editor camera renders the known aerial black-ground
	# cosmetic, so the noon/night pair and the foliage shot all share the same
	# ground-level POV. On any miss, leave the camera untouched and capture the
	# default view instead.
	var grounded := false
	var data := _find_terrain_data()
	if data == null or camera == null:
		print("mission_visual_probe: skip ground camera placement (terrain data %s, camera %s)" % [
			"found" if data != null else "missing", "found" if camera != null else "missing"])
	else:
		var painted := _find_painted_world_point(data)
		if not painted.has("position"):
			print("mission_visual_probe: skip ground camera placement (no painted foliage point with a matching def)")
		else:
			var pos: Vector3 = painted["position"]
			var ground_y := pos.y
			if data.has_method("get_height_world"):
				ground_y = data.get_height_world(Vector3(pos.x, 0.0, pos.z))
			camera.global_position = Vector3(pos.x, ground_y + 1.6, pos.z)
			camera.look_at(Vector3(pos.x, ground_y + 1.0, pos.z - 40.0), Vector3.UP)
			print("mission_visual_probe: foliage point %s (painted index %d)" % [pos, int(painted["painted"])])
			grounded = true

	var shots := 0

	# Entity-lighting A/B: the same framing at noon and at night.
	for tod in TOD_GRID:
		if env != null:
			env.time_of_day = tod
			if weather != null:
				weather.resync_colors()
		for _i in range(SETTLE_FRAMES):
			await process_frame
		if await _save_shot(out_dir.path_join("%s_t%04d.png" % [prefix, int(tod)])):
			shots += 1

	# Ground-level foliage at noon: the camera already stands on the painted
	# point from the placement above.
	if not grounded:
		print("mission_visual_probe: skip foliage ground shot (ground camera placement was skipped)")
	else:
		if env != null:
			env.time_of_day = 1200.0
			if weather != null:
				weather.resync_colors()
		for _i in range(SETTLE_FRAMES):
			await process_frame
		if await _save_shot(out_dir.path_join("%s_foliage_ground.png" % prefix)):
			shots += 1

	print("mission_visual_probe: done — %d shots" % shots)
	editor.queue_free()
	await process_frame
	quit(0)


# Mirrors env_visual_baseline_probe's capture: wait out the rasterizer, grab
# the root viewport texture, save the PNG, print the output path.
func _save_shot(path: String) -> bool:
	for _i in range(CAPTURE_WAIT_FRAMES):
		await process_frame
	var image := root.get_viewport().get_texture().get_image()
	var err := ERR_UNAVAILABLE
	if image != null:
		err = image.save_png(path)
	if err == OK:
		print("mission_visual_probe: saved ", path)
	else:
		push_error("mission_visual_probe: save failed (%d) for %s" % [err, path])
	return err == OK


# The mission workspace's active camera: the shell proxies the active
# workspace's capability hook (get_editor_camera -> get_viewport_camera); fall
# back to any workspace hook directly, then any SubViewport's current camera,
# then the first Camera3D anywhere.
func _find_workspace_camera(ws) -> Camera3D:
	if ws != null and ws.has_method("get_editor_camera"):
		var cam = ws.get_editor_camera()
		if cam is Camera3D:
			return cam
	for node in root.find_children("*", "", true, false):
		if node.has_method("get_viewport_camera"):
			var cam = node.get_viewport_camera()
			if cam is Camera3D:
				return cam
	for vp in root.find_children("*", "SubViewport", true, false):
		var cam: Camera3D = vp.get_camera_3d()
		if cam != null:
			return cam
	var cams := root.find_children("*", "Camera3D", true, false)
	if not cams.is_empty():
		return cams[0]
	return null


# Several env/weather instances can coexist in the shell (one preview world per
# 3D workspace); prefer the candidate sharing the workspace camera's viewport.
func _pick_in_camera_world(candidates: Array, camera: Camera3D):
	if camera != null:
		for node in candidates:
			if node.get_viewport() == camera.get_viewport():
				return node
	return candidates[0] if not candidates.is_empty() else null


# In the editor shell the terrain lives on the ONED terrain domain editor
# (get_data + sample_height_world), not a NovaTerrain node; play-in-editor and
# runtime worlds hang a NovaTerrain (or another get_terrain_data holder).
# Sweep all three shapes; the first loaded NovaTerrainData wins.
func _find_terrain_data() -> NovaTerrainData:
	for terrain in root.find_children("*", "NovaTerrain", true, false):
		var d = terrain.get_terrain_data()
		if d is NovaTerrainData and d.is_loaded():
			return d
	for node in root.find_children("*", "", true, false):
		if node.has_method("get_terrain_data"):
			var d = node.get_terrain_data()
			if d is NovaTerrainData and d.is_loaded():
				return d
		elif node.has_method("get_data") and node.has_method("sample_height_world"):
			var d = node.get_data()
			if d is NovaTerrainData and d.is_loaded():
				return d
	return null


func _find_painted_world_point(data: NovaTerrainData) -> Dictionary:
	var foliage_map: NovaTerrainFoliageMap = data.get_foliage_map()
	if foliage_map == null:
		return {}

	var defs: Array = data.get_foliage_defs()
	var grid := data.get_sector_grid()
	var origin_x := data.get_origin_x()
	var origin_y := data.get_origin_y()
	var width := foliage_map.get_width()
	var height := foliage_map.get_height()

	for map_y in range(height):
		for map_x in range(width):
			var painted := int(foliage_map.get_index(map_x, map_y))
			if painted <= 0 or not _has_matching_foliage_def(defs, painted):
				continue

			var sector_id := int(foliage_map.get_sector_id_at(map_x, map_y))
			var grid_cell := _find_sector_grid_cell(grid, sector_id)
			if grid_cell == INVALID_CELL:
				continue

			var hm_pos := foliage_map.get_heightmap_position(map_x, map_y)
			var quadrant_x := 512.0 if (sector_id == 3 or sector_id == 4) else 0.0
			var quadrant_z := 512.0 if (sector_id == 2 or sector_id == 4) else 0.0
			var local_x := hm_pos.x - quadrant_x
			var local_z := hm_pos.y - quadrant_z
			if local_x < 0.0 or local_x >= 512.0 or local_z < 0.0 or local_z >= 512.0:
				continue

			var world_x := float((origin_x + grid_cell.x) * 512) + local_x
			var world_z := float((origin_y + grid_cell.y) * 512) + local_z
			if int(data.get_foliage_index_world(world_x, world_z)) <= 0:
				continue

			var world_y := data.get_height_world_bilinear(Vector3(world_x, 0.0, world_z))
			return {
				"position": Vector3(world_x, world_y, world_z),
				"painted": painted,
				"map": Vector2i(map_x, map_y),
				"sector_id": sector_id,
				"grid_cell": grid_cell,
			}

	return {}


func _has_matching_foliage_def(defs: Array, painted: int) -> bool:
	for value in defs:
		if not (value is NovaTerrainFoliageDef):
			continue
		var def := value as NovaTerrainFoliageDef
		for match_value in def.matches:
			if int(match_value) == painted:
				return true
	return false


func _find_sector_grid_cell(grid: PackedInt32Array, sector_id: int) -> Vector2i:
	if sector_id <= 0:
		return INVALID_CELL
	for gy in range(16):
		for gx in range(16):
			var index := gy * 16 + gx
			if index < grid.size() and int(grid[index]) == sector_id:
				return Vector2i(gx, gy)
	return INVALID_CELL
