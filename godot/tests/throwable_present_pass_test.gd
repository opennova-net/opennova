extends GutTest

const ThrowablePresentPass := preload("res://engine/world/throwable_present_pass.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


class SimStub:
	extends RefCounted
	var visuals: Array = []

	func get_throwable_visuals() -> Array:
		return visuals


class ItemDbStub:
	extends RefCounted

	func get_graphic(def_id: int) -> String:
		return "GREN_MODEL" if def_id == 101883 else ""


class PlacerStub:
	extends RefCounted
	var built: Array[Node3D] = []

	func build_model_from_graphic(_graphic: String, _anim: String,
			parent: Node3D, _clip_key: String = "", _env_node: Node = null,
			_rig_graphic: String = "") -> Node3D:
		var model := Node3D.new()
		parent.add_child(model)
		built.append(model)
		return model


func test_present_uses_the_canonical_bms_basis_and_godot_position() -> void:
	var sim := SimStub.new()
	var container := Node3D.new()
	add_child_autofree(container)
	var placer := PlacerStub.new()
	var presenter = ThrowablePresentPass.new()
	presenter.setup(sim, container, placer, ItemDbStub.new(), null)

	var position := Vector3(12.5, -4.0, 33.25)
	for rotation in [Vector3.ZERO, Vector3(0, 90, 0), Vector3(20, 35, -15)]:
		sim.visuals = [{
			"key": 7,
			"item_id": 1883,
			"pos": position,
			"rotation_deg": rotation,
		}]
		presenter.present()
		assert_eq(placer.built.size(), 1, "the same live round reuses its model")
		var model := placer.built[0]
		assert_eq(model.position, position, "Godot-space position is applied verbatim")
		assert_true(model.basis.is_equal_approx(
				MissionObjectPlacer.bms_to_godot_basis(rotation)),
				"rotation %s uses the shared BMS placement convention" % rotation)

	presenter.teardown()
