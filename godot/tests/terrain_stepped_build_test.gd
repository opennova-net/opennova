extends GutTest

# The terrain's load and build a unit at a time (ADR 0046 S14: the OpenNova
# Editor's mission device builds a terrain over its frames, a unit a step):
# TerrainData.begin_load_from_resource_root / load_step read a file a step and
# come to what load_from_resource_root loads whole; Terrain.build_begin /
# build_step make a tile a step, then the patch pool, the surface inputs, the
# tile cache and the lights, and come to what build() builds whole. Neither is
# loaded or built until its last step; a load whose root or .trn is missing
# and a build with no loaded data are refused as the whole ones are.

const LOAD_UNITS := ["colormap", "detailmap", "detailmap_c1", "detailmap_c2",
		"detailmap_c3", "detailmap2", "detailmapdist", "detailmapdist2",
		"detailblendmap", "charmap", "foliagemap", "tilestrip", "heights"]
const BUILD_TAIL := ["patches", "heightfield", "blend", "detail", "pages", "lights"]

var _root_dir := ""


func before_all() -> void:
	_root_dir = TestFs.stage_terrain_root("stepped_build")


func after_all() -> void:
	TestFs.remove_dir_recursive(_root_dir)


func _root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK, "the staged terrain root mounts")
	return root


func _loaded(root: ResourceRoot) -> TerrainData:
	var data := TerrainData.new()
	assert_eq(data.load_from_resource_root(root, TestFs.TMAP_TRN), OK, "the Tmap fixture loads")
	return data


# A terrain over `data` under a viewport of its own world, a camera looking
# straight down at it; not built.
func _terrain_over(data: TerrainData) -> Terrain:
	var view := SubViewport.new()
	view.size = Vector2i(32, 32)
	view.own_world_3d = true
	add_child_autofree(view)
	var terrain := Terrain.new()
	view.add_child(terrain)
	terrain.set_terrain_data(data)
	var camera := Camera3D.new()
	view.add_child(camera)
	camera.make_current()
	camera.global_position = Vector3(64.0, 300.0, 64.0)
	camera.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	return terrain


func test_a_load_stepped_is_the_load_whole() -> void:
	var root := _root()
	var whole := _loaded(root)
	var stepped := TerrainData.new()
	assert_eq(stepped.load_step(), TerrainData.LOAD_STEP_FAILED, "no load begun: nothing loads")
	assert_eq(stepped.begin_load_from_resource_root(root, TestFs.TMAP_TRN), OK)
	assert_false(stepped.is_loaded(), "begun, not loaded")
	assert_eq(stepped.get_load_step_count(), LOAD_UNITS.size(), "a unit per file the load reads")
	assert_eq(stepped.get_load_steps_done(), 0)
	# The .trn is parsed as the load begins: its scalars are there before a unit runs.
	assert_eq(stepped.get_terrain_name(), whole.get_terrain_name())
	assert_eq(stepped.get_sector_grid(), whole.get_sector_grid())
	var labels: Array = []
	var step := TerrainData.LOAD_STEP_MORE
	while step == TerrainData.LOAD_STEP_MORE:
		labels.append(stepped.get_load_step_label())
		assert_false(stepped.is_loaded(), "not loaded until its last step")
		step = stepped.load_step()
		if step == TerrainData.LOAD_STEP_MORE:
			assert_eq(stepped.get_load_steps_done(), labels.size())
	assert_eq(step, TerrainData.LOAD_STEP_DONE)
	assert_eq(labels, LOAD_UNITS, "the load's units, in the load's order")
	assert_true(stepped.is_loaded())
	assert_eq(stepped.get_load_step_count(), 0, "a load done leaves no unit")
	assert_eq(stepped.get_load_step_label(), "")
	assert_eq(stepped.load_step(), TerrainData.LOAD_STEP_DONE, "a load done answers done")
	# The data, value for value (not only its size): the heights over the whole map, a sample every
	# 16 units; the foliage table the map's slots read, as many rows.
	var differ := 0
	var samples := 0
	for z in range(0, 512, 16):
		for x in range(0, 512, 16):
			var at := Vector3(float(x), 0.0, float(z))
			samples += 1
			if stepped.get_height_world_bilinear(at) != whole.get_height_world_bilinear(at):
				differ += 1
	assert_eq(differ, 0, "the heights alike at every one of %d samples" % samples)
	assert_eq(stepped.get_foliage_defs().size(), whole.get_foliage_defs().size(), "the foliage table alike")
	# What it loaded is what the whole load loaded.
	assert_eq(stepped.get_tile_count(), whole.get_tile_count())
	assert_gt(stepped.get_tile_count(), 0)
	assert_eq(stepped.get_sector_count(), whole.get_sector_count())
	assert_eq(stepped.get_sector_rows(), whole.get_sector_rows())
	assert_eq(stepped.get_origin_x(), whole.get_origin_x())
	assert_eq(stepped.get_origin_y(), whole.get_origin_y())
	assert_eq(stepped.get_water_height(), whole.get_water_height())
	assert_eq(stepped.get_horizon(), whole.get_horizon())
	assert_eq(stepped.get_detail_density(), whole.get_detail_density())
	assert_eq(stepped.get_tileinfo_filename(), whole.get_tileinfo_filename())
	assert_eq(stepped.get_foliage_map() != null, whole.get_foliage_map() != null, "the foliage map resolved alike")
	for getter in ["get_colormap", "get_detailmap", "get_detailmap_c1", "get_detailmap_c2",
			"get_detailmap_c3", "get_detailmap2", "get_detailmapdist", "get_detailmapdist2",
			"get_detailblendmap", "get_charmap_tex", "get_foliagemap_tex", "get_tilestrip_tex"]:
		var a: Texture2D = stepped.call(getter)
		var b: Texture2D = whole.call(getter)
		assert_eq(a != null, b != null, "%s resolved alike" % getter)
		if a != null and b != null:
			assert_eq(a.get_size(), b.get_size(), "%s the same size" % getter)
	assert_eq(stepped.get_height_world_bilinear(Vector3(64.0, 0.0, 64.0)),
			whole.get_height_world_bilinear(Vector3(64.0, 0.0, 64.0)))


func test_a_load_is_refused_as_the_whole_load_is() -> void:
	var data := TerrainData.new()
	assert_eq(data.begin_load_from_resource_root(null, TestFs.TMAP_TRN), ERR_INVALID_PARAMETER,
			"no root")
	assert_eq(data.begin_load_from_resource_root(ResourceRoot.new(), TestFs.TMAP_TRN),
			ERR_INVALID_PARAMETER, "a root that mounts nothing")
	var root := _root()
	assert_eq(data.begin_load_from_resource_root(root, ""), ERR_INVALID_PARAMETER, "no name")
	assert_eq(data.begin_load_from_resource_root(root, "nosuch.trn"), ERR_FILE_CANT_READ,
			"a .trn the root lacks")
	assert_false(data.is_loaded())
	assert_eq(data.get_load_step_count(), 0, "a load refused plans nothing")
	assert_eq(data.load_step(), TerrainData.LOAD_STEP_FAILED, "a load refused loads nothing")
	# A load begun over one in flight starts again from its first unit.
	assert_eq(data.begin_load_from_resource_root(root, TestFs.TMAP_TRN), OK)
	assert_eq(data.load_step(), TerrainData.LOAD_STEP_MORE)
	assert_eq(data.load_step(), TerrainData.LOAD_STEP_MORE)
	assert_eq(data.begin_load_from_resource_root(root, TestFs.TMAP_TRN), OK)
	assert_eq(data.get_load_steps_done(), 0)
	assert_eq(data.get_load_step_label(), "colormap")


func test_a_build_stepped_is_the_build_whole() -> void:
	var root := _root()
	var whole := _terrain_over(_loaded(root))
	whole.build()
	assert_true(whole.is_built(), "the whole build builds")
	var data := _loaded(root)
	var stepped := _terrain_over(data)
	assert_eq(stepped.build_step(), Terrain.BUILD_STEP_FAILED, "no build begun, nothing built")
	assert_true(stepped.build_begin())
	assert_false(stepped.is_built(), "begun, not built")
	var tiles := data.get_tile_count()
	assert_eq(stepped.get_build_step_count(), tiles + 1 + BUILD_TAIL.size(),
			"the scene, a unit per tile, then the tail")
	var labels: Array = []
	var step := Terrain.BUILD_STEP_MORE
	while step == Terrain.BUILD_STEP_MORE:
		labels.append(stepped.get_build_step_label())
		assert_false(stepped.is_built(), "not built until its last step")
		assert_eq(stepped.get_build_steps_done(), labels.size() - 1)
		step = stepped.build_step()
	assert_eq(step, Terrain.BUILD_STEP_DONE)
	assert_true(stepped.is_built())
	assert_eq(labels.size(), stepped.get_build_step_count())
	assert_eq(stepped.get_build_steps_done(), stepped.get_build_step_count())
	assert_eq(labels[0], "scene")
	for i in range(tiles):
		if labels[1 + i] != "tiles":
			fail_test("unit %d is a tile's" % (1 + i))
			break
	assert_eq(labels.slice(1 + tiles), BUILD_TAIL, "the tail's units, in the build's order")
	assert_eq(stepped.get_build_step_label(), "")
	assert_eq(stepped.build_step(), Terrain.BUILD_STEP_DONE, "built: nothing left to step")
	# What it built is what the whole build built: the same surface inputs, the
	# same patches drawn for the same camera.
	assert_eq(stepped.get_surface_inputs().get_diagnostics(), whole.get_surface_inputs().get_diagnostics())
	assert_eq(stepped.get_tile_cache_texture() != null, whole.get_tile_cache_texture() != null)
	whole.set_debug_no_frustum(true)
	stepped.set_debug_no_frustum(true)
	whole.render_frame()
	stepped.render_frame()
	assert_gt(whole.get_visible_patch_count(), 0, "the whole build draws patches")
	assert_eq(stepped.get_visible_patch_count(), whole.get_visible_patch_count())
	assert_eq(stepped.get_patches_active(), whole.get_patches_active())


func test_a_build_begun_again_starts_again_and_one_with_no_data_is_refused() -> void:
	var bare := Terrain.new()
	add_child_autofree(bare)
	assert_false(bare.build_begin(), "no terrain data")
	assert_eq(bare.build_step(), Terrain.BUILD_STEP_FAILED)
	var unloaded := Terrain.new()
	add_child_autofree(unloaded)
	unloaded.set_terrain_data(TerrainData.new())
	assert_false(unloaded.build_begin(), "terrain data not loaded")
	var terrain := _terrain_over(_loaded(_root()))
	assert_true(terrain.build_begin())
	for _i in range(3):
		assert_eq(terrain.build_step(), Terrain.BUILD_STEP_MORE)
	assert_eq(terrain.get_build_steps_done(), 3)
	# Begun again over the one in flight: from its first unit, what it made dropped.
	assert_true(terrain.build_begin())
	assert_eq(terrain.get_build_steps_done(), 0)
	assert_eq(terrain.get_build_step_label(), "scene")
	terrain.build()
	assert_true(terrain.is_built(), "build() over a build in flight builds whole")
	assert_eq(terrain.get_build_step_label(), "")
	# Other data set over a stepped build in flight: the build is of the old data, dropped (its next
	# step fails; nothing stands built).
	var other := _terrain_over(_loaded(_root()))
	assert_true(other.build_begin())
	for _i in range(3):
		assert_eq(other.build_step(), Terrain.BUILD_STEP_MORE)
	other.set_terrain_data(_loaded(_root()))
	assert_eq(other.build_step(), Terrain.BUILD_STEP_FAILED, "the build in flight dropped with its data")
	assert_false(other.is_built())
	assert_true(other.build_begin(), "a build of the new data begins")
