class_name StandaloneGameProbe
extends RefCounted

## Shared boot harness for rendered/manual probes that exercise the real game
## shell. It mounts the requested runtime root only long enough for MainGame to
## capture it, restores the user's persisted settings immediately, and normally
## starts the exact saved top-level loose mission through MainGame's public F6
## seam. A parity probe may also supply the exact saved BMS path separately
## while dependencies resolve through a packed comparison mount.

const MainGameScene := preload("res://game/main_game.tscn")
const ResourceDirSettings := preload(
		"res://engine/resource_index/resource_dir_settings.gd")
const LOAD_TIMEOUT_MSEC := 240_000


static func boot(
		mount: Node,
		resource_dir: String,
		mission_name: String,
		expansion: String = "",
		saved_mission_path: String = ""
) -> Dictionary:
	var dir: String = resource_dir.strip_edges()
	var bms: String = mission_name.strip_edges().replace("\\", "/")
	var saved_path: String = saved_mission_path.strip_edges().replace("\\", "/")
	if mount == null or not is_instance_valid(mount):
		return {"error": "probe mount is unavailable"}
	if not ResourceDirSettings.is_valid_root(dir):
		return {"error": "invalid resource dir: %s" % dir}
	if bms.is_empty() or bms != bms.get_file() \
			or bms.get_extension().to_lower() != "bms":
		return {"error": "mission must be a top-level .bms name: %s" % bms}
	var saved_mission: NovaMissionData = null
	if saved_path.is_empty() and not FileAccess.file_exists(dir.path_join(bms)):
		return {"error": "%s not found in %s" % [bms, dir]}
	if not saved_path.is_empty():
		if saved_path.get_extension().to_lower() != "bms" \
				or not FileAccess.file_exists(saved_path):
			return {"error": "saved mission file not found: %s" % saved_path}
		saved_mission = NovaMissionData.new()
		if saved_mission.open_file(saved_path) != OK:
			return {"error": "failed to parse saved mission %s: %s" % [
				saved_path, saved_mission.get_last_error()]}

	# MainGame normally receives these as process-local launch flags. A probe is
	# already running inside Godot, so briefly stage the equivalent settings,
	# let MainGame mount its private root synchronously in _ready(), then restore
	# the user's preferences before any awaited frame.
	var previous_dir: String = ResourceDirSettings.get_resource_dir()
	var previous_expansion: String = ResourceDirSettings.get_expansion()
	ResourceDirSettings.set_resource_dir(dir)
	ResourceDirSettings.set_expansion(expansion)
	var game: Node = MainGameScene.instantiate()
	mount.add_child(game)
	ResourceDirSettings.set_resource_dir(previous_dir)
	ResourceDirSettings.set_expansion(previous_expansion)

	var world: GameWorld = game.get_node_or_null("World")
	var camera: Camera3D = game.get_node_or_null("Camera3D")
	if world == null or camera == null or game.current_resource_root() == null:
		game.queue_free()
		return {"error": "MainGame failed to mount the requested runtime root"}

	var load_error: Array[String] = [""]
	world.load_failed.connect(
		func(message: String) -> void: load_error[0] = message,
		CONNECT_ONE_SHOT)
	if saved_mission == null:
		game.start_loose_mission(bms)
	else:
		# Manual parity probes may keep the saved loose BMS outside the packed
		# runtime install. Parse that exact disk file, then let the standalone
		# shell and GameWorld resolve every dependency through the mounted root.
		game.start_world_load(
			{"mission_file": bms},
			Callable(world, "load_mission_data").bind(saved_mission, bms))
	var started: int = Time.get_ticks_msec()
	while world.get_sim() == null or not world.get_sim().has_local_player():
		await mount.get_tree().process_frame
		if not load_error[0].is_empty():
			game.queue_free()
			return {"error": load_error[0]}
		if Time.get_ticks_msec() - started > LOAD_TIMEOUT_MSEC:
			game.queue_free()
			return {"error": "timed out loading %s" % bms}

	return {
		"error": "",
		"game": game,
		"world": world,
		"camera": camera,
		"viewport": camera.get_viewport(),
	}
