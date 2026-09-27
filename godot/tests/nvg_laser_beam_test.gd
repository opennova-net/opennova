extends GutTest

# The NVG IR laser beams (retail Entity_RenderNVGLaserBeam @ 0x5c6090 over the
# frame's visible persons, Render_NVGLaserBeamsForVisiblePersons @ 0x5c63b0): a remote person's beam
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


func _view_camera(size: Vector2i, fov: float) -> Camera3D:
	var view := SubViewport.new()
	view.size = size
	add_child_autofree(view)
	var camera := Camera3D.new()
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = fov
	view.add_child(camera)
	camera.global_transform = Transform3D(Basis.IDENTITY, Vector3(0, 0, 60))
	return camera


func _view_batches(presenter: EntityPresenter, inset_view: bool) -> int:
	return presenter.nvg_laser_beam_batches(HANDLE, 0, LASER_BEAM, ACTION_POINT, false, true,
			0, Transform3D(Basis(), Vector3(0, 1, 6)), inset_view)


# The weapon Inset pass walks the persons its own collect drew, into its own
# slot (engine renderer/scene_overlay.h kInsetOverlayOrder carries the
# witness): a person the main view culls and the Inset draws (the view split,
# its twins) beams in the Inset alone; one only the main view draws beams in
# the main view alone.
func test_each_view_beams_the_persons_its_collect_drew() -> void:
	var parts := _armed_presenter()
	var presenter: EntityPresenter = parts[0]
	var body: ObjectModel = parts[1]
	var gun: ObjectModel = parts[2]
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(GUN_3DI)), OK)
	body.set_object_data(data)
	var main_camera := _view_camera(Vector2i(640, 480), 90.0)
	var inset_camera := _view_camera(Vector2i(256, 256), 10.0)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	assert_eq(_view_batches(presenter, false), 1, "the main walk over the drawn person")
	assert_eq(_view_batches(presenter, true), 1, "the views agree: the Inset walk beams it too")
	for model: ObjectModel in [body, gun]:
		model.set_occlusion_hidden(true)
		model.set_inset_occlusion_hidden(false)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	assert_true(body.is_view_split() and gun.is_view_split())
	assert_gt(body.get_view_twin_count(), 0, "the Inset draws the person through its twins")
	assert_eq(_view_batches(presenter, false), 0, "the main collect culled the person")
	assert_eq(_view_batches(presenter, true), 1, "the Inset collect drew it: its walk beams it")
	for model: ObjectModel in [body, gun]:
		model.set_occlusion_hidden(false)
		model.set_inset_occlusion_hidden(true)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	assert_eq(_view_batches(presenter, false), 1, "the main collect draws the person again")
	assert_eq(_view_batches(presenter, true), 0, "the Inset collect culled it: no Inset beam")
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_false(body.is_view_split())
