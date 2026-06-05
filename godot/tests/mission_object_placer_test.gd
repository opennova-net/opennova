extends GutTest

# Phase 4: MissionObjectPlacer. Covers the asset-free pieces that must be exactly
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


func test_rotation_negates_pitch_yaw_and_half_turns_yaw() -> void:
	var rot := Placer.bms_to_godot_rotation(Vector3(30, 90, 45))
	assert_almost_eq(rot.x, deg_to_rad(-30.0), 0.0001, "pitch negated")
	assert_almost_eq(rot.y, deg_to_rad(-90.0) + PI, 0.0001, "yaw negated + half turn")
	assert_almost_eq(rot.z, deg_to_rad(45.0), 0.0001, "roll preserved")


func test_entity_transform_composes_basis_and_origin() -> void:
	var pos := Vector3(10, 20, 30)
	var rot_deg := Vector3(0, 180, 0)
	var xform := Placer.entity_transform(pos, rot_deg)
	assert_true(
		xform.origin.is_equal_approx(Placer.bms_to_godot_position(pos)),
		"origin is the converted position")
	var expected_basis := Basis.from_euler(Placer.bms_to_godot_rotation(rot_deg))
	assert_true(xform.basis.is_equal_approx(expected_basis), "basis is the converted rotation")


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
	placer._static_batch_cache["StaticCrate1"] = [{
		"mesh": mesh, "material": null, "offset": offset, "submesh": 0,
	}]

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


func test_place_single_is_a_noop_on_null_inputs() -> void:
	var placer := Placer.new(null, null)
	var delta: Dictionary = placer.place_single(null, null, NovaMissionData.KIND_ITEM, 0)
	assert_eq(int(delta.get("placed", -1)), 0, "null inputs place nothing")
	assert_eq(int(delta.get("unresolved", 0)), 0, "and do not falsely count an unresolved")
