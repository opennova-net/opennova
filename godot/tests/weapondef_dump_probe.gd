extends SceneTree
## Manual probe: dumps the EFFECTIVE weapon.def the runtime mount serves
## (base + expansion override) to NOVA_PROBE_OUT/weapon_def_mounted.txt.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")


func _init() -> void:
	var out_dir := OS.get_environment("NOVA_PROBE_OUT")
	if out_dir.is_empty():
		out_dir = "user://"
	var dir := ResourceDirSettings.get_resource_dir()
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	print("probe: dir=", dir, " expansion=", expansion)
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		print("probe: mount FAILED: ", root.get_last_error())
		quit(1)
		return
	var bytes: PackedByteArray = root.read_file("weapon.def")
	print("probe: weapon.def bytes = ", bytes.size())
	var f := FileAccess.open(out_dir.path_join("weapon_def_mounted.txt"), FileAccess.WRITE)
	f.store_buffer(bytes)
	f.close()
	print("probe: wrote ", out_dir.path_join("weapon_def_mounted.txt"))
	quit(0)
