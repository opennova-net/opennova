extends SceneTree

# Asset-backed diagnostic for the retail minimap-overlay classifier and the
# Advance & Secure zone chain. 00TRg is the important negative control: retail
# emits S2C 0x40 even though this mission has no AS chain.
#
# Run:
#   NOVA_RESOURCE_DIR=<retail-jo-directory> \
#     Godot_v4.6.1-stable_win64_console.exe --headless --path godot \
#     -s res://tests/00trg_zone_chain_probe.gd

const MISSION := "00TRg.bms"
const EXPANSION := "revx02"
const CHANGE_TEAM_ATTRIB := 0x20000
const SPAWN_POINT_ATTRIB := 0x40000
const ARMORY_ATTRIB := 0x80000
const NO_HUD_ATTRIB := 0x20000000
const FARP_ATTRIB2 := 0x2000
const EWEAP_ATTRIB := 0x20
const MissionObjectPlacer := preload("res://adapter/mission/mission_object_placer.gd")


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var resource_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if resource_dir.is_empty():
		_fail("set NOVA_RESOURCE_DIR to the retail JO directory")
		return

	var root := NovaResourceRoot.new()
	var mount_err := int(root.mount_runtime(resource_dir, EXPANSION, false, "jo"))
	if mount_err != OK:
		_fail("resource mount failed (%d): %s" % [mount_err, root.get_last_error()])
		return

	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(root, MISSION) != OK:
		_fail("cannot open %s from the %s mount" % [MISSION, EXPANSION])
		return
	var item_db := NovaItemDatabase.new()
	if item_db.load_from_resource_root(root, "items.def") != OK:
		_fail("cannot load items.def: %s" % item_db.get_last_error())
		return

	var sim := NovaSimulation.new()
	if not sim.load_from_mission_data(mission):
		sim.free()
		_fail("NovaSimulation rejected %s" % MISSION)
		return
	sim.resolve_item_traits(item_db)
	var placer := MissionObjectPlacer.new(root, item_db)
	# S3b native-only: collision extraction reads the sim's own asset root; a
	# rootless sim attaches nothing (the placer is no longer a model source).
	sim.set_asset_root(root)
	if int(sim.resolve_collision_instances(item_db, placer)) <= 0:
		sim.free()
		_fail("no model/collision instances resolved")
		return

	# The retail 00TRg oracle's first persistent 0x40 walk covers pool-2 slots
	# 2..25; later resumable visits cover pool-1 slots 8..12. Print the authored
	# rows and definition-side classifier inputs beside the zone-chain summary.
	for kind in [NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING]:
		var records: Array = mission.get_entities(kind)
		var limit := mini(records.size(), 30)
		for index in limit:
			if kind == NovaMissionData.KIND_ITEM and (index < 6 or index > 14):
				continue
			var authored: Dictionary = records[index]
			var def_id := int(authored.get("item_id", 0))
			var definition: Dictionary = item_db.get_item(def_id)
			var death_traits: Dictionary = item_db.get_death_traits(def_id)
			print(("[overlay-source] kind=%d index=%d bms=%d def=%d team=%d " +
					"map_symbol=%d type=%d attrib=0x%08x unit_type=%d ai=%s move=%s") % [
				kind, index, int(authored.get("bms_id", 0)), def_id,
				int(authored.get("team", 0)), int(authored.get("map_symbol", 0)),
				int(definition.get("type", 0)), int(item_db.get_attrib(def_id)),
				int(death_traits.get("unit_type", 0)), String(item_db.get_ai_function(def_id)),
				String(item_db.get_move_function(def_id))])

	var numbered := 0
	var capture_triggers := 0
	var chain_members := 0
	var unresolved := 0
	var persistent_overlay_handles: Array[int] = []
	for raw in mission.get_all_entities():
		var record: Dictionary = raw
		var bms_id := int(record.get("bms_id", 0))
		var state: Dictionary = sim.get_world_entity_debug(bms_id)
		if state.is_empty():
			continue
		if not bool(state.get("has_item_def", false)):
			unresolved += 1
		var zone_number := int(state.get("zone_number", 0))
		var is_trigger := bool(state.get("is_capture_trigger", false))
		var chain_index := int(state.get("zone_chain_index", -1))
		if zone_number != 0:
			numbered += 1
		if is_trigger:
			capture_triggers += 1
		if chain_index >= 0:
			chain_members += 1
		if int(state.get("pool", -1)) == 2 and _classifies_for_minimap(state):
			persistent_overlay_handles.push_back(int(state.get("handle", -1)))
		if zone_number != 0 or is_trigger:
			print(("[zone] bms=%d pool=%d item=%d def=%d attrib=0x%08x team=%d " +
					"number=%d radius=%d trigger=%s spawn=%s chain=%d") % [
				bms_id, int(state.get("pool", -1)), int(state.get("item_id", 0)),
				int(state.get("item_id", 0)) + NovaMissionData.ITEM_ID_OFFSET,
				int(state.get("item_attrib", 0)), int(state.get("team", 0)),
				zone_number, int(state.get("zone_radius", 0)), str(is_trigger),
				str(bool(state.get("is_spawn_point", false))), chain_index])

	var change_team_defs := 0
	for def_id in item_db.get_item_ids():
		if (int(item_db.get_attrib(def_id)) & CHANGE_TEAM_ATTRIB) != 0:
			change_team_defs += 1

	print(("[zone] summary mission_entities=%d numbered=%d capture_triggers=%d " +
			"chain_members=%d unresolved=%d change_team_defs=%d") % [
		mission.get_all_entities().size(), numbered, capture_triggers, chain_members,
		unresolved, change_team_defs])
	persistent_overlay_handles.sort()
	var expected: Array[int] = []
	for handle in range(0x2002, 0x201a):
		expected.push_back(handle)
	print("[overlay] persistent_count=%d handles=%s" % [
		persistent_overlay_handles.size(), str(persistent_overlay_handles)])
	if persistent_overlay_handles != expected:
		sim.free()
		_fail("persistent overlay set differs from retail's pool-2 slots 2..25")
		return
	sim.free()
	quit(0)


func _classifies_for_minimap(state: Dictionary) -> bool:
	if not bool(state.get("has_item_def", false)):
		return false
	var attrib := int(state.get("item_attrib", 0))
	if (attrib & NO_HUD_ATTRIB) != 0:
		return false
	if (attrib & (CHANGE_TEAM_ATTRIB | ARMORY_ATTRIB)) != 0:
		return true
	if (int(state.get("item_attrib2", 0)) & FARP_ATTRIB2) != 0:
		return true
	if int(state.get("item_unit_type", 0)) == 11:
		return true
	if int(state.get("item_type", 0)) == NovaItemDatabase.TYPE_BUILDING:
		return bool(state.get("has_minimap_model_marker", false))
	if int(state.get("item_type", 0)) == NovaItemDatabase.TYPE_VEHICLE:
		return true
	if (attrib & (0x8000 | EWEAP_ATTRIB | SPAWN_POINT_ATTRIB)) != 0:
		return true
	return int(state.get("item_type", 0)) == NovaItemDatabase.TYPE_PERSON


func _fail(message: String) -> void:
	push_error("00trg_zone_chain_probe: %s" % message)
	quit(1)
