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
