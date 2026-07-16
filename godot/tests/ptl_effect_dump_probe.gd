extends SceneTree
## Manual probe: spawns NOVA_EFFECT_ID through NovaEffectWorld on the persisted
## runtime mount and dumps each emitter's definition flags / emitting / alive
## across a few seconds of ticks — pins effect lifetime semantics (FOREVEREMIT,
## emit_dur vs particle age) for the muzzle-flash suppression window.
## Run: GODOT_BIN --headless --path godot -s res://tests/ptl_effect_dump_probe.gd

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const TICK_DT := 1.0 / 62.0


func _init() -> void:
	var target := OS.get_environment("NOVA_EFFECT_ID")
	if target.is_empty():
		target = "Effect_M82_Muz"
	var dir := ResourceDirSettings.get_resource_dir()
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := NovaLaunchFlags.game(ResourceDirSettings.get_game())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled(), game) != OK:
		print("probe: mount FAILED: ", root.get_last_error())
		quit(1)
		return
	var fx := NovaEffectWorld.new()
	get_root().add_child(fx)
	var loaded: int = fx.load_from_resource_root(root)
	print("probe: files=%d effects=%d" % [loaded, fx.effect_count()])
	var handle: int = fx.spawn_effect(target, Vector3.ZERO)
	print("probe: spawn %s handle=%d" % [target, handle])
	if handle <= 0:
		quit(1)
		return
	for step in range(140):
		fx.advance_fixed_tick(TICK_DT)
		if step in [0, 3, 15, 31, 62, 93, 124, 139]:
			var rows: Array = []
			for group_v in fx.get_debug_group_report():
				var group: Dictionary = group_v
				for em_v in group.get("emitters", []):
					var em: Dictionary = em_v
					rows.append("%s alive=%d emitting=%s" % [String(em.get("name", "?")),
							int(em.get("alive", 0)), str(em.get("emitting", false))])
			print("t=%5.2fs groups=%d | %s" % [float(step + 1) * TICK_DT,
					fx.live_group_count(), " ; ".join(rows)])
	quit(0)
