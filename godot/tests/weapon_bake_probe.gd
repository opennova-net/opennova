extends SceneTree
## Manual probe: resolves NOVA_WEAPON (default WPN_Barret) on the persisted
## runtime mount and prints each ACTION row's anim key, whether the viewmodel
## .adm resolves it, the clip variant lengths, and the expected baked delays —
## pins 'auto' collapse (missing clip -> delay 0 -> wrong cadence).
## Run: GODOT_BIN --headless --path godot -s res://tests/weapon_bake_probe.gd

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")


func _init() -> void:
	var weapon := OS.get_environment("NOVA_WEAPON")
	if weapon.is_empty():
		weapon = "WPN_Barret"
	var dir := ResourceDirSettings.get_resource_dir()
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		print("probe: mount FAILED: ", root.get_last_error())
		quit(1)
		return
	var db := NovaWeaponDatabase.new()
	if db.load_from_resource_root(root, "weapon.def") != OK:
		print("probe: weapon.def load FAILED: ", db.get_last_error())
		quit(1)
		return
	var index: int = db.find_weapon(weapon)
	if index < 0:
		print("probe: %s not found" % weapon)
		quit(1)
		return
	var w: Dictionary = db.get_weapon(index)
	var adm := String(w.get("animadm", ""))
	print("probe: %s animadm=%s gfx1=%s flags=0x%x" % [weapon, adm,
			String(w.get("gfx1", "")), int(w.get("flags", 0))])
	var skeletal := NovaSkeletalAnim.new()
	var adm_file := adm if adm.to_lower().ends_with(".adm") else adm + ".adm"
	var adm_ok := bool(skeletal.load_from_resource_root(root, adm_file))
	print("probe: adm load(%s) = %s (%s)" % [adm_file, str(adm_ok), skeletal.get_last_error()])
	for a in w.get("actions", []):
		var row: Dictionary = a
		var key := String(row.get("anim", ""))
		var has := false
		var lengths := []
		if adm_ok and not key.is_empty():
			has = skeletal.has_clip(key)
			if has:
				lengths = skeletal.get_clip_variant_lengths(key)
		var baked_hint := ""
		if lengths.size() > 0:
			var ticks := int(float(lengths[0]) * 62.5 + 0.5) + 1
			baked_hint = " (first variant %.3fs -> ~%d ticks)" % [float(lengths[0]), ticks]
		print("  %-10s ds=%-5s de=%-5s anim=%-22s has_clip=%s%s" % [
				String(row.get("name", "?")),
				str(row.get("delaystart", 0)), str(row.get("delayend", 0)),
				key, str(has), baked_hint])
	quit(0)
