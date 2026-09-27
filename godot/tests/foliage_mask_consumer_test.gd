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


const MASK_PROBE_SHADER := """
shader_type spatial;
render_mode unshaded;
#include "res://shaders/object/foliage_mask.gdshaderinc"
void fragment() {
	ALBEDO = vec3(obj_foliage_mask_outcome(FRAGCOORD, CAMERA_POSITION_WORLD) != 0
			? 1.0 : 0.0, 1.0, 0.0);
}
"""


# Red = the mask outcome, green = the probe drew at all (a negative case must
# not pass on an undrawn probe while its shader still compiles).
func _masked(viewport: SubViewport) -> bool:
	for _frame in 2:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var pixel := viewport.get_texture().get_image().get_pixel(8, 8)
	assert_gt(pixel.g, 0.5, "the probe quad draws")
	return pixel.r > 0.5


# Each scene pass's persons take the masks compiled for that pass: a far-wave
# person drawn by a camera at the Inset eye reads the mask texture while the
# Inset's flag is up, and neither a mismatched eye nor the main view's pair
# lets it in (the main pair keeps its own camera).
func test_inset_eye_admits_only_the_inset_view_persons() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := SubViewport.new()
	viewport.size = Vector2i(16, 16)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.current = true
	camera.position = Vector3(3.0, 1.0, 5.0)
	viewport.add_child(camera)
	var shader := Shader.new()
	shader.code = MASK_PROBE_SHADER
	var material := ShaderMaterial.new()
	material.shader = shader
	var quad := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size = Vector2(8.0, 8.0)
	quad.mesh = mesh
	quad.material_override = material
	quad.position = camera.position + Vector3(0.0, 0.0, -2.0)
	viewport.add_child(quad)
	quad.set_instance_shader_parameter("u_foliage_mask_side", 1.0)
	# A far-wave mask nearer than anything (reverse-Z depth 1).
	var mask := Image.create(1, 1, false, Image.FORMAT_RGF)
	mask.set_pixel(0, 0, Color(1.0, 0.0, 0.0))
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_depth",
			ImageTexture.create_from_image(mask))
	var eye := camera.global_position
	var elsewhere := eye + Vector3(5.0, 0.0, 0.0)
	# The probe's shader compiles off the draw path: let it land first.
	for _frame in 4:
		await get_tree().process_frame

	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_active", true)
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_eye", elsewhere)
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_inset_active", true)
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_inset_eye", eye)
	assert_true(await _masked(viewport), "the Inset eye's persons take the Inset masks")

	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_inset_active", false)
	assert_false(await _masked(viewport), "a closed Inset lets no mask in at its eye")

	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_inset_active", true)
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_inset_eye", elsewhere)
	assert_false(await _masked(viewport), "another camera never reads the Inset masks")

	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_eye", eye)
	assert_true(await _masked(viewport), "the main pair still admits its own eye")

	# Back to the project defaults (no mask pass is live in this file).
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_active", false)
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_eye", Vector3.ZERO)
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_inset_active", false)
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_inset_eye", Vector3.ZERO)
	RenderingServer.global_shader_parameter_set("opennova_foliage_mask_depth", null)
