extends GutTest

# DebugTerrainPage: the Terrain & foliage debug page. Renders canned counter
# dictionaries from duck-typed stub worlds, keeps its knobs mirroring the live
# terrain node, and degrades to empty states without a world.

const PageScript := preload("res://engine/debug/pages/debug_terrain_page.gd")


class StubTerrain:
	extends Node
	var mode := 0
	var quality := 1.0

	func get_patches_active() -> int:
		return 12

	func get_visible_patch_count() -> int:
		return 7

	func get_lod_distribution() -> PackedInt32Array:
		return PackedInt32Array([3, 4, 0, 0, 0, 0, 0, 0])

	func get_traversal_stats() -> Dictionary:
		return {"nodes_visited": 500, "rej_nearfar": 20, "rej_left": 1,
				"rej_right": 2, "rej_bottom": 3, "rej_top": 4,
				"partial_subdiv": 5, "budget_drops": 0, "leaf_emits": 60,
				"nonleaf_emits": 6, "dist_min": 0.0, "dist_max": 900.0,
				"lod_fallbacks": 1}

	func get_debug_mode() -> int:
		return mode

	func set_debug_mode(value: int) -> void:
		mode = value

	func get_lod_quality() -> float:
		return quality

	func set_lod_quality(value: float) -> void:
		quality = value


class StubDispatcher:
	extends Node

	func get_total_instances() -> int:
		return 4200

	func get_frame_stats() -> Dictionary:
		return {"detail_cells": 9, "detail_high_instances": 100,
				"detail_low_instances": 200, "silhouette_instances": 30,
				"render_batches": 14, "detail_cache_hits": 80,
				"detail_cache_misses": 3, "detail_cache_residents": 24,
				"detail_cache_evictions": 2}


class StubWorld:
	extends Node
	var terrain: Node = null
	var dispatcher: Node = null

	func get_terrain_node() -> Node:
		return terrain

	func get_foliage_dispatcher() -> Node:
		return dispatcher


func _make_page(world: Node = null) -> DebugTerrainPage:
	var ctx := NovaDebugContext.new()
	ctx.options = NovaDebugOptionState.new()
	ctx.world_source = func(): return world
	ctx.session = NovaDebugSession.new()
	NovaDebugCatalog.install(ctx.session)
	NovaDebugCatalog.bind_runtime_targets(
			ctx.session, func(): return null, ctx.world_source)
	ctx.session.set_presented(true)
	var page: DebugTerrainPage = PageScript.new()
	page.setup(ctx)
	add_child_autofree(page)
	return page


func _make_world() -> StubWorld:
	var world := StubWorld.new()
	world.terrain = StubTerrain.new()
	world.dispatcher = StubDispatcher.new()
	world.add_child(world.terrain)
	world.add_child(world.dispatcher)
	add_child_autofree(world)
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


func test_formats_the_canned_counters() -> void:
	var page := _make_page(_make_world())
	page.refresh()
	var patches := (page.find_child("TerrainPatches", true, false) as Label).text
	assert_string_contains(patches, "12 active")
	assert_string_contains(patches, "7 visible")
	assert_string_contains(patches, "L0 3")
	assert_string_contains(patches, "L1 4")
	var traversal := (page.find_child("TerrainTraversal", true, false) as Label).text
	assert_string_contains(traversal, "500 nodes")
	assert_string_contains(traversal, "60 leaf")
	var foliage := (page.find_child("FoliageStats", true, false) as Label).text
	assert_string_contains(foliage, "4200 placed")
	assert_string_contains(foliage, "batches 14")
	assert_string_contains(foliage, "80 hits")


func test_knobs_poke_the_live_terrain_and_mirror_it_back() -> void:
	var world := _make_world()
	var terrain := world.terrain as StubTerrain
	var page := _make_page(world)
	page.refresh()

	var mode := page.find_child("TerrainDrawMode", true, false) as OptionButton
	assert_eq(mode.item_count, 5, "all five terrain draw modes are offered")
	assert_false(mode.disabled)
	mode.item_selected.emit(1)
	assert_eq(terrain.mode, 1, "picking a mode uses the shared public control")

	var slider := page.find_child("TerrainDetail", true, false) as HSlider
	assert_true(slider.editable)
	slider.value = 2.0
	assert_almost_eq(terrain.quality, 2.0, 0.001,
			"the detail slider uses the shared public control")

	terrain.mode = 3
	terrain.quality = 0.5
	page.refresh()
	assert_eq(mode.selected, 3, "refresh mirrors an externally-poked mode")
	assert_almost_eq(float(slider.value), 0.5, 0.001,
			"refresh mirrors an externally-poked detail without re-firing")
	assert_almost_eq(terrain.quality, 0.5, 0.001)


func test_hide_foliage_rides_the_option_registry() -> void:
	var page := _make_page()
	var check := page.find_child("hide_foliage", true, false) as CheckBox
	assert_not_null(check, "the page carries the registry checkbox")
	assert_false(check.button_pressed, "it defaults off")
