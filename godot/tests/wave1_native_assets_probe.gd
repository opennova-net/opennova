extends SceneTree

# Wave-1 smoke (ADR 0028 trunk): with an asset root installed the sim resolves
# its collision/occlusion .3di data through its OWN SimModelCache — proven by
# attaching against a placer that supplies NOTHING — and the attach count
# matches the legacy render-cache extraction exactly. Also pins the native
# TickAccumulator bank the runtime loop now drives, and (S3) runs the
# collision pose A/B on retail data: one sim with BOTH the render placer
# (legacy leg) and the native asset root, compare mode counting divergent
# final section matrices across the PANM and skeletal legs.
#
# Run:
#   NOVA_RESOURCE_DIR=<loose JOX> godot --headless --path godot \
#     -s res://tests/wave1_native_assets_probe.gd

const MissionObjectPlacer := preload("res://adapter/mission/mission_object_placer.gd")
const ItemSeatSpecs := preload("res://adapter/world/item_seat_specs.gd")
const MissionRuntime := preload("res://adapter/world/mission_runtime.gd")

const MISSION := "00TRg.bms"


class NullPlacer:
	extends RefCounted

	func object_data_for(_graphic: String) -> NovaObjectData:
		return null

	func skeletal_anim_for(_item_id: int, _graphic: String):
		return null


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

	# Native leg: the placer supplies nothing — every attach comes from the
	# sim's own cache.
	var sim := NovaSimulation.new()
	if not sim.load_from_mission_data(mission):
		sim.free()
		_fail("NovaSimulation rejected %s" % MISSION)
		return
	sim.set_asset_root(root)
	var attached_native := int(sim.resolve_collision_instances(item_db, NullPlacer.new()))

	# Legacy reference leg: the pre-ADR-0028 render-cache extraction.
	var sim_legacy := NovaSimulation.new()
	if not sim_legacy.load_from_mission_data(mission):
		sim.free()
		sim_legacy.free()
		_fail("NovaSimulation (legacy leg) rejected %s" % MISSION)
		return
	var placer := MissionObjectPlacer.new(root, item_db)
	var attached_legacy := int(sim_legacy.resolve_collision_instances(item_db, placer))

	print("[wave1] attached native=%d legacy=%d" % [attached_native, attached_legacy])
	if attached_native <= 0:
		sim.free()
		sim_legacy.free()
		_fail("native asset cache attached nothing")
		return
	if attached_native != attached_legacy:
		sim.free()
		sim_legacy.free()
		_fail("native/legacy attach mismatch: %d vs %d" % [attached_native, attached_legacy])
		return

	# S6b: the one-step by-name mount — the sim's RETAINED weapon.def row bakes
	# the FSM and the rig's own .adm rings the clips, no shell dictionary and
	# no render model involved [orig: WeaponSlotTable_LoadAllFromDefs @0x5414e0].
	if sim.load_weapon_table(root, "weapon.def") != OK:
		sim.free()
		sim_legacy.free()
		_fail("load_weapon_table failed on the retail root")
		return
	if not bool(sim.spawn_local_player(Vector3.ZERO, 0.0, 1)):
		sim.free()
		sim_legacy.free()
		_fail("spawn_local_player failed")
		return
	if not bool(sim.install_local_player_weapon_by_name("WPN_M4AUTO")):
		sim.free()
		sim_legacy.free()
		_fail("by-name install of WPN_M4AUTO failed against the retail root")
		return
	var wstate: Dictionary = sim.get_local_player_weapon_state()
	print("[wave1] m4 by-name: active=%s clip=%d" % [
			str(wstate.get("active", false)), int(wstate.get("clip", -1))])
	if not bool(wstate.get("active", false)):
		sim.free()
		sim_legacy.free()
		_fail("by-name install left no active weapon FSM")
		return
	if bool(sim.install_local_player_weapon_by_name("WPN_NOT_A_WEAPON")):
		sim.free()
		sim_legacy.free()
		_fail("unknown weapon name must not install")
		return

	# The native 62.5 Hz bank [orig: Game_MainLoop @ 0x52b630].
	if int(sim.bank_realtime(0.032)) != 2:
		sim.free()
		sim_legacy.free()
		_fail("bank_realtime(0.032) != 2")
		return
	if int(sim.bank_realtime(0.001)) != 0:
		sim.free()
		sim_legacy.free()
		_fail("bank_realtime(0.001) != 0")
		return
	if int(sim.bank_realtime(2.0)) != 31:
		sim.free()
		sim_legacy.free()
		_fail("bank_realtime(2.0) != 31 (spiral clamp)")
		return

	sim.free()
	sim_legacy.free()

	# S3 (ADR 0028): the pose-provider A/B. One sim carries BOTH sources —
	# the render placer feeds the legacy binding path and the asset root feeds
	# the engine-side provider — and the default Compare mode shadows every
	# build_section_matrices query. get_hitbox_debug drives both legs (PANM
	# entity tris + organic skeletal spheres) with the world advancing.
	var sim_ab := NovaSimulation.new()
	if not sim_ab.load_from_mission_data(mission):
		sim_ab.free()
		_fail("NovaSimulation (A/B leg) rejected %s" % MISSION)
		return
	sim_ab.set_asset_root(root)
	var placer_ab := MissionObjectPlacer.new(root, item_db)
	var attached_ab := int(sim_ab.resolve_collision_instances(item_db, placer_ab))
	if attached_ab != attached_legacy:
		sim_ab.free()
		_fail("A/B leg attach mismatch: %d vs %d" % [attached_ab, attached_legacy])
		return
	sim_ab.resolve_item_traits(item_db)
	# S4 (ADR 0028): the static seat-spec A/B — install the shell-extracted
	# table, then re-extract natively (retained def rows + sim parses) and
	# diff the typed records.
	sim_ab.set_item_seat_specs(
			ItemSeatSpecs.build_item_seat_specs(mission, root, item_db))
	var seat_diff: Dictionary = sim_ab.debug_native_seat_spec_diff(item_db)
	print("[wave1] s4 seat diff: %s" % str(seat_diff))
	if int(seat_diff.get("compared", 0)) <= 0:
		sim_ab.free()
		_fail("S4 seat-spec diff compared nothing")
		return
	if int(seat_diff.get("mismatches", 0)) != 0 \
			or int(seat_diff.get("native_missing", 0)) != 0:
		sim_ab.free()
		_fail("S4 seat-spec divergence: %s" % str(seat_diff))
		return
	for _round in 8:
		var _hb: Dictionary = sim_ab.get_hitbox_debug()
		for _t in 8:
			sim_ab.step()
	var ab: Dictionary = sim_ab.debug_collision_pose_ab_stats()
	print("[wave1] s3 pose ab: %s" % str(ab))
	if int(ab.get("queries", 0)) <= 0:
		sim_ab.free()
		_fail("S3 compare mode saw no posed-collision queries")
		return
	if int(ab.get("divergences", 0)) != 0 or int(ab.get("result_mismatches", 0)) != 0:
		sim_ab.free()
		_fail("S3 pose divergence: %s" % str(ab))
		return
	sim_ab.free()

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
