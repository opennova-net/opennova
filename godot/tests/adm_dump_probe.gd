extends SceneTree
## Manual probe: dumps NOVA_ADM (default M82_1st.adm) from the runtime mount and
## checks each referenced .bad exists on the mount.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")


func _init() -> void:
	var adm := OS.get_environment("NOVA_ADM")
	if adm.is_empty():
		adm = "M82_1st.adm"
	var dir := ResourceDirSettings.get_resource_dir()
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		print("probe: mount FAILED")
		quit(1)
		return
	var bytes: PackedByteArray = root.read_file(adm)
	print("probe: %s bytes=%d" % [adm, bytes.size()])
	if bytes.is_empty():
		quit(1)
		return
	var text := bytes.get_string_from_ascii()
	var bads := {}
	for line in text.split("\n"):
		var s := line.strip_edges()
		if s.is_empty() or s.begins_with(";") or s.begins_with("//"):
			continue
		print("  | ", s.left(110))
		for token in s.split("\t"):
			var t := token.strip_edges().trim_prefix("\"").trim_suffix("\"")
			if not t.is_empty() and not t.begins_with("anim_") and t != s.split("\t")[0]:
				bads[t] = true
	var quoted := RegEx.new()
	quoted.compile("\"([^\"]+)\"")
	for m in quoted.search_all(text):
		var clip := m.get_string(1)
		print("probe: bad %-24s exists=%s" % [clip + ".bad", str(root.has_file(clip + ".bad"))])
	var skeletal := NovaSkeletalAnim.new()
	var rc: int = skeletal.load_from_resource_root(root, adm.get_basename())
	print("probe: skeletal load(%s) rc=%d has(anim_wpn_fire)=%s has(anim_reset)=%s" % [
			adm.get_basename(), rc, str(skeletal.has_clip("anim_wpn_fire")),
			str(skeletal.has_clip("anim_reset"))])
	quit(0)
