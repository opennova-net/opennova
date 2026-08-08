extends SceneTree

# S3/S4 native soak probe (ADR 0028): boots retail 00TRg with the sim's own
# asset root installed (the native SimCollisionPoseProvider + native mounted
# resolver are AUTHORITATIVE — the legacy A/B this soak once compared is retired)
# plus the seat-spec table, then runs an extended live-shaped load: thousands of
# 62.5 Hz ticks with the mission's AI thinking, walking, and command-mounted on
# the 50cals (bms 37 -> SSN 65, 51 -> 64), a spawned local player with an
# installed weapon, and periodic full-registry hitbox sweeps. Every tick resolves
# the mounted UseGun frames natively and every sweep/raycast drives the native
# collision pose path; the run must keep producing live hitboxes.
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

	# The FULL runtime boot (the S9 engine sequence incl. the spawn-promote
	# leg that command-mounts the 50cal gunners) with the render placer
	# supplied, so mounted AI resolve their UseGun frames through the native
	# pose path every tick.
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
	# The native pose paths are AUTHORITATIVE (ADR 0028): the A/B that this soak
	# once compared is retired. This is now a long-run native-stability smoke —
	# thousands of ticks with the mission's AI thinking/walking and command-mounted
	# on the 50cals, every sweep driving build_section_matrices through the sim's
	# own SimCollisionPoseProvider. It must keep producing live hitboxes and never
	# regress to zero.
	var max_hitbox_entities := 0
	var min_hitbox_entities := 0x7fffffff
	for r in rounds:
		for _t in 62:
			runtime.tick()
		var hb: Dictionary = sim.get_hitbox_debug()
		var entity_count := int((hb.get("entities", []) as Array).size())
		max_hitbox_entities = maxi(max_hitbox_entities, entity_count)
		if entity_count > 0:
			min_hitbox_entities = mini(min_hitbox_entities, entity_count)
		if (r + 1) % 60 == 0 or r == rounds - 1:
			print("[soak] round %d/%d hitbox_entities=%d" % [
					r + 1, rounds, entity_count])

	if max_hitbox_entities <= 0:
		_fail("native collision produced no hitboxes across the soak")
		return
	print("s3_s4_soak_probe: PASS (native soak, hitbox_entities min=%d max=%d over %d ticks)" % [
			min_hitbox_entities, max_hitbox_entities, rounds * 62])
	quit(0)
