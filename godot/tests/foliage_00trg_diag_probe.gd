extends Node

# One-off diagnostic for the trunk foliage-coverage report (00TRg.bms): loads the
# mission against a retail install, then prints — around the local player spawn —
# the charmap surface mask under both Z sign conventions, the foliage-map byte,
# the TRN's foliage def slots, and the dispatcher's live FAR cell/instance stats.
#
# Scene mode (autoloads must exist for game_world.gd to compile):
# godot --headless --path godot res://tests/foliage_00trg_diag_probe.tscn -- <install_dir> [mission.bms]


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.is_empty():
		push_error("usage: -- <install_dir> [mission.bms]")
		get_tree().quit(2)
		return
	var dir := args[0]
	var bms := args[1] if args.size() >= 2 else "00TRg.bms"

	var packed := load("res://game/main_game.tscn") as PackedScene
	var scene := packed.instantiate()
	get_tree().root.add_child(scene)
	var world: GameWorld = scene.get_node_or_null("World")
	var rc: int = world.load_mission(bms, dir)
	print("[diag] load_mission rc=", rc)

	var data: NovaTerrainData = null
	for _i in range(120):
		await get_tree().process_frame
		data = world.get_terrain_data()
		if data != null and data.is_loaded():
			break
	if data == null or not data.is_loaded():
		push_error("[diag] terrain data never loaded")
		get_tree().quit(1)
		return

	var pos: Vector3 = world.local_player_position()
	print("[diag] player spawn: ", pos)

	var defs = data.get_foliage_defs()
	for i in defs.size():
		var d = defs[i]
		print("[diag] def[%d]: graphic=%s match=%d attribs=%d colors=%d/%d" % [
			i, d.get_graphic(), d.get_match(), d.get_attrib_flags(),
			d.get_color_lower(), d.get_color_upper()])
	var fmap = data.get_foliage_map()
	if fmap != null:
		print("[diag] foliage map size=", fmap.get_width(), "x", fmap.get_height())
		var hist := {}
		for y in range(0, fmap.get_height(), 4):
			for x in range(0, fmap.get_width(), 4):
				var v: int = fmap.get_index(x, y)
				hist[v] = int(hist.get(v, 0)) + 1
		print("[diag] foliage map histogram (4-stride): ", hist)

	# Charmap mask sampling around the spawn under both conventions: the sampler's
	# contract arg is native -z; probe what the code does vs the mirrored read.
	for dz: int in [-24, -12, 0, 12, 24]:
		var row := ""
		for dx: int in [-24, -12, 0, 12, 24]:
			var wx: float = pos.x + dx
			var wz_native: float = -(pos.z + dz)  # render z -> native z
			var m_code: int = data.get_foliage_far_mask_world(wx, -wz_native)
			var m_mirror: int = data.get_foliage_far_mask_world(wx, wz_native)
			var f: int = data.get_foliage_index_world(wx, wz_native)
			row += "(%02x|%02x f%d) " % [m_code, m_mirror, f]
		print("[diag] mask dz=", dz, ": ", row)

	var dispatcher = scene.get_node_or_null("World/NovaTerrain/FoliageDispatcher")
	if dispatcher == null:
		# search
		var terr = scene.get_node_or_null("World/NovaTerrain")
		if terr != null:
			for c in terr.get_children():
				if c is NovaFoliageDispatcher:
					dispatcher = c
					break
	if dispatcher != null:
		# Let it tick a few frames near the spawn, then dump stats.
		var cam: Camera3D = scene.get_node_or_null("Camera3D")
		if cam != null:
			cam.global_position = pos + Vector3(0, 1.8, 0)
		for _i in range(30):
			await get_tree().process_frame
		if dispatcher.has_method("get_debug_stats"):
			print("[diag] dispatcher stats: ", dispatcher.get_debug_stats())
		else:
			var n := 0
			var inst := 0
			for c in dispatcher.get_children():
				n += 1
			print("[diag] dispatcher children: ", n)
	else:
		print("[diag] no dispatcher found")

	get_tree().quit(0)
