extends GutTest

# The per-entry lighting of a batched static decoration on the real stack
# (docs/render/render-lighting-re.md, the static-row lane): a decoration inside
# a building draws in the first entity wave with its containing building's
# interior light group, effectScale 1 and the 0x80 interior flag over the
# stack-base daylight 0; one outdoors takes its sun factor (1.0: a pool-2
# static has no candidate slice) and no interior (engine:
# renderer::static_row_entity_lighting carries the witness). A batched row
# carries those four floats in its atlas row's lane texel 9.
# The witness first ran on CP03 (Operation: Ghost Harvest): the armory crate
# Armry03 (items.def "Armory Version #3", type decoration) standing in
# HN_Bld1's section 5 beside the player spawn, and the light post Lpost01
# (type decoration) outside the west door; retail draws the crate interior-dim
# (a retail onhook capture at the spawn facing north). Here the building is
# fixtures/gut/armory_floor.3di (tests/fixtures/gut_world_gen.cpp): a crate
# stands on the east room's floor, inside its blink box, and the 4-unit
# fixtures/gut/light_post.3di outside beyond the room's south window, both
# placed as pool-2 decoration rows; the player stands in the room facing the
# window. Needs a windowed Forward+ run (MultiMesh instance rows).

const TEST_ROOT := "opennova_static_decoration_lighting"
const TMAP_FIXTURE_DIR := "res://../fixtures/terrain/tmap"
const ARMORY_FIXTURE := "res://../fixtures/gut/armory_floor.3di"
const CRATE_FIXTURE := "res://../fixtures/threedi/synth/crate.3di"
const LIGHT_POST_FIXTURE := "res://../fixtures/gut/light_post.3di"
const PERSON_FIXTURE := "res://../fixtures/threedi/synth/person.3di"
const BUILDING_ID := 102001
const CRATE_ID := 102101
const LIGHT_POST_ID := 102102
const ITEM_DEFS := """begin "Guard Tower"
  id 102001
  type building
  graphic GuardTwr1
  sid guardtwr1
  anim_def GuardTwr1
  husk GuardTwr1X
  hp 5000
end

begin "Armory Version #3"
  id 102101
  type decoration
  graphic crate
end

begin "Light Post"
  id 102102
  type decoration
  graphic Lpost01
end
"""
# The player's body (105310, the placer's PLAYER_VISUAL_ITEM_ID) and its
# template (105305, retail's "Player #1, Multiplayer", the type the spawn
# seeds the local body from) over the synthetic 19-bone person, leading the
# file (a type id resolves to its FIRST row; blink_reopen_edge_test.gd).
const PLAYER_DEFS := """begin "Player Character"
  id 105310
  type person
  graphic person
  sid player
  anim_def soldier
  hp 100
end

begin "Player #1, Multiplayer"
  id 105305
  type person
  graphic person
  anim_def soldier
  ai_function plyr
  disk_function PLAYER
  move_function org2
  hp 150
end
"""
const LANE_TEXEL := 9
# Mission-space (Z up) site of the building on the Tmap surface, its floor
# FLOOR_LIFT above the ground (blink_reopen_edge_test.gd's site).
const SITE := Vector2(-16.0, 24.0)
const FLOOR_LIFT := 0.5
# Armry01's placement yaw: the model's x axis points along mission -x, so the
# east room's south window looks along mission +y (north).
const BUILDING_ROTATION := Vector3(0.0, -90.0, 0.0)
# Mission-space offsets from the site: the crate on the east room's floor
# (model (4.5, -2.0)), the post on the ground five units beyond its south
# window (model (2.6, -8.5)), and the player in the room behind the window
# (model (2.6, 1.5)), facing north (mission yaw 0) through it.
const CRATE_AT := Vector2(-4.5, 2.0)
const LIGHT_POST_AT := Vector2(-2.6, 8.5)
const SPAWN_AT := Vector3(-2.6, -1.5, 0.2)

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


func _device_multimesh_rows_available() -> bool:
	return DisplayServer.get_name() != "headless" and \
			RenderingServer.get_current_rendering_method() == "forward_plus"


func _frame() -> void:
	var input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
	_world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), input)


# The Tmap terrain first, then the minimal pack, the person rig, the armory as
# item 102001's GuardTwr1 graphic and the two decorations' graphics.
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
	TestFs.copy(self, CRATE_FIXTURE, root_dir.path_join("crate.3di"))
	TestFs.copy(self, LIGHT_POST_FIXTURE, root_dir.path_join("Lpost01.3di"))
	TestFs.copy(self, PERSON_FIXTURE, root_dir.path_join("person.3di"))
	for clip in ["soldier.adm", "idle.bad", "walk.bad"]:
		TestFs.copy(self, "res://../fixtures/anim/" + clip, root_dir.path_join(clip))
	var items := FileAccess.get_file_as_string(root_dir.path_join("items.def"))
	TestFs.write_text(self, root_dir.path_join("items.def"),
			PLAYER_DEFS + "\n" + items + "\n" + ITEM_DEFS)
	return root_dir


func _boot() -> Dictionary:
	_root_dir = _stage_root()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK)
	var terrain_data := TerrainData.new()
	assert_eq(terrain_data.load_from_resource_root(root, "Tmap.trn"), OK)
	var site := Vector3(SITE.x, SITE.y,
			terrain_data.get_height_world(Vector3(SITE.x, 0, -SITE.y)) + FLOOR_LIFT)
	var post := Vector3(SITE.x + LIGHT_POST_AT.x, SITE.y + LIGHT_POST_AT.y, 0.0)
	post.z = terrain_data.get_height_world(Vector3(post.x, 0, -post.y))
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, WorldFixture.MINIMAL_MISSION), OK)
	assert_true(mission.set_header_string("terrain", "Tmap"))
	assert_true(mission.set_header_string("environment", "mnml"))
	assert_not_null(mission.add_entity(MissionData.KIND_BUILDING, BUILDING_ID, site, BUILDING_ROTATION))
	var crate := mission.add_entity(MissionData.KIND_BUILDING, CRATE_ID,
			site + Vector3(CRATE_AT.x, CRATE_AT.y, 0.0), Vector3.ZERO)
	var light_post := mission.add_entity(MissionData.KIND_BUILDING, LIGHT_POST_ID, post, Vector3.ZERO)
	assert_not_null(crate)
	assert_not_null(light_post)
	_world = WorldFixture.make_world(self)
	_world.set_resource_root(root)
	var load_error := _world.load_mission_data(mission, WorldFixture.MINIMAL_MISSION)
	assert_eq(load_error, OK)
	if load_error != OK:
		return {}
	assert_true(_world.get_sim().has_local_player(), "the playable load spawns the host player")
	_world.set_process(false)
	_camera = Camera3D.new()
	add_child_autofree(_camera)
	_camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(_world, _camera, null, ControlsModel.new())
	return {"site": site, "crate": crate.bms_id, "light_post": light_post.bms_id}


# The lane of the first live static row drawing `bms_id`: the population's
# row -> slot map, the slot's BMS id, and the row's INSTANCE_CUSTOM.x (atlas
# row + 1) into the published atlas.
func _static_lane(bms_id: int) -> Color:
	var director := _world.get_effect_light_director()
	var atlas: Image = director.scene().get_static_light_rows_image() if director != null else null
	var objects := _world.find_child("MissionObjects", true, false)
	var populations := objects.get_node_or_null("StaticPopulations") if objects != null else null
	if atlas == null or populations == null:
		return Color(-1.0, -1.0, -1.0, -1.0)
	for child in populations.get_children():
		var population := child as StaticPopulationInstance
		if population == null or population.multimesh == null:
			continue
		var slot_ids := population.get_slot_bms_ids()
		var row_slots := population.get_row_slots()
		for row in row_slots.size():
			var slot := row_slots[row]
			if slot < 0 or slot >= slot_ids.size() or slot_ids[slot] != bms_id:
				continue
			var atlas_row := int(round(population.multimesh.get_instance_custom_data(row).r)) - 1
			if atlas_row < 0 or atlas_row >= atlas.get_height():
				continue
			return atlas.get_pixel(LANE_TEXEL, atlas_row)
	return Color(-1.0, -1.0, -1.0, -1.0)


func test_a_decoration_inside_a_building_draws_with_the_interior_lane() -> void:
	if not _device_multimesh_rows_available():
		pending("MultiMesh instance rows need a windowed Forward+ run")
		return
	var booted := _boot()
	if booted.is_empty():
		return
	var site: Vector3 = booted["site"]
	# The room facing north through the window: the crate (and the light post)
	# in view, so the collector keeps their rows live.
	assert_eq(_world.get_sim().debug_teleport_local_player(site + SPAWN_AT, 0.0, 0.0), OK)
	for i in 8:
		_frame()
	var crate := _static_lane(int(booted["crate"]))
	assert_gt(crate.r, -0.5, "the crate draws as a live batched static row")
	assert_almost_eq(crate.r, 1.0, 0.001, "a contained static keeps effectScale 1")
	assert_almost_eq(crate.g, 1.0, 0.001,
			"the crate in the armory's east room takes the interior lerp (the 0x80 flag)")
	assert_almost_eq(crate.b, 0.0, 0.001,
			"a non-person entity lerps with the stack-base daylight 0")
	var post := _static_lane(int(booted["light_post"]))
	assert_gt(post.r, -0.5, "the light post draws as a batched static row")
	assert_almost_eq(post.r, 1.0, 0.001, "an outdoor static keeps effectScale 1 (no slice)")
	assert_almost_eq(post.g, 0.0, 0.001, "an outdoor static takes no interior lerp")
