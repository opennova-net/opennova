extends Node

# Terrain sector-seam probe: boots the standalone game on the mission a
# NOVA_POSE_JSON dump names, then reports what the terrain does at the sector
# boundary nearest the dumped pose — a height profile straight across the seam
# plus two screenshots (the dumped eye direction, and a side-on view along the
# seam where a boundary trench is unmistakable).
#
# The seam is where the .trn's per-quadrant lock flags decide whether a boundary
# tap wraps inside the quadrant or crosses the atlas-internal seam into the next
# one; a terrain that declares a lock is only continuous when it is honored.
#
#   NOVA_POSE_JSON=<dump.json> NOVA_RESOURCE_DIR=<jox> \
#     "$GODOT_BIN" --path godot res://tests/terrain_seam_probe.tscn
#
# Writes .scratch/terrain_seam/<tag>_{eye,side}.png (tag = NOVA_SEAM_TAG or "run").

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const OUT_DIR := "res://../.scratch/terrain_seam"

const SECTOR_SIZE := 512.0

var _out_abs := ""
var _tag := "run"


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	_tag = OS.get_environment("NOVA_SEAM_TAG").strip_edges()
	if _tag.is_empty():
		_tag = "run"

	var pose_path := OS.get_environment("NOVA_POSE_JSON").strip_edges()
	if pose_path.is_empty():
		push_error("[seam] NOVA_POSE_JSON not set")
		get_tree().quit(1)
		return
	var pose_file := FileAccess.open(pose_path, FileAccess.READ)
	if pose_file == null:
		push_error("[seam] cannot read pose dump: " + pose_path)
		get_tree().quit(1)
		return
	var pose: Dictionary = JSON.parse_string(pose_file.get_as_text())
	var mission_file := String((pose.get("mission", {}) as Dictionary).get("file", ""))
	var cam: Dictionary = (pose.get("view", {}) as Dictionary).get("camera", {})
	var from_g: Vector3 = _v3(cam.get("position_godot", {}))
	var fwd_g: Vector3 = _v3(cam.get("forward_godot", {})).normalized()
	print("[seam] tag=%s mission=%s cam=%s fwd=%s" % [_tag, mission_file, str(from_g), str(fwd_g)])
	if mission_file.is_empty():
		push_error("[seam] pose dump names no mission")
		get_tree().quit(1)
		return

	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	NovaWindow.set_fullscreen(get_window(), true)
	await _settle(6)

	var session: Dictionary = await StandaloneProbe.boot(
		self, root, mission_file,
		OS.get_environment("NOVA_WR_EXPANSION").strip_edges())
	if not String(session.get("error", "")).is_empty():
		push_error("[seam] " + String(session.error))
		get_tree().quit(1)
		return

	var terrain_data: NovaTerrainData = _find_terrain_data()
	if terrain_data == null:
		push_error("[seam] no NovaTerrainData in the scene (stale DLL?)")
		get_tree().quit(1)
		return
	print("[seam] terrain=%s locks=%s water=%d" % [
			terrain_data.get_terrain_name(), str(terrain_data.get_quadrant_locks()),
			terrain_data.get_water_height()])

	# The sector boundary nearest the dumped camera, on each axis.
	var seam_z: float = round(from_g.z / SECTOR_SIZE) * SECTOR_SIZE
	print("[seam] nearest z sector boundary: godot z = %.1f (%.2f u from the camera)"
			% [seam_z, abs(from_g.z - seam_z)])

	# Height straight across it. A boundary tap that crosses into the next
	# quadrant shows up as a one-unit-wide dive at the seam row.
	print("[seam] height profile across the boundary at godot x = %.2f:" % from_g.x)
	var worst := 0.0
	var prev := INF
	for i in range(-6, 7):
		var z: float = seam_z + float(i) * 0.5
		var h: float = terrain_data.get_height_world_bilinear(Vector3(from_g.x, 0.0, z))
		var mark := "   <- boundary" if is_equal_approx(z, seam_z) else ""
		print("[seam]   z %9.2f   h %8.3f%s" % [z, h, mark])
		if prev != INF:
			worst = max(worst, abs(h - prev))
		prev = h
	print("[seam] largest step between adjacent 0.5 u samples: %.3f u" % worst)

	# And along the seam, so a trench that runs the width of the map is visible
	# as a line rather than one bad sample.
	var along_worst := 0.0
	var along_worst_x := 0.0
	for i in range(-60, 61):
		var x: float = from_g.x + float(i) * 4.0
		var on: float = terrain_data.get_height_world_bilinear(Vector3(x, 0.0, seam_z))
		var off: float = terrain_data.get_height_world_bilinear(Vector3(x, 0.0, seam_z - 2.0))
		if abs(on - off) > along_worst:
			along_worst = abs(on - off)
			along_worst_x = x
	print("[seam] worst boundary-vs-2u-back drop along 480 u of seam: %.3f u at x %.1f"
			% [along_worst, along_worst_x])

	# End-to-end grounding. The height the PLAYER stands on comes from
	# NovaSimulation's OWN TerrainHeightField, not NovaTerrainData's — a second
	# construction of the same struct, and the one that decides whether you fall
	# through. Teleport across the boundary and read back where the motor settles.
	var runtime := _find_by_method(get_tree().root, "get_sim")
	var sim = runtime.get_sim() if runtime != null else null
	if sim != null and sim.has_method("debug_teleport_local_player") \
			and sim.has_method("get_local_player_position"):
		print("[seam] player grounding across the boundary (bms y = -godot z):")
		var fell := 0
		for i in range(-6, 10):
			var by: float = -seam_z + float(i) * 0.25
			var h: float = terrain_data.get_height_world_bilinear(Vector3(from_g.x, 0.0, -by))
			sim.debug_teleport_local_player(Vector3(from_g.x, by, h + 3.0), 0.0, 0.0)
			await _settle(30)
			var p: Vector3 = sim.get_local_player_position()
			var bad := p.y < h - 1.0
			if bad:
				fell += 1
			print("[seam]   bms y %8.3f   terrain %8.3f   settled %8.3f%s"
					% [by, h, p.y, "   <- FELL THROUGH" if bad else ""])
		print("[seam] fell through at %d of 16 samples across the boundary" % fell)
	else:
		print("[seam] no sim / debug_teleport_local_player (stale DLL?); grounding not checked")

	# Screenshots: the dumped eye, then a side-on view 40 u off the seam looking
	# along it, which is the angle a boundary trench cannot hide from.
	await _shoot("eye", from_g, from_g + fwd_g)
	var side_from := Vector3(from_g.x - 45.0, from_g.y + 12.0, seam_z - 30.0)
	await _shoot("side", side_from, Vector3(from_g.x + 60.0, from_g.y - 6.0, seam_z))

	# The trench is one world unit wide, so the shot that settles it is a close
	# grazing view along the boundary: eye height, a few units back, aimed down
	# the seam line so it fills the frame edge-on.
	var ground: float = terrain_data.get_height_world_bilinear(Vector3(from_g.x, 0.0, seam_z - 6.0))
	await _shoot("close", Vector3(from_g.x - 6.0, ground + 1.7, seam_z - 5.0),
			Vector3(from_g.x + 30.0, ground + 0.2, seam_z + 0.5))

	print("[seam] done -> ", _out_abs)
	get_tree().quit(0)


func _shoot(name: String, from_g: Vector3, look_at: Vector3) -> void:
	# Shoot through the viewport NovaTerrain itself renders into: NovaTerrain
	# collects visible patches against `get_viewport()->get_camera_3d()`, so a
	# camera in any other viewport (the water reflection one, say) would frame a
	# scene whose terrain patches were never gathered for it.
	var terrain := _find_by_class(get_tree().root, "NovaTerrain")
	if terrain == null:
		print("[seam] no NovaTerrain; skipping %s" % name)
		return
	var vp3d: Viewport = (terrain as Node3D).get_viewport()
	if vp3d == null:
		print("[seam] NovaTerrain has no viewport; skipping %s" % name)
		return
	if name == "eye":
		print("[seam] terrain viewport=%s  current camera=%s" % [
				str(vp3d.get_path()),
				str(vp3d.get_camera_3d().get_path()) if vp3d.get_camera_3d() != null else "<none>"])
	var prev := vp3d.get_camera_3d()
	var shot_cam := Camera3D.new()
	vp3d.add_child(shot_cam)
	shot_cam.far = 4000.0
	shot_cam.global_position = from_g
	shot_cam.look_at(look_at, Vector3.UP)
	shot_cam.current = true
	await _settle(20)
	await RenderingServer.frame_post_draw
	var img: Image = vp3d.get_texture().get_image()
	if img != null:
		var path := _out_abs.path_join("%s_%s.png" % [_tag, name])
		img.save_png(path)
		print("[seam] wrote %s" % path)
	if prev != null:
		prev.current = true
	shot_cam.queue_free()


func _find_terrain_data() -> NovaTerrainData:
	var terrain := _find_by_class(get_tree().root, "NovaTerrain")
	if terrain == null:
		return null
	return terrain.get_terrain_data() as NovaTerrainData


func _find_by_class(node: Node, klass: String) -> Node:
	if node.is_class(klass) or node.get_class() == klass:
		return node
	for child in node.get_children():
		var found := _find_by_class(child, klass)
		if found != null:
			return found
	return null


func _find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for child in node.get_children():
		var found := _find_by_method(child, method)
		if found != null:
			return found
	return null


func _settle(frames: int) -> void:
	for _i in frames:
		await get_tree().process_frame


static func _v3(d: Dictionary) -> Vector3:
	return Vector3(float(d.get("x", 0.0)), float(d.get("y", 0.0)), float(d.get("z", 0.0)))
