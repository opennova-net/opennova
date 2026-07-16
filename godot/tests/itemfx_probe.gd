extends SceneTree
## Manual probe (not collected by GUT — no _test suffix): mounts the persisted runtime
## resource dir and dumps every items.def entry that authors per-item particle effects,
## plus the DBuggy1 chain end-to-end (item db -> particlefx -> the model's userpoints).
## Run: GODOT_BIN --headless --path godot -s res://tests/itemfx_probe.gd


const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")


func _init() -> void:
	var dir := ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		dir = OS.get_environment("NOVA_RESOURCE_DIR")
	print("probe: resource dir = ", dir)
	var root := NovaResourceRoot.new()
	# The game's own runtime mount: PFF archives + the persisted expansion + SCR key.
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		print("probe: mount FAILED: ", root.get_last_error())
		quit(1)
		return
	var db := NovaItemDatabase.new()
	if db.load_from_resource_root(root, "items.def") != OK:
		print("probe: items.def load FAILED: ", db.get_last_error())
		quit(1)
		return
	print("probe: %d items" % db.get_count())
	var with_fx := 0
	for id in db.get_item_ids():
		var fx: Dictionary = db.get_particle_effects(id)
		var a: Dictionary = fx.get("particlefx", {})
		if String(a.get("effect", "")).is_empty():
			continue
		with_fx += 1
		if with_fx <= 12:
			print("  id %d %-28s particlefx %s @ %s" % [id, db.get_display_name(id),
					a.get("effect"), a.get("userpoint")])
	print("probe: %d items author particlefx" % with_fx)
	# The DBuggy1 def leg (the model side — FX00 with direction — is pinned by the
	# pyopennova userpoint dump; the attach matches it case-insensitively).
	var fx: Dictionary = db.get_particle_effects(101291)
	print("probe: DBuggy1 -> ", fx.get("particlefx"))
	quit(0)
