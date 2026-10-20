extends GutTest

# The blink letters' reopen edge on the real stack (docs/render/
# render-occlusion-re.md §4): the letters gate the frame they are read in, so
# the tick whose letters clear the indoors bit draws the terrain in its own
# frame (renderer_render_order pins the edge law, scene_pass_gate_edges).
# The building is fixtures/gut/armory_floor.3di (tests/fixtures/
# gut_world_gen.cpp): the synthetic armory's floor plan, whose blink boxes copy
# the layout of 00TRa's Armry01 this witness first ran on (bms 20, yaw -90):
# the west room's box (section 2, flags 0x28; the blink walk accumulates
# flags ^ 6, so its letters are 0x2E, the indoors bit) behind the middle
# hall's (section 3, flags 0x2E, letters 0x28: indoors clear, the water
# letter kept). Its rooms stand on the floor slab Armry01 authors as its
# COBJ 0 volume 0: an indoors body skips the heightfield, so a room without a
# solid under it drops its occupant through the floor. While the letter
# holds, the hidden terrain still tracks the frame's visible bounds (the water
# leg's water-active input). The terrain is the render-capable synthetic Tmap.

const TEST_ROOT := "opennova_blink_reopen_edge"
const TMAP_FIXTURE_DIR := "res://../fixtures/terrain/tmap"
const ARMORY_FIXTURE := "res://../fixtures/gut/armory_floor.3di"
const PERSON_FIXTURE := "res://../fixtures/threedi/synth/person.3di"
const BUILDING_ID := 102001
const BUILDING_ITEM_DEF := """begin "Guard Tower"
  id 102001
  type building
  graphic GuardTwr1
  sid guardtwr1
  anim_def GuardTwr1
  husk GuardTwr1X
  hp 5000
end
"""
# The player's body: items.def person row 105310 (the placer's
# PLAYER_VISUAL_ITEM_ID) over the synthetic 19-bone person, leading the file
# (a type id resolves to its FIRST row).
const PLAYER_ITEM_DEF := """begin "Player Character"
  id 105310
  type person
  graphic person
  sid player
  anim_def soldier
  hp 100
end
"""
# Mission-space (Z up) site of the building on the Tmap surface, west of the
# world origin where the ground carries only its gentle z slope; the floor
# stands FLOOR_LIFT above the ground there, clear of it over the whole plan,
# the slab reaching into the ground below it.
const SITE := Vector2(-16.0, 24.0)
const FLOOR_LIFT := 0.5
# Armry01's placement yaw: the model's x axis points along mission -x.
const BUILDING_ROTATION := Vector3(0.0, -90.0, 0.0)
# Mission-space offsets from the site on the armory floor, the retail
# witness's own (00TRa: (286.2, -407.4) and (282.2, -407.0) beside the
# armory at (282.49, -407.12)): inside the west room's box and in the middle
# hall beside it.
const INDOORS_ROOM := Vector3(3.7, -0.3, 0.2)
const HALL := Vector3(-0.3, 0.1, 0.2)
# Mission yaw 270 faces -X: from the room toward the hall.
const FACING_HALL := 270.0

var _world: GameWorld
var _presenter: LocalPlayerPresenter
var _camera: Camera3D
var _root_dir := ""


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null
	if not _root_dir.is_empty():
		TestFs.remove_dir_recursive(_root_dir)
	_root_dir = ""


func _frame() -> void:
	var input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
	_world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), input)


func _indoors(sim: Simulation) -> bool:
	return (sim.local_player_blink_flags() & Simulation.BLINK_INDOORS) != 0


# The Tmap terrain first, then the minimal pack (its items.def wins), the
# player's person rig and the armory as item 102001's GuardTwr1 graphic.
func _stage_root() -> String:
	var root_dir := OS.get_cache_dir().path_join(TEST_ROOT).path_join(
			"root_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	for dir in [TMAP_FIXTURE_DIR, WorldFixture.BOOT_FIXTURE_DIR]:
		var source_dir := ProjectSettings.globalize_path(dir)
		for file_name in DirAccess.get_files_at(source_dir):
			var target := root_dir.path_join(file_name)
			if FileAccess.file_exists(target):
				assert_eq(DirAccess.remove_absolute(target), OK)
			assert_eq(DirAccess.copy_absolute(source_dir.path_join(file_name), target), OK)
	TestFs.copy(self, ARMORY_FIXTURE, root_dir.path_join("GuardTwr1.3di"))
	TestFs.copy(self, PERSON_FIXTURE, root_dir.path_join("person.3di"))
	for clip in ["soldier.adm", "idle.bad", "walk.bad"]:
		TestFs.copy(self, "res://../fixtures/anim/" + clip, root_dir.path_join(clip))
	var items := FileAccess.get_file_as_string(root_dir.path_join("items.def"))
	TestFs.write_text(self, root_dir.path_join("items.def"), PLAYER_ITEM_DEF + "\n"
			+ items + "\n" + BUILDING_ITEM_DEF)
	return root_dir


func _boot() -> Dictionary:
	_root_dir = _stage_root()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK)
	var terrain_data := TerrainData.new()
	assert_eq(terrain_data.load_from_resource_root(root, "Tmap.trn"), OK)
	var site := Vector3(SITE.x, SITE.y,
			terrain_data.get_height_world(Vector3(SITE.x, 0, -SITE.y)) + FLOOR_LIFT)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, WorldFixture.MINIMAL_MISSION), OK)
	assert_true(mission.set_header_string("terrain", "Tmap"))
	assert_true(mission.set_header_string("environment", "mnml"))
	var placed := mission.add_entity(MissionData.KIND_BUILDING, BUILDING_ID, site, BUILDING_ROTATION)
	assert_not_null(placed)
	_world = WorldFixture.make_world(self)
	_world.set_resource_root(root)
	var load_error := _world.load_mission_data(mission, WorldFixture.MINIMAL_MISSION)
	assert_eq(load_error, OK)
	if load_error != OK:
		return {}
	var sim := _world.get_sim()
	assert_true(sim.has_local_player(), "the playable load spawns the host player")
	assert_true(sim.has_collision_instance(placed.bms_id), "the armory enters the collision world")
	_world.set_process(false)
	_camera = Camera3D.new()
	add_child_autofree(_camera)
	_camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(_world, _camera, null, ControlsModel.new())
	return {"sim": sim, "site": site}


func test_indoors_letter_clear_draws_the_terrain_in_its_own_frame() -> void:
	var booted := _boot()
	if booted.is_empty():
		return
	var sim: Simulation = booted["sim"]
	var site: Vector3 = booted["site"]
	var terrain := _world.get_node("Terrain") as Terrain
	assert_eq(sim.debug_teleport_local_player(site + INDOORS_ROOM, FACING_HALL, 0.0), OK)
	for i in 60:
		_frame()
		if _indoors(sim):
			break
	assert_true(_indoors(sim), "the west room's box carries the indoors letter")
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
	assert_eq(sim.debug_teleport_local_player(site + HALL, FACING_HALL, 0.0), OK)
	var cleared := false
	for i in 60:
		_frame()
		if not _indoors(sim):
			cleared = true
			break
	assert_true(cleared, "the middle hall clears the indoors letter")
	assert_ne(sim.local_player_blink_flags() & Simulation.BLINK_WATER_OFF, 0,
			"the hall keeps the water letter")
	assert_true(terrain.visible, "the cleared letter shows the terrain")
	assert_gt(terrain.get_visible_patch_count(), 0,
			"the terrain draws in the frame its letter cleared, not the next")
