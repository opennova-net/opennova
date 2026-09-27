extends GutTest

# The blink letters' reopen edge on the retail stack (docs/render/
# render-occlusion-re.md §4): the letters gate the frame they are read in, so
# the tick whose letters clear the indoors bit draws the terrain in its own
# frame (renderer_render_order pins the edge law, scene_pass_gate_edges).
# 00TRa's Armry01 (bms 20, yaw -90 at (282.49, -407.12)) carries the one
# indoors room of the stock training maps: the section 2 box (flags 0x28,
# letters 0x2E) behind the section 3 hall (flags 0x2E, letters 0x28), which
# keeps the water letter. While the letter holds, the hidden terrain still
# tracks the frame's visible bounds (the water leg's water-active input).
# Needs OPENNOVA_JO_DIR.

const MISSION := "00TRa.bms"
# Mission-space points (Z up) on the armory floor: inside the section 2 box
# and in the middle of the section 3 hall beside it.
const INDOORS_ROOM := Vector3(286.2, -407.4, 28.2)
const HALL := Vector3(282.2, -407.0, 28.2)
# Mission yaw 270 faces -X: from the room toward the hall.
const FACING_HALL := 270.0

var _world: GameWorld
var _presenter: LocalPlayerPresenter
var _camera: Camera3D


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null


func _frame() -> void:
	var input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
	_world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), input)
	_presenter.after_world_tick()


func _indoors(sim: Simulation) -> bool:
	return (sim.local_player_blink_flags() & Simulation.BLINK_INDOORS) != 0


func _boot() -> Simulation:
	if RetailData.install().is_empty():
		pending("OPENNOVA_JO_DIR with 00TRa.bms is required")
		return null
	var root := RetailData.mount_install_with(MISSION)
	assert_not_null(root, "the install serves 00TRa.bms")
	if root == null:
		return null
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, MISSION), OK)
	_world = WorldFixture.make_world(self)
	_world.set_resource_root(root)
	var load_error := _world.load_mission_data(mission, MISSION)
	assert_eq(load_error, OK)
	if load_error != OK:
		return null
	_world.set_process(false)
	_camera = Camera3D.new()
	add_child_autofree(_camera)
	_camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(_world, _camera, null, ControlsModel.new())
	return _world.get_sim()


func test_indoors_letter_clear_draws_the_terrain_in_its_own_frame() -> void:
	var sim := _boot()
	if sim == null:
		return
	var terrain := _world.get_node("Terrain") as Terrain
	assert_eq(sim.debug_teleport_local_player(INDOORS_ROOM, FACING_HALL, 0.0), OK)
	for i in 60:
		_frame()
		if _indoors(sim):
			break
	assert_true(_indoors(sim), "the section 2 box carries the indoors letter")
	_frame()
	assert_false(terrain.visible, "the indoors letter hides the terrain")
	assert_eq(terrain.get_visible_patch_count(), 0, "a hidden terrain draws no patch")
	# The letter skips the traversal and the sector pass only: the frame's
	# bounds walk still feeds the water-active test (terrain_frame.h
	# track_terrain_visible_bounds).
	assert_true(terrain.has_visible_terrain_bounds(),
			"a hidden terrain still tracks the frame's visible bounds")

	# Into the hall: the frame whose tick clears the indoors letter ran its
	# terrain leg closed, before that tick's letters reached the gates; the
	# reopen edge re-runs the leg so the terrain draws in that same frame.
	assert_eq(sim.debug_teleport_local_player(HALL, FACING_HALL, 0.0), OK)
	var cleared := false
	for i in 60:
		_frame()
		if not _indoors(sim):
			cleared = true
			break
	assert_true(cleared, "the section 3 hall clears the indoors letter")
	assert_ne(sim.local_player_blink_flags() & Simulation.BLINK_WATER_OFF, 0,
			"the hall keeps the water letter")
	assert_true(terrain.visible, "the cleared letter shows the terrain")
	assert_gt(terrain.get_visible_patch_count(), 0,
			"the terrain draws in the frame its letter cleared, not the next")
