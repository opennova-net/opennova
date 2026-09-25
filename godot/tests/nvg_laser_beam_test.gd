extends GutTest

# The NVG IR laser beams (retail Entity_RenderNVGLaserBeam @ 0x5c6090 over the
# frame's visible persons, sub_5C63B0 @ 0x5c63b0): a remote person's beam
# rides the overlay tail's NvgLaserBeams slot only under the local view's
# night vision and the first-person camera, only for an unmounted person that
# is not the local player and whose held weapon carries LaserBeam, and only
# from a drawn body holding a drawn gun (the action point is that gun's
# userpoint). The engine gate, the ray clip and the ribbon are ctest's
# (nvg_laser, scene_overlay); this pins the presenter leg that applies them.

const GUN_3DI := 'res://../fixtures/threedi/synth/gun.3di'
const LASER_BEAM := 0x40000000
const HANDLE := 0x0004
const ACTION_POINT := 1  # gun.3di's MFlash01, authored along +x


func _armed_presenter() -> Array:
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(null, null, null)
	var body := ObjectModel.new()
	add_child_autofree(body)
	var gun := ObjectModel.new()
	add_child_autofree(gun)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(GUN_3DI)), OK,
			'the gun fixture loads')
	gun.set_object_data(data)
	gun.position = Vector3(2, 1, -3)
	presenter.register_wire_node(HANDLE, body)
	presenter.register_wire_held_weapon(HANDLE, gun)
	return [presenter, body, gun]


func _batches(presenter: EntityPresenter, attach_bone: int, flags: int, local_player: bool,
		nvg_active: bool, camera_mode: int) -> int:
	return presenter.nvg_laser_beam_batches(HANDLE, attach_bone, flags, ACTION_POINT,
			local_player, nvg_active, camera_mode, Transform3D(Basis(), Vector3(0, 1, 6)))


func test_the_beam_draws_only_under_night_vision_in_first_person() -> void:
	var parts := _armed_presenter()
	var presenter: EntityPresenter = parts[0]
	assert_eq(_batches(presenter, 0, LASER_BEAM, false, true, 0), 1,
			'an armed remote person draws its beam under NVG in first person')
	assert_eq(_batches(presenter, 0, LASER_BEAM, false, false, 0), 0,
			'no beam without night vision (g_NVGActive)')
	assert_eq(_batches(presenter, 0, LASER_BEAM, false, true, 1), 0,
			'no beam from the chase camera (g_camera_mode != 0)')


func test_the_person_gate() -> void:
	var parts := _armed_presenter()
	var presenter: EntityPresenter = parts[0]
	assert_eq(_batches(presenter, 3, LASER_BEAM, false, true, 0), 0,
			'a seat-mounted person (attach bone +0x157) draws none')
	assert_eq(_batches(presenter, 0, 0x08000000, false, true, 0), 0,
			'a weapon without LaserBeam draws none')
	assert_eq(_batches(presenter, 0, LASER_BEAM, true, true, 0), 0,
			'the local player draws none')


func test_the_beam_needs_the_drawn_body_and_gun() -> void:
	var parts := _armed_presenter()
	var presenter: EntityPresenter = parts[0]
	var body: ObjectModel = parts[1]
	var gun: ObjectModel = parts[2]
	gun.visible = false
	assert_eq(_batches(presenter, 0, LASER_BEAM, false, true, 0), 0,
			'no drawn gun, no action point')
	gun.visible = true
	body.visible = false
	assert_eq(_batches(presenter, 0, LASER_BEAM, false, true, 0), 0,
			'a person the frame does not draw is not in the visible list')
	body.visible = true
	assert_eq(_batches(presenter, 0, LASER_BEAM, false, true, 0), 1)
