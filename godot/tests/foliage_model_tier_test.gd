extends GutTest

# The two-tier foliage host (docs/foliage/foliage-re.md §The model tier;
# D-FOLIAGE-4): anchor depth gating, the per-tile cap, the derived
# transform-vs-libs-corner parity pin (model tier), the far-tier
# full-source-mesh emission pin, and cache/stagger observability - all
# through the dispatcher's public seams (ADR 0018).

const NEAR_DEPTH_ANCHOR := Vector3(24.0, 0.0, -20.0)  # view depth 20 < 38
const FAR_DEPTH_ANCHOR := Vector3(24.0, 0.0, -100.0)  # view depth 100 >= 38

var _dispatcher: NovaFoliageDispatcher
var _split_far_slot_mask := 0
var _split_foliage_index := 0
var _split_far_slot_samples: Array[Vector2] = []
var _owned_dispatchers: Array[NovaFoliageDispatcher] = []


func before_each() -> void:
	_owned_dispatchers.clear()
	_dispatcher = NovaFoliageDispatcher.new()
	_own_dispatcher(_dispatcher)

	var def := NovaTerrainFoliageDef.new()
	def.graphic = "test_model"
	def.match = 1

	var mesh := BoxMesh.new()
	# AABB (-1, -3, -1)..(1, 3, 1): bound center (0, 0), Chebyshev radius 1,
	# max Y 3 - a valid model-tier slot.
	mesh.size = Vector3(2.0, 6.0, 2.0)

	_dispatcher.foliage_defs = [def]
	_dispatcher.slot_meshes = [mesh]
	_dispatcher.height_sampler = Callable(self, "_sample_height")
	_dispatcher.foliage_sampler = Callable(self, "_sample_foliage_index")
	_dispatcher.far_slot_mask_sampler = Callable(self, "_sample_far_slot_mask")


func after_each() -> void:
	# Dispatcher reset intentionally detaches renderer nodes synchronously and
	# queues their destruction. Flush that queue before GUT checks for orphans.
	for dispatcher in _owned_dispatchers:
		if is_instance_valid(dispatcher):
			dispatcher.reset()
	await get_tree().process_frame


func _own_dispatcher(dispatcher: NovaFoliageDispatcher) -> void:
	add_child_autofree(dispatcher)
	_owned_dispatchers.append(dispatcher)


# A sloped, non-planar height field so corner heights and folds carry signal.
func _sample_flat_height(_world_x: float, _world_z: float) -> float:
	return 0.0


func _sample_height(world_x: float, world_z: float) -> float:
	return 0.08 * world_x - 0.05 * world_z + 0.004 * world_x * world_z


func _sample_steep_cell_height(world_x: float, _world_z: float) -> float:
	# In cell key 16 (x=[0,16], z=[0,16]), the center is y=80 while the
	# footprint spans y=[0,160]. A camera at y=0 is inside the leaf Y range.
	return world_x * 10.0


func _sample_foliage_index(_world_x: float, _world_z: float) -> int:
	return 1  # everywhere painted with the def's match index


func _sample_far_slot_mask(_world_x: float, _native_z: float) -> int:
	return 1  # FOLIAGEMAP index already match-remapped to the slot-0 bit


func _sample_split_far_slot_mask(world_x: float, native_z: float) -> int:
	_split_far_slot_samples.append(Vector2(world_x, native_z))
	return _split_far_slot_mask


func _sample_split_foliage_index(_world_x: float, _world_z: float) -> int:
	return _split_foliage_index


func _make_far_dispatcher() -> NovaFoliageDispatcher:
	var dispatcher := NovaFoliageDispatcher.new()
	_own_dispatcher(dispatcher)
	var def := NovaTerrainFoliageDef.new()
	def.graphic = "test_far_pool"
	def.match = 1
	var mesh := BoxMesh.new()
	mesh.size = Vector3(2.0, 6.0, 2.0)
	dispatcher.foliage_defs = [def]
	dispatcher.slot_meshes = [mesh]
	dispatcher.height_sampler = Callable(self, "_sample_height")
	dispatcher.foliage_sampler = Callable(self, "_sample_foliage_index")
	dispatcher.far_slot_mask_sampler = Callable(self, "_sample_far_slot_mask")
	return dispatcher


# The cell that contains the origin camera: key = pack(0, 0) = 0 (a cell
# keyed (kx, kz) covers x in [kx, kx+16], z in [kz, kz+16] - both candidate
# axes ADD from the unbiased cell base).
const ORIGIN_CELL_KEY := 0


func _far_cell_node(dispatcher: NovaFoliageDispatcher, slot: int, key: int) -> MeshInstance3D:
	return dispatcher.find_child("FarCell%d_%d" % [slot, key], false, false) as MeshInstance3D


func _visible_model_draw_nodes(dispatcher: NovaFoliageDispatcher) -> Array:
	var out: Array = []
	for child in dispatcher.get_children():
		if child is MultiMeshInstance3D and child.visible 				and String(child.name).begins_with("FoliageModelDraw"):
			out.push_back(child)
	return out


# Camera looking down -Z (Godot forward); anchors sit on -Z so the engine
# view depth is -view_local.z = -anchor.z. Slightly raised because the exact
# IDENTITY transform means "no camera supplied" to the dispatcher (the FAR
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
	assert_eq(_visible_model_draw_nodes(_dispatcher).size(), 0,
		"No visible model draw batch below the depth gate.")


func test_deep_anchor_stamps_bounded_instances() -> void:
	_dispatch(FAR_DEPTH_ANCHOR)
	var stats: Dictionary = _dispatcher.get_dispatch_stats()
	assert_gt(int(stats.model_instances), 0,
		"An anchor at view depth >= 38 must stamp model instances.")
	assert_gt(int(stats.model_tiles_emitted), 0, "At least one tile draws.")
	assert_true(int(stats.model_instances) <= 21 * int(stats.model_tiles_emitted),
		"No tile may exceed the witnessed 21-instance cap.")

	var model_nodes: Array = _visible_model_draw_nodes(_dispatcher)
	assert_eq(model_nodes.size(), int(stats.model_tiles_emitted),
		"The model tier renders one MultiMesh batch per tile draw.")
	var rendered_instances := 0
	for node in model_nodes:
		rendered_instances += (node as MultiMeshInstance3D).multimesh.instance_count
	assert_eq(rendered_instances, int(stats.model_instances),
		"Per-tile MultiMesh instance counts sum to the model stats.")
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
	# source-model orientation rotY(yaw + PI/2) makes mesh corner (sx, sz)
	# land on engine fit axes (A, B) = (F*sz, F*sx):
	#   (-1,-1) -> c0, (+1,-1) -> c2, (-1,+1) -> c1, (+1,+1) -> c3
	var mesh_corners := [Vector2(-1, -1), Vector2(1, -1), Vector2(-1, 1), Vector2(1, 1)]
	var engine_corner_for := [0, 2, 1, 3]

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

		# COLOR carries the corner height deltas in source-mesh corner order
		# (h0, h2, h1, h3) - together with the transform the corner heights close.
		assert_almost_eq(color.r, corners[0].y - hbase, 0.002, "COLOR.r = h0 - hbase")
		assert_almost_eq(color.g, corners[2].y - hbase, 0.002, "COLOR.g = h2 - hbase")
		assert_almost_eq(color.b, corners[1].y - hbase, 0.002, "COLOR.b = h1 - hbase")
		assert_almost_eq(color.a, corners[3].y - hbase, 0.002, "COLOR.a = h3 - hbase")

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


func test_anchorless_frames_still_advance_model_stagger_clock() -> void:
	_dispatch(FAR_DEPTH_ANCHOR) # frame 1: populate slot-0's four tiles
	var first: Dictionary = _dispatcher.get_dispatch_stats()
	var regens_before := int(first.model_regenerations)

	# Retail's frame counter is global, not conditional on there being a
	# qualifying sector-entity anchor. Six empty frames take us through frame 7;
	# restoring the same anchor on frame 8 must hit slot 0's stagger phase.
	_dispatcher.set_model_anchors(PackedVector3Array())
	for i in range(6):
		_dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	_dispatch(FAR_DEPTH_ANCHOR)

	var after: Dictionary = _dispatcher.get_dispatch_stats()
	assert_eq(int(after.model_regenerations) - regens_before,
		int(first.model_cache_misses),
		"Anchorless render frames advance the witnessed ((frame + 2*slot) & 7) MODEL stagger.")


func test_uploads_move_only_with_regenerated_content() -> void:
	_dispatch(FAR_DEPTH_ANCHOR)
	var prev: Dictionary = _dispatcher.get_dispatch_stats()
	assert_gt(int(prev.model_uploads), 0, "The initial bake uploads its tiles.")
	# Across an 8-frame window, MultiMesh uploads move ONLY on frames whose
	# stagger regenerated content (the generation stamp changed); pure-hit
	# frames re-use the retained buffers — the 76k-uploads/frame churn the
	# keyed draw-node pool replaced (D-FOLIAGE-11 host mechanics).
	for i in range(8):
		_dispatch(FAR_DEPTH_ANCHOR)
		var cur: Dictionary = _dispatcher.get_dispatch_stats()
		var upload_delta := int(cur.model_uploads) - int(prev.model_uploads)
		var regen_delta := int(cur.model_regenerations) - int(prev.model_regenerations)
		if regen_delta == 0:
			assert_eq(upload_delta, 0, "Pure-hit frames re-upload nothing.")
		else:
			assert_gt(upload_delta, 0, "Stagger-regenerated content re-uploads.")
		prev = cur


func test_no_anchors_clears_model_tier() -> void:
	_dispatch(FAR_DEPTH_ANCHOR)
	assert_gt(_dispatcher.get_model_tile_debug(0).size(), 0, "Anchored dispatch stamps.")
	_dispatcher.set_model_anchors(PackedVector3Array())
	_dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	await get_tree().process_frame
	assert_eq(_dispatcher.get_model_tile_debug(0).size(), 0,
		"Dropping every anchor clears the stamped model instances.")


func test_far_tier_replicates_full_source_mesh_and_bends_each_vertex() -> void:
	# [orig: generate_foliage_instances_0 @ 0x5ffdd0] FAR copies every source
	# vertex for every accepted placement, baked once per 16u cell into the
	# persistent pool [orig: Foliage_UpdateFarCellSlots @ 0x601b30].
	_dispatch(FAR_DEPTH_ANCHOR)
	var mi := _far_cell_node(_dispatcher, 0, ORIGIN_CELL_KEY)
	assert_not_null(mi, "The camera cell owns one baked far-cell mesh node.")
	if mi == null:
		return
	assert_true(mi.visible, "A collected cell's node is visible.")

	var debug: Array = _dispatcher.get_far_tile_debug(0)
	assert_gt(debug.size(), 0, "The 42u collect emits placements.")
	assert_eq(debug.size() % 36, 0,
		"Permissive FAR cells accept all 36 candidates each.")
	var cell_placements: Array = []
	for placement: Dictionary in debug:
		if int(placement.cell_key) == ORIGIN_CELL_KEY:
			cell_placements.push_back(placement)
	assert_eq(cell_placements.size(), 36,
		"The camera cell accepts all 36 candidates under permissive samplers.")
	if cell_placements.is_empty():
		return

	var source_mesh := _dispatcher.slot_meshes[0] as Mesh
	var source_arrays := source_mesh.surface_get_arrays(0)
	var source_positions: PackedVector3Array = source_arrays[Mesh.ARRAY_VERTEX]
	var source_uvs: PackedVector2Array = source_arrays[Mesh.ARRAY_TEX_UV]
	var source_indices: PackedInt32Array = source_arrays[Mesh.ARRAY_INDEX]

	var emitted_arrays := mi.mesh.surface_get_arrays(0)
	var emitted_positions: PackedVector3Array = emitted_arrays[Mesh.ARRAY_VERTEX]
	var emitted_colors: PackedColorArray = emitted_arrays[Mesh.ARRAY_COLOR]
	var emitted_uvs: PackedVector2Array = emitted_arrays[Mesh.ARRAY_TEX_UV]
	var emitted_indices: PackedInt32Array = emitted_arrays[Mesh.ARRAY_INDEX]
	assert_eq(emitted_positions.size(), source_positions.size() * cell_placements.size(),
		"Every placement in the cell receives the complete authored source vertex array.")
	assert_eq(emitted_colors.size(), emitted_positions.size(),
		"Every emitted vertex carries wind weight plus the underlying terrain normal.")
	assert_eq(emitted_uvs.size(), emitted_positions.size(), "Source UVs are replicated.")
	assert_eq(emitted_indices.size(), source_indices.size() * cell_placements.size(),
		"Source topology is replicated for every placement.")

	for placement_index in range(mini(cell_placements.size(), 3)):
		var placement: Dictionary = cell_placements[placement_index]
		var center: Vector3 = placement.center
		var yaw: float = placement.yaw
		for source_index in range(source_positions.size()):
			var source: Vector3 = source_positions[source_index]
			var emitted_index := placement_index * source_positions.size() + source_index
			var emitted: Vector3 = emitted_positions[emitted_index]
			# Witnessed native transform X' = x*cos - z*sin, Z' = x*sin + z*cos
			# [orig: generate_foliage_instances_0 @ 0x600112..0x60014d],
			# composed with the importer's -X mesh flip and world z = -native z.
			var expected_x := center.x - (source.x * cos(yaw) + source.z * sin(yaw))
			var expected_z := center.z + (source.x * sin(yaw) - source.z * cos(yaw))
			var expected_y := _sample_height(expected_x, expected_z) + source.y * 0.5
			assert_almost_eq(emitted.x, expected_x, 0.002, "FAR vertex X uses the witnessed transform.")
			assert_almost_eq(emitted.z, expected_z, 0.002, "FAR vertex Z uses the witnessed transform.")
			assert_almost_eq(emitted.y, expected_y, 0.002,
				"Each source vertex samples terrain independently and keeps Y scale 0.5.")
			assert_eq(emitted_uvs[emitted_index], source_uvs[source_index],
				"Source UV is preserved.")
			var wind_byte := clampi(int(source.y * 128.0), 0, 255)
			assert_almost_eq(emitted_colors[emitted_index].r,
				float(wind_byte) / 255.0, 0.00001,
				"COLOR.r is clamp(trunc(sourceY*128),0,255), not terrain tint.")
			var ground_normal := Vector3(
				_sample_height(expected_x - 1.0, expected_z) - _sample_height(expected_x + 1.0, expected_z),
				2.0,
				_sample_height(expected_x, expected_z - 1.0) - _sample_height(expected_x, expected_z + 1.0)
			).normalized()
			var encoded_normal := ground_normal * 0.5 + Vector3(0.5, 0.5, 0.5)
			var packed_normal := Vector3(
				floorf(encoded_normal.x * 255.0) / 255.0,
				floorf(encoded_normal.y * 255.0) / 255.0,
				floorf(encoded_normal.z * 255.0) / 255.0
			)
			assert_almost_eq(emitted_colors[emitted_index].g,
				packed_normal.x, 0.00001,
				"COLOR.g encodes the underlying terrain-normal X channel.")
			assert_almost_eq(emitted_colors[emitted_index].b,
				packed_normal.y, 0.00001,
				"COLOR.b encodes the underlying terrain-normal Y channel.")
			assert_almost_eq(emitted_colors[emitted_index].a,
				packed_normal.z, 0.00001,
				"COLOR.a encodes the underlying terrain-normal Z channel.")

	if not source_indices.is_empty() and cell_placements.size() > 1:
		var vertex_stride := source_positions.size()
		var index_stride := source_indices.size()
		for i in range(index_stride):
			assert_eq(emitted_indices[index_stride + i], source_indices[i] + vertex_stride,
				"The next full-mesh copy offsets source indices by one vertex stride.")

	var material := mi.material_override as ShaderMaterial
	assert_not_null(material, "The FAR cells share the slot's FAR shader material.")
	if material != null:
		assert_true(material.shader.resource_path.ends_with("foliage_far.gdshader"),
			"FAR does not share the MODEL ground-fit shader.")
	# The camera cell sits well inside 20u: full fade, high pass
	# [orig: render_terrain_lightmaps @ 0x60a171 / 0x60a45d].
	assert_almost_eq(float(mi.get_instance_shader_parameter("u_cell_fade")), 1.0, 0.000001,
		"Cells inside distance 20 draw with c6.a = 1.")
	assert_almost_eq(float(mi.get_instance_shader_parameter("u_cell_alpha_ref")), 180.0, 0.000001,
		"Cells under distance 33 draw the high pass (alpha-test ref 180).")


func test_far_pool_evicts_stalest_at_the_128_cap() -> void:
	# The pool cap is the witnessed 128 [orig: Foliage_UpdateFarCellSlots
	# @ 0x601b30]. Fill past it from five disjoint views (~33 cells each):
	# residency never exceeds the cap and a return to the first view re-bakes
	# what was evicted (bake-once holds for RESIDENT keys only).
	var dispatcher := _make_far_dispatcher()
	# Flat ground for THIS test: the shared saddle mock steepens with |x| and
	# the collect metric is 3D (camera_y - height counts against the 42u
	# sphere), which would starve the distant views below the cap.
	dispatcher.height_sampler = Callable(self, "_sample_flat_height")
	dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var first: Dictionary = dispatcher.get_dispatch_stats()
	var first_baked := int(first.far_cells_baked)
	assert_gt(first_baked, 0, "The first view bakes its cells.")
	for x in [200.0, 400.0, 600.0, 800.0]:
		dispatcher.dispatch(Vector3(x, 0, 0), _camera_xform())
	var crowded: Dictionary = dispatcher.get_dispatch_stats()
	assert_gt(int(crowded.far_cells_baked), first_baked * 3,
		"Disjoint views keep baking new cells (the fill crossed the cap).")
	assert_lte(int(crowded.cached_cells), 128,
		"Pool residency never exceeds the witnessed 128 cap.")
	var baked_before_return := int(crowded.far_cells_baked)
	dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var returned: Dictionary = dispatcher.get_dispatch_stats()
	assert_gt(int(returned.far_cells_baked), baked_before_return,
		"Evicted cells re-bake when their view returns.")
	assert_lte(int(returned.cached_cells), 128, "Residency stays capped after the return.")


func test_far_collect_clamps_camera_to_steep_cell_height_range() -> void:
	# Retail measures distance to the leaf AABB, including a clamped Y term.
	# The target cell's center is 80u above the camera, so the former
	# center-only approximation rejected it despite its low edge meeting y=0.
	var dispatcher := _make_far_dispatcher()
	dispatcher.height_sampler = Callable(self, "_sample_steep_cell_height")
	dispatcher.dispatch(Vector3(8.0, 0.0, 8.0), _camera_xform())

	var node := _far_cell_node(dispatcher, 0, ORIGIN_CELL_KEY)
	assert_not_null(node,
		"A steep cell is collected when the camera lies inside its footprint height range.")
	var found_target := false
	for placement: Dictionary in dispatcher.get_far_tile_debug(0):
		if int(placement.cell_key) != ORIGIN_CELL_KEY:
			continue
		found_target = true
		assert_almost_eq(float(placement.distance), 0.0, 0.000001,
			"Clamping camera Y to the leaf range contributes zero vertical distance.")
		break
	assert_true(found_target, "The steep target cell exposes its collected draw state.")


func test_far_pool_bakes_once_fades_by_distance_and_survives_reset() -> void:
	# The witnessed slot-pool mechanics [orig: Foliage_UpdateFarCellSlots
	# @ 0x601b30]: resident keys are only re-stamped; per-cell fade/pass state
	# tracks the camera distance [orig: render_terrain_lightmaps @ 0x60a171].
	var dispatcher := _make_far_dispatcher()
	dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var first: Dictionary = dispatcher.get_dispatch_stats()
	assert_gt(int(first.far_cells_visible), 0, "The 42u disc collects cells.")
	assert_eq(int(first.far_pool_misses), int(first.far_cells_visible),
		"Every collected cell bakes exactly once on first sight.")
	assert_eq(int(first.far_cells_baked), int(first.far_pool_misses),
		"Bake count matches pool misses.")

	dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var second: Dictionary = dispatcher.get_dispatch_stats()
	assert_eq(int(second.far_cells_baked), int(first.far_cells_baked),
		"A steady frame re-bakes nothing - the pool serves every cell.")
	assert_gt(int(second.far_pool_hits), 0, "Steady frames are pure pool hits.")

	# Distance bands: fade 1 through 20; the low pass (ref 8) beyond 33.
	var saw_full_fade := false
	var saw_partial_fade := false
	var saw_low_pass := false
	for placement: Dictionary in dispatcher.get_far_tile_debug(0):
		var distance := float(placement.distance)
		var fade := float(placement.fade)
		var ref := float(placement.alpha_ref)
		assert_true(distance <= 42.0 + 0.001, "No placement comes from beyond the 42u collect gate.")
		if distance <= 20.0:
			assert_almost_eq(fade, 1.0, 0.000001, "c6.a = 1 through distance 20.")
			saw_full_fade = true
		else:
			assert_almost_eq(fade, 1.0 - (distance - 20.0) / 22.0, 0.0001,
				"c6.a = 1 - (d - 20)/22 beyond the knee.")
			saw_partial_fade = true
		if distance < 33.0:
			assert_almost_eq(ref, 180.0, 0.000001, "High pass under 33.")
		else:
			assert_almost_eq(ref, 8.0, 0.000001, "Low pass at 33 and beyond.")
			saw_low_pass = true
	assert_true(saw_full_fade, "The disc contains full-fade cells.")
	assert_true(saw_partial_fade, "The disc contains fading cells past 20u.")
	assert_true(saw_low_pass, "The disc contains low-pass cells past 33u.")

	# Leaving the disc hides the cells but keeps them pooled; returning is
	# hit-only.
	dispatcher.dispatch(Vector3(200.0, 0.0, 200.0), _camera_xform())
	var away: Dictionary = dispatcher.get_dispatch_stats()
	var origin_node := _far_cell_node(dispatcher, 0, ORIGIN_CELL_KEY)
	assert_not_null(origin_node, "Out-of-range cells stay pooled.")
	if origin_node != null:
		assert_false(origin_node.visible, "Out-of-range cells are hidden, not freed.")
	dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var back: Dictionary = dispatcher.get_dispatch_stats()
	assert_eq(int(back.far_cells_baked), int(away.far_cells_baked),
		"Returning to pooled cells re-bakes nothing.")

	dispatcher.reset()
	assert_eq(dispatcher.get_cached_cells(), 0, "Reset drops the pool.")


func test_colormap_swap_rebinds_resident_far_pool_material() -> void:
	var dispatcher := _make_far_dispatcher()
	var source_a := NovaTerrainData.new()
	var source_b := NovaTerrainData.new()
	var image_a := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	var image_b := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	image_a.fill(Color.RED)
	image_b.fill(Color.BLUE)
	var texture_a := ImageTexture.create_from_image(image_a)
	var texture_b := ImageTexture.create_from_image(image_b)
	source_a.colormap = texture_a
	source_b.colormap = texture_b

	dispatcher.colormap_source = source_a
	dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var node := _far_cell_node(dispatcher, 0, ORIGIN_CELL_KEY)
	assert_not_null(node, "The stable camera view bakes a resident FAR node.")
	if node == null:
		return
	var material_a := node.material_override as ShaderMaterial
	assert_not_null(material_a)
	assert_same(material_a.get_shader_parameter("u_terrain_light_texture"), texture_a)
	var baked_before := int(dispatcher.get_dispatch_stats().far_cells_baked)

	dispatcher.colormap_source = source_b
	dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var material_b := node.material_override as ShaderMaterial
	assert_eq(int(dispatcher.get_dispatch_stats().far_cells_baked), baked_before,
		"A colormap-only swap keeps the bake-once placement pool resident.")
	assert_ne(material_b, material_a,
		"The resident node adopts a recreated shared material after the source swap.")
	assert_same(material_b.get_shader_parameter("u_terrain_light_texture"), texture_b,
		"The recreated material binds the new terrain-light/colormap source.")


func test_far_only_requires_height_plus_slot_mask_sampler() -> void:
	var dispatcher := _make_far_dispatcher()
	dispatcher.foliage_sampler = Callable()
	dispatcher.dispatch(Vector3(8.0, 0.0, -8.0), _camera_xform())
	assert_gt(dispatcher.get_far_tile_debug(0).size(), 0,
		"FAR only requires height plus its match-remapped FOLIAGEMAP slot-mask sampler.")


func test_reset_detaches_far_nodes_before_queue_free() -> void:
	var dispatcher := _make_far_dispatcher()
	dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var far_node := _far_cell_node(dispatcher, 0, ORIGIN_CELL_KEY)
	assert_not_null(far_node, "The fixture bakes the camera cell.")
	if far_node == null:
		return

	dispatcher.reset()
	assert_null(_far_cell_node(dispatcher, 0, ORIGIN_CELL_KEY),
		"Reset removes the far cell nodes from the scene tree synchronously.")
	assert_null(far_node.get_parent(),
		"The queued far cell node is detached before deferred destruction.")


func test_shared_map_gate_feeds_both_tiers() -> void:
	# BOTH tiers gate on the same FOLIAGEMAP texel at the candidate's own
	# world position: retail's two samplers read the same load-remapped
	# buffer [orig: Foliage_SampleFoliageMapMask @ 0x606620 (MODEL);
	# Foliage_SampleFarMapMask @ 0x6066d0 over source-space FAR keys]. The
	# byte-returning foliage_sampler seam feeds both; the mask-returning
	# far_slot_mask_sampler is the fallback when no byte source is bound.
	var shared := NovaFoliageDispatcher.new()
	_own_dispatcher(shared)
	var def := NovaTerrainFoliageDef.new()
	def.graphic = "test_model"
	def.match = 7
	var mesh := BoxMesh.new()
	mesh.size = Vector3(2.0, 6.0, 2.0)
	shared.foliage_defs = [def]
	shared.slot_meshes = [mesh]
	shared.height_sampler = Callable(self, "_sample_height")
	shared.foliage_sampler = Callable(self, "_sample_split_foliage_index")
	shared.model_anchors = PackedVector3Array([FAR_DEPTH_ANCHOR])

	# Painted byte matches the def -> BOTH tiers accept.
	_split_foliage_index = 7
	shared.dispatch(Vector3.ZERO, _camera_xform())
	var far_debug: Array = shared.get_far_tile_debug(0)
	assert_gt(far_debug.size(), 0, "A def-matching painted byte accepts FAR.")
	assert_eq(far_debug.size() % 36, 0,
		"Permissive painted texels accept all 36 candidates per collected cell.")
	assert_gt(shared.get_model_tile_debug(0).size(), 0,
		"The same painted byte accepts MODEL through the same gate.")

	# Unpainted byte -> BOTH tiers reject: one map, one gate.
	_split_foliage_index = 0
	shared.reset()
	shared.dispatch(Vector3.ZERO, _camera_xform())
	assert_eq(shared.get_far_tile_debug(0).size(), 0,
		"An unpainted texel rejects FAR through the shared gate.")
	assert_eq(shared.get_model_tile_debug(0).size(), 0,
		"An unpainted texel rejects MODEL through the shared gate.")

	# Without a byte source, the pre-remapped mask seam gates both tiers.
	var mask_only := NovaFoliageDispatcher.new()
	_own_dispatcher(mask_only)
	mask_only.foliage_defs = [def]
	mask_only.slot_meshes = [mesh]
	mask_only.height_sampler = Callable(self, "_sample_height")
	mask_only.far_slot_mask_sampler = Callable(self, "_sample_split_far_slot_mask")
	mask_only.model_anchors = PackedVector3Array([FAR_DEPTH_ANCHOR])

	_split_far_slot_mask = 1 << 0
	_split_far_slot_samples.clear()
	mask_only.dispatch(Vector3.ZERO, _camera_xform())
	far_debug = mask_only.get_far_tile_debug(0)
	assert_gt(far_debug.size(), 0,
		"The already match-remapped slot bit accepts FAR without a second def.match translation.")
	assert_gt(_split_far_slot_samples.size(), 0, "FAR queried the mask fallback seam.")
	if not far_debug.is_empty() and not _split_far_slot_samples.is_empty():
		var placement: Dictionary = far_debug[0]
		var center: Vector3 = placement.center
		var found_boundary_sample := false
		for sample in _split_far_slot_samples:
			if absf(sample.x - center.x) < 0.0001 and absf(sample.y - center.z) < 0.0001:
				found_boundary_sample = true
				break
		assert_true(found_boundary_sample,
			"The seam carries the placement's own Godot world (x, z) - no boundary negation (placement.h).")

	_split_far_slot_mask = 0
	mask_only.reset()
	mask_only.dispatch(Vector3.ZERO, _camera_xform())
	assert_eq(mask_only.get_far_tile_debug(0).size(), 0,
		"A zero mask from the fallback seam rejects FAR.")


func test_far_mesh_uv2_carries_sector_routed_source_texels() -> void:
	# FAR T1 addressing: retail binds one patch-cache RT per tile through a
	# per-tile world-planar transform [orig: render_terrain_lightmaps
	# @ 0x60a220..0x60a356], so world position resolves to that tile's own
	# SOURCE-ATLAS texels. The host carries those texels per vertex in UV2 via
	# the sector-routed seam; a flat world tap reads the wrong atlas quadrant
	# on origin-shifted layouts (the 00TRg white-tuft report).
	var data := NovaTerrainData.new()
	data.set_trn_path(ProjectSettings.globalize_path(
			"res://../fixtures/godot/dvxi5").path_join("Dvxi5.trn"))
	assert_eq(data.load(), OK, "Dvxi5 fixture should load.")

	# A cell interior to one 512u sector whose routed source offset is nonzero
	# (flat == routed would make the pin vacuous).
	var cell := Vector2(-1e9, 0)
	for wz in range(-768, 769, 16):
		for wx in range(-768, 769, 16):
			if posmod(wx, 512) < 64 or posmod(wx, 512) > 432 \
					or posmod(wz, 512) < 64 or posmod(wz, 512) > 432:
				continue  # keep the whole 16u cell inside one sector
			var routed := data.world_to_source_coords_wrapped(wx + 8.0, wz + 8.0)
			if routed.x < 0.0:
				continue
			if (routed - Vector2(wx + 8.0, wz + 8.0)).length() > 0.5:
				cell = Vector2(wx, wz)
				break
		if cell.x > -1e8:
			break
	assert_gt(cell.x, -1e8, "Dvxi5 exposes a cell where routed and flat texels differ.")
	if cell.x < -1e8:
		return

	var dispatcher := _make_far_dispatcher()
	dispatcher.colormap_source = data
	# Flat heights: the witness cell can sit far from the origin, where the
	# sloped fixture height field would push the cell's Y bounds outside the
	# 42u clamped-box collect distance.
	dispatcher.height_sampler = Callable(self, "_sample_flat_height")
	var centre := Vector3(cell.x + 8.0, 0.0, cell.y + 8.0)
	dispatcher.dispatch(centre, _camera_xform())

	var checked := 0
	for child in dispatcher.get_children():
		if not (child is MeshInstance3D) or child.mesh == null:
			continue
		var arrays: Array = (child as MeshInstance3D).mesh.surface_get_arrays(0)
		var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var uv2s: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
		assert_eq(uv2s.size(), positions.size(),
			"Every FAR vertex carries its source-atlas texel coordinates in UV2.")
		if uv2s.size() != positions.size():
			return
		for i in range(0, positions.size(), 7):
			var routed := data.world_to_source_coords_wrapped(positions[i].x, positions[i].z)
			if routed.x < 0.0:
				continue
			assert_almost_eq(uv2s[i].x, routed.x, 0.01,
				"UV2.x is the sector-routed source texel, not the flat world coordinate.")
			assert_almost_eq(uv2s[i].y, routed.y, 0.01,
				"UV2.y is the sector-routed source texel, not the flat world coordinate.")
			checked += 1
		break
	assert_gt(checked, 0, "The routed cell baked far vertices to check.")
