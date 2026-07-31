extends GutTest

# Phase 4: Placer. Covers the asset-free pieces that must be exactly
# right (BMS -> Godot coordinate conversion, cross-checked against the equivalent
# Basis) and the graceful resolution-miss path (fixtures ship items.def but no
# .3di, so nothing resolves to a model and the placer must place zero without
# error). Full render-placement is validated against real assets out-of-band.

const Placer := preload("res://engine/mission/mission_object_placer.gd")

const BMS_PATH := "res://../fixtures/bms/ash_i5b.reference.bms"
const ITEMS_PATH := "res://../fixtures/def/items.def"


func _abs(res_path: String) -> String:
	return ProjectSettings.globalize_path(res_path)


func test_position_is_minus_90_about_x() -> void:
	# (x, y, z) -> (x, z, -y); equivalent to a -90 deg rotation about X.
	assert_eq(Placer.bms_to_godot_position(Vector3(1, 2, 3)), Vector3(1, 3, -2))
	var basis := Basis.from_euler(Vector3(deg_to_rad(-90.0), 0.0, 0.0))
	for v in [Vector3(5, -7, 11), Vector3(-1, 0, 4), Vector3(100, 50, -25)]:
		assert_true(
			Placer.bms_to_godot_position(v).is_equal_approx(basis * v),
			"position conversion matches the -90 deg X basis for %s" % v)


# Yaw-only must stay byte-for-byte the long-standing (visually-correct) heading: the engine's
# Rz(90 - yaw) about the up axis, conjugated into Godot and composed with the +Z-forward model
# correction C = RotY(90), collapses to RotY(180 - yaw) -- exactly the previous euler form. This is
# the no-regression guard for the common (untilted) case. [orig: @0x40eb66 / @0x613f40]
func test_yaw_only_basis_matches_the_legacy_heading() -> void:
	for yaw in [0.0, 45.0, 90.0, 180.0, 270.0]:
		var got := Placer.bms_to_godot_basis(Vector3(0, yaw, 0))
		var legacy := Basis(Vector3.UP, PI - deg_to_rad(yaw))
		assert_true(got.is_equal_approx(legacy),
			"yaw=%s basis is RotY(180 - yaw), unchanged from the legacy heading" % yaw)


# Pitch must tip the model's nose the way retail does. The model's local forward is +Z
# (Vector3.BACK). At yaw=0, a +30 degree authored pitch points the nose UP:
# forward = (0, +sin30, -cos30), matching M * Rz(90) * Ry(-30) * (+X_engine).
# [orig: @0x40eb86 / @0x613f40]
func test_pitch_tips_the_nose_up_like_retail() -> void:
	var basis := Placer.bms_to_godot_basis(Vector3(30, 0, 0))
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
	var basis := Placer.bms_to_godot_basis(Vector3(0, 90, 30))
	var up: Vector3 = basis * Vector3.UP
	assert_true(up.is_equal_approx(Vector3(0.0, cos(deg_to_rad(30.0)), sin(deg_to_rad(30.0)))),
		"roll banks the up vector engine-faithfully")


func test_entity_transform_composes_basis_and_origin() -> void:
	var pos := Vector3(10, 20, 30)
	var rot_deg := Vector3(15, 180, 25)
	var xform := Placer.entity_transform(pos, rot_deg)
	assert_true(
		xform.origin.is_equal_approx(Placer.bms_to_godot_position(pos)),
		"origin is the converted position")
	assert_true(xform.basis.is_equal_approx(Placer.bms_to_godot_basis(rot_deg)),
		"basis is the converted rotation")


func test_godot_to_bms_position_inverts_bms_to_godot() -> void:
	# The editor writes a dragged object's new ground point back through this inverse;
	# it must exactly undo bms_to_godot_position for any mission-space point.
	for v in [Vector3(5, -7, 11), Vector3(-1, 0, 4), Vector3(100, 50, -25), Vector3.ZERO]:
		assert_true(
			Placer.godot_to_bms_position(Placer.bms_to_godot_position(v)).is_equal_approx(v),
			"godot_to_bms_position round-trips %s" % v)


func test_place_handles_unresolvable_models_without_error() -> void:
	var mission := NovaMissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK, "fixture BMS parses")

	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK, "fixture items.def loads")

	var root := NovaResourceRoot.new()
	# Points at the def fixtures dir: it has items.def but no .3di, so no model
	# resolves. set_root_dir may reject it; resolve_file then simply returns "".
	root.set_root_dir(_abs("res://../fixtures/def"))

	var placer := Placer.new(root, item_db)
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_not_null(parent.get_node_or_null("MissionObjects"), "MissionObjects container is created")
	assert_true(stats.has("placed"), "stats expose placed")
	assert_true(stats.has("unresolved"), "stats expose unresolved")
	assert_eq(int(stats.get("placed", -1)), 0, "no .3di in fixtures -> nothing placed")
	assert_gt(int(stats.get("unresolved", 0)), 0, "real entities recorded as unresolved")


func test_place_is_a_noop_on_null_inputs() -> void:
	var placer := Placer.new(null, null)
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
	# path (no .3di -> no NovaObjectData). The static branch is covered separately below.
	var mission := NovaMissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))  # items.def but no .3di

	var placer := Placer.new(root, item_db)
	placer.edit_mode = true
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)  # builds the MissionObjects container
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container, "the container exists to place into")

	# Add a real entity, then render just that one.
	var record := mission.add_entity(NovaMissionData.KIND_ITEM, 101291, Vector3(1, 2, 3), Vector3.ZERO)
	var index := int(record["index"])
	var pickable_before := placer.pickable_records.size()
	var delta: Dictionary = placer.place_single(mission, container, NovaMissionData.KIND_ITEM, index)

	assert_eq(int(delta.get("unresolved", 0)), 1, "a graphic with no .3di reports unresolved")
	assert_eq(int(delta.get("placed", -1)), 0, "and places nothing")
	assert_eq(placer.pickable_records.size(), pickable_before, "an unrendered entity adds no pickable record")


func test_place_single_static_branch_reports_unresolved_without_a_model() -> void:
	# Item 105004 "Static Crate" is type object with no anim_def, so it takes the STATIC
	# branch. With no .3di and no seeded batch cache it resolves no geometry and must
	# report unresolved without recording a pickable.
	var mission := NovaMissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := Placer.new(root, item_db)
	placer.edit_mode = true
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")

	var record := mission.add_entity(NovaMissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO)
	var index := int(record["index"])
	var pickable_before := placer.pickable_records.size()
	var delta: Dictionary = placer.place_single(mission, container, NovaMissionData.KIND_ITEM, index)

	assert_eq(int(delta.get("unresolved", 0)), 1, "a static graphic with no .3di reports unresolved")
	assert_eq(int(delta.get("placed", -1)), 0, "and places nothing")
	assert_eq(placer.pickable_records.size(), pickable_before, "an unrendered static adds no pickable record")


func test_place_single_static_branch_builds_a_single_instance_batch() -> void:
	# The static success branch (a single-instance MultiMesh + the pickable record that
	# later select / drag depend on) is the load-bearing new code. Exercise it asset-free
	# by pre-seeding the per-graphic batch cache with a dummy mesh, so place_single
	# renders without a real .3di. Full render fidelity is validated against real assets
	# out-of-band, like place() itself.
	var mission := NovaMissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := Placer.new(root, item_db)
	placer.edit_mode = true
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")

	# Seed the static-batch cache so the static branch has geometry to instance.
	var mesh := BoxMesh.new()
	mesh.size = Vector3(2, 2, 2)
	var offset := Transform3D(Basis(), Vector3(0, 1, 0))
	var object_data := NovaObjectData.new()
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", object_data, [{
		"mesh": mesh, "material": null, "offset": offset, "submesh": 0,
	}]))

	var record := mission.add_entity(NovaMissionData.KIND_ITEM, 105004, Vector3(3, 4, 5), Vector3.ZERO)
	var index := int(record["index"])
	var pickable_before := placer.pickable_records.size()
	var delta: Dictionary = placer.place_single(mission, container, NovaMissionData.KIND_ITEM, index)

	assert_eq(int(delta.get("placed", -1)), 1, "the static entity is placed")
	assert_eq(int(delta.get("batched", -1)), 1, "via the static-batch branch")
	assert_eq(int(delta.get("batches", -1)), 1, "one draw group for its single submesh")
	assert_eq(placer.pickable_records.size(), pickable_before + 1, "it appends exactly one pickable record")

	var rec: Dictionary = placer.pickable_records.back()
	assert_eq(int(rec["kind"]), NovaMissionData.KIND_ITEM)
	assert_eq(int(rec["index"]), index, "the record points back at the placed entity")
	assert_eq(int(rec["slot"]), 0, "a single-instance batch uses slot 0")
	assert_false(bool(rec["animated"]))
	var mmi := rec["mmi"] as MultiMeshInstance3D
	assert_eq(mmi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
			"retail static batches receive dynamic silhouettes but never cast them")
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

	# The batch has no per-entity Node3D, but mission-start item effects still
	# receive one immutable value descriptor for the successfully rendered entity.
	var effect_sources: Array = placer.get_static_item_effect_sources()
	assert_eq(effect_sources.size(), 1)
	var source: Dictionary = effect_sources[0]
	assert_eq(int(source.get("kind", -1)), NovaMissionData.KIND_ITEM)
	assert_eq(int(source.get("item_id", 0)), 105004)
	assert_eq(String(source.get("graphic", "")), "StaticCrate1")
	assert_eq(source.get("object_data"), object_data)
	var expected_transform := Placer.entity_transform(Vector3(3, 4, 5), Vector3.ZERO)
	var actual_transform: Transform3D = source.get("world_transform", Transform3D.IDENTITY)
	assert_true(actual_transform.is_equal_approx(expected_transform),
			"the descriptor carries the BASE entity transform, not a submesh offset")
	# Getter rows are copies; callers cannot rewrite the placer's retained identity.
	source["item_id"] = 0
	assert_eq(int(placer.get_static_item_effect_sources()[0].get("item_id", 0)), 105004)


func test_place_single_static_caster_reuses_its_visible_instance() -> void:
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := Placer.new(root, item_db)
	placer.edit_mode = true
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", NovaObjectData.new(), [{
				"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D.IDENTITY, "submesh": 0,
			}]))
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	var record := mission.add_entity(
			NovaMissionData.KIND_BUILDING, 105004,
			Vector3(3, 4, 5), Vector3.ZERO)

	var delta: Dictionary = placer.place_single(
			mission, container, NovaMissionData.KIND_BUILDING,
			int(record["index"]))

	assert_eq(int(delta.get("batched", -1)), 1)
	var visible_batch := placer.pickable_records.back()["mmi"] \
			as MultiMeshInstance3D
	assert_eq(visible_batch.layers,
			NovaWater.VISUAL_LAYER_WORLD \
			| NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER,
			"the one visible draw also enters the isolated static-caster pass")
	assert_eq(visible_batch.cast_shadow,
			GeometryInstance3D.SHADOW_CASTING_SETTING_ON)
	assert_null(container.get_node_or_null(
			"StaticShadow_StaticCrate1_k%d_i%d_s0" % [
				NovaMissionData.KIND_BUILDING, int(record["index"])]),
			"place_single avoids a second node referencing the same MultiMesh")


func test_dynamic_shadow_caster_policy_matches_retail_entity_slot_admission() -> void:
	assert_true(Placer.item_casts_dynamic_shadow(
			NovaItemDatabase.TYPE_PERSON, 0, 0),
			"people always receive a retail shadow render slot")
	assert_true(Placer.item_casts_dynamic_shadow(
			NovaItemDatabase.TYPE_VEHICLE, 0, 0x10),
			"DynamicShadow admits a non-person model")
	assert_false(Placer.item_casts_dynamic_shadow(
			NovaItemDatabase.TYPE_BUILDING, 0, 0),
			"portal/static buildings never become silhouette casters")
	assert_true(Placer.item_casts_dynamic_shadow(
			NovaItemDatabase.TYPE_PERSON, 0x04000000, 0x10),
			"the witnessed dynamic-slot allocator does not consult ItemDef NoShadow")


func test_static_shadow_caster_policy_matches_retail_terrain_tile_admission() -> void:
	assert_true(Placer.item_casts_static_terrain_shadow(
			NovaMissionData.KIND_BUILDING, 0, 0, 0),
			"pool-2 buildings enter the terrain-tile caster pass by default")
	assert_true(Placer.item_casts_static_terrain_shadow(
			NovaMissionData.KIND_ITEM, 0, 0, 0x20),
			"pool-1 items require StaticShadow")
	assert_false(Placer.item_casts_static_terrain_shadow(
			NovaMissionData.KIND_ITEM, 0, 0, 0),
			"an ordinary pool-1 item is absent from the static pass")
	assert_false(Placer.item_casts_static_terrain_shadow(
			NovaMissionData.KIND_BUILDING, 0x01000000, 0, 0),
			"BMS NoShadow suppresses a pool-2 caster")
	assert_false(Placer.item_casts_static_terrain_shadow(
			NovaMissionData.KIND_BUILDING, 0, 0x04000000, 0),
			"ItemDef NoShadow suppresses a pool-2 caster")


func test_all_eligible_static_batch_reuses_its_visible_instance_as_caster() -> void:
	# The reimpl's static light reaches only the terrain receiver layer, so an
	# all-eligible batch can carry both the ordinary world and static-caster
	# marker without self-shadowing. This avoids one duplicate MultiMesh per
	# submesh while preserving the visible draw.
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			NovaMissionData.KIND_BUILDING, 105004,
			Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	assert_false(mission.add_entity(
			NovaMissionData.KIND_BUILDING, 105004,
			Vector3(4, 5, 6), Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := Placer.new(root, item_db)
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", NovaObjectData.new(), [{
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
				NovaWater.VISUAL_LAYER_WORLD \
				| NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER,
				"the visible batch joins the isolated static-caster layer")
		assert_eq(visible_batch.cast_shadow,
				GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
				"the visible batch supplies the static silhouette")
	assert_null(container.get_node_or_null("StaticShadow_StaticCrate1_0"),
			"an all-eligible batch needs no shadow-only duplicate")


func test_mixed_static_batch_keeps_a_filtered_shadow_only_duplicate() -> void:
	# The two mission pools share one graphic here, but only the building is
	# admitted to retail's terrain-tile shadow pass. A visible batch cannot
	# express that per-instance difference, so this case still needs a parallel
	# MultiMesh with the ineligible slot zero-scaled.
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			NovaMissionData.KIND_BUILDING, 105004,
			Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	assert_false(mission.add_entity(
			NovaMissionData.KIND_ITEM, 105004,
			Vector3(4, 5, 6), Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := Placer.new(root, item_db)
	placer.edit_mode = true
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", NovaObjectData.new(), [{
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
		assert_eq(visible_batch.layers, NovaWater.VISUAL_LAYER_WORLD)
		assert_eq(visible_batch.cast_shadow,
				GeometryInstance3D.SHADOW_CASTING_SETTING_OFF)
	if shadow_batch != null:
		assert_eq(shadow_batch.layers,
				NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER)
		assert_eq(shadow_batch.cast_shadow,
				GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY)
		if visible_batch != null:
			assert_ne(shadow_batch.multimesh, visible_batch.multimesh,
					"the filtered caster owns transforms independent of the visible batch")
		assert_eq(shadow_batch.multimesh.instance_count, 2,
				"slot identity stays parallel for destruction updates")
	var building_record: Dictionary = {}
	var item_record: Dictionary = {}
	for record_v in placer.pickable_records:
		var record: Dictionary = record_v
		if int(record.get("kind", -1)) == NovaMissionData.KIND_BUILDING:
			building_record = record
		elif int(record.get("kind", -1)) == NovaMissionData.KIND_ITEM:
			item_record = record
	var expected_shadow_mm := shadow_batch.multimesh \
			if shadow_batch != null else null
	assert_same(building_record.get("shadow_mm"), expected_shadow_mm,
			"editor records move the eligible parallel caster with its visible slot")
	assert_true(bool(building_record.get("casts_static_shadow", false)))
	assert_same(item_record.get("shadow_mm"), expected_shadow_mm)
	assert_false(bool(item_record.get("casts_static_shadow", true)),
			"moving an ineligible peer keeps its parallel slot zero-scaled")


func test_individual_building_gets_an_unmasked_static_shadow_sibling() -> void:
	# Portal buildings must keep their camera-driven ROBJ visibility on the
	# visible NovaObjectModel, while retail's tile pass independently submits
	# every ROBJ. Seed one harvested batch so the shadow-only sibling can be
	# asserted without shipping the retail GuardTwr asset.
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
			NovaMissionData.KIND_BUILDING, 102001,
			Vector3(3, 4, 5), Vector3.ZERO).is_empty())
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := Placer.new(root, item_db)
	var object_data := NovaObjectData.new()
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
	var static_shadow := visible_model.get_node_or_null(
			"StaticShadow_GuardTwr1_live0_0") as MultiMeshInstance3D
	assert_not_null(static_shadow,
			"an independent all-section caster survives portal-mask changes")
	if static_shadow != null:
		assert_eq(static_shadow.cast_shadow,
				GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY)
		assert_eq(static_shadow.layers,
				NovaWater.VISUAL_LAYER_STATIC_SHADOW_CASTER)
		assert_same(static_shadow.get_parent(), visible_model,
				"the unmasked caster follows editor moves and husk visibility lifecycle")
		var before := static_shadow.global_position
		visible_model.position += Vector3(7, 0, 0)
		assert_eq(static_shadow.global_position, before + Vector3(7, 0, 0),
				"moving the individual entity cannot strand its caster")


func test_place_single_vehicle_without_anim_def_stays_in_static_batch() -> void:
	var mission := NovaMissionData.new()
	assert_eq(mission.open_file(_abs(BMS_PATH)), OK)
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	assert_eq(item_db.get_item_type(106002), NovaItemDatabase.TYPE_VEHICLE)
	assert_true(item_db.get_anim_def(106002).is_empty(),
			"the fixture must exercise the no-anim vehicle policy")
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := Placer.new(root, item_db)
	placer.edit_mode = true
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)
	var container: Node3D = parent.get_node_or_null("MissionObjects")

	var mesh := BoxMesh.new()
	assert_true(placer.register_resolved_static_graphic(
			"StaticVehicle1", NovaObjectData.new(), [{
		"mesh": mesh, "material": null, "offset": Transform3D.IDENTITY, "submesh": 0,
	}]))

	var record := mission.add_entity(NovaMissionData.KIND_ITEM, 106002, Vector3.ZERO, Vector3.ZERO)
	var delta: Dictionary = placer.place_single(
		mission, container, NovaMissionData.KIND_ITEM, int(record["index"]))

	assert_eq(int(delta.get("placed", -1)), 1, "the vehicle is placed")
	assert_eq(int(delta.get("batched", -1)), 1, "a vehicle without anim_def uses static batching")
	assert_eq(int(delta.get("animated", -1)), 0, "the vehicle does not enter runtime presentation")
	assert_false(bool(placer.pickable_records.back()["animated"]))


func test_place_single_is_a_noop_on_null_inputs() -> void:
	var placer := Placer.new(null, null)
	var delta: Dictionary = placer.place_single(null, null, NovaMissionData.KIND_ITEM, 0)
	assert_eq(int(delta.get("placed", -1)), 0, "null inputs place nothing")
	assert_eq(int(delta.get("unresolved", 0)), 0, "and do not falsely count an unresolved")


# --- Engine-facade bake parity --------------------------------------------------
# The authoring facade (libs/mission authoring.h) bakes the Ground anchor in mission
# space with the conjugated engine matrix [orig: sub_401A90, dfx2med.exe;
# Math_BuildFixedPointMatrixFromEulerAngles @ 0x613F40 + the .3di import's
# model-forward correction]; the editor's live drag bakes in Godot space with
# bms_to_godot_basis. The two MUST agree, or an anchored object would shift between
# the drag preview and the committed record.
func test_ground_bake_parity_with_engine_facade() -> void:
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK)
	var rec: Dictionary = md.add_entity(NovaMissionData.KIND_BUILDING, 102001, Vector3.ZERO, Vector3.ZERO)
	assert_false(rec.is_empty(), "seed entity added")
	var index := int(rec["index"])

	var anchor_godot := Vector3(0.75, 0.5, -1.25)
	var anchor_bms := Placer.godot_to_bms_position(anchor_godot)
	var hit_godot := Vector3(33.0, 8.0, -21.0)
	var hit_bms := Placer.godot_to_bms_position(hit_godot)

	# Integer-degree rotations only (the format stores integer degrees).
	for rot in [Vector3.ZERO, Vector3(0, 90, 0), Vector3(15, 0, 0), Vector3(0, 0, 30),
			Vector3(10, 45, -20), Vector3(-35, 220, 75), Vector3(90, 0, 0)]:
		assert_true(md.set_entity_transform(NovaMissionData.KIND_BUILDING, index, hit_bms, rot))
		assert_true(md.move_entity_grounded(NovaMissionData.KIND_BUILDING, index, hit_bms, anchor_bms),
			"facade re-grounds at rot %s" % rot)
		var moved: Dictionary = md.get_entity(NovaMissionData.KIND_BUILDING, index)
		var stored_bms: Vector3 = moved["position"]
		assert_eq(moved["rotation_deg"], rot, "rotation preserved")
		# The editor's Godot-space bake of the same gesture:
		var expected_godot := hit_godot - Placer.bms_to_godot_basis(rot) * anchor_godot
		var expected_bms := Placer.godot_to_bms_position(expected_godot)
		assert_true(stored_bms.is_equal_approx(expected_bms),
			"facade bake == editor bake at rot %s (facade %s vs editor %s)" % [rot, stored_bms, expected_bms])


func test_ground_anchor_bms_is_the_axis_remap() -> void:
	# ground_anchor_bms is godot_to_bms_position applied to the anchor offset — linear,
	# so valid on offset vectors. Pin the remap so the facade's anchor input stays correct.
	assert_eq(Placer.godot_to_bms_position(Vector3(1, 2, 3)), Vector3(1, -3, 2))


# --- Live-PANM graphics must not freeze into static batches ----------------------
# A decoration whose .3di carries a live PANM track (free-running wave/spin, SET pose,
# or a control-register binding) must place as an individual NovaObjectModel even
# though items.def gives it no anim_def: a MultiMesh batch captures the rest pose once
# and never evaluates PANM again, while the engine re-poses PANM from the global clock
# every rendered frame [orig: PANM_SampleTrack (sub_4354B0) idle gate, clock
# Render_ShaderTickMs @0x2721A40]. DFX2's "Oil Pump" (graphic Pmpjk01,
# type decoration, control-0x32 sine tracks) is the witnessed case. Inert PANM
# blocks (Armry01 as shipped: entries
# present, every control idle) must keep the perf-tier static batching.

class PanmDataPlacer:
	extends Placer
	# Injected object data for one graphic, so PANM classification can be tested
	# independently of the fixture's separate portal/occlusion classification.
	var panm_graphic := ""
	var panm_data: NovaObjectData = null
	var expose_occlusion := false

	func _load_object_data(graphic: String) -> NovaObjectData:
		if graphic == panm_graphic:
			return panm_data
		return super(graphic)

	func _has_occlusion_records(_item_id: int) -> bool:
		return expose_occlusion and panm_data != null and panm_data.has_occlusion()


class PanmRuntimeModel:
	extends NovaObjectModel
	var robj_eval_count := 0

	func _apply_robj_transforms() -> bool:
		robj_eval_count += 1
		return false

	func run_runtime_frame(delta: float = 0.016) -> void:
		_process(delta)


const ARMRY_3DI := "res://../fixtures/3dp/armry01/Armry01.3di"


func _armry_data(with_live_rotation: bool) -> NovaObjectData:
	var data := NovaObjectData.new()
	if data.open_file(_abs(ARMRY_3DI)) != OK:
		return null
	if with_live_rotation:
		# Author one live track the way the object workspace does. Armry01 ships all
		# PANM controls idle; "slide" maps to an active control function.
		if not data.set_part_anim_channel_enabled(0, 0, "rotation", true):
			return null
		if not data.set_part_anim_channel_mode(0, 0, "rotation", "x", "slide", -1):
			return null
		if not data.set_part_anim_channel_values(0, 0, "rotation", "x", 0.0, 90.0, 1.0):
			return null
	return data


func _panm_placer(live: bool) -> PanmDataPlacer:
	var item_db := NovaItemDatabase.new()
	assert_eq(item_db.load(_abs(ITEMS_PATH)), OK)
	var root := NovaResourceRoot.new()
	root.set_root_dir(_abs("res://../fixtures/def"))
	var placer := PanmDataPlacer.new(root, item_db)
	placer.edit_mode = true
	placer.panm_graphic = "StaticCrate1"  # item 105004: type object, no anim_def
	placer.panm_data = _armry_data(live)
	return placer


func test_place_routes_live_panm_graphic_to_a_live_model() -> void:
	var placer := _panm_placer(true)
	assert_not_null(placer.panm_data, "fixture data authored with one live PANM track")
	if placer.panm_data == null:
		return
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
		NovaMissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO).is_empty())
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
	assert_not_null(placer.panm_data, "fixture data loads")
	if placer.panm_data == null:
		return
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
		NovaMissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	# Seed batch geometry so the static branch can render without a resource-root .3di.
	assert_true(placer.register_resolved_static_graphic(
			"StaticCrate1", placer.panm_data, [{
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
	assert_not_null(placer.panm_data, "fixture data loads")
	if placer.panm_data == null:
		return
	assert_true(placer.panm_data.has_occlusion(),
		"fixture carries the portal payload that requires per-section visibility")
	placer.expose_occlusion = true
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_false(mission.add_entity(
		NovaMissionData.KIND_ITEM, 105004, Vector3(1, 2, 3), Vector3.ZERO).is_empty())
	var parent := Node3D.new()
	add_child_autofree(parent)

	var stats: Dictionary = placer.place(mission, parent)

	assert_eq(int(stats.get("animated", -1)), 1,
		"portal sections require an individual model even when PANM is inert")
	assert_eq(int(stats.get("batched", -1)), 0,
		"the per-instance section mask cannot be represented by a MultiMesh batch")


func test_place_single_routes_live_panm_graphic_to_a_live_model() -> void:
	var placer := _panm_placer(true)
	assert_not_null(placer.panm_data, "fixture data authored with one live PANM track")
	if placer.panm_data == null:
		return
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	var parent := Node3D.new()
	add_child_autofree(parent)
	placer.place(mission, parent)  # builds the MissionObjects container
	var container: Node3D = parent.get_node_or_null("MissionObjects")
	assert_not_null(container)
	if container == null:
		return

	var record := mission.add_entity(
		NovaMissionData.KIND_ITEM, 105004, Vector3(3, 4, 5), Vector3.ZERO)
	var delta: Dictionary = placer.place_single(
		mission, container, NovaMissionData.KIND_ITEM, int(record["index"]))

	assert_eq(int(delta.get("animated", -1)), 1,
		"place_single routes a live-PANM graphic to a live model")
	assert_eq(int(delta.get("batched", -1)), 0, "not to a single-instance batch")


func test_inert_panm_model_applies_robj_base_once_not_every_frame() -> void:
	var data := _armry_data(false)
	assert_not_null(data, "inert PANM fixture loads")
	if data == null:
		return
	assert_false(data.has_live_panm_for_lod(0), "fixture PANM is exactly inert")
	var model := PanmRuntimeModel.new()
	add_child_autofree(model)
	model.set_object_data(data)
	var build_evals := model.robj_eval_count
	assert_eq(build_evals, 1, "rebuild applies the authored ROBJ base pose exactly once")

	model.run_runtime_frame()
	model.run_runtime_frame()

	assert_eq(model.robj_eval_count, build_evals,
			"inert ROBJ transforms are retained instead of re-evaluated per frame")
	model.set_ctrl_value("VEHICLE_SPECIAL1", 123)
	assert_eq(model.robj_eval_count, build_evals + 1,
			"an exact runtime mutator still forces one defensive ROBJ refresh")
	model.run_runtime_frame()
	assert_eq(model.robj_eval_count, build_evals + 1,
			"the mutation refresh does not turn back into continuous work")


func test_live_panm_model_keeps_evaluating_robj_each_frame() -> void:
	var data := _armry_data(true)
	assert_not_null(data, "live PANM fixture loads")
	if data == null:
		return
	assert_true(data.has_live_panm_for_lod(0), "fixture carries a live PANM track")
	var model := PanmRuntimeModel.new()
	add_child_autofree(model)
	model.set_object_data(data)
	var build_evals := model.robj_eval_count

	model.run_runtime_frame()
	model.run_runtime_frame()

	assert_eq(model.robj_eval_count, build_evals + 2,
			"live time/register PANM retains one ROBJ evaluation per frame")
