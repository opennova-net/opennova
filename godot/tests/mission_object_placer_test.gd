extends GutTest

# Phase 4: MissionObjectPlacer. Covers the asset-free pieces that must be exactly
# right (BMS -> Godot coordinate conversion, cross-checked against the equivalent
# Basis) and the graceful resolution-miss path (fixtures ship items.def but no
# .3di, so nothing resolves to a model and the placer must place zero without
# error). Full render-placement is validated against real assets out-of-band.


const BMS_PATH := "res://../fixtures/bms/ash_i5b.reference.bms"
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


# --- Phase 3: incremental placement (place_single) ----------------------------
# place_single renders one freshly-added entity into an existing container without
# rebuilding the world. Asset-free coverage: the unresolved path (fixtures ship no
# .3di, so a placed item resolves a graphic but no model) and the null guards. Real
# render-placement is validated against assets out-of-band, like place() above.

func test_place_single_reports_unresolved_when_no_model_resolves() -> void:
	# Item 101291 carries an anim_def, so this exercises the ANIMATED branch's unresolved
	# path (no .3di -> no ObjectData). The static branch is covered separately below.
	var mission := MissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))  # items.def but no .3di

	var placer := MissionObjectPlacer.create(root, item_db)
	placer.edit_mode = true
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)  # builds the MissionObjects container
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container, "the container exists to place into")

	# Add a real entity, then render just that one.
	var record := mission.add_entity(MissionData.KIND_ITEM, 101291, Vector3(1, 2, 3), Vector3.ZERO)
	var index := int(record["index"])
	var pickable_before := placer.pickable_records.size()
	var delta: Dictionary = placer.place_single(mission, container, MissionData.KIND_ITEM, index)

	assert_eq(int(delta.get("unresolved", 0)), 1, "a graphic with no .3di reports unresolved")
	assert_eq(int(delta.get("placed", -1)), 0, "and places nothing")
	assert_eq(placer.pickable_records.size(), pickable_before, "an unrendered entity adds no pickable record")


func test_place_single_static_branch_reports_unresolved_without_a_model() -> void:
	# Item 105004 "Static Crate" is type object with no anim_def, so it takes the STATIC
	# branch. With no .3di and no seeded batch cache it resolves no geometry and must
	# report unresolved without recording a pickable.
	var mission := MissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	placer.edit_mode = true
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")

	var record := mission.add_entity(MissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO)
	var index := int(record["index"])
	var pickable_before := placer.pickable_records.size()
	var delta: Dictionary = placer.place_single(mission, container, MissionData.KIND_ITEM, index)

	assert_eq(int(delta.get("unresolved", 0)), 1, "a static graphic with no .3di reports unresolved")
	assert_eq(int(delta.get("placed", -1)), 0, "and places nothing")
	assert_eq(placer.pickable_records.size(), pickable_before, "an unrendered static adds no pickable record")


func test_place_single_static_branch_builds_a_single_instance_batch() -> void:
	# The static success branch (a single-instance MultiMesh + the pickable record that
	# later select / drag depend on) is the load-bearing new code. Exercise it asset-free
	# by pre-seeding the per-graphic batch cache with a dummy mesh, so place_single
	# renders without a real .3di. Full render fidelity is validated against real assets
	# out-of-band, like place() itself.
	var mission := MissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	placer.edit_mode = true
	var env_state := EnvLightState.new()
	var dry_values := EnvLightValues.retail_noon_defaults()
	var dry_fog := Vector3(0.71, 0.18, 0.33)
	dry_values.fog_color = dry_fog
	env_state.publish(dry_values)
	placer.set_environment_state(env_state)
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")

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

	var record := mission.add_entity(MissionData.KIND_ITEM, 105004, Vector3(3, 4, 5), Vector3.ZERO)
	var index := int(record["index"])
	var pickable_before := placer.pickable_records.size()
	var delta: Dictionary = placer.place_single(mission, container, MissionData.KIND_ITEM, index)

	assert_eq(int(delta.get("placed", -1)), 1, "the static entity is placed")
	assert_eq(int(delta.get("batched", -1)), 1, "via the static-batch branch")
	assert_eq(int(delta.get("batches", -1)), 1, "one draw group for its single submesh")
	assert_eq(placer.pickable_records.size(), pickable_before + 1, "it appends exactly one pickable record")

	var rec: Dictionary = placer.pickable_records.back()
	assert_eq(int(rec["kind"]), MissionData.KIND_ITEM)
	assert_eq(int(rec["index"]), index, "the record points back at the placed entity")
	assert_eq(int(rec["slot"]), 0, "a single-instance batch uses slot 0")
	assert_false(bool(rec["animated"]))
	var mmi := rec["mmi"] as MultiMeshInstance3D
	assert_eq(mmi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
			"retail static batches receive dynamic silhouettes but never cast them")
	assert_ne(mmi.layers & Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
			"a batched non-vehicle item stays in the separately filtered entity "
			+ "population [orig: Water_RenderReflectedWorldScene @ 0x5c857b -> "
			+ "Terrain_RenderSectorEntities; collection wrapper @ 0x5c90a0 -> "
			+ "collectors @ 0x5c6f20/@0x5c8c60; flag writer @ 0x40e208]")
	assert_eq(mmi.layers & Water.VISUAL_LAYER_WORLD, 0,
			"presentation batching does not promote an item into the sector-building pass")
	var mm: MultiMesh = rec["mm"]
	assert_eq(mm.instance_count, 1, "the new static gets its own single-instance MultiMesh")
	assert_true((rec["mmi"] as MultiMeshInstance3D).is_inside_tree(), "the batch instance is in the container")
	assert_eq(rec["mesh_aabb"], mesh.get_aabb(), "the pick AABB is the batch mesh's bounds")
	# The record carries the batch offset the drag path composes with the entity transform
	# (_apply_selected_xform writes mm.set_instance_transform(slot, _selected_xform * offset)).
	# The rendered instance transform itself can't be asserted headless: the dummy
	# RenderingServer does not persist MultiMesh instance transforms (set/get_instance_transform
	# round-trips to identity), which is also why the drag/commit tests assert the mission
	# record rather than the MultiMesh. Render fidelity is validated against real assets
	# out-of-band; here the placed entity's position is already pinned by the controller tests.
	assert_eq(rec["offset"], offset, "the record carries the batch offset the drag path rewrites through")
	placer.update_environment()
	var batch_fog: Variant = batch_material.get_shader_parameter("u_fog_color")
	assert_not_null(batch_fog,
			"registered production-equivalent batches retain their material for relighting")
	if batch_fog == null:
		return
	assert_true(Vector3(batch_fog)
			.is_equal_approx(dry_fog),
			"the retained static-batch material consumes the shared environment")
	var underwater_values := EnvLightValues.retail_noon_defaults()
	var underwater_fog := Vector3(0.03, 0.14, 0.08)
	underwater_values.fog_color = underwater_fog
	underwater_values.fog_end = 24.0
	underwater_values.fog_type = 1
	env_state.publish(underwater_values)
	placer.update_environment()
	assert_true(Vector3(batch_material.get_shader_parameter("u_fog_color"))
			.is_equal_approx(underwater_fog),
			"pose refresh can restamp a static batch to the selected underwater payload")
	assert_almost_eq(float(batch_material.get_shader_parameter("u_fog_end")), 24.0, 0.001)
	assert_eq(int(batch_material.get_shader_parameter("u_fog_type")), 1)

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
	# Getter rows are copies; callers cannot rewrite the placer's retained identity.
	source["item_id"] = 0
	assert_eq(int(placer.get_static_item_effect_sources()[0].get("item_id", 0)), 105004)


func test_place_single_static_vehicle_rides_the_mirror_visible_layer() -> void:
	# env #30: the water mirror's above-water collection keeps only
	# ItemDefType==vehicle entities [orig: Entity_InitFromModel @ 0x40e20a
	# entity+36 |= 0x400; Terrain_CollectVisibleEntitiesForReflection
	# @ 0x5c90a0 filterMask 0x400]. Fixture 106002 "Static Vehicle" is the
	# type=vehicle twin of the crate case above.
	var mission := MissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	placer.edit_mode = true
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")

	var mesh := BoxMesh.new()
	mesh.size = Vector3(2, 1, 4)
	assert_true(placer.register_resolved_static_graphic(
			"StaticVehicle1", ObjectData.new(), [{
		"mesh": mesh, "material": null,
		"offset": Transform3D.IDENTITY, "submesh": 0,
	}]))

	var record := mission.add_entity(
			MissionData.KIND_ITEM, 106002, Vector3(1, 2, 3), Vector3.ZERO)
	var delta: Dictionary = placer.place_single(
			mission, container, MissionData.KIND_ITEM, int(record["index"]))
	assert_eq(int(delta.get("placed", -1)), 1, "the static vehicle places")

	var rec: Dictionary = placer.pickable_records.back()
	var mmi := rec["mmi"] as MultiMeshInstance3D
	assert_ne(mmi.layers & Water.VISUAL_LAYER_WORLD, 0,
			"a type=vehicle entity stays on the mirror-visible world layer")
	assert_eq(mmi.layers & Water.VISUAL_LAYER_WORLD_NO_MIRROR, 0,
			"the vehicle batch never rides the no-mirror layer")


func test_place_single_static_caster_reuses_its_visible_instance() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	placer.edit_mode = true
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	var record := mission.add_entity(
			MissionData.KIND_BUILDING, 105004,
			Vector3(3, 4, 5), Vector3.ZERO)

	var delta: Dictionary = placer.place_single(
			mission, container, MissionData.KIND_BUILDING,
			int(record["index"]))

	assert_eq(int(delta.get("batched", -1)), 1)
	var visible_batch := placer.pickable_records.back()["mmi"] \
			as MultiMeshInstance3D
	assert_eq(visible_batch.layers,
			Water.VISUAL_LAYER_WORLD_NO_MIRROR \
			| Water.VISUAL_LAYER_STATIC_SHADOW_CASTER,
			"the one visible draw also enters the isolated static-caster pass; "
			+ "a building without the authored Reflective attribute stays out "
			+ "of the above-water mirror [orig: Entity_SpawnFromBMSRecord "
			+ "@ 0x40ed1d..0x40ed2b; collector mask @ 0x5c90a0]")
	assert_eq(visible_batch.cast_shadow,
			GeometryInstance3D.SHADOW_CASTING_SETTING_ON)
	assert_null(container.get_node_or_null(
			"StaticShadow_StaticCrate1_k%d_i%d_s0" % [
				MissionData.KIND_BUILDING, int(record["index"])]),
			"place_single avoids a second node referencing the same MultiMesh")


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
	# The reimpl's static light reaches only the terrain receiver layer, so an
	# all-eligible batch can carry both the ordinary world and static-caster
	# marker without self-shadowing. This avoids one duplicate MultiMesh per
	# submesh while preserving the visible draw.
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
	var visible_batch := container.get_node_or_null("Batch_StaticCrate1_0") \
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
	assert_null(container.get_node_or_null("StaticShadow_StaticCrate1_0"),
			"an all-eligible batch needs no shadow-only duplicate")
	var shadow_sources := placer.get_static_terrain_shadow_source_diagnostics()
	assert_eq(shadow_sources.size(), 2)
	assert_eq(int((shadow_sources[0] as Dictionary).get("team", -1)), 1,
			"the typed caster snapshot retains TEX_TEAM input for frame selection")
	assert_eq(int((shadow_sources[1] as Dictionary).get("team", -1)), 2)


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
	placer.edit_mode = true
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	placer.place(mission, parent)

	var container := parent.get_node_or_null("MissionObjects")
	var visible_batch := container.get_node_or_null("Batch_StaticCrate1_0") \
			as MultiMeshInstance3D
	var shadow_batch := container.get_node_or_null(
			"StaticShadow_StaticCrate1_0") as MultiMeshInstance3D
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
	var eligible_record: Dictionary = {}
	var ineligible_record: Dictionary = {}
	for record_v in placer.pickable_records:
		var record: Dictionary = record_v
		if int(record.get("kind", -1)) != MissionData.KIND_BUILDING:
			continue
		if int(record.get("index", -1)) == 0:
			eligible_record = record
		elif int(record.get("index", -1)) == 1:
			ineligible_record = record
	var expected_shadow_mm := shadow_batch.multimesh \
			if shadow_batch != null else null
	assert_same(eligible_record.get("shadow_mm"), expected_shadow_mm,
			"editor records move the eligible parallel caster with its visible slot")
	assert_true(bool(eligible_record.get("casts_static_shadow", false)))
	assert_same(ineligible_record.get("shadow_mm"), expected_shadow_mm)
	assert_false(bool(ineligible_record.get("casts_static_shadow", true)),
			"moving an ineligible peer keeps its parallel slot zero-scaled")


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
	placer.edit_mode = true
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	placer.place(mission, parent)

	var building_batch: MultiMeshInstance3D = null
	var item_batch: MultiMeshInstance3D = null
	for record_v in placer.pickable_records:
		var record: Dictionary = record_v
		if int(record.get("kind", -1)) == MissionData.KIND_BUILDING:
			building_batch = record.get("mmi") as MultiMeshInstance3D
		elif int(record.get("kind", -1)) == MissionData.KIND_ITEM:
			item_batch = record.get("mmi") as MultiMeshInstance3D
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


func test_manual_static_instance_publishes_typed_terrain_shadow_source() -> void:
	var placer := MissionObjectPlacer.create(null, null)
	var data := ObjectData.new()
	assert_true(placer.register_object_data("House", data))
	var source_revision := placer.get_static_terrain_shadow_source_revision()
	var xform := Transform3D(Basis.from_euler(Vector3(0.1, 0.2, 0.3)),
			Vector3(12, 34, -56))
	placer.register_static_instance(100, "House", 0, xform, true)
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision)
	source_revision = placer.get_static_terrain_shadow_source_revision()

	var rows: Array = placer.get_static_terrain_shadow_source_diagnostics()
	assert_eq(rows.size(), 1,
			"the deterministic registration seam feeds the page-shadow provider")
	if rows.is_empty():
		return
	var row: Dictionary = rows[0]
	assert_eq(int(row.get("bms_id", 0)), 100)
	assert_eq(String(row.get("graphic", "")), "House")
	assert_eq(int(row.get("entity_kind", -1)), MissionData.KIND_BUILDING,
			"manual admitted casters use the collector's building policy")
	assert_eq(row.get("world_transform", Transform3D()), xform)
	assert_same(row.get("object_data"), data,
			"geometry resolution retains the injected ObjectData identity")
	assert_true(bool(row.get("active", false)))
	var moved := Transform3D(Basis(), Vector3(-4, 8, 16))
	assert_true(placer.update_static_terrain_shadow_source_transform(
			MissionData.KIND_BUILDING, 0, moved))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision)
	source_revision = placer.get_static_terrain_shadow_source_revision()
	assert_true(placer.update_static_terrain_shadow_source_transform(
			MissionData.KIND_BUILDING, 0, moved),
			"an admitted source remains addressable when its pose is unchanged")
	assert_eq(placer.get_static_terrain_shadow_source_revision(), source_revision,
			"re-presenting an identical transform must not invalidate terrain pages")
	rows = placer.get_static_terrain_shadow_source_diagnostics()
	assert_eq((rows[0] as Dictionary).get("world_transform"), moved,
			"editor/settling writes advance the typed source transform")

	assert_false(placer.hide_static_instance(100) == null)
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision)
	source_revision = placer.get_static_terrain_shadow_source_revision()
	rows = placer.get_static_terrain_shadow_source_diagnostics()
	assert_false(bool((rows[0] as Dictionary).get("active", true)),
			"a carved static stops contributing to subsequently composed pages")
	assert_true(placer.show_static_instance(100))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision)
	source_revision = placer.get_static_terrain_shadow_source_revision()
	rows = placer.get_static_terrain_shadow_source_diagnostics()
	assert_true(bool((rows[0] as Dictionary).get("active", false)),
			"restoring the static re-admits its page projection")

	var husk_data := ObjectData.new()
	assert_true(placer.register_object_data("HouseHusk", husk_data))
	var alternate_husk_data := ObjectData.new()
	assert_true(placer.register_object_data("HouseHuskDamaged", alternate_husk_data))
	var husk_xform := Transform3D(Basis(), Vector3(7, 9, 11))
	assert_true(placer.set_static_terrain_shadow_replacement(
			100, "HouseHusk", husk_xform, true))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision)
	source_revision = placer.get_static_terrain_shadow_source_revision()
	rows = placer.get_static_terrain_shadow_source_diagnostics()
	row = rows[0]
	assert_eq(String(row.get("graphic", "")), "HouseHusk")
	assert_same(row.get("object_data"), husk_data,
			"destruction swaps the provider to current husk geometry")
	assert_eq(row.get("world_transform"), husk_xform)
	assert_true(placer.set_static_terrain_shadow_replacement(
			100, "HouseHusk", husk_xform, true))
	assert_eq(placer.get_static_terrain_shadow_source_revision(), source_revision,
			"the per-present husk publication is idempotent")
	var settled_husk := Transform3D(Basis(), Vector3(8, 9, 11))
	assert_true(placer.update_static_terrain_shadow_source_transform(
			MissionData.KIND_BUILDING, 0, settled_husk))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision,
			"a real settling transform advances the replacement source")
	source_revision = placer.get_static_terrain_shadow_source_revision()
	rows = placer.get_static_terrain_shadow_source_diagnostics()
	assert_eq((rows[0] as Dictionary).get("world_transform"), settled_husk,
			"live registry transforms override the original husk-placement snapshot")
	assert_true(placer.set_static_terrain_shadow_replacement(
			100, "HouseHuskDamaged", settled_husk, true))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision,
			"a genuine replacement graphic/identity change invalidates pages")
	source_revision = placer.get_static_terrain_shadow_source_revision()
	assert_true(placer.set_static_terrain_shadow_replacement(
			100, "HouseHuskDamaged", settled_husk, false))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision,
			"a genuine caster-admission change invalidates pages")
	source_revision = placer.get_static_terrain_shadow_source_revision()
	assert_true(placer.set_static_terrain_shadow_replacement(
			100, "HouseHuskDamaged", settled_husk, false))
	assert_eq(placer.get_static_terrain_shadow_source_revision(), source_revision,
			"repeating the inactive replacement state is idempotent too")
	var inactive_move := Transform3D(Basis(), Vector3(8, 10, 11))
	assert_true(placer.update_static_terrain_shadow_source_transform(
			MissionData.KIND_BUILDING, 0, inactive_move))
	assert_eq(placer.get_static_terrain_shadow_source_revision(), source_revision,
			"an inactive replacement tracks pose without invalidating pages")
	rows = placer.get_static_terrain_shadow_source_diagnostics()
	assert_eq((rows[0] as Dictionary).get("world_transform"), inactive_move)
	assert_true(placer.set_static_terrain_shadow_replacement(
			100, "HouseHuskDamaged", inactive_move, true))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision,
			"re-admitting the current husk pose invalidates pages")
	source_revision = placer.get_static_terrain_shadow_source_revision()
	assert_true(placer.set_static_terrain_shadow_replacement(
			100, "HouseHuskDamaged", inactive_move, false))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision)
	source_revision = placer.get_static_terrain_shadow_source_revision()
	assert_true(placer.clear_static_terrain_shadow_replacement(100))
	assert_gt(placer.get_static_terrain_shadow_source_revision(), source_revision)
	rows = placer.get_static_terrain_shadow_source_diagnostics()
	assert_eq(String((rows[0] as Dictionary).get("graphic", "")), "House")


func test_rejected_static_source_updates_do_not_advance_the_page_revision() -> void:
	var placer := MissionObjectPlacer.new()
	assert_true(placer.register_object_data("NoShadow", ObjectData.new()))
	assert_true(placer.register_object_data("NoShadowHusk", ObjectData.new()))
	placer.register_static_instance(200, "NoShadow", 3,
			Transform3D.IDENTITY, false)
	var revision := placer.get_static_terrain_shadow_source_revision()
	var moved := Transform3D(Basis.IDENTITY, Vector3(1, 2, 3))
	assert_true(placer.update_static_terrain_shadow_source_transform(
			MissionData.KIND_BUILDING, 3, moved))
	assert_eq(placer.get_static_terrain_shadow_source_revision(), revision,
			"a rejected source may track pose without dirtying terrain pages")
	assert_true(placer.set_static_terrain_shadow_replacement(
			200, "NoShadowHusk", moved, true))
	assert_eq(placer.get_static_terrain_shadow_source_revision(), revision,
			"a replacement cannot admit a source vetoed by base policy")
	assert_true(placer.clear_static_terrain_shadow_replacement(200))
	assert_eq(placer.get_static_terrain_shadow_source_revision(), revision,
			"clearing an unobservable replacement is revision-stable")


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
	placer.edit_mode = true
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", ObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)

	placer.place(mission, parent)

	var batch: MultiMeshInstance3D = null
	for record_v in placer.pickable_records:
		var record: Dictionary = record_v
		if int(record.get("kind", -1)) == MissionData.KIND_ITEM:
			batch = record.get("mmi") as MultiMeshInstance3D
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


func test_place_single_vehicle_without_anim_def_stays_in_static_batch() -> void:
	var mission := MissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	assert_eq(item_db.get_item_type(106002), ItemDatabase.TYPE_VEHICLE)
	assert_true(item_db.get_anim_def(106002).is_empty(),
			"the fixture must exercise the no-anim vehicle policy")
	var root := ResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := MissionObjectPlacer.create(root, item_db)
	placer.edit_mode = true
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")

	var mesh := BoxMesh.new()
	assert_true(placer.register_resolved_static_graphic(
			"StaticVehicle1", ObjectData.new(), [{
		"mesh": mesh, "material": null, "offset": Transform3D.IDENTITY, "submesh": 0,
	}]))

	var record := mission.add_entity(MissionData.KIND_ITEM, 106002, Vector3.ZERO, Vector3.ZERO)
	var delta: Dictionary = placer.place_single(
		mission, container, MissionData.KIND_ITEM, int(record["index"]))

	assert_eq(int(delta.get("placed", -1)), 1, "the vehicle is placed")
	assert_eq(int(delta.get("batched", -1)), 1, "a vehicle without anim_def uses static batching")
	assert_eq(int(delta.get("animated", -1)), 0, "the vehicle does not enter runtime presentation")
	assert_false(bool(placer.pickable_records.back()["animated"]))


func test_place_single_is_a_noop_on_null_inputs() -> void:
	var placer := MissionObjectPlacer.new()
	var delta: Dictionary = placer.place_single(null, null, MissionData.KIND_ITEM, 0)
	assert_eq(int(delta.get("placed", -1)), 0, "null inputs place nothing")
	assert_eq(int(delta.get("unresolved", 0)), 0, "and do not falsely count an unresolved")


# --- Engine-facade bake parity --------------------------------------------------
# The authoring facade (engine/runtime/mission authoring.h) bakes the Ground anchor in mission
# space with the conjugated engine matrix [orig: sub_401A90, dfx2med.exe;
# Math_BuildFixedPointMatrixFromEulerAngles @ 0x613F40 + the .3di import's
# model-forward correction]; the editor's live drag bakes in Godot space with
# bms_to_godot_basis. The two MUST agree, or an anchored object would shift between
# the drag preview and the committed record.
func test_ground_bake_parity_with_engine_facade() -> void:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	var rec: Dictionary = md.add_entity(MissionData.KIND_BUILDING, 102001, Vector3.ZERO, Vector3.ZERO)
	assert_false(rec.is_empty(), "seed entity added")
	var index := int(rec["index"])

	var anchor_godot := Vector3(0.75, 0.5, -1.25)
	var anchor_bms := MissionObjectPlacer.godot_to_bms_position(anchor_godot)
	var hit_godot := Vector3(33.0, 8.0, -21.0)
	var hit_bms := MissionObjectPlacer.godot_to_bms_position(hit_godot)

	# Integer-degree rotations only (the format stores integer degrees).
	for rot in [Vector3.ZERO, Vector3(0, 90, 0), Vector3(15, 0, 0), Vector3(0, 0, 30),
			Vector3(10, 45, -20), Vector3(-35, 220, 75), Vector3(90, 0, 0)]:
		assert_true(md.set_entity_transform(MissionData.KIND_BUILDING, index, hit_bms, rot))
		assert_true(md.move_entity_grounded(MissionData.KIND_BUILDING, index, hit_bms, anchor_bms),
			"facade re-grounds at rot %s" % rot)
		var moved: Dictionary = md.get_entity(MissionData.KIND_BUILDING, index)
		var stored_bms: Vector3 = moved["position"]
		assert_eq(moved["rotation_deg"], rot, "rotation preserved")
		# The editor's Godot-space bake of the same gesture:
		var expected_godot := hit_godot - MissionObjectPlacer.bms_to_godot_basis(rot) * anchor_godot
		var expected_bms := MissionObjectPlacer.godot_to_bms_position(expected_godot)
		assert_true(stored_bms.is_equal_approx(expected_bms),
			"facade bake == editor bake at rot %s (facade %s vs editor %s)" % [rot, stored_bms, expected_bms])


func test_ground_anchor_bms_is_the_axis_remap() -> void:
	# ground_anchor_bms is godot_to_bms_position applied to the anchor offset — linear,
	# so valid on offset vectors. Pin the remap so the facade's anchor input stays correct.
	assert_eq(MissionObjectPlacer.godot_to_bms_position(Vector3(1, 2, 3)), Vector3(1, -3, 2))


# --- Live-PANM graphics must not freeze into static batches ----------------------
# A decoration whose .3di carries a live PANM track (free-running wave/spin, SET pose,
# or a control-register binding) must place as an individual ObjectModel even
# though items.def gives it no anim_def: a MultiMesh batch captures the rest pose once
# and never evaluates PANM again, while the engine re-poses PANM from the global clock
# every rendered frame [orig: PANM_SampleTrack (sub_4354B0) idle gate, clock
# Render_ShaderTickMs @0x2721A40]. DFX2's "Oil Pump" (graphic Pmpjk01,
# type decoration, control-0x32 sine tracks) is the witnessed case. Inert PANM
# blocks (Armry01 as shipped: entries
# present, every control idle) must keep the perf-tier static batching.

const ARMRY_3DI := "res://../fixtures/3dp/armry01/Armry01.3di"


func _armry_data(with_live_rotation: bool) -> ObjectData:
	var data := ObjectData.new()
	if data.open_file(_abs(ARMRY_3DI)) != OK:
		return null
	if with_live_rotation:
		# Author one live track the way the object workspace does. Armry01 ships
		# all PANM controls idle; a sine wave is both active AND time-varying
		# (retail's "slide" samples to a constant until a state machine drives it).
		if not data.set_part_anim_channel_enabled(0, 0, "rotation", true):
			return null
		if not data.set_part_anim_channel_mode(0, 0, "rotation", "x", "sine_wave", -1):
			return null
		if not data.set_part_anim_channel_values(0, 0, "rotation", "x", 0.0, 90.0, 1.0):
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
	placer.edit_mode = true
	# item 105004: type object, no anim_def
	placer.register_object_data("StaticCrate1", _armry_data(live))
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
	# Armry01 as shipped has idle PANM plus an independent OOBJ portal payload.
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


func test_place_single_routes_live_panm_graphic_to_a_live_model() -> void:
	var placer := _panm_placer(true)
	assert_not_null(placer.object_data_for("StaticCrate1"), "fixture data authored with one live PANM track")
	if placer.object_data_for("StaticCrate1") == null:
		return
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)  # builds the MissionObjects container
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return

	var record := mission.add_entity(
		MissionData.KIND_ITEM, 105004, Vector3(3, 4, 5), Vector3.ZERO)
	var delta: Dictionary = placer.place_single(
		mission, container, MissionData.KIND_ITEM, int(record["index"]))

	assert_eq(int(delta.get("animated", -1)), 1,
		"place_single routes a live-PANM graphic to a live model")
	assert_eq(int(delta.get("batched", -1)), 0, "not to a single-instance batch")


func test_inert_panm_model_retains_robj_base_instead_of_rederiving() -> void:
	var data := _armry_data(false)
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
	var data := _armry_data(true)
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
	assert_true(parts.has(0), "the authored live rotation drives robj part 0")
	var part := parts[0] as Node3D

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
