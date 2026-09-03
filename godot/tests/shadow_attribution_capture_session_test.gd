extends GutTest

const CaptureSession := preload(
		"res://probes/render/shadow_attribution_capture_session.gd")
const CaptureVariant := preload(
		"res://probes/render/render_capture_variant.gd")
const ITEMS_DEF_FIXTURE := "res://../fixtures/def/items.def"

var _root_dir := ""


func after_each() -> void:
	if not _root_dir.is_empty():
		TestFs.remove_dir_recursive(_root_dir)
		_root_dir = ""


func _add_caster(
		world: GameWorld,
		bms_id: int,
		item_id: int,
		graphic: String,
		attrib2: int,
		position: Vector3,
		dynamic_enabled: bool = true,
		static_enabled: bool = false,
		) -> ObjectModel:
	var model := ObjectModel.new()
	model.name = "Caster%d" % bms_id
	model.position = position
	var ref := EntityRef.make(-1, -1, bms_id, item_id)
	ref.graphic = graphic
	ref.attrib2 = attrib2
	model.entity_ref = ref
	world.add_child(model)
	model.set_shadow_caster_enabled(dynamic_enabled)
	model.set_static_shadow_caster_enabled(static_enabled)
	return model


func test_public_session_isolates_shadow_systems_and_only_suppresses_bms58() -> void:
	var world := preload("res://game/world/game_world.tscn").instantiate() as GameWorld
	add_child_autofree(world)
	var viewport := SubViewport.new()
	viewport.debug_draw = Viewport.DEBUG_DRAW_OVERDRAW
	world.add_child(viewport)
	var dynamic_shadow := world.get_node("SunShadow") as DirectionalLight3D
	var terrain := world.get_terrain_node()
	dynamic_shadow.shadow_enabled = false
	terrain.set_static_terrain_shadow_enabled(true)

	var sibling := _add_caster(world, 77, 101111, "Sibling.3di", 0x10,
			Vector3(-2.0, 3.0, 4.0))
	var target := _add_caster(world, 58, 101294, "DTruck1.3di", 0x12345678,
			Vector3(5.0, 6.0, 7.0), true, true)
	var target_parent := target.get_parent()
	var target_transform := target.transform
	var target_visible := target.visible

	var session = CaptureSession.new()
	assert_null(session.get_variant_diagnostics(),
			"an inactive session has no realized renderer snapshot")
	assert_eq(session.begin(world, viewport), OK)
	var without_target = CaptureVariant.new(
			"dynamic_without_bms58", Viewport.DEBUG_DRAW_DISABLED,
			true, false, PackedInt32Array([58]))
	assert_eq(session.apply_variant(without_target), OK)
	assert_true(dynamic_shadow.shadow_enabled)
	assert_false(terrain.is_static_terrain_shadow_enabled())
	assert_false(target.is_shadow_caster_enabled())
	assert_true(target.is_static_shadow_caster_enabled(),
			"the diagnostic suppresses only the dynamic caster layer")
	assert_true(sibling.is_shadow_caster_enabled())
	assert_eq(target.get_parent(), target_parent)
	assert_eq(target.transform, target_transform)
	assert_eq(target.visible, target_visible)
	var realized := session.get_variant_diagnostics()
	assert_true(realized is CaptureVariant,
			"realized diagnostics remain typed until the JSON boundary")
	assert_ne(realized, without_target,
			"diagnostics are a fresh realized snapshot, not the request record")
	assert_eq(realized.to_json_value(), {
		"id": "dynamic_without_bms58",
		"debug_draw": Viewport.DEBUG_DRAW_DISABLED,
		"dynamic_shadow_enabled": true,
		"static_terrain_shadow_enabled": false,
		"suppressed_dynamic_caster_bms_ids": [58],
		"suppressed_static_caster_bms_ids": [],
	})
	target.set_shadow_caster_enabled(true)
	assert_eq(Array(session.get_variant_diagnostics().suppressed_dynamic_caster_bms_ids),
			[], "diagnostics must inspect the realized caster bit, not echo the request")
	target.set_shadow_caster_enabled(false)
	dynamic_shadow.shadow_enabled = false
	assert_false(bool(session.get_variant_diagnostics().dynamic_shadow_enabled),
			"diagnostics must inspect the realized light state, not echo the request")
	dynamic_shadow.shadow_enabled = true

	var static_only = CaptureVariant.new(
			"static_terrain_shadow_only", Viewport.DEBUG_DRAW_DISABLED,
			false, true)
	assert_eq(session.apply_variant(static_only), OK)
	assert_false(dynamic_shadow.shadow_enabled)
	assert_true(terrain.is_static_terrain_shadow_enabled())
	assert_true(target.is_shadow_caster_enabled(),
			"moving to the next variant restores BMS58 before capture")

	session.finish()
	assert_null(session.get_variant_diagnostics(),
			"finish clears the realized renderer snapshot")
	assert_false(dynamic_shadow.shadow_enabled,
			"finish restores the pre-session dynamic-light state")
	assert_true(terrain.is_static_terrain_shadow_enabled())
	assert_eq(viewport.debug_draw, Viewport.DEBUG_DRAW_OVERDRAW)
	assert_true(target.is_shadow_caster_enabled())


func test_static_batch_inventory_and_suppression_leave_visible_geometry_intact() -> void:
	var world := preload("res://game/world/game_world.tscn").instantiate() as GameWorld
	add_child_autofree(world)
	var viewport := SubViewport.new()
	world.add_child(viewport)
	var terrain := world.get_terrain_node()
	var mesh := BoxMesh.new()
	mesh.size = Vector3(2, 4, 6)
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = mesh
	multimesh.instance_count = 2
	multimesh.set_instance_transform(0, Transform3D(Basis(), Vector3(5, 2, 7)))
	multimesh.set_instance_transform(1, Transform3D(Basis(), Vector3(-3, 2, 9)))
	var batch := StaticPopulationInstance.new()
	batch.name = "Batch_DTruck1_0"
	batch.multimesh = multimesh
	batch.layers = Water.VISUAL_LAYER_WORLD_NO_MIRROR \
			| Water.VISUAL_LAYER_STATIC_SHADOW_CASTER
	batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	batch.shadow_tagged = true
	batch.slot_bms_ids = PackedInt32Array([58, 77])
	batch.slot_item_ids = PackedInt32Array([101294, 101111])
	batch.slot_attrib2 = PackedInt64Array([0x20, 0x20])
	batch.slot_casts_shadow = PackedByteArray([1, 1])
	batch.graphic = "DTruck1"
	batch.row_slots = PackedInt32Array([0, 1])
	world.add_child(batch)

	var original_layers := batch.layers
	var original_first := multimesh.get_instance_transform(0)
	var original_second := multimesh.get_instance_transform(1)
	var session = CaptureSession.new()
	assert_eq(session.begin(world, viewport), OK)
	var inventory: Array = session.get_static_caster_inventory()
	assert_eq(inventory.size(), 2)
	var encoded: Array = inventory.map(func(row): return row.to_json_value())
	assert_eq(encoded.map(func(row): return row.bms_id), [58, 77])
	assert_eq(encoded[0].graphic, "DTruck1")
	assert_eq(encoded[0].layer, Water.VISUAL_LAYER_STATIC_SHADOW_CASTER)
	assert_true(encoded[0].aabb is AABB,
			"inventory carries the exact batch-slot world-bound type; headless "
			+ "RenderingServer does not round-trip MultiMesh transforms")

	var without_first = CaptureVariant.new(
			"static_without_bms58", Viewport.DEBUG_DRAW_DISABLED,
			false, true, PackedInt32Array(), PackedInt32Array([58]))
	assert_eq(session.apply_variant(without_first), OK)
	assert_eq(batch.layers, original_layers,
			"page-provider suppression never mutates visible/caster scene geometry")
	assert_eq(multimesh.get_instance_transform(0), original_first)
	assert_eq(multimesh.get_instance_transform(1), original_second,
			"attribution never edits the visible MultiMesh")
	assert_eq(Array(terrain.get_suppressed_static_shadow_bms_ids()), [58],
			"the exact BMS identity is filtered at the typed page provider")
	assert_eq(Array(session.get_variant_diagnostics().suppressed_static_caster_bms_ids),
			[58])
	terrain.set_suppressed_static_shadow_bms_ids(PackedInt32Array())
	assert_eq(Array(session.get_variant_diagnostics().suppressed_static_caster_bms_ids),
			[], "diagnostics must inspect the realized page-provider set")
	terrain.set_suppressed_static_shadow_bms_ids(PackedInt32Array([58]))

	session.finish()
	assert_eq(batch.layers, original_layers)
	assert_true(terrain.get_suppressed_static_shadow_bms_ids().is_empty(),
			"finish restores the provider's original suppression set")


func test_dynamic_caster_inventory_is_typed_complete_and_deterministic() -> void:
	var world := preload("res://game/world/game_world.tscn").instantiate() as GameWorld
	add_child_autofree(world)
	var viewport := SubViewport.new()
	world.add_child(viewport)
	_add_caster(world, 77, 101111, "Sibling.3di", 0x10,
			Vector3(-2.0, 3.0, 4.0))
	_add_caster(world, 58, 101294, "DTruck1.3di", 0x12345678,
			Vector3(5.0, 6.0, 7.0))
	_add_caster(world, 12, 100012, "StaticOnly.3di", 0x20,
			Vector3.ZERO, false, true)

	var session = CaptureSession.new()
	assert_eq(session.begin(world, viewport), OK)
	var rows: Array = session.get_dynamic_caster_inventory()
	assert_eq(rows.size(), 2)
	var encoded: Array = rows.map(func(row): return row.to_json_value())
	assert_eq(encoded.map(func(row): return row.bms_id), [58, 77],
			"authored identity defines inventory order, not scene insertion")
	assert_eq(encoded[0].bms_id, 58)
	assert_eq(encoded[0].item_id, 101294)
	assert_eq(encoded[0].graphic, "DTruck1.3di")
	assert_eq(encoded[0].attrib2, 0x12345678,
			"the diagnostic carries its source attrib2 value without inference")
	assert_eq(encoded[0].layer,
			Water.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER)
	assert_true(encoded[0].aabb is AABB)
	assert_eq(encoded, session.get_dynamic_caster_inventory().map(
			func(row): return row.to_json_value()))
	session.finish()


func test_inventory_resolves_graphic_and_attrib2_through_the_public_item_db() -> void:
	# A REAL loaded world whose staged root carries the committed def fixture
	# as its items.def, so world.get_item_db() is the placer's own database.
	_root_dir = WorldFixture.stage_minimal_root("shadow_item_db", false, {
		"items.def": FileAccess.get_file_as_string(
				ProjectSettings.globalize_path(ITEMS_DEF_FIXTURE)),
	})
	var world := WorldFixture.boot_minimal(self, _root_dir)
	assert_not_null(world.get_item_db(),
			"the loaded mission mounted the placer's item database")
	var viewport := SubViewport.new()
	world.add_child(viewport)
	var model := ObjectModel.new()
	model.entity_ref = EntityRef.make(-1, -1, 291, 101291)
	world.add_child(model)
	model.set_shadow_caster_enabled(true)

	var session = CaptureSession.new()
	assert_eq(session.begin(world, viewport), OK)
	var rows: Array = session.get_dynamic_caster_inventory()
	assert_eq(rows.size(), 1)
	assert_eq(rows[0].to_json_value().graphic, "Dbuggy1")
	assert_eq(rows[0].to_json_value().attrib2, 0x10,
			"the committed DynamicShadow row is carried without reinterpretation")
	session.finish()
