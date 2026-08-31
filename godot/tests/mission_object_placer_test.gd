extends GutTest

# Phase 4: MissionObjectPlacer. Covers the asset-free pieces that must be exactly
# right (BMS -> Godot coordinate conversion, cross-checked against the equivalent
# Basis) and the graceful resolution-miss path (fixtures ship items.def but no
# .3di, so nothing resolves to a model and the placer must place zero without
# error). Full render-placement is validated against real assets out-of-band.


const BMS_PATH := "res://../fixtures/bms/synth_dense.bms"
const ITEMS_PATH := "res://../fixtures/def/items.def"


func _abs(res_path: String) -> String:
	return ProjectSettings.globalize_path(res_path)


func test_position_is_minus_90_about_x() -> void:
	# (x, y, z) -> (x, z, -y); equivalent to a -90 deg rotation about X.
	assert_eq(MissionObjectPlacer.bms_to_godot_position(Vector3(1, 2, 3)), Vector3(1, 3, -2))
	var basis := Basis.from_euler(Vector3(deg_to_rad(-90.0), 0.0, 0.0))
	for v in [Vector3(5, -7, 11), Vector3(-1, 0, 4), Vector3(100, 50, -25)]:
		assert_true(
			MissionObjectPlacer.bms_to_godot_position(v).is_equal_approx(basis * v),
			"position conversion matches the -90 deg X basis for %s" % v)


# Yaw-only must stay byte-for-byte the long-standing (visually-correct) heading: the engine's
# Rz(90 - yaw) about the up axis, conjugated into Godot and composed with the +Z-forward model
# correction C = RotY(90), collapses to RotY(180 - yaw) -- exactly the previous euler form. This is
# the no-regression guard for the common (untilted) case. [orig: @0x40eb66 / @0x613f40]
func test_yaw_only_basis_matches_the_legacy_heading() -> void:
	for yaw in [0.0, 45.0, 90.0, 180.0, 270.0]:
		var got := MissionObjectPlacer.bms_to_godot_basis(Vector3(0, yaw, 0))
		var legacy := Basis(Vector3.UP, PI - deg_to_rad(yaw))
		assert_true(got.is_equal_approx(legacy),
			"yaw=%s basis is RotY(180 - yaw), unchanged from the legacy heading" % yaw)


# Pitch must tip the model's nose the way retail does. The model's local forward is +Z
# (Vector3.BACK). At yaw=0, a +30 degree authored pitch points the nose UP:
# forward = (0, +sin30, -cos30), matching M * Rz(90) * Ry(-30) * (+X_engine).
# [orig: @0x40eb86 / @0x613f40]
func test_pitch_tips_the_nose_up_like_retail() -> void:
	var basis := MissionObjectPlacer.bms_to_godot_basis(Vector3(30, 0, 0))
	var forward: Vector3 = basis * Vector3.BACK
	var up: Vector3 = basis * Vector3.UP
	assert_true(forward.is_equal_approx(Vector3(0.0, sin(deg_to_rad(30.0)), -cos(deg_to_rad(30.0)))),
		"positive authored pitch follows retail Ry(-pitch)")
	assert_true(up.is_equal_approx(Vector3(0.0, cos(deg_to_rad(30.0)), sin(deg_to_rad(30.0)))),
		"and the model's up tilts to match")


# Roll banks the model about its forward axis the way the engine does. At yaw=90 (so the heading
# term is identity), a +30 deg roll tilts the up vector to (0, cos30, sin30), matching
# M * Rx(30) * (+Z_engine). [orig: @0x40eba6 / @0x613f40]
func test_roll_banks_like_the_engine() -> void:
	var basis := MissionObjectPlacer.bms_to_godot_basis(Vector3(0, 90, 30))
	var up: Vector3 = basis * Vector3.UP
	assert_true(up.is_equal_approx(Vector3(0.0, cos(deg_to_rad(30.0)), sin(deg_to_rad(30.0)))),
		"roll banks the up vector engine-faithfully")


func test_entity_transform_composes_basis_and_origin() -> void:
	var pos := Vector3(10, 20, 30)
	var rot_deg := Vector3(15, 180, 25)
	var xform := MissionObjectPlacer.entity_transform(pos, rot_deg)
	assert_true(
		xform.origin.is_equal_approx(MissionObjectPlacer.bms_to_godot_position(pos)),
		"origin is the converted position")
	assert_true(xform.basis.is_equal_approx(MissionObjectPlacer.bms_to_godot_basis(rot_deg)),
		"basis is the converted rotation")


func test_godot_to_bms_position_inverts_bms_to_godot() -> void:
	# The editor writes a dragged object's new ground point back through this inverse;
	# it must exactly undo bms_to_godot_position for any mission-space point.
	for v in [Vector3(5, -7, 11), Vector3(-1, 0, 4), Vector3(100, 50, -25), Vector3.ZERO]:
		assert_true(
			MissionObjectPlacer.godot_to_bms_position(MissionObjectPlacer.bms_to_godot_position(v)).is_equal_approx(v),
			"godot_to_bms_position round-trips %s" % v)


func test_place_handles_unresolvable_models_without_error() -> void:
	var mission := MissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK, "fixture BMS parses")

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK, "fixture items.def loads")

	var root := ResourceRoot.new()
	# Points at the def fixtures dir: it has items.def but no .3di, so no model
	# resolves. set_root_dir may reject it; resolve_file then simply returns "".
	root.set_root_dir(_abs("res://../fixtures/def"))

	var placer := MissionObjectPlacer.create(root, item_db)
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_not_null(parent.get_node_or_null("MissionObjects"), "MissionObjects container is created")
	assert_true(stats.has("placed"), "stats expose placed")
	assert_true(stats.has("unresolved"), "stats expose unresolved")
	assert_eq(int(stats.get("placed", -1)), 0, "no .3di in fixtures -> nothing placed")
	assert_gt(int(stats.get("unresolved", 0)), 0, "real entities recorded as unresolved")


func test_place_is_a_noop_on_null_inputs() -> void:
	var placer := MissionObjectPlacer.new()
	var parent := Node3D.new()
	add_child_autofree(parent)
	var stats: Dictionary = placer.place(null, parent)
	assert_eq(int(stats.get("placed", -1)), 0, "null mission places nothing")
	assert_null(parent.get_node_or_null("MissionObjects"), "no container without a mission")


func test_runtime_static_batch_publishes_effect_source() -> void:
	# Exercise runtime static placement asset-free by pre-seeding the
	# per-graphic batch cache with a dummy mesh.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var parent := Node3D.new()
	add_child_autofree(parent)

	# Seed the static-batch cache so the static branch has geometry to instance.
	var mesh := BoxMesh.new()
	mesh.size = Vector3(2, 2, 2)
	var batch_material := ShaderMaterial.new()
	batch_material.shader = load("res://shaders/object/fixed/opaque.gdshader")
	var offset := Transform3D(Basis(), Vector3(0, 1, 0))
	var object_data := ObjectData.new()
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", object_data, [{
		"mesh": mesh, "material": batch_material, "offset": offset, "submesh": 0,
	}]))

	var record := mission.add_entity(
			MissionData.KIND_ITEM, 105004, Vector3(3, 4, 5), Vector3.ZERO)
	assert_false(record.is_empty())
	var index := int(record.get("index", -1))
	var stats: Dictionary = placer.place(mission, parent)
	assert_eq(int(stats.get("placed", -1)), 1, "the static entity is placed")
	assert_eq(int(stats.get("batched", -1)), 1, "via the static-batch branch")
	assert_eq(int(stats.get("batches", -1)), 1, "one draw group for its single submesh")
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	var mmi := container.get_node_or_null("StaticPopulations/Batch_StaticCrate1_0") \
			as MultiMeshInstance3D
	assert_not_null(mmi)
	if mmi == null:
		return
	assert_eq(mmi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
			"retail static batches receive dynamic silhouettes but never cast them")
	assert_ne(mmi.layers & Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
			"a batched non-vehicle item stays in the separately filtered entity "
			+ "population [orig: Water_RenderReflectedWorldScene @ 0x5c857b -> "
			+ "Terrain_RenderSectorEntities; collection wrapper @ 0x5c90a0 -> "
			+ "collectors @ 0x5c6f20/@0x5c8c60; flag writer @ 0x40e208]")
	assert_eq(mmi.layers & Water.VISUAL_LAYER_WORLD, 0,
			"presentation batching does not promote an item into the sector-building pass")
	var mm: MultiMesh = mmi.multimesh
	assert_eq(mm.instance_count, 1, "the runtime static has one MultiMesh slot")
	assert_true(mm.use_custom_data,
			"static batches expose their per-ROBJ light-atlas identity")
	# The dummy headless RenderingServer does not persist instance buffers (the
	# same limitation as transforms below). The rendered shader probe verifies
	# that atlas row zero arrives as INSTANCE_CUSTOM.x == 1.0.
	assert_true(mmi.is_inside_tree(), "the batch instance is in the runtime container")

	# The batch has no per-entity Node3D, but mission-start item effects still
	# receive one immutable value descriptor for the successfully rendered entity.
	var effect_sources: Array = placer.get_static_item_effect_sources()
	assert_eq(effect_sources.size(), 1)
	var source: Dictionary = effect_sources[0]
	assert_eq(int(source.get("kind", -1)), MissionData.KIND_ITEM)
	assert_eq(int(source.get("item_id", 0)), 105004)
	assert_eq(String(source.get("graphic", "")), "StaticCrate1")
	assert_eq(source.get("object_data"), object_data)
	var expected_transform := MissionObjectPlacer.entity_transform(Vector3(3, 4, 5), Vector3.ZERO)
	var actual_transform: Transform3D = source.get("world_transform", Transform3D.IDENTITY)
	assert_true(actual_transform.is_equal_approx(expected_transform),
			"the descriptor carries the BASE entity transform, not a submesh offset")
	var light_draws: Array = placer.get_static_light_draw_sources()
	assert_eq(light_draws.size(), 1,
			"one retained ROBJ owns one point-light selection row")
	var light_draw: Dictionary = light_draws[0]
	assert_eq(int(light_draw.get("atlas_row", -1)), 0)
	assert_eq(int(light_draw.get("source_index", -1)), 0,
			"the draw row points at its exact static effect source")
	assert_eq(int(light_draw.get("kind", -1)), MissionData.KIND_ITEM)
	assert_eq(int(light_draw.get("entity_index", -1)), index)
	assert_eq(int(light_draw.get("bms_id", 0)), int(record.get("bms_id", 0)))
	assert_eq(int(light_draw.get("item_id", 0)), 105004)
	assert_eq(int(light_draw.get("robj_index", -1)), 0)
	assert_true(bool(light_draw.get("active", false)))
	var expected_bounds: AABB = (expected_transform * offset) * mesh.get_aabb()
	var actual_bounds: AABB = light_draw.get("world_bounds", AABB())
	assert_true(actual_bounds.position.is_equal_approx(expected_bounds.position))
	assert_true(actual_bounds.size.is_equal_approx(expected_bounds.size),
			"selection uses the exact transformed bounds of that ROBJ's surfaces")
	# Getter rows are copies; callers cannot rewrite the placer's retained identity.
	source["item_id"] = 0
	assert_eq(int(placer.get_static_item_effect_sources()[0].get("item_id", 0)), 105004)
	light_draw["atlas_row"] = 99
	assert_eq(int(placer.get_static_light_draw_sources()[0].get("atlas_row", -1)), 0)


func test_static_batches_partition_opaque_geometry_but_keep_blended_global() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var first := mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(10, -10, 0), Vector3.ZERO)
	var second := mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(530, -10, 0), Vector3.ZERO)
	assert_false(first.is_empty())
	assert_false(second.is_empty())
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var opaque_mesh := BoxMesh.new()
	opaque_mesh.size = Vector3(4, 6, 8)
	var blended_mesh := BoxMesh.new()
	blended_mesh.size = Vector3(2, 2, 2)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": opaque_mesh, "material": null,
				"offset": Transform3D(Basis(), Vector3(0, 3, 0)),
				"submesh": 0,
			}, {
				"mesh": blended_mesh, "material": null,
				"offset": Transform3D(Basis(), Vector3(0, 7, 0)),
				"submesh": 1, "blended_draw": true,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("static_bins", -1)), 2,
			"opaque populations follow the terrain's 512-unit sector cells")
	assert_eq(int(stats.get("static_binned_batches", -1)), 2)
	assert_eq(int(stats.get("static_global_batches", -1)), 1,
			"the blended strip remains one explicitly global population")
	assert_eq(int(stats.get("batches", -1)), 3)
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return
	var binned: Array[MultiMeshInstance3D] = []
	var global: MultiMeshInstance3D = null
	for child in _populations(container):
		var mmi := child as MultiMeshInstance3D
		if mmi == null:
			continue
		if String(mmi.get_meta("static_batch_population", "")) == "bin":
			binned.append(mmi)
		elif String(mmi.get_meta("static_batch_population", "")) == "global":
			global = mmi
	assert_eq(binned.size(), 2)
	assert_not_null(global)
	if global != null:
		assert_eq(global.multimesh.instance_count, 2,
				"the global blended population retains both placements")
		assert_false(global.has_meta("static_batch_bin_x"))
	var bin_xs: Array[int] = []
	for mmi in binned:
		var bin_x := int(mmi.get_meta("static_batch_bin_x", -99))
		bin_xs.append(bin_x)
		assert_eq(int(mmi.get_meta("static_batch_bin_z", -99)), 0)
		assert_eq(mmi.multimesh.instance_count, 1)
		var authored_position := Vector3(10, -10, 0) \
				if bin_x == 0 else Vector3(530, -10, 0)
		var entity_xform := MissionObjectPlacer.entity_transform(
				authored_position, Vector3.ZERO)
		var batch_offset := Transform3D(Basis(), Vector3(0, 3, 0))
		var expected_bounds: AABB = (entity_xform * batch_offset) \
				* opaque_mesh.get_aabb()
		assert_true(mmi.custom_aabb.position.is_equal_approx(
				expected_bounds.position))
		assert_true(mmi.custom_aabb.size.is_equal_approx(expected_bounds.size),
				"each MultiMesh advertises the exact bounds of its emitted geometry")
	bin_xs.sort()
	assert_eq(bin_xs, [0, 1])

	var first_bms_id := int(first.get("bms_id", 0))
	assert_ne(first_bms_id, 0)
	assert_eq(placer.get_static_instance_binding_count(first_bms_id), 2,
			"destruction owns the emitted bin slot and global blended slot")
	assert_false(placer.hide_static_instance(first_bms_id) == null)
	assert_true(placer.is_static_instance_hidden(first_bms_id))
	assert_true(placer.show_static_instance(first_bms_id),
			"one destruction record restores its binned opaque and global "
			+ "blended slots")


func test_runtime_static_vehicle_rides_the_mirror_visible_layer() -> void:
	# env #30: the water mirror's above-water collection keeps only
	# ItemDefType==vehicle entities [orig: Entity_InitFromModel @ 0x40e20a
	# entity+36 |= 0x400; Terrain_CollectVisibleEntitiesForReflection
	# @ 0x5c90a0 filterMask 0x400]. Fixture 106002 "Static Vehicle" is the
	# type=vehicle twin of the crate case above.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var parent := Node3D.new()
	add_child_autofree(parent)

	var mesh := BoxMesh.new()
	mesh.size = Vector3(2, 1, 4)
	assert_true(placer.register_resolved_static_graphic(
			"StaticVehicle1", ObjectData.new(), [{
		"mesh": mesh, "material": null,
		"offset": Transform3D.IDENTITY, "submesh": 0,
	}]))

	assert_false(mission.add_entity(
			MissionData.KIND_ITEM, 106002, Vector3(1, 2, 3),
			Vector3.ZERO).is_empty())
	var stats: Dictionary = placer.place(mission, parent)
	assert_eq(int(stats.get("placed", -1)), 1, "the static vehicle places")
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	var mmi := container.get_node_or_null("StaticPopulations/Batch_StaticVehicle1_0") \
			as MultiMeshInstance3D
	assert_not_null(mmi)
	if mmi == null:
		return
	assert_ne(mmi.layers & Water.VISUAL_LAYER_WORLD, 0,
			"a type=vehicle entity stays on the mirror-visible world layer")
	assert_eq(mmi.layers & Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
			"the vehicle batch never rides the no-mirror layer")


func test_dynamic_shadow_caster_policy_matches_retail_entity_slot_admission() -> void:
	assert_true(MissionObjectPlacer.item_casts_dynamic_shadow(
			ItemDatabase.TYPE_PERSON, 0, 0),
			"people always receive a retail shadow render slot")
	assert_true(MissionObjectPlacer.item_casts_dynamic_shadow(
			ItemDatabase.TYPE_VEHICLE, 0, 0x10),
			"DynamicShadow admits a non-person model")
	assert_false(MissionObjectPlacer.item_casts_dynamic_shadow(
			ItemDatabase.TYPE_BUILDING, 0, 0),
			"portal/static buildings never become silhouette casters")
	assert_true(MissionObjectPlacer.item_casts_dynamic_shadow(
			ItemDatabase.TYPE_PERSON, 0x04000000, 0x10),
			"the witnessed dynamic-slot allocator does not consult ItemDef NoShadow")


func test_static_shadow_caster_policy_matches_retail_terrain_tile_admission() -> void:
	assert_true(MissionObjectPlacer.item_casts_static_terrain_shadow(
			MissionData.KIND_BUILDING, 0, 0, 0),
			"pool-2 buildings enter the terrain-tile caster pass by default")
	assert_true(MissionObjectPlacer.item_casts_static_terrain_shadow(
			MissionData.KIND_ITEM, 0, 0, 0x20),
			"pool-1 items require StaticShadow")
	assert_false(MissionObjectPlacer.item_casts_static_terrain_shadow(
			MissionData.KIND_ITEM, 0, 0, 0),
			"an ordinary pool-1 item is absent from the static pass")
	assert_false(MissionObjectPlacer.item_casts_static_terrain_shadow(
			MissionData.KIND_BUILDING, 0x01000000, 0, 0),
			"BMS NoShadow suppresses a pool-2 caster")
	assert_false(MissionObjectPlacer.item_casts_static_terrain_shadow(
			MissionData.KIND_BUILDING, 0, 0x04000000, 0),
			"ItemDef NoShadow suppresses a pool-2 caster")


func test_all_eligible_static_batch_reuses_its_visible_instance_as_caster() -> void:
	# An all-eligible batch carries both the ordinary world and static-caster
	# marker on its one visible MultiMesh: every slot casts into the scene
	# sun's CSM (ADR 0043), so no shadow-only duplicate is needed.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	assert_false(mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(4, 5, 6), Vector3.ZERO).is_empty())
	assert_true(mission.set_entity_property_int(
			MissionData.KIND_BUILDING, 0, "team", 1))
	assert_true(mission.set_entity_property_int(
			MissionData.KIND_BUILDING, 1, "team", 2))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("batched", -1)), 2)
	var container := parent.get_node_or_null("MissionObjects")
	var visible_batch := container.get_node_or_null("StaticPopulations/Batch_StaticCrate1_0") \
			as MultiMeshInstance3D
	assert_not_null(visible_batch)
	if visible_batch != null:
		assert_eq(visible_batch.layers,
				Water.VISUAL_LAYER_WORLD_NO_MIRROR \
				| Water.VISUAL_LAYER_STATIC_SHADOW_CASTER,
				"the visible batch joins the isolated static-caster layer; "
				+ "plain buildings stay out of the above-water mirror "
				+ "(no authored Reflective attribute)")
		assert_eq(visible_batch.cast_shadow,
				GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
				"the visible batch supplies the static silhouette")
		var expected_bms_ids: Array = []
		for index in range(2):
			expected_bms_ids.append(int(mission.get_entity(
					MissionData.KIND_BUILDING, index).get("bms_id", 0)))
		assert_eq(visible_batch.get_meta("static_shadow_bms_ids"),
				expected_bms_ids,
				"scratch attribution retains exact slot identity without changing geometry")
		assert_eq(visible_batch.get_meta("static_shadow_slots"), [true, true])
	assert_null(container.get_node_or_null("StaticPopulations/StaticShadow_StaticCrate1_0"),
			"an all-eligible batch needs no shadow-only duplicate")


func test_mixed_static_batch_keeps_a_filtered_shadow_only_duplicate() -> void:
	# Both instances are sector buildings (and therefore share reflection
	# admission), but the second carries the BMS NoShadow flag. A visible batch
	# cannot express that per-instance shadow difference, so this case still
	# needs a parallel MultiMesh with the ineligible slot zero-scaled.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	assert_false(mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(4, 5, 6), Vector3.ZERO).is_empty())
	assert_true(mission.set_entity_property_int(
			MissionData.KIND_BUILDING, 1, "ai_flags", 0x01000000),
			"the second building carries the witnessed BMS NoShadow gate")
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	placer.place(mission, parent)

	var container := parent.get_node_or_null("MissionObjects")
	var visible_batch := container.get_node_or_null("StaticPopulations/Batch_StaticCrate1_0") \
			as MultiMeshInstance3D
	var shadow_batch := container.get_node_or_null(
			"StaticPopulations/StaticShadow_StaticCrate1_0") as MultiMeshInstance3D
	assert_not_null(visible_batch)
	assert_not_null(shadow_batch,
			"mixed admission retains a filtered shadow-only batch")
	if visible_batch != null:
		assert_eq(visible_batch.layers, Water.VISUAL_LAYER_WORLD_NO_MIRROR,
				"plain buildings render outside the above-water mirror "
				+ "(reflection requires the authored Reflective attribute)")
		assert_eq(visible_batch.cast_shadow,
				GeometryInstance3D.SHADOW_CASTING_SETTING_OFF)
	if shadow_batch != null:
		assert_eq(shadow_batch.layers,
				Water.VISUAL_LAYER_STATIC_SHADOW_CASTER)
		assert_eq(shadow_batch.cast_shadow,
				GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY)
		if visible_batch != null:
			assert_ne(shadow_batch.multimesh, visible_batch.multimesh,
					"the filtered caster owns transforms independent of the visible batch")
		assert_eq(shadow_batch.multimesh.instance_count, 2,
				"slot identity stays parallel for destruction updates")


func test_shared_graphic_splits_authored_reflective_from_plain_reflection() -> void:
	# Godot MultiMeshes are grouped by graphic, but retail's above-water mirror
	# admits exactly the records whose entity carries flag 0x400: vehicles by
	# item type, otherwise the mission-authored BMS Reflective attribute
	# (shipped missions author it per record — CP01 has the same building
	# graphic placed both ways). A plain record that shares a graphic with an
	# authored-Reflective one must not hitchhike into the RTT.
	# [orig: Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b (BMS attrib
	# 0x800000 -> flags 0x400); Terrain_CollectVisibleEntitiesForReflection
	# @ 0x5c90a0 mask 0x400 above water -> collect_visible_sector_userpoints
	# @ 0x5c6c32 + collectors @ 0x5c6f20/@0x5c8c60; vehicle writer @ 0x40e208]
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var building_entity := mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(1, 2, 3), Vector3.ZERO)
	var item_entity := mission.add_entity(
			MissionData.KIND_ITEM, 105004,
			Vector3(4, 5, 6), Vector3.ZERO)
	assert_false(building_entity.is_empty())
	assert_false(item_entity.is_empty())
	assert_true(mission.set_entity_property_int(
			MissionData.KIND_BUILDING, 0, "ai_flags", 0x00800000),
			"the building authors the witnessed BMS Reflective attribute")
	var building_bms_id := int(building_entity.get("bms_id", 0))
	var item_bms_id := int(item_entity.get("bms_id", 0))
	assert_ne(building_bms_id, 0)
	assert_ne(item_bms_id, 0)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	placer.place(mission, parent)

	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	var building_batch := container.get_node_or_null(
			"StaticPopulations/Batch_StaticCrate1_Mirror_0") as MultiMeshInstance3D
	var item_batch := container.get_node_or_null(
			"StaticPopulations/Batch_StaticCrate1_NoMirror_0") as MultiMeshInstance3D
	assert_not_null(building_batch)
	assert_not_null(item_batch)
	assert_ne(building_batch, item_batch,
			"opposite retail populations cannot share one layer-masked MultiMesh")
	if building_batch != null:
		assert_ne(building_batch.layers & Water.VISUAL_LAYER_WORLD, 0,
				"the authored-Reflective record enters the mirror population")
		assert_eq(building_batch.multimesh.instance_count, 1,
				"only the authored record occupies the mirror-visible batch")
	if item_batch != null:
		assert_ne(item_batch.layers & Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
				"the plain record remains outside the above-water RTT")
		assert_eq(item_batch.layers & Water.VISUAL_LAYER_WORLD, 0)
		assert_eq(item_batch.multimesh.instance_count, 1,
				"only the plain record occupies the main-scene-only batch")
	var building_batch_key := placer.get_static_instance_batch_key(
			building_bms_id)
	var item_batch_key := placer.get_static_instance_batch_key(item_bms_id)
	assert_false(building_batch_key.is_empty())
	assert_false(item_batch_key.is_empty())
	assert_ne(building_batch_key, item_batch_key,
			"destruction routing retains the reflection-population split")
	var static_light_draws: Array = placer.get_static_light_draw_sources()
	assert_eq(static_light_draws.size(), 2,
			"each placed entity/ROBJ keeps an independent atlas row")
	var building_light_row := -1
	var item_light_row := -1
	for row_index in static_light_draws.size():
		var row: Dictionary = static_light_draws[row_index]
		if int(row.get("bms_id", 0)) == building_bms_id:
			building_light_row = row_index
		elif int(row.get("bms_id", 0)) == item_bms_id:
			item_light_row = row_index
	assert_gte(building_light_row, 0)
	assert_gte(item_light_row, 0)
	assert_ne(building_light_row, item_light_row)
	assert_false(placer.hide_static_instance(building_bms_id) == null)
	assert_true(placer.is_static_instance_hidden(building_bms_id))
	assert_false(placer.is_static_instance_hidden(item_bms_id),
			"carving the building population leaves its same-graphic item live")
	static_light_draws = placer.get_static_light_draw_sources()
	assert_false(bool(static_light_draws[building_light_row].get("active", true)),
			"the carved building no longer participates in atlas selection")
	assert_true(bool(static_light_draws[item_light_row].get("active", false)),
			"carving one population cannot darken its same-graphic peer")
	assert_true(placer.show_static_instance(building_bms_id))
	assert_false(placer.hide_static_instance(item_bms_id) == null)
	assert_true(placer.is_static_instance_hidden(item_bms_id))
	assert_false(placer.is_static_instance_hidden(building_bms_id),
			"carving the item population leaves its same-graphic building live")
	static_light_draws = placer.get_static_light_draw_sources()
	assert_true(bool(static_light_draws[building_light_row].get("active", false)))
	assert_false(bool(static_light_draws[item_light_row].get("active", true)))
	assert_true(placer.show_static_instance(item_bms_id))
	static_light_draws = placer.get_static_light_draw_sources()
	assert_true(bool(static_light_draws[building_light_row].get("active", false)))
	assert_true(bool(static_light_draws[item_light_row].get("active", false)),
			"restoring the batch re-admits its original stable atlas row")
	assert_true(placer.static_instance_is_mirror_reflected(building_bms_id),
			"destruction bookkeeping carries the authored reflection policy")
	assert_false(placer.static_instance_is_mirror_reflected(item_bms_id))

func test_authored_reflective_pool1_item_enters_the_mirror_population() -> void:
	# CP01 authors Reflective on plain pool-1 items too (five records), so the
	# attribute path must not be building-specific.
	# [orig: Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b maps the record
	# attribute for every spawned pool]
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			MissionData.KIND_ITEM, 105004,
			Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	assert_true(mission.set_entity_property_int(
			MissionData.KIND_ITEM, 0, "ai_flags", 0x00800000))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	placer.place(mission, parent)

	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	var batch := container.get_node_or_null("StaticPopulations/Batch_StaticCrate1_0") \
			as MultiMeshInstance3D
	assert_not_null(batch)
	if batch != null:
		assert_ne(batch.layers & Water.VISUAL_LAYER_WORLD, 0,
				"an authored-Reflective pool-1 item reflects above water")
		assert_eq(batch.layers & Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0)


func test_individual_building_gets_an_unmasked_static_shadow_sibling() -> void:
	# Portal buildings must keep their camera-driven ROBJ visibility on the
	# visible ObjectModel, while retail's tile pass independently submits
	# every ROBJ. Seed one harvested batch so the shadow-only sibling can be
	# asserted without shipping the retail GuardTwr asset.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			MissionData.KIND_BUILDING, 102001,
			Vector3(3, 4, 5), Vector3.ZERO).is_empty())
	assert_true(mission.set_entity_property_int(
			MissionData.KIND_BUILDING, 0, "ai_flags", 0x00800000),
			"this building authors the BMS Reflective attribute")
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var object_data := ObjectData.new()
	assert_true(placer.register_resolved_static_graphic(
			"GuardTwr1", object_data, [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("animated", -1)), 1,
			"the anim_def building remains an individual visible model")
	var container := parent.get_node_or_null("MissionObjects")
	var visible_model := container.get_node_or_null("Anim_GuardTwr1_0")
	assert_not_null(visible_model)
	if visible_model != null:
		assert_true(bool(visible_model.get("mirror_reflected")),
				"the authored Reflective attribute keeps an individual "
				+ "building in the mirror population "
				+ "[orig: Entity_SpawnFromBMSRecord @ 0x40ed1d..0x40ed2b]")
	var static_shadow := visible_model.get_node_or_null(
			"StaticShadow_GuardTwr1_live0_0") as MultiMeshInstance3D
	assert_not_null(static_shadow,
			"an independent all-section caster survives portal-mask changes")
	if static_shadow != null:
		assert_eq(static_shadow.cast_shadow,
				GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY)
		assert_eq(static_shadow.layers,
				Water.VISUAL_LAYER_STATIC_SHADOW_CASTER)
		assert_same(static_shadow.get_parent(), visible_model,
				"the unmasked caster follows editor moves and husk visibility lifecycle")
		var before := static_shadow.global_position
		visible_model.position += Vector3(7, 0, 0)
		assert_eq(static_shadow.global_position, before + Vector3(7, 0, 0),
				"moving the individual entity cannot strand its caster")


func test_runtime_vehicle_without_anim_def_stays_in_static_batch() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	assert_eq(item_db.get_item_type(106002), ItemDatabase.TYPE_VEHICLE)
	assert_true(item_db.get_anim_def(106002).is_empty(),
			"the fixture must exercise the no-anim vehicle policy")
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var parent := Node3D.new()
	add_child_autofree(parent)

	var mesh := BoxMesh.new()
	assert_true(placer.register_resolved_static_graphic(
			"StaticVehicle1", ObjectData.new(), [{
		"mesh": mesh, "material": null, "offset": Transform3D.IDENTITY, "submesh": 0,
	}]))

	assert_false(mission.add_entity(
			MissionData.KIND_ITEM, 106002, Vector3.ZERO,
			Vector3.ZERO).is_empty())
	var stats: Dictionary = placer.place(mission, parent)
	assert_eq(int(stats.get("placed", -1)), 1, "the vehicle is placed")
	assert_eq(int(stats.get("batched", -1)), 1,
			"a vehicle without anim_def uses static batching")
	assert_eq(int(stats.get("animated", -1)), 0,
			"the vehicle does not enter runtime presentation")
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container.get_node_or_null("StaticPopulations/Batch_StaticVehicle1_0"))


func test_godot_to_bms_position_axis_remap() -> void:
	assert_eq(MissionObjectPlacer.godot_to_bms_position(Vector3(1, 2, 3)), Vector3(1, -3, 2))


# --- Live-PANM graphics must not freeze into static batches ----------------------
# A decoration whose .3di carries a live PANM track (free-running wave/spin, SET pose,
# or a control-register binding) must place as an individual ObjectModel even
# though items.def gives it no anim_def: a MultiMesh batch captures the rest pose once
# and never evaluates PANM again, while the engine re-poses PANM from the global clock
# every rendered frame [orig: PANM_SampleTrack @0x5b2270 idle gate, clock
# Render_ShaderTickMs @0x2721A40]. DFX2's "Oil Pump" (graphic pump,
# type decoration, control-0x32 sine tracks) is the witnessed case. Inert PANM
# blocks (armory as shipped: entries
# present, every control idle) must keep the perf-tier static batching.

const ARMRY_3DI := "res://../fixtures/threedi/synth/armory.3di"
const PMPJK_3DI := "res://../fixtures/threedi/synth/pump.3di"


func _panm_data(live: bool) -> ObjectData:
	var data := ObjectData.new()
	var path := PMPJK_3DI if live else ARMRY_3DI
	if data.open_file(_abs(path)) != OK:
		return null
	return data


# The classification seams replace the old script-subclass harness (native
# methods cannot be overridden from GDScript): the authored fixture data is
# cache-injected for the graphic and the occlusion verdict is pre-filled so
# the PANM rule is isolated from the fixture's independent portal payload.
func _panm_placer(live: bool) -> MissionObjectPlacer:
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	# item 105004: type object, no anim_def
	placer.register_object_data("StaticCrate1", _panm_data(live))
	placer.register_occlusion_verdict(105004, false)
	return placer


func test_place_routes_live_panm_graphic_to_a_live_model() -> void:
	var placer := _panm_placer(true)
	assert_not_null(placer.object_data_for("StaticCrate1"), "fixture data authored with one live PANM track")
	if placer.object_data_for("StaticCrate1") == null:
		return
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
		MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("animated", -1)), 1,
		"a live-PANM graphic places as an individual animated model")
	assert_eq(int(stats.get("batched", -1)), 0,
		"and never enters a static MultiMesh batch")
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return
	var live_model: Node3D = null
	for child in container.get_children():
		if String(child.name).begins_with("Anim_"):
			live_model = child as Node3D
	assert_not_null(live_model, "the placed node is a live Anim_ model")


func test_place_keeps_inert_panm_graphic_in_static_batches() -> void:
	# armory as shipped has idle PANM plus an independent OOBJ portal payload.
	# The harness suppresses that second classifier here so this test isolates
	# the rule that inert PANM alone does not defeat static batching.
	var placer := _panm_placer(false)
	assert_not_null(placer.object_data_for("StaticCrate1"), "fixture data loads")
	if placer.object_data_for("StaticCrate1") == null:
		return
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
		MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	# Seed batch geometry so the static branch can render without a resource-root .3di.
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", placer.object_data_for("StaticCrate1"), [{
		"mesh": BoxMesh.new(), "material": null,
		"offset": Transform3D.IDENTITY, "submesh": 0,
	}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("batched", -1)), 1,
		"inert PANM keeps the static MultiMesh batching")
	assert_eq(int(stats.get("animated", -1)), 0,
		"and does not force a live model")


func test_occlusion_records_take_precedence_over_inert_panm_batching() -> void:
	var placer := _panm_placer(false)
	assert_not_null(placer.object_data_for("StaticCrate1"), "fixture data loads")
	if placer.object_data_for("StaticCrate1") == null:
		return
	assert_true(placer.object_data_for("StaticCrate1").has_occlusion(),
		"fixture carries the portal payload that requires per-section visibility")
	placer.register_occlusion_verdict(105004, true)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
		MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("animated", -1)), 1,
		"portal sections require an individual model even when PANM is inert")
	assert_eq(int(stats.get("batched", -1)), 0,
		"the per-instance section mask cannot be represented by a MultiMesh batch")


func test_inert_panm_model_retains_robj_base_instead_of_rederiving() -> void:
	var data := _panm_data(false)
	assert_not_null(data, "inert PANM fixture loads")
	if data == null:
		return
	assert_false(data.has_live_panm_for_lod(0), "fixture PANM is exactly inert")
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_object_data(data)
	var parts: Dictionary = model.get_render_part_nodes()
	assert_gt(parts.size(), 0, "rebuild applies the authored ROBJ base pose")

	# Poison every part node: an inert model must RETAIN the base pose the
	# rebuild wrote — nothing may re-derive (and so rewrite) it per frame.
	var poison := Transform3D(Basis(), Vector3(123.0, 456.0, 789.0))
	for key in parts.keys():
		(parts[key] as Node3D).transform = poison
	model.advance_runtime_frame(0.016)
	model.advance_runtime_frame(0.016)
	for key in parts.keys():
		assert_eq((parts[key] as Node3D).transform, poison,
				"inert ROBJ transforms are retained instead of re-derived per frame")

	# An exact runtime mutator forces one defensive refresh, but an inert
	# pose does not depend on registers — the derived pose is unchanged, so
	# the write gate stays shut and no per-frame churn restarts.
	model.set_ctrl_value("VEHICLE_SPECIAL1", 123)
	model.advance_runtime_frame(0.016)
	for key in parts.keys():
		assert_eq((parts[key] as Node3D).transform, poison,
				"the mutation refresh does not turn back into continuous work")


func test_live_panm_model_keeps_evaluating_robj_each_frame() -> void:
	var data := _panm_data(true)
	assert_not_null(data, "live PANM fixture loads")
	if data == null:
		return
	assert_true(data.has_live_panm_for_lod(0), "fixture carries a live PANM track")
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_object_data(data)
	var clock := PanmClock.new()
	clock.set_time_ms_for_test(0)
	model.set_panm_clock(clock)
	var parts: Dictionary = model.get_render_part_nodes()
	var targets := data.get_effective_panm_targets(0)
	assert_gt(targets.size(), 0, "the immutable fixture identifies a live ROBJ")
	if targets.is_empty():
		return
	# An authored PANM block may mix inert parents with live children. Select the
	# retained ROBJ whose immutable track actually moves over the sampled clock
	# interval instead of assuming the first effective node is the live one.
	var pose_400: Dictionary = data.evaluate_panm(0, 400, {})
	var pose_800: Dictionary = data.evaluate_panm(0, 800, {})
	var target := -1
	for candidate in targets:
		var part_index := int(candidate)
		if pose_400.get(part_index) != pose_800.get(part_index):
			target = part_index
			break
	assert_ne(target, -1, "the immutable fixture has a clock-driven ROBJ")
	if target == -1:
		return
	assert_true(parts.has(target), "the authored live track has a retained ROBJ")
	var part := parts[target] as Node3D

	# The authored slide sweeps rotation over time: each visible frame must
	# re-derive part transforms from the absolute clock.
	var poison := Transform3D(Basis(), Vector3(123.0, 456.0, 789.0))
	part.transform = poison
	clock.set_time_ms_for_test(400)
	model.advance_runtime_frame(0.016)
	assert_ne(part.transform, poison,
			"live time/register PANM re-derives ROBJ transforms on the frame")
	var first := part.transform
	part.transform = poison
	clock.set_time_ms_for_test(800)
	model.advance_runtime_frame(0.016)
	assert_ne(part.transform, poison, "and again on the next frame")
	assert_ne(part.transform, first,
			"the sweep advances with the clock, not a retained pose")


# --- Per-instance RLOD selection inside the static bins ---------------------------
# Retail selects the RLOD per entity in its sector walk; batching is a device-era
# mechanism (D-RORD-2). A multi-RLOD graphic therefore stays batched: every
# authored level is emitted as its own population over the same slot list, and
# only the populations of an instance's selected level carry its live transform
# [engine: renderer::select_object_lod, object_lod_frame_scale,
# project_bound_sphere_radius_q16 and kObjectLodSubPixelCullQ16].


## The static populations the placer emitted: every child of the container's
## StaticPopulations holder (the container's own children are the models).
func _populations(container: Node) -> Array[Node]:
	var holder := container.get_node_or_null("StaticPopulations")
	return holder.get_children() if holder != null else []


func _lod_population(container: Node, population_name: String) -> MultiMeshInstance3D:
	var mmi := container.get_node_or_null("StaticPopulations/" + population_name) \
			as MultiMeshInstance3D
	assert_not_null(mmi, "population %s is emitted" % population_name)
	return mmi


func _live_populations(placer: MissionObjectPlacer, bms_id: int) -> Array:
	# Headless Godot stores no MultiMesh instance data (readback is identity),
	# so the slot state is pinned through the placer's typed bookkeeping.
	var out: Array = placer.get_static_instance_live_populations(bms_id)
	out.sort()
	return out


func test_multi_lod_static_selects_its_rlod_per_instance_inside_the_bin() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	# Godot positions (10, 0, 10) and (10, 0, 200): one 512-unit bin.
	var first := mission.add_entity(
			MissionData.KIND_BUILDING, 105004, Vector3(10, -10, 0), Vector3.ZERO)
	var second := mission.add_entity(
			MissionData.KIND_BUILDING, 105004, Vector3(10, -200, 0), Vector3.ZERO)
	assert_false(first.is_empty())
	assert_false(second.is_empty())
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var fine := BoxMesh.new()
	fine.size = Vector3(4, 4, 4)
	var coarse := BoxMesh.new()
	coarse.size = Vector3(3, 3, 3)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": fine, "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0, "lod_index": 0,
			}, {
				"mesh": coarse, "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 1, "lod_index": 1,
			}], {
				# Fine to coarse; row 0 is unused, row 1 = 20 px in Q16.16.
				"thresholds_q16": PackedInt32Array([0, 20 << 16]),
				"sphere_radius": 2.0,
			}))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("animated", -1)), 0,
			"a multi-RLOD graphic never becomes an individual model on its own")
	assert_eq(int(stats.get("batched", -1)), 2)
	assert_eq(int(stats.get("static_instances_retained", -1)), 2)
	assert_eq(int(stats.get("static_lod_populations", -1)), 1,
			"one extra population carries the coarser level")
	assert_eq(int(stats.get("batches", -1)), 2)
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return
	var level0 := _lod_population(container, "Batch_StaticCrate1_0")
	var level1 := _lod_population(container, "Batch_StaticCrate1_1")
	if level0 == null or level1 == null:
		return
	assert_eq(int(level0.get_meta("static_batch_lod", -1)), 0)
	assert_eq(int(level1.get_meta("static_batch_lod", -1)), 1)
	assert_eq(level0.multimesh.instance_count, 2,
			"every population of the bin holds the same slot list")
	assert_eq(level1.multimesh.instance_count, 2)
	var first_bms := int(first.get("bms_id", 0))
	var second_bms := int(second.get("bms_id", 0))
	assert_eq(placer.get_static_instance_binding_count(first_bms), 2,
			"one slot per level population")
	assert_eq(_live_populations(placer, first_bms), ["Batch_StaticCrate1_0"],
			"before the first camera frame every slot is live at level 0 only")
	assert_eq(_live_populations(placer, second_bms), ["Batch_StaticCrate1_0"])
	assert_eq(placer.get_static_instance_lod(first_bms), 0)

	# 640x480 at 70 deg vertical: focal 343 px, frame scale 2.0. The near
	# entity projects to ~34 px (level 0); the far one to ~3.3 px, which
	# scaled (6.5 px) sits at or below the 20 px row: level 1.
	var near_camera := Transform3D(Basis.IDENTITY, Vector3(10, 0, 220))
	assert_eq(placer.update_static_lods(near_camera, 70.0, 640.0, 480.0), 1,
			"exactly one instance crosses to the coarser level")
	assert_eq(placer.get_static_instance_lod(first_bms), 1)
	assert_eq(placer.get_static_instance_lod(second_bms), 0)
	assert_eq(_live_populations(placer, first_bms), ["Batch_StaticCrate1_1"],
			"the far slot moved from the fine population to the coarse one")
	assert_eq(_live_populations(placer, second_bms), ["Batch_StaticCrate1_0"],
			"the near slot stays fine")
	assert_eq(placer.update_static_lods(near_camera, 70.0, 640.0, 480.0), 0,
			"an unchanged frame rewrites nothing")

	# Moving the camera beside the far entity brings it back to level 0; the
	# near entity is now behind the camera and keeps its level untouched.
	var far_camera := Transform3D(Basis.IDENTITY, Vector3(10, 0, 30))
	assert_eq(placer.update_static_lods(far_camera, 70.0, 640.0, 480.0), 1)
	assert_eq(placer.get_static_instance_lod(first_bms), 0)
	assert_eq(placer.get_static_instance_lod(second_bms), 0)
	assert_eq(_live_populations(placer, first_bms), ["Batch_StaticCrate1_0"])
	assert_eq(_live_populations(placer, second_bms), ["Batch_StaticCrate1_0"])

	# Retail's sub-pixel floor: at ~0.14 px neither instance is drawn at any
	# level (render_sector_entity's 0.75 px gate, ported as the engine's
	# kObjectLodSubPixelCullQ16).
	var distant_camera := Transform3D(Basis.IDENTITY, Vector3(10, 0, 5010))
	assert_eq(placer.update_static_lods(distant_camera, 70.0, 640.0, 480.0), 2)
	assert_eq(placer.get_static_instance_lod(first_bms), -1)
	assert_eq(placer.get_static_instance_lod(second_bms), -1)
	assert_eq(_live_populations(placer, first_bms), [])
	assert_eq(_live_populations(placer, second_bms), [])

	# Destruction carves the slot in every level population and restores it at
	# the level last selected, re-evaluated by the next frame.
	assert_eq(placer.update_static_lods(far_camera, 70.0, 640.0, 480.0), 1)
	assert_eq(placer.get_static_instance_lod(first_bms), 0)
	assert_eq(_live_populations(placer, first_bms), ["Batch_StaticCrate1_0"])
	assert_true(placer.hide_static_instance(first_bms) is Transform3D)
	assert_eq(_live_populations(placer, first_bms), [],
			"the carve empties every level population")
	# The near entity (culled while behind the camera) re-enters at level 0;
	# the carved one is not evaluated and keeps its stored level and no slot.
	assert_eq(placer.update_static_lods(near_camera, 70.0, 640.0, 480.0), 1,
			"only the near entity re-enters; a carved instance is not evaluated")
	assert_eq(placer.get_static_instance_lod(second_bms), 0)
	assert_eq(placer.get_static_instance_lod(first_bms), 0,
			"the carved instance's stored level is untouched")
	assert_eq(_live_populations(placer, first_bms), [])
	assert_true(placer.show_static_instance(first_bms))
	assert_eq(_live_populations(placer, first_bms), ["Batch_StaticCrate1_0"],
			"restored at its last selected level")
	assert_eq(placer.update_static_lods(near_camera, 70.0, 640.0, 480.0), 1,
			"the next frame re-evaluates the restored instance")
	assert_eq(_live_populations(placer, first_bms), ["Batch_StaticCrate1_1"])


func test_multi_lod_document_harvests_every_level_into_the_bins() -> void:
	# The real harvest: an inert-PANM document with more than one authored
	# RLOD (the occlusion verdict is pre-filled so only the RLOD rule is under
	# test) emits one population per level and stays out of the individual
	# count.
	var placer := _panm_placer(false)
	var data := placer.object_data_for("StaticCrate1")
	assert_not_null(data, "fixture data loads")
	if data == null:
		return
	var lod_count := int(data.get_summary().get("lod_count", 0))
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("animated", -1)), 0,
			"authored RLODs alone never force an individual model")
	assert_eq(int(stats.get("batched", -1)), 1)
	assert_eq(int(stats.get("static_instances_retained", -1)), 1)
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return
	var levels := {}
	for child in _populations(container):
		var mmi := child as MultiMeshInstance3D
		if mmi != null and mmi.has_meta("static_batch_lod"):
			levels[int(mmi.get_meta("static_batch_lod"))] = true
	assert_true(levels.has(0), "level 0 populations are emitted")
	if lod_count > 1:
		assert_true(levels.has(1), "the coarser authored level is emitted too")
		assert_gt(int(stats.get("static_lod_populations", 0)), 0)
	else:
		assert_eq(int(stats.get("static_lod_populations", -1)), 0)


# --- Dense populations: only live rows reach the GPU and the cull -------------
# A population carries rows only for the slots at its level, packed [0, live)
# with visible_instance_count = live; a level switch swap-removes the slot from
# the old level's population (the last live row fills the hole) and appends it
# to the new one; a population with no live row is hidden so Godot's cull tree
# skips it. Headless Godot stores no MultiMesh instance data, so the row order
# is pinned through the placer's bookkeeping (get_static_population_live_bms_ids)
# and the device rows windowed.

const DENSE_NEAR_CAMERA := Transform3D(Basis.IDENTITY, Vector3(10, 0, 220))
const DENSE_MID_CAMERA := Transform3D(Basis.IDENTITY, Vector3(10, 0, 30))
const DENSE_DISTANT_CAMERA := Transform3D(Basis.IDENTITY, Vector3(10, 0, 5010))


func _dense_lod_switches(placer: MissionObjectPlacer, camera: Transform3D) -> int:
	return placer.update_static_lods(camera, 70.0, 640.0, 480.0)


# Three same-graphic buildings in one 512-unit bin at Godot z = 10 / 100 /
# 200 (BMS y = -10 / -100 / -200), a two-level graphic (row 1 = 20 px). The
# second one optionally carries the BMS NoShadow gate so the level
# populations need a filtered shadow twin.
func _dense_fixture(parent: Node3D, no_shadow_second: bool) -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var bms_ids: Array[int] = []
	for y in [-10.0, -100.0, -200.0]:
		var entity := mission.add_entity(
				MissionData.KIND_BUILDING, 105004, Vector3(10, y, 0), Vector3.ZERO)
		assert_false(entity.is_empty())
		bms_ids.append(int(entity.get("bms_id", 0)))
	if no_shadow_second:
		assert_true(mission.set_entity_property_int(
				MissionData.KIND_BUILDING, 1, "ai_flags", 0x01000000))
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var fine := BoxMesh.new()
	fine.size = Vector3(4, 4, 4)
	var coarse := BoxMesh.new()
	coarse.size = Vector3(3, 3, 3)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": fine, "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0, "lod_index": 0,
			}, {
				"mesh": coarse, "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 1, "lod_index": 1,
			}], {
				"thresholds_q16": PackedInt32Array([0, 20 << 16]),
				"sphere_radius": 2.0,
			}))
	var stats: Dictionary = placer.place(mission, parent)
	return {
		"placer": placer,
		"mission": mission,
		"stats": stats,
		"bms": bms_ids,
		"container": parent.get_node_or_null("MissionObjects"),
	}


func _live_bms(placer: MissionObjectPlacer, population: MultiMeshInstance3D) -> Array:
	return Array(placer.get_static_population_live_bms_ids(population))


func test_dense_population_packs_only_live_rows_and_hides_empty_levels() -> void:
	var parent := Node3D.new()
	add_child_autofree(parent)
	var fixture := _dense_fixture(parent, false)
	var placer: MissionObjectPlacer = fixture.placer
	var bms: Array[int] = fixture.bms
	var container: Node3D = fixture.container
	assert_not_null(container)
	if container == null:
		return
	var level0 := _lod_population(container, "Batch_StaticCrate1_0")
	var level1 := _lod_population(container, "Batch_StaticCrate1_1")
	if level0 == null or level1 == null:
		return
	assert_eq(level0.multimesh.instance_count, 3,
			"capacity is the bin's slot count")
	assert_eq(level1.multimesh.instance_count, 3)
	assert_eq(_live_bms(placer, level0), [bms[0], bms[1], bms[2]],
			"before the first camera frame the level-0 population holds every "
			+ "slot in slot order")
	assert_eq(_live_bms(placer, level1), [],
			"the coarser level starts with no live row")
	assert_true(level0.visible)
	assert_false(level1.visible,
			"a population with no live row is hidden from the cull")
	assert_eq(int(fixture.stats.get("static_live_populations", -1)), 1)
	assert_eq(placer.get_static_live_population_count(), 1)

	# 640x480 at 70 deg vertical: the z = 10 and z = 100 entities project to
	# 6.5 px and 11.4 px (level 1); the z = 200 one to ~34 px (level 0).
	assert_eq(_dense_lod_switches(placer, DENSE_NEAR_CAMERA), 2)
	assert_eq(_live_bms(placer, level0), [bms[2]],
			"swap-remove: the last live row (the third entity) filled the first "
			+ "hole, then the second entity's row was the last and simply shrank")
	assert_eq(_live_bms(placer, level1), [bms[0], bms[1]],
			"appended in switch order")
	assert_true(level1.visible, "the first append shows the population")
	assert_true(level0.visible)
	assert_eq(placer.get_static_live_population_count(), 2)
	assert_eq(_live_populations(placer, bms[0]), ["Batch_StaticCrate1_1"])
	assert_eq(_live_populations(placer, bms[1]), ["Batch_StaticCrate1_1"])
	assert_eq(_live_populations(placer, bms[2]), ["Batch_StaticCrate1_0"])
	assert_eq(placer.get_static_instance_binding_count(bms[0]), 2,
			"a slot per level population, live or not")

	# Beside the first entity (the other two are behind the camera and keep
	# their level): the first entity returns to level 0; the second entity's
	# row moves into its vacated level-1 row 0.
	assert_eq(_dense_lod_switches(placer, DENSE_MID_CAMERA), 1)
	assert_eq(_live_bms(placer, level1), [bms[1]])
	assert_eq(_live_bms(placer, level0), [bms[2], bms[0]])

	# Retail's sub-pixel floor drops every instance from every level: both
	# populations empty and hide.
	assert_eq(_dense_lod_switches(placer, DENSE_DISTANT_CAMERA), 3)
	assert_eq(_live_bms(placer, level0), [])
	assert_eq(_live_bms(placer, level1), [])
	assert_false(level0.visible)
	assert_false(level1.visible)
	assert_eq(placer.get_static_live_population_count(), 0)
	assert_eq(level0.multimesh.instance_count, 3,
			"the capacity never changes; only the live count does")


func test_dense_population_carve_and_restore_follow_the_compaction() -> void:
	var parent := Node3D.new()
	add_child_autofree(parent)
	var fixture := _dense_fixture(parent, false)
	var placer: MissionObjectPlacer = fixture.placer
	var bms: Array[int] = fixture.bms
	var container: Node3D = fixture.container
	if container == null:
		return
	var level0 := _lod_population(container, "Batch_StaticCrate1_0")
	var level1 := _lod_population(container, "Batch_StaticCrate1_1")
	if level0 == null or level1 == null:
		return
	assert_eq(_dense_lod_switches(placer, DENSE_NEAR_CAMERA), 2)
	assert_eq(_dense_lod_switches(placer, DENSE_MID_CAMERA), 1)
	assert_eq(_live_bms(placer, level0), [bms[2], bms[0]])
	assert_eq(_live_bms(placer, level1), [bms[1]])

	# Carving the third entity (row 0 of level 0) moves the first entity's
	# row into the hole; its remap is what the later carve of the first
	# entity must follow.
	assert_true(placer.hide_static_instance(bms[2]) is Transform3D)
	assert_eq(_live_bms(placer, level0), [bms[0]])
	assert_eq(_live_populations(placer, bms[2]), [])
	assert_eq(_live_populations(placer, bms[0]), ["Batch_StaticCrate1_0"],
			"the moved slot is still live in the population it was moved within")
	assert_true(placer.hide_static_instance(bms[0]) is Transform3D)
	assert_eq(_live_bms(placer, level0), [],
			"the remapped row was removed, not the stale one")
	assert_false(level0.visible, "the emptied population hides")
	assert_eq(_live_bms(placer, level1), [bms[1]],
			"a carve never disturbs another population")
	assert_true(placer.show_static_instance(bms[0]))
	assert_true(placer.show_static_instance(bms[2]))
	assert_eq(_live_bms(placer, level0), [bms[0], bms[2]],
			"restored at the last selected level, appended in restore order")
	assert_true(level0.visible)
	# Carving the level-1 entity empties and hides that population; the
	# restore re-shows it and the next frame re-evaluates it like any other.
	assert_true(placer.hide_static_instance(bms[1]) is Transform3D)
	assert_eq(_live_bms(placer, level1), [])
	assert_false(level1.visible)
	assert_true(placer.show_static_instance(bms[1]))
	assert_eq(_live_bms(placer, level1), [bms[1]])
	assert_true(level1.visible)
	assert_eq(_dense_lod_switches(placer, DENSE_NEAR_CAMERA), 1,
			"only the first entity (level 0, now far) crosses; the second "
			+ "already sits at level 1")
	assert_eq(_live_bms(placer, level0), [bms[2]])
	assert_eq(_live_bms(placer, level1), [bms[1], bms[0]])


func test_dense_shadow_twin_and_shadow_row_map_follow_the_compaction() -> void:
	var parent := Node3D.new()
	add_child_autofree(parent)
	var fixture := _dense_fixture(parent, true)
	var placer: MissionObjectPlacer = fixture.placer
	var bms: Array[int] = fixture.bms
	var container: Node3D = fixture.container
	if container == null:
		return
	var level0 := _lod_population(container, "Batch_StaticCrate1_0")
	var level1 := _lod_population(container, "Batch_StaticCrate1_1")
	var shadow0 := _lod_population(container, "StaticShadow_StaticCrate1_0")
	var shadow1 := _lod_population(container, "StaticShadow_StaticCrate1_1")
	if level0 == null or level1 == null or shadow0 == null or shadow1 == null:
		return
	assert_eq(_live_bms(placer, shadow0), [bms[0], bms[2]],
			"the shadow twin carries rows only for the slots that cast")
	assert_eq(_live_bms(placer, shadow1), [])
	assert_false(shadow1.visible)
	assert_eq(Array(level0.get_meta("static_shadow_rows")), [0, 1, 2],
			"the row -> slot map starts in slot order")
	assert_eq(Array(shadow0.get_meta("static_shadow_rows")), [0, 2],
			"the twin's rows name the casting slots")
	assert_eq(level0.get_meta("static_shadow_bms_ids"), [bms[0], bms[1], bms[2]],
			"the slot identity arrays stay slot-ordered")
	assert_eq(int(fixture.stats.get("static_live_populations", -1)), 2)

	assert_eq(_dense_lod_switches(placer, DENSE_NEAR_CAMERA), 2)
	assert_eq(_live_bms(placer, level0), [bms[2]])
	assert_eq(_live_bms(placer, level1), [bms[0], bms[1]])
	assert_eq(_live_bms(placer, shadow0), [bms[2]],
			"the twin swap-removed the first entity")
	assert_eq(_live_bms(placer, shadow1), [bms[0]],
			"the NoShadow entity never enters a twin")
	assert_true(shadow1.visible)
	assert_eq(Array(level0.get_meta("static_shadow_rows")), [2])
	assert_eq(Array(level1.get_meta("static_shadow_rows")), [0, 1])
	assert_eq(Array(shadow0.get_meta("static_shadow_rows")), [2])
	assert_eq(Array(shadow1.get_meta("static_shadow_rows")), [0])
	assert_eq(_live_populations(placer, bms[0]), ["Batch_StaticCrate1_1"],
			"the live-population read-back names visible populations only")


func _device_multimesh_rows_available() -> bool:
	return DisplayServer.get_name() != "headless" and \
			RenderingServer.get_current_rendering_method() == "forward_plus"


func test_dense_population_device_rows_match_the_bookkeeping() -> void:
	if not _device_multimesh_rows_available():
		pending("MultiMesh instance rows need a windowed Forward+ run")
		return
	var parent := Node3D.new()
	add_child_autofree(parent)
	var fixture := _dense_fixture(parent, false)
	var placer: MissionObjectPlacer = fixture.placer
	var bms: Array[int] = fixture.bms
	var container: Node3D = fixture.container
	if container == null:
		return
	var level0 := _lod_population(container, "Batch_StaticCrate1_0")
	var level1 := _lod_population(container, "Batch_StaticCrate1_1")
	if level0 == null or level1 == null:
		return
	var expected := {}
	for index in range(3):
		expected[bms[index]] = MissionObjectPlacer.entity_transform(
				Vector3(10, [-10.0, -100.0, -200.0][index], 0), Vector3.ZERO)
	var check := func(population: MultiMeshInstance3D, label: String) -> void:
		var rows := _live_bms(placer, population)
		assert_eq(population.multimesh.visible_instance_count, rows.size(),
				"%s draws exactly its live rows" % label)
		for row in range(rows.size()):
			var device: Transform3D = population.multimesh.get_instance_transform(row)
			assert_true(device.is_equal_approx(expected[rows[row]]),
					"%s row %d carries the entity it is booked for" % [label, row])
			assert_gt(absf(device.basis.determinant()), 0.5,
					"no zero-scaled row inside the live range")
	check.call(level0, "level 0 at placement")
	assert_eq(level1.multimesh.visible_instance_count, 0)
	assert_eq(_dense_lod_switches(placer, DENSE_NEAR_CAMERA), 2)
	check.call(level0, "level 0 after the switch")
	check.call(level1, "level 1 after the switch")
	assert_eq(_dense_lod_switches(placer, DENSE_MID_CAMERA), 1)
	check.call(level0, "level 0 after the return")
	check.call(level1, "level 1 after the return")
	assert_true(placer.hide_static_instance(bms[2]) is Transform3D)
	check.call(level0, "level 0 after the carve")
	assert_true(placer.show_static_instance(bms[2]))
	check.call(level0, "level 0 after the restore")
