extends SceneTree

# S3/S4 soak probe (ADR 0028 gates): boots retail 00TRg with BOTH pose sources
# installed (render placer = legacy authoritative, asset root = native shadow,
# Compare mode default) plus the shell-extracted seat-spec table, then runs an
# extended live-shaped load: thousands of 62.5 Hz ticks with the mission's AI
# thinking, walking, and command-mounted on the 50cals (bms 37 -> SSN 65,
# 51 -> 64), a spawned local player with an installed weapon, and periodic
# full-registry hitbox sweeps. Every tick resolves the mounted UseGun frames
# through the S4 compare seam and every sweep/raycast drives the S3 one.
#
# Run:
#   NOVA_RESOURCE_DIR=<retail JOX corpus> godot --headless --path godot \
#     -s res://tests/s3_s4_soak_probe.gd
# Optional: SOAK_ROUNDS (default 300; one round = 62 ticks + one sweep).

const MissionObjectPlacer := preload("res://adapter/mission/mission_object_placer.gd")
const MissionRuntime := preload("res://adapter/world/mission_runtime.gd")

const MISSION := "00TRg.bms"


func _init() -> void:
	call_deferred("_run")


func _fail(message: String) -> void:
	push_error("s3_s4_soak_probe: " + message)
	quit(1)


func _run() -> void:
	var resource_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if resource_dir.is_empty():
		_fail("set NOVA_RESOURCE_DIR to a loose or mounted retail JOX corpus")
		return
	var rounds := 300
	var rounds_env := OS.get_environment("SOAK_ROUNDS").strip_edges()
	if not rounds_env.is_empty():
		rounds = maxi(int(rounds_env), 1)

	var root := NovaResourceRoot.new()
	if int(root.mount_runtime(resource_dir, "", false, "jo")) != OK:
		root.set_root_dir(resource_dir)
		if not root.has_file(MISSION):
			_fail("resource root failed: %s" % root.get_last_error())
			return
	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(root, MISSION) != OK:
		_fail("cannot open %s" % MISSION)
		return
	var item_db := NovaItemDatabase.new()
	if item_db.load_from_resource_root(root, "items.def") != OK:
		_fail("cannot load items.def: %s" % item_db.get_last_error())
		return

	# The FULL runtime boot (the S9 engine sequence incl. the host-spawn
	# promote leg that command-mounts the 50cal gunners) with the render
	# placer supplied, so the sim carries BOTH pose sources and mounted AI
	# resolve their UseGun frames through the S4 compare every tick.
	var placer := MissionObjectPlacer.new(root, item_db)
	var runtime := MissionRuntime.new()
	get_root().add_child(runtime)
	var container := Node3D.new()
	get_root().add_child(container)
	var boot_count := int(runtime.setup(mission, container, {
		"resource_root": root,
		"item_db": item_db,
		"mission_file": MISSION,
		"placer": placer,
		"playable": true,
	}))
	if boot_count <= 0 or int(runtime.get_setup_error()) != OK:
		_fail("runtime boot failed (count=%d err=%d)" % [
				boot_count, int(runtime.get_setup_error())])
		return
	var sim: NovaSimulation = runtime.get_sim()
	if sim == null:
		_fail("runtime has no sim")
		return
	if not bool(sim.install_local_player_weapon_by_name("WPN_M4AUTO")):
		_fail("weapon install failed")
		return

	print("[soak] 00TRg runtime up: boot=%d rounds=%d (%d ticks)" % [
			boot_count, rounds, rounds * 62])
	for r in rounds:
		for _t in 62:
			runtime.tick()
		var _hb: Dictionary = sim.get_hitbox_debug()
		if (r + 1) % 60 == 0 or r == rounds - 1:
			print("[soak] round %d/%d s3=%s" % [
					r + 1, rounds, str(sim.debug_collision_pose_ab_stats())])
			print("[soak] round %d/%d s4=%s" % [
					r + 1, rounds, str(sim.debug_mounted_pose_ab_stats())])

	var s3: Dictionary = sim.debug_collision_pose_ab_stats()
	var s4: Dictionary = sim.debug_mounted_pose_ab_stats()
	if int(s3.get("queries", 0)) <= 0:
		_fail("S3 compare saw no queries")
		return
	if int(s3.get("divergences", 0)) != 0 or int(s3.get("result_mismatches", 0)) != 0:
		_fail("S3 divergence: %s" % str(s3))
		return
	if int(s4.get("queries", 0)) <= 0:
		_fail("S4 compare saw no mounted queries")
		return
	if int(s4.get("divergences", 0)) != 0:
		_fail("S4 divergence: %s" % str(s4))
		return
	print("s3_s4_soak_probe: PASS (s3 queries=%d, s4 queries=%d, 0 divergences)" % [
			int(s3.get("queries", 0)), int(s4.get("queries", 0))])
	quit(0)
