extends GutTest

# MODEL draw-state parity recovered from Foliage_UpdateModelTiles @ 0x601f50,
# Foliage_UploadModelTileVSConstants @ 0x600f00, and the visible sector-entity
# caller. A cached tile is still submitted once per anchor; alpha-test state is
# anchor-derived and the wind counter advances once per submitted tile.

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


func test_shared_tiles_remain_distinct_draws_with_anchor_state() -> void:
	_dispatch_shared_tiles()
	var draws: Array = _dispatcher.get_model_draw_debug()
	assert_gt(draws.size(), 1,
		"Two anchors in the same snapped region submit each non-empty cached tile twice.")
	if draws.size() < 2:
		return

	var draws_by_key := {}
	var previous_counter := -1
	for i in range(draws.size()):
		var draw: Dictionary = draws[i]
		var key := int(draw.tile_key)
		var same_key: Array = draws_by_key.get(key, [])
		same_key.push_back(draw)
		draws_by_key[key] = same_key

		var anchor: Vector3 = draw.anchor
		var expected_distance := anchor.distance_to(_camera_xform().origin)
		assert_almost_eq(float(draw.anchor_distance), expected_distance, 0.001,
			"Alpha state carries the Euclidean camera-to-anchor distance.")
		assert_eq(int(draw.alpha_ref), _expected_alpha_ref(anchor),
			"Alpha ref is clamp(int(4096/(floor(distance)+1)), 8, 128) in byte space.")

		var counter := int(draw.wind_counter)
		assert_almost_eq(float(draw.wind_phase), float(counter) * 0.001, 0.000001,
			"Each draw carries counter * 0.001 as its wind phase.")
		if previous_counter >= 0:
			assert_eq(counter, previous_counter + 1,
				"The global model wind counter advances once per tile draw.")
		previous_counter = counter

	assert_gt(draws_by_key.size(), 0, "At least one shared tile contains model instances.")
	for key in draws_by_key:
		var same_key: Array = draws_by_key[key]
		assert_eq(same_key.size(), 2,
			"Tile %s is drawn once for each visible anchor; it is not deduplicated." % key)
		if same_key.size() == 2:
			var first_anchor: Vector3 = same_key[0].anchor
			var second_anchor: Vector3 = same_key[1].anchor
			assert_false(first_anchor.is_equal_approx(second_anchor),
				"The duplicate tile draws retain their distinct anchors.")

	assert_ne(_expected_alpha_ref(ANCHOR_A), _expected_alpha_ref(ANCHOR_B),
		"Fixture anchors exercise distinct alpha-test references.")

	var nodes := _model_draw_nodes()
	assert_eq(nodes.size(), draws.size(), "Each retail tile draw owns one MultiMesh batch.")
	for i in range(mini(nodes.size(), draws.size())):
		var node := nodes[i] as MultiMeshInstance3D
		var draw: Dictionary = draws[i]
		assert_eq(node.multimesh.instance_count, int(draw.instance_count),
			"The batch contains exactly that tile draw's instances.")
		var material := node.material_override as ShaderMaterial
		assert_not_null(material, "Each tile draw clones the slot's model material.")
		if material != null:
			assert_almost_eq(float(material.get_shader_parameter("u_model_alpha_ref")),
				float(draw.alpha_ref), 0.000001, "Batch material keeps its anchor alpha ref.")
			assert_almost_eq(float(material.get_shader_parameter("u_model_wind_phase")),
				float(draw.wind_phase), 0.000001, "Batch material keeps its tile wind phase.")


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
