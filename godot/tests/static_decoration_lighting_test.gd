extends GutTest

# The per-entry lighting of a batched static decoration on the retail stack
# (docs/render/render-lighting-re.md, the static-row lane): a decoration inside
# a building draws in the first entity wave with its containing building's
# interior light group, effectScale 1 and the 0x80 interior flag over the
# stack-base daylight 0; one outdoors takes its sun factor (1.0: a pool-2
# static has no candidate slice) and no interior (engine:
# renderer::static_row_entity_lighting carries the witness). A batched row
# carries those four floats in its atlas row's lane texel 9.
# CP03 (Operation: Ghost Harvest): the armory crate Armry03 (bms 2538, items.def
# "Armory Version #3", type decoration) stands in HN_Bld1's section 5 beside the
# player spawn; the light post Lpost01 (bms 672, type decoration) stands outside
# the west door. Retail draws the crate interior-dim (a retail onhook capture at
# the spawn facing north). Needs OPENNOVA_JO_DIR and a windowed Forward+ run
# (MultiMesh instance rows).

const MISSION := "CP03.bms"
const CRATE_BMS := 2538
const LIGHT_POST_BMS := 672
const LANE_TEXEL := 9
# The authored spawn (mission space, Z up), the retail deploy's pose to 1 mm.
const SPAWN := Vector3(1161.5025, 436.5958, 21.8551)

var _world: GameWorld
var _presenter: LocalPlayerPresenter
var _camera: Camera3D


func after_each() -> void:
	if is_instance_valid(_presenter):
		_presenter.teardown()
	_presenter = null


func _device_multimesh_rows_available() -> bool:
	return DisplayServer.get_name() != "headless" and \
			RenderingServer.get_current_rendering_method() == "forward_plus"


func _frame() -> void:
	var input := _presenter.before_world_tick(Simulation.tick_dt(), false, true)
	_world.tick(_camera.global_position, _camera.global_transform, Simulation.tick_dt(), input)
	_presenter.after_world_tick()


func _boot() -> bool:
	if RetailData.install().is_empty():
		pending("OPENNOVA_JO_DIR with CP03.bms is required")
		return false
	var root := RetailData.mount_install_with(MISSION)
	assert_not_null(root, "the install serves CP03.bms")
	if root == null:
		return false
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, MISSION), OK)
	_world = WorldFixture.make_world(self)
	_world.set_resource_root(root)
	var load_error := _world.load_mission_data(mission, MISSION)
	assert_eq(load_error, OK)
	if load_error != OK:
		return false
	_world.set_process(false)
	_camera = Camera3D.new()
	add_child_autofree(_camera)
	_camera.make_current()
	_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_presenter)
	_presenter.setup(_world, _camera, null, ControlsModel.new())
	return true


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
	if not _boot():
		return
	# The spawn facing north: the crate (and the light post) in view, so the
	# collector keeps their rows live.
	assert_eq(_world.get_sim().debug_teleport_local_player(SPAWN, 0.0, 0.0), OK)
	for i in 8:
		_frame()
	var crate := _static_lane(CRATE_BMS)
	assert_gt(crate.r, -0.5, "the crate draws as a live batched static row")
	assert_almost_eq(crate.r, 1.0, 0.001, "a contained static keeps effectScale 1")
	assert_almost_eq(crate.g, 1.0, 0.001,
			"the crate in HN_Bld1 section 5 takes the interior lerp (the 0x80 flag)")
	assert_almost_eq(crate.b, 0.0, 0.001,
			"a non-person entity lerps with the stack-base daylight 0")
	var post := _static_lane(LIGHT_POST_BMS)
	assert_gt(post.r, -0.5, "the light post draws as a batched static row")
	assert_almost_eq(post.r, 1.0, 0.001, "an outdoor static keeps effectScale 1 (no slice)")
	assert_almost_eq(post.g, 0.0, 0.001, "an outdoor static takes no interior lerp")
