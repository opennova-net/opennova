extends GutTest

# The one-view XR interface (godot/src/render/target_projection_xr_interface.h):
# a 512 x 512 SubViewport served through it rasterises the served projection,
# which no Camera3D can draw on a square target (Godot builds every camera
# projection from its viewport's own ratio). Retail rasterises the NVG scene
# and the water mirror 512 x 512 through the main view's frustum, with
# non-square texels. The raster pins need a windowed Forward+ run; the
# bookkeeping pins run headless too.

const SIDE := 512
const FOV_H := 80.0
# The 2000 x 1200 fixture frame (the registered 16:10 comparison profile).
const FRAME_ASPECT := 2000.0 / 1200.0
const INTERFACE_NAME := "OpenNovaTargetProjection"


func after_each() -> void:
	# Never leave a foreign interface primary for the next test file.
	var primary := XRServer.primary_interface
	if primary != null and not (primary is TargetProjectionXrInterface):
		XRServer.primary_interface = null


func _make_target() -> Dictionary:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(SIDE, SIDE)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var cam := Camera3D.new()
	cam.keep_aspect = Camera3D.KEEP_WIDTH
	cam.fov = FOV_H
	cam.near = 0.2
	cam.far = 1000.0
	viewport.add_child(cam)
	cam.make_current()
	return {"viewport": viewport, "camera": cam}


func test_serving_is_per_target_and_the_last_release_retires_the_interface() -> void:
	var first: SubViewport = _make_target()["viewport"]
	var second: SubViewport = _make_target()["viewport"]
	var projection := Projection.create_perspective(FOV_H, FRAME_ASPECT, 0.2, 1000.0, true)
	var moved := Transform3D(Basis.IDENTITY, Vector3(1.0, 2.0, 3.0))
	assert_true(TargetProjectionXrInterface.serve(first, Transform3D.IDENTITY, projection))
	assert_true(TargetProjectionXrInterface.serve(second, moved, Projection.IDENTITY))
	var primary := XRServer.primary_interface
	assert_true(primary is TargetProjectionXrInterface, "serving makes the interface primary")
	if primary == null:
		return
	assert_true(primary.is_initialized())
	assert_eq(primary.get_render_target_size(), Vector2(SIDE, SIDE))
	assert_eq(primary.get_view_count(), 1)
	assert_eq(TargetProjectionXrInterface.TARGET_SIDE, SIDE)
	assert_true(first.use_xr)
	assert_true(second.use_xr)
	assert_eq(TargetProjectionXrInterface.served_projection(first), projection,
			"each target keeps its own projection")
	assert_eq(TargetProjectionXrInterface.served_transform(second), moved,
			"each target keeps its own transform")

	TargetProjectionXrInterface.release(first)
	assert_false(first.use_xr)
	assert_false(TargetProjectionXrInterface.is_serving(first))
	assert_true(TargetProjectionXrInterface.is_serving(second))
	assert_true(XRServer.primary_interface is TargetProjectionXrInterface,
			"a served target keeps the interface")
	TargetProjectionXrInterface.release(second)
	assert_false(second.use_xr)
	assert_null(XRServer.primary_interface, "the last release retires the interface")
	assert_null(XRServer.find_interface(INTERFACE_NAME), "and leaves the XR server")


func test_serving_never_takes_the_primary_slot_from_another_interface() -> void:
	var target: SubViewport = _make_target()["viewport"]
	var foreign := XRInterfaceExtension.new()
	XRServer.add_interface(foreign)
	XRServer.primary_interface = foreign
	assert_false(TargetProjectionXrInterface.serve(target, Transform3D.IDENTITY,
			Projection.IDENTITY), "another primary interface keeps the XR branch")
	assert_false(target.use_xr, "the target draws through its own camera")
	assert_eq(XRServer.primary_interface, foreign)
	XRServer.primary_interface = null
	XRServer.remove_interface(foreign)
