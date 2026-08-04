extends SceneTree

# Asset-backed oracle for the mission-authored portion of retail S2C 0x0F on
# 00TRg: the blue player route and the localized deploy-map location labels.
#
# Run:
#   NOVA_RESOURCE_DIR=<retail-jo-directory> \
#     Godot_v4.6.1-stable_win64_console.exe --headless --path godot \
#     -s res://tests/00trg_world_state_probe.gd

const EXPANSION := "revx02"
const MISSION := "00TRg.bms"
const MISSION_TEXT := "00TRg.bin"


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

	var table := RtxtStringFile.new()
	if table.load_from_byte_array(root.read_file(MISSION_TEXT)) != OK:
		_fail("cannot parse %s from the %s mount" % [MISSION_TEXT, EXPANSION])
		return
	var locations := [
		table.get_string_in_section("Locations", "LOCATION001"),
		table.get_string_in_section("Locations", "LOCATION002"),
	]
	if locations != ["Weapons Cache", "Rebel Outpost"]:
		_fail("00TRg location strings differ from the retail 0x0F witness: %s" %
				str(locations))
		return

	var blue_paths: Array = []
	for raw_path in mission.get_waypoint_paths():
		var path: Dictionary = raw_path
		# BMS WaypointFlags::BlueTeam is bit 1.
		if (int(path.get("flags", 0)) & 0x2) != 0:
			blue_paths.push_back(path)
	print("[world-state] locations=%s blue_paths=%s" % [str(locations), str(blue_paths)])
	if blue_paths.size() != 1:
		_fail("00TRg must have exactly one blue player waypoint path")
		return
	var blue: Dictionary = blue_paths[0]
	if int(blue.get("flags", 0)) != 3 or Array(blue.get("marker_indices", [])) != [12]:
		_fail("00TRg blue path differs from retail's pool-3 slot 0x300c")
		return
	quit(0)


func _fail(message: String) -> void:
	push_error("00trg_world_state_probe: %s" % message)
	quit(1)
