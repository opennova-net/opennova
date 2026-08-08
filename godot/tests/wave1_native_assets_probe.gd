extends SceneTree

# Wave-1 smoke (ADR 0028 trunk, S3b/S16 full): the sim resolves every
# collision/occlusion .3di through its OWN SimModelCache (the render-cache
# extraction is gone — the placer no longer participates), the runtime boot
# installs the seat/mount table through the NATIVE extractor, the booted
# world keeps producing native hitboxes with ZERO provider declines, and the
# native TickAccumulator bank drives the loop. WAVE1_EXPECT_ATTACH overrides
# the retail-00TRg attach pin (859 on the JOX corpus).
#
# Run:
#   NOVA_RESOURCE_DIR=<loose JOX> godot --headless --path godot \
#     -s res://tests/wave1_native_assets_probe.gd

const MissionRuntime := preload("res://adapter/world/mission_runtime.gd")

const MISSION := "00TRg.bms"


func _init() -> void:
	call_deferred("_run")


# S9 oracle: the pre-S9 shell resolution of the .aip speed table, retained
# here as the probe's independent reference against the engine's
# resolve_ai_profile_speeds (which production boots through boot_mission).
static func _oracle_aip_speeds(mission, resource_root) -> Dictionary:
	var out := {}
	for raw in mission.get_all_entities():
		var entity: Dictionary = raw
		var profile := String(entity.get("name2", "")).strip_edges().to_lower()
		if profile.is_empty() or out.has(profile):
			continue
		if not resource_root.has_file(profile + ".aip"):
			continue
		var bytes: PackedByteArray = resource_root.read_file(profile + ".aip")
		if bytes.is_empty():
			continue
		var speeds := {}
		for line in bytes.get_string_from_ascii().split("\n"):
			var tokens := line.replace("\t", " ").strip_edges().split(" ", false)
			if tokens.size() < 2:
				continue
			var key := String(tokens[0]).to_lower()
			if key == "patrol_speed":
				speeds["patrol"] = int(tokens[1])
			elif key == "combat_speed":
				speeds["combat"] = int(tokens[1])
		if not speeds.is_empty():
			out[profile] = speeds
	return out


func _fail(message: String) -> void:
	push_error("wave1_native_assets_probe: " + message)
	quit(1)


func _run() -> void:
	var resource_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if resource_dir.is_empty():
		_fail("set NOVA_RESOURCE_DIR to a loose or mounted retail JOX corpus")
		return

	var root := NovaResourceRoot.new()
	var mount_err := int(root.mount_runtime(resource_dir, "", false, "jo"))
	if mount_err != OK:
		root.set_root_dir(resource_dir)
		if not root.has_file(MISSION):
			_fail("resource root failed (%d): %s" % [mount_err, root.get_last_error()])
			return

	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(root, MISSION) != OK:
		_fail("cannot open %s" % MISSION)
		return
	var item_db := NovaItemDatabase.new()
	if item_db.load_from_resource_root(root, "items.def") != OK:
		_fail("cannot load items.def: %s" % item_db.get_last_error())
		return

	# The sim resolves every attach from its own cache; the legacy render-side
	# reference leg died with S3b — the pin is the recorded retail attach
	# count instead.
	var expect_attach := 859
	var expect_env := OS.get_environment("WAVE1_EXPECT_ATTACH").strip_edges()
	if not expect_env.is_empty():
		expect_attach = int(expect_env)
	var sim := NovaSimulation.new()
	if not sim.load_from_mission_data(mission):
		sim.free()
		_fail("NovaSimulation rejected %s" % MISSION)
		return
	sim.set_asset_root(root)
	var attached_native := int(sim.resolve_collision_instances(item_db))

	print("[wave1] attached native=%d (expect %d)" % [attached_native, expect_attach])
	if attached_native != expect_attach:
		sim.free()
		_fail("native attach count %d != recorded retail %d" % [
				attached_native, expect_attach])
		return

	# S6b: the one-step by-name mount — the sim's RETAINED weapon.def row bakes
	# the FSM and the rig's own .adm rings the clips, no shell dictionary and
	# no render model involved [orig: WeaponSlotTable_LoadAllFromDefs @0x5414e0].
	if sim.load_weapon_table(root, "weapon.def") != OK:
		sim.free()
		_fail("load_weapon_table failed on the retail root")
		return
	if not bool(sim.spawn_local_player(Vector3.ZERO, 0.0, 1)):
		sim.free()
		_fail("spawn_local_player failed")
		return
	if not bool(sim.install_local_player_weapon_by_name("WPN_M4AUTO")):
		sim.free()
		_fail("by-name install of WPN_M4AUTO failed against the retail root")
		return
	var wstate: Dictionary = sim.get_local_player_weapon_state()
	print("[wave1] m4 by-name: active=%s clip=%d" % [
			str(wstate.get("active", false)), int(wstate.get("clip", -1))])
	if not bool(wstate.get("active", false)):
		sim.free()
		_fail("by-name install left no active weapon FSM")
		return
	if bool(sim.install_local_player_weapon_by_name("WPN_NOT_A_WEAPON")):
		sim.free()
		_fail("unknown weapon name must not install")
		return

	# The native 62.5 Hz bank [orig: Game_MainLoop @ 0x52b630].
	if int(sim.bank_realtime(0.032)) != 2:
		sim.free()
		_fail("bank_realtime(0.032) != 2")
		return
	if int(sim.bank_realtime(0.001)) != 0:
		sim.free()
		_fail("bank_realtime(0.001) != 0")
		return
	if int(sim.bank_realtime(2.0)) != 31:
		sim.free()
		_fail("bank_realtime(2.0) != 31 (spiral clamp)")
		return

	sim.free()

	# S9 (ADR 0028): the mission boot on retail data through the runtime
	# driver — setup() routes the feed/load/spawn/tables sequence through the
	# engine's run_mission_boot, and its native file resolution (mission-text
	# fallback, .aip profile speeds) must match the legacy shell resolution
	# recomputed here.
	var runtime := MissionRuntime.new()
	get_root().add_child(runtime)
	var boot_container := Node3D.new()
	get_root().add_child(boot_container)
	var boot_count := int(runtime.setup(mission, boot_container, {
		"resource_root": root,
		"item_db": item_db,
		"mission_file": MISSION,
		"playable": true,
	}))
	if boot_count <= 0 or int(runtime.get_setup_error()) != OK:
		_fail("S9 boot on %s failed (count=%d err=%d)" % [
				MISSION, boot_count, int(runtime.get_setup_error())])
		return
	# S3b/S16 (full cutover): the booted world poses natively end to end.
	# Drive the runtime and gate on the provider health counters — queries
	# flow and NOTHING declines — plus the native seat/mount install the boot
	# ran (the shell extractor is gone; sources > 0 proves the S16 step fed
	# the mounted resolver).
	var boot_sim: NovaSimulation = runtime.get_sim()
	if boot_sim == null:
		_fail("runtime has no sim")
		return
	var hitbox_entities := 0
	for _round in 8:
		for _t in 8:
			runtime.tick()
		var hb: Dictionary = boot_sim.get_hitbox_debug()
		hitbox_entities = maxi(
				hitbox_entities, int((hb.get("entities", []) as Array).size()))
	var pose_stats: Dictionary = boot_sim.debug_native_pose_stats()
	print("[wave1] native pose: hitbox_entities=%d stats=%s" % [
			hitbox_entities, str(pose_stats)])
	if hitbox_entities <= 0:
		_fail("native collision provider produced no hitboxes")
		return
	if int(pose_stats.get("collision_queries", 0)) <= 0 \
			or int(pose_stats.get("collision_declines", -1)) != 0:
		_fail("native collision declined in production: %s" % str(pose_stats))
		return
	if int(pose_stats.get("mounted_declines", -1)) != 0:
		_fail("native mounted resolver declined: %s" % str(pose_stats))
		return
	if int(pose_stats.get("mounted_graphic_sources", 0)) <= 0:
		_fail("the boot installed no native mounted model sources: %s" % str(pose_stats))
		return
	var boot_debug: Dictionary = runtime.get_sim().get_mission_boot_debug()
	var legacy_text := PackedByteArray()
	var text_base := MISSION.get_basename()
	if root.has_file(text_base + ".bin"):
		legacy_text = root.read_file(text_base + ".bin")
	else:
		legacy_text = root.read_file("medmssn.bin")
	if int(boot_debug.get("text_size", -1)) != legacy_text.size():
		_fail("S9 text resolution diverged: native %d vs legacy %d bytes" % [
				int(boot_debug.get("text_size", -1)), legacy_text.size()])
		return
	var legacy_aip: Dictionary = _oracle_aip_speeds(mission, root)
	var native_aip: Dictionary = boot_debug.get("aip", {})
	if legacy_aip.size() != native_aip.size():
		_fail("S9 .aip rows diverged: native %d vs legacy %d (%s vs %s)" % [
				native_aip.size(), legacy_aip.size(),
				str(native_aip), str(legacy_aip)])
		return
	for profile in legacy_aip:
		var legacy_row: Dictionary = legacy_aip[profile]
		var native_row: Dictionary = native_aip.get(profile, {})
		if int(legacy_row.get("patrol", -1)) != int(native_row.get("patrol", -1)) \
				or int(legacy_row.get("combat", -1)) != int(native_row.get("combat", -1)):
			_fail("S9 .aip '%s' diverged: native %s vs legacy %s" % [
					profile, str(native_row), str(legacy_row)])
			return
	print("[wave1] s9 boot: count=%d text=%d bytes (src %d) aip=%d rows" % [
			boot_count, int(boot_debug.get("text_size", 0)),
			int(boot_debug.get("text_source", 0)), native_aip.size()])

	print("wave1 native assets probe: PASS")
	quit(0)
