extends GutTest

# The foliage MODEL tier host (docs/foliage/foliage-re.md §The model tier;
# D-FOLIAGE-4): anchor depth gating, the per-tile cap, the derived
# transform-vs-libs-corner parity pin, and cache/stagger observability -
# all through the dispatcher's public seams (ADR 0018).

const NEAR_DEPTH_ANCHOR := Vector3(24.0, 0.0, -20.0)  # view depth 20 < 38
const FAR_DEPTH_ANCHOR := Vector3(24.0, 0.0, -100.0)  # view depth 100 >= 38

var _dispatcher: NovaFoliageDispatcher


func before_each() -> void:
	_dispatcher = NovaFoliageDispatcher.new()
	add_child_autofree(_dispatcher)

	var def := NovaTerrainFoliageDef.new()
	def.graphic = "test_model"
	def.match = 1

	var mesh := BoxMesh.new()
	# AABB (-1, -3, -1)..(1, 3, 1): bound center (0, 0), Chebyshev radius 1,
	# max Y 3 - a valid model-tier slot.
	mesh.size = Vector3(2.0, 6.0, 2.0)

	_dispatcher.dispatch_algorithm = NovaFoliageDispatcher.DISPATCH_ALGORITHM_CELL_GRID
	_dispatcher.cell_grid_radius = 0
	_dispatcher.foliage_defs = [def]
	_dispatcher.slot_meshes = [mesh]
	_dispatcher.height_sampler = Callable(self, "_sample_height")
	_dispatcher.foliage_sampler = Callable(self, "_sample_foliage_index")


# A sloped, non-planar height field so corner heights and folds carry signal.
func _sample_height(world_x: float, world_z: float) -> float:
	return 0.08 * world_x - 0.05 * world_z + 0.004 * world_x * world_z


func _sample_foliage_index(_world_x: float, _world_z: float) -> int:
	return 1  # everywhere painted with the def's match index


# Camera looking down -Z (Godot forward); anchors sit on -Z so the engine
# view depth is -view_local.z = -anchor.z. Slightly raised because the exact
# IDENTITY transform means "no camera supplied" to the dispatcher (the quad
# path's convention) and would bypass the depth gate.
func _camera_xform() -> Transform3D:
	return Transform3D(Basis(), Vector3(0.0, 1.5, 0.0))


func _dispatch(anchor: Vector3) -> void:
	_dispatcher.set_model_anchors(PackedVector3Array([anchor]))
	_dispatcher.dispatch(Vector3.ZERO, _camera_xform())


func test_depth_gate_blocks_near_anchors() -> void:
	_dispatch(NEAR_DEPTH_ANCHOR)
	var stats: Dictionary = _dispatcher.get_dispatch_stats()
	assert_eq(int(stats.model_instances), 0,
		"An anchor at view depth < 38 must stamp no model instances [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50].")
	assert_eq(_dispatcher.get_model_tile_debug(0).size(), 0,
		"No debug instances below the depth gate.")
	assert_null(_dispatcher.find_child("FoliageModelSlot0", false, false),
		"No model MultiMesh child below the depth gate.")


func test_deep_anchor_stamps_bounded_instances() -> void:
	_dispatch(FAR_DEPTH_ANCHOR)
	var stats: Dictionary = _dispatcher.get_dispatch_stats()
	assert_gt(int(stats.model_instances), 0,
		"An anchor at view depth >= 38 must stamp model instances.")
	assert_gt(int(stats.model_tiles_emitted), 0, "At least one tile draws.")
	assert_true(int(stats.model_instances) <= 21 * int(stats.model_tiles_emitted),
		"No tile may exceed the witnessed 21-instance cap.")

	var mmi := _dispatcher.find_child("FoliageModelSlot0", false, false) as MultiMeshInstance3D
	assert_not_null(mmi, "The model tier renders through its own MultiMesh child.")
	if mmi != null:
		assert_eq(mmi.multimesh.instance_count, int(stats.model_instances),
			"MultiMesh instance count matches the stats.")
	assert_eq(_dispatcher.get_model_tile_debug(0).size(), int(stats.model_instances),
		"The debug view lists every stamped instance.")


func test_transform_lands_on_libs_corner_positions() -> void:
	_dispatch(FAR_DEPTH_ANCHOR)
	var debug: Array = _dispatcher.get_model_tile_debug(0)
	assert_gt(debug.size(), 0, "Need instances for the parity pin.")
	if debug.is_empty():
		return

	# Mesh-corner -> engine-corner mapping derived with the transform
	# (nova_foliage_dispatcher.cpp _model_instance_transform): the 3DI->Godot
	# X negation makes mesh corner (sx, sz) land on engine corner
	# (A, B) = (-F*sx, +F*sz):
	#   (-1,-1) -> c1, (+1,-1) -> c0, (-1,+1) -> c3, (+1,+1) -> c2
	var mesh_corners := [Vector2(-1, -1), Vector2(1, -1), Vector2(-1, 1), Vector2(1, 1)]
	var engine_corner_for := [1, 0, 3, 2]

	for inst_value in debug:
		var inst: Dictionary = inst_value
		var xform: Transform3D = inst.transform
		var corners: PackedVector3Array = inst.corners
		var bc: Vector2 = inst.bound_center
		var radius: float = inst.bound_radius
		var hbase: float = inst.hbase
		var color: Color = inst.color
		var custom: Color = inst.custom

		for i in range(4):
			var sc: Vector2 = mesh_corners[i]
			var k: int = engine_corner_for[i]
			# The model-space footprint corner: bound center +- R on each axis
			# (the 0.75 scale in the transform lands it at +-0.75R world).
			var model_pt := Vector3(bc.x + sc.x * radius, 0.0, bc.y + sc.y * radius)
			var world_pt := xform * model_pt
			assert_almost_eq(world_pt.x, corners[k].x, 0.002,
				"Transformed mesh corner %d lands on libs corner %d world X" % [i, k])
			assert_almost_eq(world_pt.z, corners[k].z, 0.002,
				"Transformed mesh corner %d lands on libs corner %d world Z" % [i, k])
			assert_almost_eq(world_pt.y, hbase, 0.002,
				"The transform maps model y=0 onto hbase; the shader adds the delta.")

		# COLOR carries the corner height deltas in the shader corner order
		# (h1, h0, h3, h2) - together with the transform the corner heights close.
		assert_almost_eq(color.r, corners[1].y - hbase, 0.002, "COLOR.r = h1 - hbase")
		assert_almost_eq(color.g, corners[0].y - hbase, 0.002, "COLOR.g = h0 - hbase")
		assert_almost_eq(color.b, corners[3].y - hbase, 0.002, "COLOR.b = h3 - hbase")
		assert_almost_eq(color.a, corners[2].y - hbase, 0.002, "COLOR.a = h2 - hbase")

		# hbase = corner average + sag(0,0) = E_A + E_B (CUSTOM x/z carry E_A, E_B).
		var corner_avg := (corners[0].y + corners[1].y + corners[2].y + corners[3].y) * 0.25
		assert_almost_eq(hbase - corner_avg, custom.r + custom.b, 0.002,
			"hbase - corner average equals the sag-fold center E_A + E_B.")


func test_cache_stable_and_regen_on_stagger() -> void:
	_dispatch(FAR_DEPTH_ANCHOR)
	var first: Dictionary = _dispatcher.get_dispatch_stats()
	var first_debug: Array = _dispatcher.get_model_tile_debug(0)
	assert_gt(int(first.model_cache_misses), 0, "First dispatch generates (misses).")

	# 8 more dispatches: cached content stays bit-stable; hits accumulate;
	# regeneration fires exactly once per 8-frame window per slot
	# [orig: Foliage_UpdateModelTiles @ 0x601f50 - ((frame + 2*def) & 7) == 0].
	var misses_after_first := int(first.model_cache_misses)
	var regens_before := int(first.model_regenerations)
	for i in range(8):
		_dispatch(FAR_DEPTH_ANCHOR)
	var after: Dictionary = _dispatcher.get_dispatch_stats()
	assert_eq(int(after.model_cache_misses), misses_after_first,
		"Re-dispatching the same anchor never misses again.")
	assert_gt(int(after.model_cache_hits), 0, "Re-dispatches hit the tile cache.")
	var tiles := int(after.model_tiles_emitted)
	assert_eq(int(after.model_regenerations) - regens_before, int(first.model_cache_misses),
		"Exactly one regeneration per cached tile across an 8-frame window.")

	var after_debug: Array = _dispatcher.get_model_tile_debug(0)
	assert_eq(after_debug.size(), first_debug.size(), "Instance count is stable across frames.")
	for i in range(min(after_debug.size(), first_debug.size())):
		assert_eq(after_debug[i].center, first_debug[i].center,
			"Instance placements are bit-stable across cached frames (same anchor).")
	assert_gt(tiles, 0, "Tiles keep drawing from the cache.")


func test_no_anchors_clears_model_tier() -> void:
	_dispatch(FAR_DEPTH_ANCHOR)
	assert_gt(_dispatcher.get_model_tile_debug(0).size(), 0, "Anchored dispatch stamps.")
	_dispatcher.set_model_anchors(PackedVector3Array())
	_dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	assert_eq(_dispatcher.get_model_tile_debug(0).size(), 0,
		"Dropping every anchor clears the stamped model instances.")
