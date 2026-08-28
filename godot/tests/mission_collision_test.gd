extends GutTest

# 3di collision-volume exposure through ObjectData: which models carry BVOL
# volumes, the dict shape each volume exposes, and the skinned person's
# CFAC/COBJ-only collision. Uses the synthetic 3di fixtures; the def fixtures
# ship no .3di, so the placer's asset-free tests live elsewhere. The hull
# geometry itself is native (engine/runtime/world collision; tests/collision).

const FIXTURE_DIR := "res://../fixtures/threedi/synth"


func _model(name: String) -> ObjectData:
	var data := ObjectData.new()
	var path := ProjectSettings.globalize_path("%s/%s.3di" % [FIXTURE_DIR, name])
	assert_eq(data.open_file(path), OK, "%s.3di loads" % name)
	return data


func test_models_expose_collision_volumes() -> void:
	for name in ["house", "shed", "carrier"]:
		var data := _model(name)
		assert_true(data.has_collision(), "%s reports collision" % name)
		var vols: Array = data.get_collision_volumes()
		assert_gt(vols.size(), 0, "%s exposes volumes" % name)
		var v: Dictionary = vols[0]
		for key in ["type", "min", "max", "planes"]:
			assert_true(v.has(key), "%s volume dict has '%s'" % [name, key])
		assert_true(v["planes"] is Array, "planes is an array of Plane")


func test_model_without_collision_has_no_volumes() -> void:
	var data := _model("gun")
	assert_false(data.has_collision(), "gun carries no collision volumes")
	assert_eq(data.get_collision_volumes().size(), 0, "and none are exposed")


func test_skinned_person_reports_face_and_sphere_collision_without_volumes() -> void:
	var data := _model("person")
	assert_true(data.is_skinned(0), "person is the skeletal collision fixture")
	assert_eq(data.get_collision_volumes().size(), 0, "person has no BVOL collision")
	assert_true(data.has_collision(), "person still reports its CFAC/COBJ collision")
