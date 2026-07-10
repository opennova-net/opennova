extends GutTest

# MODEL draw-state parity recovered from Foliage_UpdateModelTiles @ 0x601f50,
# Foliage_UploadModelTileVSConstants @ 0x600f00, and the visible sector-entity
# caller. A cached tile is still SUBMITTED once per anchor and the wind counter
# advances once per submission, but retail's later immediate-mode draw
# overwrites the earlier one in the framebuffer (z-write on, LESSEQUAL
# [orig: Foliage_DrawModelTileSlot @ 0x601e33]) - so the retained host renders
# ONE batch per (slot, tile) carrying the LAST submission's anchor state
# (coexisting duplicates would z-fight, which retail never shows;
# docs/foliage/foliage-re.md D-FOLIAGE-10).

const ANCHOR_A := Vector3(24.0, 0.0, -100.0)
const ANCHOR_B := Vector3(31.0, 0.0, -101.0)

var _dispatcher: NovaFoliageDispatcher


func before_each() -> void:
	_dispatcher = NovaFoliageDispatcher.new()
	add_child_autofree(_dispatcher)

	var def := NovaTerrainFoliageDef.new()
	def.graphic = "test_model"
	def.match = 1

	var mesh := BoxMesh.new()
	mesh.size = Vector3(2.0, 6.0, 2.0)

	_dispatcher.foliage_defs = [def]
	_dispatcher.slot_meshes = [mesh]
	_dispatcher.height_sampler = Callable(self, "_sample_height")
	_dispatcher.foliage_sampler = Callable(self, "_sample_foliage_index")


func _sample_height(world_x: float, world_z: float) -> float:
	return 0.04 * world_x - 0.03 * world_z + 0.002 * world_x * world_z


func _sample_foliage_index(_world_x: float, _world_z: float) -> int:
	return 1


func _camera_xform() -> Transform3D:
	return Transform3D(Basis(), Vector3(0.0, 1.5, 0.0))


func _dispatch_shared_tiles() -> void:
	_dispatcher.model_anchors = PackedVector3Array([ANCHOR_A, ANCHOR_B])
	_dispatcher.dispatch(Vector3.ZERO, _camera_xform())


func _expected_alpha_ref(anchor: Vector3) -> int:
	var distance_units: float = floorf(anchor.distance_to(_camera_xform().origin))
	return clampi(int(4096.0 / (distance_units + 1.0)), 8, 128)


func _model_draw_nodes() -> Array:
	var out: Array = []
	for child in _dispatcher.get_children():
		if child is MultiMeshInstance3D and child.visible 				and String(child.name).begins_with("FoliageModelDraw"):
			out.push_back(child)
	return out


func test_shared_tiles_render_once_with_last_submission_state() -> void:
	_dispatch_shared_tiles()
	var draws: Array = _dispatcher.get_model_draw_debug()
	assert_gt(draws.size(), 0, "Two in-range anchors produce rendered model batches.")
	if draws.is_empty():
		return

	assert_ne(_expected_alpha_ref(ANCHOR_A), _expected_alpha_ref(ANCHOR_B),
		"Fixture anchors exercise distinct alpha-test references.")

	var seen_keys := {}
	var previous_counter := -1
	var total_submissions := 0
	for i in range(draws.size()):
		var draw: Dictionary = draws[i]
		var key := int(draw.tile_key)
		assert_false(seen_keys.has(key),
			"Tile %s renders exactly ONE batch per frame (retail's later draw overwrites; coexisting copies would z-fight)." % key)
		seen_keys[key] = true

		assert_eq(int(draw.submissions), 2,
			"Both anchors share the fixture tiles, so each batch folds two retail submissions.")
		total_submissions += int(draw.submissions)

		var anchor: Vector3 = draw.anchor
		assert_true(anchor.is_equal_approx(ANCHOR_B),
			"The rendered batch carries the LAST submission's anchor (walk order).")
		var expected_distance := anchor.distance_to(_camera_xform().origin)
		assert_almost_eq(float(draw.anchor_distance), expected_distance, 0.001,
			"Alpha state carries the Euclidean camera-to-anchor distance.")
		assert_eq(int(draw.alpha_ref), _expected_alpha_ref(ANCHOR_B),
			"Alpha ref = clamp(int(4096/(floor(distance)+1)), 8, 128) of the LAST submitting anchor.")

		var counter := int(draw.wind_counter)
		assert_almost_eq(float(draw.wind_phase), float(counter) * 0.001, 0.000001,
			"Each batch carries counter * 0.001 as its wind phase.")
		if previous_counter >= 0:
			assert_gt(counter, previous_counter,
				"Wind counters stay strictly increasing across rendered batches.")
		previous_counter = counter

	var stats: Dictionary = _dispatcher.get_dispatch_stats()
	assert_eq(int(stats.model_tiles_emitted), total_submissions,
		"The wind counter advances once per SUBMISSION [orig: 0x600f00], not per rendered batch.")
	assert_eq(int(stats.model_batches), draws.size(),
		"The stats expose the deduped rendered-batch count.")

	var nodes := _model_draw_nodes()
	assert_eq(nodes.size(), draws.size(), "Each rendered batch owns one MultiMesh node.")
	for i in range(mini(nodes.size(), draws.size())):
		var node := nodes[i] as MultiMeshInstance3D
		var draw: Dictionary = draws[i]
		assert_eq(node.multimesh.instance_count, int(draw.instance_count),
			"The batch contains exactly that tile's instances.")
		var material := node.material_override as ShaderMaterial
		assert_not_null(material, "Each rendered batch clones the slot's model material.")
		if material != null:
			assert_almost_eq(float(material.get_shader_parameter("u_model_alpha_ref")),
				float(draw.alpha_ref), 0.000001, "Batch material keeps the last submission's alpha ref.")
			assert_almost_eq(float(material.get_shader_parameter("u_model_wind_phase")),
				float(draw.wind_phase), 0.000001, "Batch material keeps the last submission's wind phase.")


func test_slot_mesh_change_invalidates_model_tile_cache_and_batches() -> void:
	_dispatch_shared_tiles()
	assert_gt(int(_dispatcher.get_dispatch_stats().model_cached_tiles), 0,
		"Fixture first populates the model tile cache.")
	assert_gt(_dispatcher.get_model_draw_debug().size(), 0,
		"Fixture first creates model draw batches.")

	var replacement := BoxMesh.new()
	replacement.size = Vector3(4.0, 6.0, 2.0)
	_dispatcher.slot_meshes = [replacement]
	# Model draw nodes are detached immediately and queued for deletion; let
	# Godot flush the queue before GUT's orphan tracker observes them.
	await get_tree().process_frame

	var cleared: Dictionary = _dispatcher.get_dispatch_stats()
	assert_eq(int(cleared.model_cached_tiles), 0,
		"Mesh bounds change the footprint, so every model placement cache is invalidated.")
	assert_eq(_dispatcher.get_model_draw_debug().size(), 0,
		"Stale draw batches are removed with the placement cache.")
	assert_eq(_model_draw_nodes().size(), 0, "Stale model render batches are detached immediately.")

	_dispatcher.dispatch(Vector3.ZERO, _camera_xform())
	var rebuilt: Dictionary = _dispatcher.get_dispatch_stats()
	assert_gt(int(rebuilt.model_cache_misses), 0,
		"The first dispatch with new mesh bounds regenerates model tiles.")
	assert_gt(_dispatcher.get_model_draw_debug().size(), 0,
		"Model draw batches rebuild against the replacement mesh.")
