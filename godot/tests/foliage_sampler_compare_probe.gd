extends SceneTree

# Compares the two foliagemap samplers along a native-Z line: the sector-routed
# index sampler (proven right by MODEL-tier palm placement) vs the flat FAR mask
# sampler. Prints (native_z, sector_pixel, far_mask_at_witnessed_arg,
# far_mask_at_mirrored_arg) so mirror/offset relationships are readable.
#
# Use: godot --headless --path godot -s res://tests/foliage_sampler_compare_probe.gd -- <install_dir> [mission.bms] [x]


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var dir := args[0]
	var bms := args[1] if args.size() >= 2 else "00TRg.bms"
	var x := float(args[2]) if args.size() >= 3 else 142.0

	var packed := load("res://game/main_game.tscn") as PackedScene
	var scene := packed.instantiate()
	root.add_child(scene)
	var world: GameWorld = scene.get_node_or_null("World")
	world.load_mission(bms, dir)
	var data: NovaTerrainData = null
	for _i in range(120):
		await process_frame
		data = world.get_terrain_data()
		if data != null and data.is_loaded():
			break

	print("[cmp] sector grid: ", data.get_sector_grid())
	for nz in range(256, 480, 16):
		var sector_px: int = data.get_foliage_index_world(x, nz)
		# witnessed arg = -nativeZ
		var far_w: int = data.get_foliage_far_mask_world(x, -nz)
		var far_m: int = data.get_foliage_far_mask_world(x, nz)
		print("[cmp] nz=%4d sector_px=%3d far(w)=%02x far(m)=%02x" % [nz, sector_px, far_w, far_m])
	quit(0)
