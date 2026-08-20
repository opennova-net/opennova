extends SceneTree

# One-off asset-gated geometry probe for the 03tr-sun-sky glare ray: loads the
# mission terrain the way the runtime does and reports the clearance margin
# along the corrected 1024-unit glare ray, plus the retail start/end height
# variants [orig: render_skybox_sun_glow @ 0x5acde4]. Run:
#   GODOT_BIN --headless --path godot -s res://tests/glare_ray_geometry_probe.gd
# Requires NOVA_RUNTIME_RESOURCE_DIR + NOVA_MISSION_RESOURCE_DIR.


func _init() -> void:
	var runtime_dir := OS.get_environment("NOVA_RUNTIME_RESOURCE_DIR")
	if runtime_dir.is_empty():
		print("SKIP: NOVA_RUNTIME_RESOURCE_DIR unset")
		quit(0)
		return
	var root := ResourceRoot.new()
	var err: int = root.mount_runtime(runtime_dir, "revx02", false, "")
	print("root err=", err)
	var mission := MissionData.new()
	var mission_dir := OS.get_environment("NOVA_MISSION_RESOURCE_DIR")
	err = mission.open_file(mission_dir.path_join("03TR.bms"))
	print("bms err=", err, " terrain_ref=", mission.get_terrain_ref())
	var data := TerrainData.new()
	err = data.load_from_resource_root(root, mission.get_terrain_ref() + ".trn")
	print("trn load err=", err)
	if err != OK:
		quit(1)
		return

	var cam := Vector3(-606.053955078125, 4.21702575683594, 1070.25)
	var sun := Vector3(0.9316, 0.1227, -0.342)
	var to := cam + sun * 1024.0
	print("base ray hit=", data.raycast_terrain(cam, to))

	var worst := 1.0e9
	var worst_d := 0.0
	var lines := PackedStringArray()
	for i in range(0, 513):
		var t := float(i) * 2.0
		var p := cam + sun * t
		var h: float = data.get_height_world_bilinear(p)
		if is_nan(h):
			continue
		var margin := p.y - h
		if margin < worst:
			worst = margin
			worst_d = t
		if margin < 2.0:
			lines.append("d=%5.0f ray_h=%7.2f terrain=%7.2f margin=%6.2f" % [
					t, p.y, h, margin])
	print("worst margin %.2f at d=%.0f" % [worst, worst_d])
	for line in lines.slice(0, 40):
		print(line)
	for off in [1.0, 1.5, 2.0, 2.5]:
		var start := cam + Vector3(0.0, off, 0.0)
		print("start +%.1f -> hit=%s" % [off, str(data.raycast_terrain(start, to))])
	print("start +1 end +16 -> hit=", data.raycast_terrain(
			cam + Vector3(0.0, 1.0, 0.0), to + Vector3(0.0, 16.0, 0.0)))
	quit(0)
