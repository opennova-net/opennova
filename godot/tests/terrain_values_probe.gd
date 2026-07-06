extends SceneTree
func _initialize() -> void:
	call_deferred("_run")
func _run() -> void:
	var root := NovaResourceRoot.new()
	root.set_root_dir("C:/Users/taylor/Desktop/JOX")
	var td := NovaTerrainData.new()
	var err = td.load_from_resource_root(root, "Dvxi5.trn")
	print("PROBE load err=", err, " loaded=", td.is_loaded())
	if not td.is_loaded():
		quit(1)
		return
	var img: Image = td.get_colormap_image()
	if img == null:
		print("PROBE no colormap image")
		quit(1)
		return
	img = img.duplicate()
	if img.is_compressed(): img.decompress()
	var w := img.get_width(); var h := img.get_height()
	var acc := Vector3.ZERO; var mx := 0.0; var n := 0
	for y in range(0, h, 8):
		for x in range(0, w, 8):
			var c := img.get_pixel(x, y)
			acc += Vector3(c.r, c.g, c.b); n += 1
			mx = maxf(mx, maxf(c.r, maxf(c.g, c.b)))
	acc /= float(n)
	print("PROBE colormap ", w, "x", h, " avg=", acc, " max=", mx, " avg255=", acc * 255.0)
	quit(0)
