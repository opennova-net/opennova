extends GutTest

# The foliage depth-mask consumers: every person draw (and each model drawn
# inside a person's slot, the held weapon) takes the masks of the person's
# BySide wave, 1 = far and 2 = the camera side, by its z - 1.0 against the
# water (runtime/renderer/foliage_frame.h carries the witness); any other
# model is not a consumer (0). The foliage leg runs the refresh each frame
# with the compiling camera and the frame's water height.


func _model(p_y: float) -> ObjectModel:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.global_position = Vector3(0.0, p_y, 0.0)
	return model


func test_person_draws_take_their_byside_wave() -> void:
	var person := _model(0.0)
	person.set_slot_shadow_person(true)
	var weapon := _model(1.2)
	weapon.set_slot_shadow_capture_with(person)
	var building := _model(0.0)

	# Camera above water 0.5; the person's z - 1 is below it: the far wave.
	ObjectModel.refresh_foliage_mask_frame(10.0, 0.5)
	assert_eq(person.get_foliage_mask_side(), 1.0)
	assert_eq(weapon.get_foliage_mask_side(), 1.0,
		"the held weapon rides its person's wave, not its own height")
	assert_eq(building.get_foliage_mask_side(), 0.0)

	# The water well below the person: the camera side.
	ObjectModel.refresh_foliage_mask_frame(10.0, -5.0)
	assert_eq(person.get_foliage_mask_side(), 2.0)
	assert_eq(weapon.get_foliage_mask_side(), 2.0)

	# The camera below the water: the below-water person is its side.
	ObjectModel.refresh_foliage_mask_frame(0.0, 0.5)
	assert_eq(person.get_foliage_mask_side(), 2.0)

	# Dropping the person flag and the slot link ends consumption.
	weapon.set_slot_shadow_capture_with(null)
	assert_eq(weapon.get_foliage_mask_side(), 0.0)
	person.set_slot_shadow_person(false)
	assert_eq(person.get_foliage_mask_side(), 0.0)
	ObjectModel.refresh_foliage_mask_frame(10.0, 0.5)
	assert_eq(person.get_foliage_mask_side(), 0.0)
	assert_eq(weapon.get_foliage_mask_side(), 0.0)
