extends SceneTree
## Manual probe: reads NOVA_PTL_LIST (semicolon-separated .ptl names) through the
## runtime mount and writes each to NOVA_PROBE_OUT for offline inspection.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")


func _init() -> void:
	var out_dir := OS.get_environment("NOVA_PROBE_OUT")
	if out_dir.is_empty():
		out_dir = "user://"
	var names := OS.get_environment("NOVA_PTL_LIST").split(";", false)
	var dir := ResourceDirSettings.get_resource_dir()
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		print("probe: mount FAILED")
		quit(1)
		return
	for n in names:
		var bytes: PackedByteArray = root.read_file(n)
		print("probe: %s bytes=%d" % [n, bytes.size()])
		if bytes.is_empty():
			continue
		var f := FileAccess.open(out_dir.path_join(String(n).get_file() + ".txt"), FileAccess.WRITE)
		f.store_buffer(bytes)
		f.close()
	quit(0)
