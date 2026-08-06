extends SceneTree

# Wave-1 smoke (ADR 0028 trunk): with an asset root installed the sim resolves
# its collision/occlusion .3di data through its OWN SimModelCache — proven by
# attaching against a placer that supplies NOTHING — and the attach count
# matches the legacy render-cache extraction exactly. Also pins the native
# TickAccumulator bank the runtime loop now drives.
#
# Run:
#   NOVA_RESOURCE_DIR=<loose JOX> godot --headless --path godot \
#     -s res://tests/wave1_native_assets_probe.gd

const MissionObjectPlacer := preload("res://adapter/mission/mission_object_placer.gd")

const MISSION := "00TRg.bms"


class NullPlacer:
	extends RefCounted

	func object_data_for(_graphic: String) -> NovaObjectData:
		return null

	func skeletal_anim_for(_item_id: int, _graphic: String):
		return null


func _init() -> void:
	call_deferred("_run")


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
	print("wave1 native assets probe: PASS")
	quit(0)
