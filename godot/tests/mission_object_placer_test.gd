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

	var stats := placer.place(mission, parent)

	assert_not_null(parent.get_node_or_null("MissionObjects"), "MissionObjects container is created")
	assert_not_null(stats, "place returns the placement census")
	assert_eq(stats.placed, 0, "no .3di in fixtures -> nothing placed")
	assert_gt(stats.unresolved, 0, "real entities recorded as unresolved")


func test_place_is_a_noop_on_null_inputs() -> void:
	var placer := MissionObjectPlacer.new()
	var parent := Node3D.new()
	add_child_autofree(parent)
	var stats := placer.place(null, parent)
	assert_eq(stats.placed, 0, "null mission places nothing")
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
	assert_not_null(record)
	var index := record.index
	var stats := placer.place(mission, parent)
	assert_eq(stats.placed, 1, "the static entity is placed")
	assert_eq(stats.batched, 1, "via the static-batch branch")
	assert_eq(stats.batches, 1, "one draw group for its single submesh")
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

	# Native source identities, order and detached snapshots are covered by
	# mission_static_sources; effect/light GUT tests exercise this placer's consumers.


func test_static_batches_partition_opaque_geometry_but_keep_blended_global() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var first := mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(10, -10, 0), Vector3.ZERO)
	var second := mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(530, -10, 0), Vector3.ZERO)
	assert_not_null(first)
	assert_not_null(second)
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

	var stats := placer.place(mission, parent)

	assert_eq(stats.static_bins, 2,
			"opaque populations follow the terrain's 512-unit sector cells")
	assert_eq(stats.static_binned_batches, 2)
	assert_eq(stats.static_global_batches, 1,
			"the blended strip remains one explicitly global population")
	assert_eq(stats.batches, 3)
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return
	var binned: Array[StaticPopulationInstance] = []
	var global: StaticPopulationInstance = null
	for child in _populations(container):
		var mmi := child as StaticPopulationInstance
		if mmi == null:
			continue
		if mmi.population_kind == StaticPopulationInstance.POPULATION_BIN:
			binned.append(mmi)
		else:
			global = mmi
	assert_eq(binned.size(), 2)
	assert_not_null(global)
	if global != null:
		assert_eq(global.multimesh.instance_count, 2,
				"the global blended population retains both placements")
		assert_eq(global.population_kind, StaticPopulationInstance.POPULATION_GLOBAL)
	var bin_xs: Array[int] = []
	for mmi in binned:
		var bin_x := mmi.bin_x
		bin_xs.append(bin_x)
		assert_eq(mmi.bin_z, 0)
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

	var first_bms_id := first.bms_id
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

	assert_not_null(mission.add_entity(
			MissionData.KIND_ITEM, 106002, Vector3(1, 2, 3),
			Vector3.ZERO))
	var stats := placer.place(mission, parent)
	assert_eq(stats.placed, 1, "the static vehicle places")
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
	assert_true(MissionObjectPlacer.item_casts_dynamic_shadow(ItemDatabase.TYPE_PERSON, 0),
			"people always receive a retail shadow render slot")
	assert_true(MissionObjectPlacer.item_casts_dynamic_shadow(ItemDatabase.TYPE_VEHICLE, 0x10),
			"DynamicShadow admits a non-person model")
	assert_false(MissionObjectPlacer.item_casts_dynamic_shadow(ItemDatabase.TYPE_BUILDING, 0),
			"portal/static buildings never become silhouette casters")
	assert_true(MissionObjectPlacer.item_casts_dynamic_shadow(ItemDatabase.TYPE_PERSON, 0x10),
			"the witnessed dynamic-slot allocator reads only attrib2 (never ItemDef NoShadow)")


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
	# The reimpl's static light reaches only the terrain receiver layer, so an
	# all-eligible batch can carry both the ordinary world and static-caster
	# marker without self-shadowing. This avoids one duplicate MultiMesh per
	# submesh while preserving the visible draw.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_not_null(mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(1, 2, 3), Vector3.ZERO))
	assert_not_null(mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(4, 5, 6), Vector3.ZERO))
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

	var stats := placer.place(mission, parent)

	assert_eq(stats.batched, 2)
	var container := parent.get_node_or_null("MissionObjects")
	var visible_batch := container.get_node_or_null("StaticPopulations/Batch_StaticCrate1_0") \
			as StaticPopulationInstance
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
			expected_bms_ids.append(mission.get_entity_ref(
					MissionData.KIND_BUILDING, index).bms_id)
		assert_eq(Array(visible_batch.slot_bms_ids),
				expected_bms_ids,
				"scratch attribution retains exact slot identity without changing geometry")
		assert_eq(Array(visible_batch.slot_casts_shadow), [1, 1])
	assert_null(container.get_node_or_null("StaticPopulations/StaticShadow_StaticCrate1_0"),
			"an all-eligible batch needs no shadow-only duplicate")


func test_mixed_static_batch_keeps_a_filtered_shadow_only_duplicate() -> void:
	# Both instances are sector buildings (and therefore share reflection
	# admission), but the second carries the BMS NoShadow flag. A visible batch
	# cannot express that per-instance shadow difference, so this case still
	# needs a parallel MultiMesh with the ineligible slot zero-scaled.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_not_null(mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(1, 2, 3), Vector3.ZERO))
	assert_not_null(mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(4, 5, 6), Vector3.ZERO))
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
			as StaticPopulationInstance
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
	# @ 0x5c90a0 mask 0x400 above water -> Terrain_CollectVisibleSectorUserpoints
	# @ 0x5c6c32 + collectors @ 0x5c6f20/@0x5c8c60; vehicle writer @ 0x40e208]
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var building_entity := mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(1, 2, 3), Vector3.ZERO)
	var item_entity := mission.add_entity(
			MissionData.KIND_ITEM, 105004,
			Vector3(4, 5, 6), Vector3.ZERO)
	assert_not_null(building_entity)
	assert_not_null(item_entity)
	assert_true(mission.set_entity_property_int(
			MissionData.KIND_BUILDING, 0, "ai_flags", 0x00800000),
			"the building authors the witnessed BMS Reflective attribute")
	var building_bms_id := building_entity.bms_id
	var item_bms_id := item_entity.bms_id
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
	assert_false(placer.hide_static_instance(building_bms_id) == null)
	assert_true(placer.is_static_instance_hidden(building_bms_id))
	assert_false(placer.is_static_instance_hidden(item_bms_id),
			"carving the building population leaves its same-graphic item live")
	assert_true(placer.show_static_instance(building_bms_id))
	assert_false(placer.hide_static_instance(item_bms_id) == null)
	assert_true(placer.is_static_instance_hidden(item_bms_id))
	assert_false(placer.is_static_instance_hidden(building_bms_id),
			"carving the item population leaves its same-graphic building live")
	assert_true(placer.show_static_instance(item_bms_id))
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
	assert_not_null(mission.add_entity(
			MissionData.KIND_ITEM, 105004,
			Vector3(1, 2, 3), Vector3.ZERO))
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
	assert_not_null(mission.add_entity(
			MissionData.KIND_BUILDING, 102001,
			Vector3(3, 4, 5), Vector3.ZERO))
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

	var stats := placer.place(mission, parent)

	assert_eq(stats.animated, 1,
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

	assert_not_null(mission.add_entity(
			MissionData.KIND_ITEM, 106002, Vector3.ZERO,
			Vector3.ZERO))
	var stats := placer.place(mission, parent)
	assert_eq(stats.placed, 1, "the vehicle is placed")
	assert_eq(stats.batched, 1,
			"a vehicle without anim_def uses static batching")
	assert_eq(stats.animated, 0,
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
# g_RenderShaderTickMs @0x2721A40]. DFX2's "Oil Pump" (graphic pump,
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
	assert_not_null(mission.add_entity(
		MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats := placer.place(mission, parent)

	assert_eq(stats.animated, 1,
		"a live-PANM graphic places as an individual animated model")
	assert_eq(stats.batched, 0,
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
	assert_not_null(mission.add_entity(
		MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO))
	# Seed batch geometry so the static branch can render without a resource-root .3di.
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", placer.object_data_for("StaticCrate1"), [{
		"mesh": BoxMesh.new(), "material": null,
		"offset": Transform3D.IDENTITY, "submesh": 0,
	}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats := placer.place(mission, parent)

	assert_eq(stats.batched, 1,
		"inert PANM keeps the static MultiMesh batching")
	assert_eq(stats.animated, 0,
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
	assert_not_null(mission.add_entity(
		MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats := placer.place(mission, parent)

	assert_eq(stats.animated, 1,
		"portal sections require an individual model even when PANM is inert")
	assert_eq(stats.batched, 0,
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


func _lod_population(container: Node, population_name: String) -> StaticPopulationInstance:
	var mmi := container.get_node_or_null("StaticPopulations/" + population_name) \
			as StaticPopulationInstance
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
	assert_not_null(first)
	assert_not_null(second)
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
				# Fine to coarse; slot i is level i's own threshold (retail
				# Model_SelectRlodLevel @0x5c3b3b): level 0 above 20 px, level 1 below.
				"thresholds_q16": PackedInt32Array([20 << 16, 0]),
				"sphere_radius": 2.0,
			}))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats := placer.place(mission, parent)

	assert_eq(stats.animated, 0,
			"a multi-RLOD graphic never becomes an individual model on its own")
	assert_eq(stats.batched, 2)
	assert_eq(stats.static_instances_retained, 2)
	assert_eq(stats.static_lod_populations, 1,
			"one extra population carries the coarser level")
	assert_eq(stats.batches, 2)
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return
	var level0 := _lod_population(container, "Batch_StaticCrate1_0")
	var level1 := _lod_population(container, "Batch_StaticCrate1_1")
	if level0 == null or level1 == null:
		return
	assert_eq(level0.lod_index, 0)
	assert_eq(level1.lod_index, 1)
	assert_eq(level0.multimesh.instance_count, 2,
			"every population of the bin holds the same slot list")
	assert_eq(level1.multimesh.instance_count, 2)
	var first_bms := first.bms_id
	var second_bms := second.bms_id
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
	# level (Render_SectorEntity's 0.75 px gate, ported as the engine's
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

	# The frame scale reads the object detail the view draws at (game.cfg's
	# object_polydetail, the session copy; engine renderer/object_lod.h): 50 u
	# off, the entity projects to ~13.7 px, which detail 3 (x2.0) lifts past the
	# 20 px row to level 0 and detail 2 (x1.0) leaves at level 1.
	var detail_camera := Transform3D(Basis.IDENTITY, Vector3(10, 0, 60))
	placer.update_static_lods(detail_camera, 70.0, 640.0, 480.0, 3)
	assert_eq(placer.get_static_instance_lod(first_bms), 0)
	placer.update_static_lods(detail_camera, 70.0, 640.0, 480.0, 2)
	assert_eq(placer.get_static_instance_lod(first_bms), 1,
			"detail 2 draws the coarser level from half the distance")
	placer.update_static_lods(detail_camera, 70.0, 640.0, 480.0)
	assert_eq(placer.get_static_instance_lod(first_bms), 0,
			"the scripted seam's default is detail 3")


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
	var lod_count := data.get_lod_count()
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_not_null(mission.add_entity(
			MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO))
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats := placer.place(mission, parent)

	assert_eq(stats.animated, 0,
			"authored RLODs alone never force an individual model")
	assert_eq(stats.batched, 1)
	assert_eq(stats.static_instances_retained, 1)
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return
	var levels := {}
	for child in _populations(container):
		var mmi := child as StaticPopulationInstance
		if mmi != null:
			levels[mmi.lod_index] = true
	assert_true(levels.has(0), "level 0 populations are emitted")
	if lod_count > 1:
		assert_true(levels.has(1), "the coarser authored level is emitted too")
		assert_gt(stats.static_lod_populations, 0)
	else:
		assert_eq(stats.static_lod_populations, 0)


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
# 200 (BMS y = -10 / -100 / -200), a two-level graphic (level 0 above 20 px). The
# second one optionally carries the BMS NoShadow gate so the level
# populations need a filtered shadow twin.
func _dense_fixture(parent: Node3D, no_shadow_second: bool) -> Dictionary:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var bms_ids: Array[int] = []
	for y in [-10.0, -100.0, -200.0]:
		var entity := mission.add_entity(
				MissionData.KIND_BUILDING, 105004, Vector3(10, y, 0), Vector3.ZERO)
		assert_not_null(entity)
		bms_ids.append(entity.bms_id)
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
				"thresholds_q16": PackedInt32Array([20 << 16, 0]),
				"sphere_radius": 2.0,
			}))
	var stats := placer.place(mission, parent)
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
	assert_eq(fixture.stats.static_live_populations, 1)
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


func test_occlusion_verdict_drops_a_batched_static_at_every_level() -> void:
	# A batched static has no ObjectModel, so the occlusion frame's collector
	# and building-batch verdicts land on its retained instance: a culled
	# instance carries no row at any level until released (retail
	# Terrain_CollectVisibleEntities_0 @0x5c7022..0x5c708a / @0x5c7118..0x5c7162,
	# Terrain_CollectVisibleSectorUserpoints @0x5c6cd1..0x5c6d0d).
	var parent := Node3D.new()
	add_child_autofree(parent)
	var fixture := _dense_fixture(parent, false)
	var placer: MissionObjectPlacer = fixture.placer
	var bms: Array[int] = fixture.bms
	_dense_lod_switches(placer, DENSE_NEAR_CAMERA)
	var first_level := placer.get_static_instance_lod(bms[0])
	var second_level := placer.get_static_instance_lod(bms[1])
	assert_true(first_level >= 0, "the fixture instance draws before any verdict")
	assert_true(second_level >= 0)

	placer.set_static_instance_occlusion_hidden(bms[0], true)
	_dense_lod_switches(placer, DENSE_NEAR_CAMERA)
	assert_eq(placer.get_static_instance_lod(bms[0]), -1, "a culled static draws at no level")
	assert_eq(_live_populations(placer, bms[0]), [], "every level population drops its row")
	assert_eq(placer.get_static_instance_lod(bms[1]), second_level,
			"the verdict is per instance")

	placer.set_static_instance_occlusion_hidden(bms[0], false)
	_dense_lod_switches(placer, DENSE_NEAR_CAMERA)
	assert_eq(placer.get_static_instance_lod(bms[0]), first_level,
			"a released instance re-selects its level on the next walk")

	placer.set_static_instance_occlusion_hidden(bms[1], true)
	_dense_lod_switches(placer, DENSE_NEAR_CAMERA)
	assert_eq(placer.get_static_instance_lod(bms[1]), -1)
	placer.clear_static_instance_occlusion()
	_dense_lod_switches(placer, DENSE_NEAR_CAMERA)
	assert_eq(placer.get_static_instance_lod(bms[1]), second_level,
			"the unload/A-B release clears every verdict")


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
	assert_eq(Array(level0.row_slots), [0, 1, 2],
			"the row -> slot map starts in slot order")
	assert_eq(Array(shadow0.row_slots), [0, 2],
			"the twin's rows name the casting slots")
	assert_eq(Array(level0.slot_bms_ids), [bms[0], bms[1], bms[2]],
			"the slot identity arrays stay slot-ordered")
	assert_eq(fixture.stats.static_live_populations, 2)

	assert_eq(_dense_lod_switches(placer, DENSE_NEAR_CAMERA), 2)
	assert_eq(_live_bms(placer, level0), [bms[2]])
	assert_eq(_live_bms(placer, level1), [bms[0], bms[1]])
	assert_eq(_live_bms(placer, shadow0), [bms[2]],
			"the twin swap-removed the first entity")
	assert_eq(_live_bms(placer, shadow1), [bms[0]],
			"the NoShadow entity never enters a twin")
	assert_true(shadow1.visible)
	assert_eq(Array(level0.row_slots), [2])
	assert_eq(Array(level1.row_slots), [0, 1])
	assert_eq(Array(shadow0.row_slots), [2])
	assert_eq(Array(shadow1.row_slots), [0])
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


func test_static_and_live_placement_share_initialized_entity_light_radius() -> void:
	var db := ItemDatabase.new()
	assert_eq(db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_abs("res://../fixtures/threedi/synth")), OK)
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, "crate.3di"), OK)
	var parent := Node3D.new()
	add_child_autofree(parent)
	# crate.3di's authored GHDR sphere is 80265 Q16. The second existing
	# definition has scale 1.5: RHU(80265 * 1.5) + 4096 = 124494.
	for definition in [[105004, "StaticCrate1", 84361], [106103, "pump", 124494]]:
		var placer := MissionObjectPlacer.create(root, db)
		assert_true(placer.register_resolved_static_graphic(definition[1], data, [{
			"mesh": BoxMesh.new(), "material": null,
			"offset": Transform3D.IDENTITY, "submesh": 0,
		}]))
		var mission := MissionData.new()
		assert_eq(mission.create_default(), OK)
		assert_not_null(mission.add_entity(MissionData.KIND_ITEM,
				definition[0], Vector3.ZERO, Vector3.ZERO))
		var branch := Node3D.new()
		parent.add_child(branch)
		var stats := placer.place(mission, branch)
		assert_eq(stats.batched, 1)
		var director := EffectLightDirector.new()
		director.setup(null, placer)
		director.reattach()
		var static_scene := director.scene()
		var static_radius := float(definition[2]) / 65536.0
		for offset in [-0.01, 0.01]:
			assert_gt(static_scene.spawn_model_light(ModelLightSpawn.make(
					Vector3(static_radius + offset, 0.0, 0.0), 0.001)), 0)
		var camera := Camera3D.new()
		branch.add_child(camera)
		director.render_frame(camera, GameWorld.current_frame_clock_ms())
		assert_almost_eq(static_scene.get_static_light_rows_image().get_pixel(0, 0).r,
				1.0, 0.001, "static selection uses the authored scale and entity pad")
		var live_placer := MissionObjectPlacer.create(root, db)
		live_placer.register_object_data(definition[1], data)
		live_placer.register_occlusion_verdict(definition[0], true)
		var live_branch := Node3D.new()
		parent.add_child(live_branch)
		var live_stats := live_placer.place(mission, live_branch)
		assert_eq(live_stats.animated, 1)
		var live_models := live_placer.get_placed_models()
		assert_eq(live_models.size(), 1)
		if live_models.is_empty():
			continue
		var live: Node3D = live_models[0]
		var scene := LightScene.new()
		var radius := float(definition[2]) / 65536.0
		for offset in [-0.01, 0.01]:
			assert_gt(scene.spawn_model_light(ModelLightSpawn.make(
					Vector3(radius + offset, 0.0, 0.0), 0.001)), 0)
		var models: Array[Node3D] = [live]
		assert_eq(scene.render_model_frame(models, PackedInt64Array([0]),
				PackedInt64Array([0]), PackedInt32Array([0]), PackedByteArray([0]),
				Vector3.ONE, 0, null), 1)
		var surfaces: Array[Node] = live.find_children("*", "GeometryInstance3D", true, false)
		assert_gt(surfaces.size(), 0)
		if not surfaces.is_empty():
			assert_eq(float((surfaces[0] as GeometryInstance3D).get_instance_shader_parameter(
					"u_point_light_count")), 1.0,
					"the live query has the same padded radius as its static source")


# ADR 0046 S14: the editor moves a retained static in place (move_static_instance).
# Its rows stay where they are in their populations (the packing and the
# swap-remove order stand), its transform reads back (get_static_instance_transform),
# its level's sphere moves with it (the next LOD walk evaluates it where it is now)
# and the population's bounds grow to hold it; a bms id that is no retained static
# is refused; a carved instance moves too and shows again where it went.
func test_move_static_instance_rewrites_rows_in_place() -> void:
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
	var placed: Variant = placer.get_static_instance_transform(bms[1])
	assert_eq(placed, MissionObjectPlacer.entity_transform(Vector3(10, -100, 0), Vector3.ZERO),
			"the read-back is the entity transform the placement made")
	assert_null(placer.get_static_instance_transform(999999), "no retained static: null")
	assert_false(placer.move_static_instance(999999, Transform3D.IDENTITY), "no retained static: refused")

	# The second building moved beside the third (BMS y -100 to -200, turned 90):
	# the same rows in the same order, the transform the new one, the bounds grown.
	var moved := MissionObjectPlacer.entity_transform(Vector3(10, -200, 3), Vector3(0, 90, 0))
	var before := level0.custom_aabb
	var level1_before := level1.custom_aabb
	assert_true(placer.move_static_instance(bms[1], moved))
	assert_eq(placer.get_static_instance_transform(bms[1]), moved)
	assert_eq(_live_bms(placer, level0), [bms[0], bms[1], bms[2]], "the rows stand as packed")
	assert_eq(_live_bms(placer, level1), [])
	assert_eq(_live_populations(placer, bms[1]), ["Batch_StaticCrate1_0"])
	assert_eq(placer.get_static_instance_binding_count(bms[1]), 2, "its bindings stand")
	assert_true(level0.custom_aabb.encloses(before), "the bounds only grow")
	assert_true(level0.custom_aabb.has_point(moved.origin), "and hold the row where it is now")
	# Every population it may draw in grows with it, live or not: a later level switch (or a show)
	# appends it there, inside the bounds the population advertises (review M6).
	assert_true(level1.custom_aabb.encloses(level1_before), "the level 1 bounds only grow")
	assert_true(level1.custom_aabb.has_point(moved.origin), "and hold it before it switches there")

	# The LOD walk evaluates it where it is now: at the near camera the third
	# building (z 200) stays at level 0, and so does the second one beside it;
	# only the first switches.
	assert_eq(_dense_lod_switches(placer, DENSE_NEAR_CAMERA), 1)
	assert_eq(_live_bms(placer, level1), [bms[0]])
	assert_eq(_live_bms(placer, level0), [bms[2], bms[1]],
			"swap-remove: the third filled the first one's hole; the moved one stayed last")
	assert_eq(_live_populations(placer, bms[1]), ["Batch_StaticCrate1_0"])
	# A move while a row sits after a swap-remove: still in place, still right.
	var nudged := MissionObjectPlacer.entity_transform(Vector3(11, -200, 3), Vector3(0, 90, 0))
	assert_true(placer.move_static_instance(bms[1], nudged))
	assert_eq(placer.get_static_instance_transform(bms[1]), nudged)
	assert_eq(_live_bms(placer, level0), [bms[2], bms[1]])
	assert_eq(placer.get_static_instance_transform(bms[2]),
			MissionObjectPlacer.entity_transform(Vector3(10, -200, 0), Vector3.ZERO),
			"the other rows keep their transforms")

	# A carved instance moves with no live row, and shows again where it went.
	placer.hide_static_instance(bms[2])
	assert_eq(_live_bms(placer, level0), [bms[1]])
	var far := MissionObjectPlacer.entity_transform(Vector3(10, -150, 0), Vector3.ZERO)
	assert_true(placer.move_static_instance(bms[2], far))
	assert_eq(placer.get_static_instance_transform(bms[2]), far)
	assert_eq(_live_bms(placer, level0), [bms[1]], "carved: no row to rewrite")
	assert_true(placer.show_static_instance(bms[2]))
	assert_eq(_live_bms(placer, level0), [bms[1], bms[2]])
	assert_eq(placer.get_static_instance_transform(bms[2]), far)
	assert_true(level0.custom_aabb.has_point(far.origin), "hidden, moved, shown: the bounds held it all along")

	# A far move out of its 512-unit bin (700 m east): each level's bounds hold it, live or not, so the
	# level switch the camera makes later never draws it outside the advertised bounds.
	var away := MissionObjectPlacer.entity_transform(Vector3(700, -200, 3), Vector3.ZERO)
	assert_true(placer.move_static_instance(bms[1], away))
	assert_true(level0.custom_aabb.has_point(away.origin), "level 0 holds it where it went")
	assert_true(level1.custom_aabb.has_point(away.origin), "and level 1, before any switch")


# ADR 0046 S14: a graphic's static batches warmed before a placement names it
# (warm_static_graphic): a graphic whose batches are registered or resolve is
# warm, one that resolves to nothing is not, and an empty name never.
func test_warm_static_graphic_caches_the_batches() -> void:
	var parent := Node3D.new()
	add_child_autofree(parent)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	assert_false(placer.warm_static_graphic("", parent), "no name")
	assert_false(placer.warm_static_graphic("NoSuchGraphic", parent), "nothing resolves")
	var mesh := BoxMesh.new()
	assert_true(placer.register_resolved_static_graphic("StaticCrate1", ObjectData.new(), [{
		"mesh": mesh, "material": null, "offset": Transform3D.IDENTITY, "submesh": 0,
	}]))
	assert_true(placer.warm_static_graphic("StaticCrate1", parent), "registered: warm")
	assert_eq(parent.get_child_count(), 0, "a warm graphic harvests nothing under the parent")


# ADR 0046 S14 (decision D5): the placement a unit at a time (MissionPlacementRun) comes to what
# place() places whole, over the same walk: the rows bucketed, a unit per static group, the animated
# models four a unit, the finish; nothing is placed until its units ran, the census comes with the
# last; a run begun after it on the placer cancels it (its next step does nothing, no census).
func test_a_stepped_placement_is_the_whole_placement() -> void:
	var whole_parent := Node3D.new()
	add_child_autofree(whole_parent)
	var whole := _dense_fixture(whole_parent, false)
	var whole_stats: MissionPlacementStats = whole["stats"]
	assert_eq(whole_stats.placed, 3, "the fixture places its three statics whole")

	var parent := Node3D.new()
	add_child_autofree(parent)
	var mission: MissionData = whole["mission"]
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
				"thresholds_q16": PackedInt32Array([20 << 16, 0]),
				"sphere_radius": 2.0,
			}))
	var run := placer.begin_place(mission, parent)
	assert_not_null(run)
	assert_false(run.is_done(), "begun, not done")
	assert_eq(run.get_steps_done(), 0)
	assert_eq(run.get_step_label(), "bucket", "the rows bucketed first")
	assert_eq(run.get_step_count(), 2, "one bucket unit and the finish, before the groups are known")
	assert_null(run.get_stats(), "no census before the end")
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container, "the container stands as the run begins")
	var labels: Array = []
	var step := MissionPlacementRun.STEP_MORE
	while step == MissionPlacementRun.STEP_MORE:
		labels.append(run.get_step_label())
		step = run.step()
	assert_eq(step, MissionPlacementRun.STEP_DONE)
	assert_eq(labels, ["bucket", "statics", "finish"], "a unit per static group, no animated model")
	assert_true(run.is_done())
	assert_false(run.is_cancelled())
	assert_eq(run.get_steps_done(), 3)
	assert_eq(run.get_step_count(), 3)
	assert_eq(run.get_step_label(), "")
	assert_eq(run.step(), MissionPlacementRun.STEP_DONE, "done: nothing left to step")
	# The same census, the same populations.
	var stats := run.get_stats()
	assert_not_null(stats)
	for field in ["placed", "batched", "animated", "unresolved", "graphics", "batches", "markers",
			"static_bins", "static_binned_batches", "static_global_batches",
			"static_instances_retained", "static_lod_populations", "static_live_populations",
			"static_shadow_batches"]:
		assert_eq(stats.get(field), whole_stats.get(field), "%s as the whole placement's" % field)
	assert_not_null(container.get_node_or_null("StaticPopulations/Batch_StaticCrate1_0"))
	assert_not_null(container.get_node_or_null("StaticPopulations/Batch_StaticCrate1_1"))
	assert_eq(placer.get_static_live_population_count(), whole["placer"].get_static_live_population_count())
	for bms_id in whole["bms"]:
		assert_eq(placer.get_static_instance_lod(bms_id), whole["placer"].get_static_instance_lod(bms_id))

	# A run begun after another cancels it: the older run's step does nothing and it is done with no
	# census; the newer runs to its end.
	var first := placer.begin_place(mission, parent)
	assert_eq(first.step(), MissionPlacementRun.STEP_MORE)
	var second := placer.begin_place(mission, parent)
	assert_true(second.get_generation() > first.get_generation())
	assert_eq(first.step(), MissionPlacementRun.STEP_DONE, "cancelled by the newer run")
	assert_true(first.is_cancelled() and first.is_done())
	assert_null(first.get_stats())
	while second.step() == MissionPlacementRun.STEP_MORE:
		pass
	assert_false(second.is_cancelled())
	assert_eq(second.get_stats().placed, 3)

	# No mission: done at once with an empty census, no container made.
	var bare_parent := Node3D.new()
	add_child_autofree(bare_parent)
	var empty := placer.begin_place(null, bare_parent)
	assert_eq(empty.get_step_label(), "finish")
	assert_eq(empty.step(), MissionPlacementRun.STEP_DONE)
	assert_eq(empty.get_stats().placed, 0)
	assert_null(bare_parent.get_node_or_null("MissionObjects"))


# S14 review m13: the stepped run is the whole placement over a mission that crosses its unit sizes:
# 600 statics of one graphic (two bucket units of 512 rows) and 6 individual models (two models units
# of four): the labels in order, the same census and as many individual models as place() makes.
func _crossing_placer(root: ResourceRoot, item_db: ItemDatabase, data: ObjectData) -> MissionObjectPlacer:
	var placer := MissionObjectPlacer.create(root, item_db)
	assert_true(placer.register_resolved_static_graphic("StaticCrate1", data, [{
		"mesh": BoxMesh.new(), "material": null, "offset": Transform3D.IDENTITY, "submesh": 0,
	}]))
	placer.register_object_data("pump", data)
	placer.register_occlusion_verdict(106103, true)
	return placer


func test_a_stepped_placement_crossing_its_units_is_the_whole_placement() -> void:
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_abs("res://../fixtures/threedi/synth")), OK)
	var data := ObjectData.new()
	assert_eq(data.open_from_resource_root(root, "crate.3di"), OK)
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	for i in 600:
		assert_not_null(mission.add_entity(MissionData.KIND_ITEM, 105004, Vector3(float(i % 30) * 40.0, float(i / 30) * 40.0, 0.0),
				Vector3.ZERO))
	for i in 6:
		assert_not_null(mission.add_entity(MissionData.KIND_ITEM, 106103, Vector3(float(i) * 10.0, -50.0, 0.0), Vector3.ZERO))
	var whole_parent := Node3D.new()
	add_child_autofree(whole_parent)
	var whole := _crossing_placer(root, item_db, data)
	var whole_stats := whole.place(mission, whole_parent)
	assert_eq(whole_stats.batched, 600)
	assert_eq(whole_stats.animated, 6)
	var parent := Node3D.new()
	add_child_autofree(parent)
	var placer := _crossing_placer(root, item_db, data)
	var run := placer.begin_place(mission, parent)
	var labels: Array = []
	var step := MissionPlacementRun.STEP_MORE
	while step == MissionPlacementRun.STEP_MORE:
		var label := run.get_step_label()
		if labels.is_empty() or labels[labels.size() - 1] != label:
			labels.append(label)
		step = run.step()
	assert_eq(labels, ["bucket", "statics", "models", "finish"])
	assert_eq(run.get_steps_done(), 2 + 1 + 2 + 1, "two buckets of 512 rows, one static group, two models units of four")
	var stats := run.get_stats()
	for field in ["placed", "batched", "animated", "unresolved", "graphics", "batches", "markers",
			"static_bins", "static_binned_batches", "static_global_batches",
			"static_instances_retained", "static_lod_populations", "static_live_populations",
			"static_shadow_batches"]:
		assert_eq(stats.get(field), whole_stats.get(field), "%s as the whole placement's" % field)
	assert_eq(placer.get_placed_models().size(), whole.get_placed_models().size())
	assert_eq(placer.get_placed_models().size(), 6)


# ADR 0046 S14: the transform the placement draws an item's entity at (item_entity_transform): the
# entity transform for an item of no scale, scaled by the item's `scale` (106103, 1.5) otherwise;
# what the editor's moves hand move_static_instance and an individual model's node.
func test_item_entity_transform_carries_the_item_scale() -> void:
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	var position := Vector3(10, -40, 3)
	var rotation := Vector3(0, 90, 0)
	var plain := MissionObjectPlacer.entity_transform(position, rotation)
	assert_eq(placer.item_entity_transform(position, rotation, 106100), plain, "no scale: the entity transform")
	var scaled := placer.item_entity_transform(position, rotation, 106103)
	assert_eq(scaled.origin, plain.origin)
	assert_true(scaled.basis.is_equal_approx(plain.basis.scaled(Vector3(1.5, 1.5, 1.5))), "scale 1.5")