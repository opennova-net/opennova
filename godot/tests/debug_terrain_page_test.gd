extends GutTest

# DebugTerrainPage: the Terrain & foliage debug page. Reads the typed
# GameWorld -> Terrain/FoliageDispatcher seams (real native nodes — an idle
# terrain formats its zero counters), keeps its knobs mirroring the live
# terrain node, and degrades to empty states without a world.

const PageScript := preload("res://game/debug/pages/debug_terrain_page.gd")


class WorldHarness:
	extends GameWorld
	var terrain: Terrain = null
	var dispatcher: FoliageDispatcher = null

	func get_terrain_node() -> Terrain:
		return terrain

	func get_foliage_dispatcher() -> FoliageDispatcher:
		return dispatcher


func _make_page(world: GameWorld = null) -> DebugTerrainPage:
	var ctx := DebugContext.new()
	ctx.options = DebugOptionState.new()
	ctx.world_source = func(): return world
	ctx.session = DebugSession.new()
	DebugCatalog.install(ctx.session)
	DebugCatalog.bind_runtime_targets(
			ctx.session, func(): return null, ctx.world_source)
	ctx.session.set_presented(true)
	var page: DebugTerrainPage = PageScript.new()
	page.setup(ctx)
	add_child_autofree(page)
	return page


func _make_world() -> WorldHarness:
	# Off-tree: the GameWorld script class alone has no scene children, and
	# the harness getters hand out direct refs.
	var world := WorldHarness.new()
	world.terrain = Terrain.new()
	world.dispatcher = FoliageDispatcher.new()
	world.add_child(world.terrain)
	world.add_child(world.dispatcher)
	autofree(world)
	return world


func test_renders_empty_states_without_a_world() -> void:
	var page := _make_page()
	page.refresh()
	assert_string_contains((page.find_child("TerrainPatches", true, false) as Label).text,
			"No terrain")
	assert_string_contains((page.find_child("FoliageStats", true, false) as Label).text,
			"No foliage")
	assert_true((page.find_child(
			"TerrainDrawMode", true, false) as OptionButton).disabled,
			"draw mode is unavailable without a live terrain target")
	assert_false((page.find_child(
			"TerrainDetail", true, false) as HSlider).editable,
			"terrain detail is unavailable without a live terrain target")


func test_formats_the_live_terrain_counters() -> void:
	# An idle (no .trn) terrain still serves the full counter surface; the
	# page formats its real zeros rather than blanking the pane.
	var page := _make_page(_make_world())
	page.refresh()
	var patches := (page.find_child("TerrainPatches", true, false) as Label).text
	assert_string_contains(patches, "0 active")
	assert_string_contains(patches, "0 visible")
	assert_string_contains(patches, "Detail split: -",
			"an idle terrain reports no split rather than empty LOD bits")
	var traversal := (page.find_child("TerrainTraversal", true, false) as Label).text
	assert_string_contains(traversal, "0 nodes")
	assert_string_contains(traversal, "0 leaf")
	var foliage := (page.find_child("FoliageStats", true, false) as Label).text
	assert_string_contains(foliage, "0 placed")
	assert_string_contains(foliage, "batches 0")
	assert_string_contains(foliage, "0 hits")


func test_knobs_poke_the_live_terrain_and_mirror_it_back() -> void:
	var world := _make_world()
	var terrain := world.terrain
	var page := _make_page(world)
	page.refresh()

	var mode := page.find_child("TerrainDrawMode", true, false) as OptionButton
	assert_eq(mode.item_count, 5, "all five terrain draw modes are offered")
	assert_false(mode.disabled)
	mode.item_selected.emit(1)
	assert_eq(int(terrain.get_debug_mode()), 1,
			"picking a mode uses the shared public control")

	var slider := page.find_child("TerrainDetail", true, false) as HSlider
	assert_true(slider.editable)
	slider.value = 2.0
	assert_almost_eq(float(terrain.get_lod_quality()), 2.0, 0.001,
			"the detail slider uses the shared public control")

	terrain.set_debug_mode(3)
	terrain.set_lod_quality(0.5)
	page.refresh()
	assert_eq(mode.selected, 3, "refresh mirrors an externally-poked mode")
	assert_almost_eq(float(slider.value), 0.5, 0.001,
			"refresh mirrors an externally-poked detail without re-firing")
	assert_almost_eq(float(terrain.get_lod_quality()), 0.5, 0.001)


func test_hide_foliage_rides_the_option_registry() -> void:
	var page := _make_page()
	var check := page.find_child("hide_foliage", true, false) as CheckBox
	assert_not_null(check, "the page carries the registry checkbox")
	assert_false(check.button_pressed, "it defaults off")
