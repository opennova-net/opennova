extends GutTest

# The methods of tests/object_projection_lod_test.gd that need a RenderingDevice, split
# out of it so the windowed run selects them (G-100).

var _fixture_root := ""
var _resources: ResourceRoot
var _items: ItemDatabase

# The beauty camera (FrameFx kBeautyCameraMask) and the Inset camera it
# derives (HudInsetScope: minus the viewmodel, FP body, caster, main-view,
# main-view-no-reflection and main-view foliage bits, plus INSET_VIEW).
const MAIN_MASK := 0x0CD78C01
const INSET_MASK := 0x04258401


func before_all() -> void:
	_fixture_root = WorldFixture.stage_minimal_root("entity_projection_lod", false, {
		"items.def": """begin "Projection object"
id 106401
type object
graphic pump_lod20
scale 1.5
end
begin "Projection person"
id 106402
type person
graphic pump_lod20
scale 1.5
end
begin "Projection crate"
id 106403
type object
graphic crate
scale 2
husk pump_lod20
end
begin "Projection parachute"
id 100185
type object
graphic armory
end
""",
		"avatars.def": """define head HEAD
{
 graphic pump_lod80.3di
 camo 32 64 96
 voice 3
 sex m
}
define body BODY
{
 graphic pump_lod20.3di
 camo 100 120 140
}
define arms ARMS
{
 graphic pump_lod20.3di
 camo 200 210 220
}
nationality 0 NAT
{
 alignment good
 division 0 DIV
 {
  combo 2 HEAD BODY ARMS
 }
}
"""})
	_items = ItemDatabase.new()
	assert_eq(_items.load(_fixture_root.path_join("items.def")), OK)
	_resources = ResourceRoot.new()
	assert_eq(_resources.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth")), OK)


func after_all() -> void:
	_resources = null
	_items = null
	TestFs.remove_dir_recursive(_fixture_root)


func _placer() -> MissionObjectPlacer:
	return MissionObjectPlacer.create(_resources, _items)


func _place(placer: MissionObjectPlacer, item_id: int,
		rotations: Array[Vector3] = [Vector3.ZERO]) -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var entities: Array = []
	for rotation in rotations:
		var entity = mission.add_entity(MissionData.KIND_ITEM, item_id,
				Vector3.ZERO, rotation)
		assert_not_null(entity)
		entities.append(entity)
	var parent := Node3D.new()
	add_child_autofree(parent)
	var stats := placer.place(mission, parent)
	return {"parent": parent, "entities": entities, "stats": stats}


func _camera(depth: float) -> Transform3D:
	return Transform3D(Basis.IDENTITY, Vector3(0, 0, depth))


# --- the weapon Inset view -----------------------------------------------------
# Retail runs the whole scene pass per view (Render_WeaponInsetScene's own
# Terrain_RenderWorldScene: collector, section masks, sub-pixel floor, RLOD
# walk, each on the pass's frame scale). Where the two views disagree the node
# draws the main view's on the main-view bits and a twin draws the Inset's.

func _view_camera(size: Vector2i, fov: float, depth: float, mask := 0xFFFFF) -> Camera3D:
	var view := SubViewport.new()
	view.size = size
	view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(view)
	var camera := Camera3D.new()
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = fov
	camera.cull_mask = mask
	view.add_child(camera)
	camera.current = true
	camera.global_transform = _camera(depth)
	return camera


func _grab(camera: Camera3D) -> Image:
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	return (camera.get_viewport() as SubViewport).get_texture().get_image()


func test_captures_draw_each_views_own_level() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice capture: run windowed")
		return
	var placer := _placer()
	var placed := _place(placer, 106401)
	assert_eq(placed.stats.animated, 1)
	var model := placer.get_placed_models()[0] as ObjectModel
	var main := _view_camera(Vector2i(160, 120), 90.0, 200.0, MAIN_MASK)
	var inset := _view_camera(Vector2i(256, 256), 10.0, 200.0, INSET_MASK)
	ObjectModel.update_authored_lods_for_views(main, 640.0, inset, 256.0)
	assert_eq(model.get_active_lod(), 1)
	assert_eq(model.get_inset_view_lod(), 0)
	var split_main: Image = await _grab(main)
	var split_inset: Image = await _grab(inset)
	# References: the Inset closed and the one node posed at each level.
	ObjectModel.update_authored_lods_for_views(main, 640.0, null, 0.0)
	model.set_active_lod(1)
	var level1_main: Image = await _grab(main)
	var level1_inset: Image = await _grab(inset)
	model.set_active_lod(0)
	var level0_inset: Image = await _grab(inset)
	assert_ne(level0_inset.get_data(), level1_inset.get_data(),
			"the two levels draw differently through the Inset (level 1 drops the post)")
	assert_eq(split_main.get_data(), level1_main.get_data(),
			"the main capture draws the main view's level 1")
	assert_eq(split_inset.get_data(), level0_inset.get_data(),
			"the Inset capture draws its own level 0 on the twin")


func test_capture_of_a_main_view_sub_pixel_model_draws_in_the_inset_only() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice capture: run windowed")
		return
	var main := _view_camera(Vector2i(160, 120), 90.0, 3000.0, MAIN_MASK)
	var inset := _view_camera(Vector2i(256, 256), 2.0, 3000.0, INSET_MASK)
	# The reference with nothing placed.
	var empty_main: Image = await _grab(main)
	var empty_inset: Image = await _grab(inset)
	var placer := _placer()
	var placed := _place(placer, 106401)
	assert_eq(placed.stats.animated, 1)
	var model := placer.get_placed_models()[0] as ObjectModel
	ObjectModel.update_authored_lods_for_views(main, 640.0, inset, 256.0)
	assert_true(model.is_subpixel_hidden())
	var split_main: Image = await _grab(main)
	var split_inset: Image = await _grab(inset)
	# The reference with the Inset view's own selection as the only view (its
	# level, drawn by the one node).
	ObjectModel.update_authored_lods_for_views(inset, 256.0, null, 0.0)
	assert_false(model.is_subpixel_hidden())
	var alone_inset: Image = await _grab(inset)
	assert_eq(split_main.get_data(), empty_main.get_data(), "the main view draws nothing")
	assert_ne(split_inset.get_data(), empty_inset.get_data(), "the Inset draws the model")
	assert_eq(split_inset.get_data(), alone_inset.get_data(),
			"exactly as the one node draws it at the Inset's level")
