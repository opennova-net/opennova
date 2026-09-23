extends GutTest

# The local view's VIRTUAL DISPLAY: the first-person cockpit a vehicle's render
# class draws INSTEAD of its hull while this machine's player is the vehicle's
# claimant (stock data: the M1A1's `Virtualdisplay tankdrvr camera`, the T80's
# `t80_drvr camera`). The engine decides the swap and publishes its verdict on
# the per-frame local view (active, the carrier's packed handle, the lowercased
# graphic key; the ctest local_player_view suite pins that rule) while the
# hull's present row is PF_LOCAL_VIEW_SUPPRESSED the same frame.
# EntityPresenter.present_virtual_display is the device half. The synthetic
# cases feed it the same triple over real components: real ObjectModel nodes
# over the minted models, a real EntityIndex / wire registry, a real
# MissionObjectPlacer and PF-layout rows through the public present_snapshot.
# The installed cases drive the whole path (GameWorld frame legs -> Simulation
# -> LocalPlayerPresenter -> EntityPresenter) over the retail M1A1; they pend
# without OPENNOVA_JO_DIR.

const MountLook := preload("res://tests/support/mount_look.gd")

const HULL_3DI := "res://../fixtures/threedi/synth/tank.3di"
# The stand-in cockpit: like the retail driver displays, `mount` authors a
# `Camera` userpoint.
const DISPLAY_3DI := "res://../fixtures/threedi/synth/mount.3di"
const OTHER_DISPLAY_3DI := "res://../fixtures/threedi/synth/crate.3di"
const DISPLAY_KEY := "tankdrvr"
const OTHER_DISPLAY_KEY := "t80_drvr"
const HULL_BMS_ID := 41
const HULL_HANDLE := 0x1005 # pool 1, slot 5
const M1A1_ITEM := 100164
const TRAINING_MISSION := "07TR.bms"

var _installed_presenter: LocalPlayerPresenter


func after_each() -> void:
	if is_instance_valid(_installed_presenter):
		_installed_presenter.teardown()
	_installed_presenter = null


# --- real-component fixtures --------------------------------------------------

func _data(res_path: String) -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(res_path)), OK)
	return data


func _hull(container: Node3D) -> ObjectModel:
	var hull := ObjectModel.new()
	container.add_child(hull)
	hull.set_process(false)
	hull.set_object_data(_data(HULL_3DI))
	return hull


func _placer() -> MissionObjectPlacer:
	var placer := MissionObjectPlacer.new()
	assert_true(placer.register_object_data(DISPLAY_KEY, _data(DISPLAY_3DI)))
	assert_true(placer.register_object_data(OTHER_DISPLAY_KEY, _data(OTHER_DISPLAY_3DI)))
	return placer


# The placed walk over one authored hull (the host / single-player shape).
func _placed_pass(hull: ObjectModel, placer: MissionObjectPlacer) -> EntityPresenter:
	hull.entity_ref = EntityRef.make(1, HULL_BMS_ID, HULL_BMS_ID)
	var models: Array[ObjectModel] = [hull]
	var index := EntityIndex.new()
	index.build(models, null)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(null, index, placer)
	return presenter


# One PF-layout hull row, as Simulation.get_present_snapshot() emits it (pure
# data): identity, the present pose, the two visibility verdicts, and the
# "no clip" defaults that keep the body legs inert.
func _hull_row(position: Vector3, rotation_deg: Vector3, suppressed: bool) -> PackedFloat32Array:
	var row := PackedFloat32Array()
	row.resize(Simulation.PF_STRIDE)
	row[Simulation.PF_KIND] = 1.0
	row[Simulation.PF_INDEX] = float(HULL_BMS_ID)
	row[Simulation.PF_BMS_ID] = float(HULL_BMS_ID)
	row[Simulation.PF_WIRE_HANDLE] = float(HULL_HANDLE)
	row[Simulation.PF_POS_X] = position.x
	row[Simulation.PF_POS_Y] = position.y
	row[Simulation.PF_POS_Z] = position.z
	row[Simulation.PF_PITCH_DEG] = rotation_deg.x
	row[Simulation.PF_YAW_DEG] = rotation_deg.y
	row[Simulation.PF_ROLL_DEG] = rotation_deg.z
	row[Simulation.PF_BODY_ANIM_SLOT] = -1.0
	row[Simulation.PF_ANIM_STATE] = -1.0
	row[Simulation.PF_ANIM_SOURCE_STATE] = -1.0
	row[Simulation.PF_ANIM_SOURCE_PHASE_TICKS] = -1.0
	row[Simulation.PF_ANIM_BLEND_WEIGHT] = 1.0
	row[Simulation.PF_ALIVE] = 1.0
	row[Simulation.PF_LOCAL_VIEW_SUPPRESSED] = 1.0 if suppressed else 0.0
	return row


func _present(presenter: EntityPresenter, row: PackedFloat32Array) -> void:
	presenter.present_snapshot(row, Simulation.PF_STRIDE, 1)


func _surfaces(model: ObjectModel) -> Array[Node]:
	return model.find_children("*", "GeometryInstance3D", true, false)


# --- the synthetic cases ------------------------------------------------------

func test_display_draws_in_the_suppressed_hulls_place_and_hides_when_the_frame_clears() -> void:
	var container := PresentPassFixture.container(self)
	var hull := _hull(container)
	var placer := _placer()
	var presenter := _placed_pass(hull, placer)
	_present(presenter, _hull_row(Vector3(12, 3, -40), Vector3(4, 35, -6), true))
	assert_false(hull.visible, "the local-view suppressed hull row hides its own node")
	assert_eq(hull.global_transform.origin, Vector3(12, 3, -40),
			"the hidden hull keeps being stamped with its present pose")
	assert_false(hull.global_transform.basis.is_equal_approx(Basis.IDENTITY),
			"the fixture pose is a real attitude, not the identity")
	assert_eq(presenter.resolve_present_handle(HULL_HANDLE), hull,
			"the placed row plan resolves the carrier by its packed handle")
	assert_null(presenter.virtual_display_node(), "nothing is built before the frame asks")

	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	var display := presenter.virtual_display_node()
	assert_not_null(display, "the active frame builds the display model")
	if display == null:
		return
	assert_eq(display.get_graphic_name(), DISPLAY_KEY, "built from the frame's graphic key")
	assert_eq(display.get_object_data(), placer.object_data_for(DISPLAY_KEY),
			"...through the placer's one model cache")
	assert_true(display.visible)
	assert_true(display.is_visible_in_tree())
	assert_eq(display.global_transform, hull.global_transform,
			"rigidly at the hidden hull's current presentation transform")
	assert_eq(display.get_parent(), container,
			"a sibling of its carrier in the mission container, not a child of the hidden hull")

	# The frame clears (dismount, the chase camera, death): the hull row is no
	# longer suppressed and the display goes away.
	_present(presenter, _hull_row(Vector3(12, 3, -40), Vector3(4, 35, -6), false))
	presenter.present_virtual_display(false, Simulation.INVALID_WIRE_HANDLE, "")
	assert_true(hull.visible, "the hull draws again")
	assert_false(display.visible, "the display does not")
	assert_eq(presenter.virtual_display_node(), display, "the built model waits for the next show")


func test_display_follows_the_hull_each_frame_and_builds_one_model_per_key() -> void:
	var container := PresentPassFixture.container(self)
	var hull := _hull(container)
	var presenter := _placed_pass(hull, _placer())
	_present(presenter, _hull_row(Vector3(1, 0, 2), Vector3(0, 10, 0), true))
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	var display := presenter.virtual_display_node()
	assert_not_null(display)
	if display == null:
		return
	var children_after_build := container.get_child_count()
	for frame in range(1, 6):
		# The walk stamps the hull first, the display reads that stamp after.
		_present(presenter, _hull_row(Vector3(1 + frame * 0.75, frame * 0.1, 2 - frame),
				Vector3(frame * 1.5, 10 + frame * 7, -frame), true))
		presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
		assert_eq(presenter.virtual_display_node(), display, "frame %d reuses the model" % frame)
		assert_eq(display.global_transform, hull.global_transform,
				"frame %d: the display sits on this frame's hull stamp" % frame)
	assert_eq(container.get_child_count(), children_after_build, "no node is added per frame")

	# Hidden and shown again: still the one model.
	presenter.present_virtual_display(false, Simulation.INVALID_WIRE_HANDLE, "")
	assert_false(display.visible)
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	assert_eq(presenter.virtual_display_node(), display)
	assert_true(display.visible)
	assert_eq(container.get_child_count(), children_after_build)


func test_display_is_an_ordinary_world_model_lit_as_its_carrier() -> void:
	var container := PresentPassFixture.container(self)
	var hull := _hull(container)
	var presenter := _placed_pass(hull, _placer())
	hull.set_entity_lighting_context(0.5, false, 0.0)
	_present(presenter, _hull_row(Vector3(5, 0, 5), Vector3.ZERO, true))
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	var display := presenter.virtual_display_node()
	assert_not_null(display)
	if display == null:
		return
	var surfaces := _surfaces(display)
	assert_gt(surfaces.size(), 0, "the display model built its surfaces")
	var world_layers: int = Water.VISUAL_LAYER_WORLD | Water.VISUAL_LAYER_WORLD_NO_MIRROR
	for node in surfaces:
		var surface := node as GeometryInstance3D
		assert_ne(surface.layers & world_layers, 0, "%s draws on a world layer" % surface.name)
		assert_eq(surface.layers & (Water.VISUAL_LAYER_VIEWMODEL
				| Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY), 0,
				"%s is no first-person viewmodel/body layer member" % surface.name)
	assert_eq(display.get_entity_light_owner(), hull,
			"one entity submission: the display shares its carrier's light query")
	assert_almost_eq(display.get_lighting_effect_scale(), 0.5, 0.0001,
			"the carrier's sun-visibility factor lights the display")
	assert_false(display.is_interior_lerp())

	# The carrier's context moves (a new sun-quality verdict, an interior
	# parent): the next frame carries it over.
	hull.set_entity_lighting_context(0.25, true, 0.4)
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	assert_almost_eq(display.get_lighting_effect_scale(), 0.25, 0.0001)
	assert_true(display.is_interior_lerp())
	assert_almost_eq(display.get_interior_daylight(), 0.4, 0.0001)


func test_no_authored_display_unknown_carrier_and_a_held_hull_draw_nothing() -> void:
	var container := PresentPassFixture.container(self)
	var hull := _hull(container)
	var presenter := _placed_pass(hull, _placer())
	_present(presenter, _hull_row(Vector3(3, 0, 3), Vector3.ZERO, true))

	# The def authors no virtual display: the hull is swapped for NOTHING.
	presenter.present_virtual_display(true, HULL_HANDLE, "")
	assert_null(presenter.virtual_display_node(), "an empty graphic key builds nothing")
	# A carrier neither walk presents, and a graphic the root does not carry.
	presenter.present_virtual_display(true, 0x1777, DISPLAY_KEY)
	assert_null(presenter.virtual_display_node(), "an unresolved carrier builds nothing")
	presenter.present_virtual_display(true, HULL_HANDLE, "no_such_display")
	assert_null(presenter.virtual_display_node(), "an unresolved graphic builds nothing")

	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	var display := presenter.virtual_display_node()
	assert_not_null(display)
	if display == null:
		return
	assert_true(display.visible)
	# The occlusion frame's collector gate still holds for the swapped draw.
	hull.set_occlusion_hidden(true)
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	assert_false(display.visible, "a hull the collector holds draws no display either")
	hull.set_occlusion_hidden(false)
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	assert_true(display.visible)
	# An active frame whose key went empty hides the built model too.
	presenter.present_virtual_display(true, HULL_HANDLE, "")
	assert_false(display.visible)


func test_a_new_key_and_the_runtime_reset_free_the_model() -> void:
	var container := PresentPassFixture.container(self)
	var hull := _hull(container)
	var presenter := _placed_pass(hull, _placer())
	_present(presenter, _hull_row(Vector3(7, 1, -7), Vector3(0, 90, 0), true))
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	var first := presenter.virtual_display_node()
	assert_not_null(first)
	if first == null:
		return

	# Another vehicle's display: the old model is freed, never stacked.
	presenter.present_virtual_display(true, HULL_HANDLE, OTHER_DISPLAY_KEY)
	var second := presenter.virtual_display_node()
	assert_not_null(second)
	if second == null:
		return
	assert_ne(second, first)
	assert_true(first.is_queued_for_deletion(), "the previous key's model is freed")
	assert_eq(second.get_graphic_name(), OTHER_DISPLAY_KEY)
	assert_eq(second.global_transform, hull.global_transform)

	# The Stop -> Play / round-restart boundary (teardown runs the same reset).
	presenter.reset_wire_runtime_state()
	assert_null(presenter.virtual_display_node(), "the reset forgets the display")
	assert_true(second.is_queued_for_deletion(), "...and frees its node")
	await get_tree().process_frame
	assert_false(is_instance_valid(first))
	assert_false(is_instance_valid(second))
	for child in container.get_children():
		assert_eq(child, hull, "only the hull is left in the container")


func test_a_wire_hull_resolves_through_the_wire_registry() -> void:
	# A joiner has no authored placed nodes: its hull is a wire body.
	var container := PresentPassFixture.container(self)
	var hull := _hull(container)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_wire(null, _placer(), container, null)
	presenter.register_wire_node(HULL_HANDLE, hull)
	hull.transform = Transform3D(Basis.from_euler(Vector3(0.05, 1.2, -0.1)), Vector3(-30, 2, 18))
	hull.visible = false # what the wire walk writes for a suppressed row
	assert_eq(presenter.resolve_present_handle(HULL_HANDLE), hull)

	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	var display := presenter.virtual_display_node()
	assert_not_null(display)
	if display == null:
		return
	assert_true(display.is_visible_in_tree())
	assert_eq(display.get_parent(), container)
	assert_eq(display.global_transform, hull.global_transform)
	# The wire collector gate holds the display like the body it replaces.
	presenter.set_render_culled(HULL_HANDLE, true)
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	assert_false(display.visible)
	presenter.set_render_culled(HULL_HANDLE, false)
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	assert_true(display.visible)

	# A respawned body (a new node under the same handle) takes a fresh model:
	# the light select reads a model's light owner once, at registration.
	var respawned := _hull(container)
	respawned.transform = Transform3D(Basis.from_euler(Vector3(0, -0.7, 0)), Vector3(44, 1, -9))
	presenter.register_wire_node(HULL_HANDLE, respawned)
	presenter.present_virtual_display(true, HULL_HANDLE, DISPLAY_KEY)
	var next := presenter.virtual_display_node()
	assert_not_null(next)
	if next == null:
		return
	assert_ne(next, display, "another carrier node is another model")
	assert_true(display.is_queued_for_deletion(), "...and the old one is freed")
	assert_eq(next.get_entity_light_owner(), respawned)
	assert_eq(next.global_transform, respawned.global_transform)


# --- the installed M1A1, end to end -------------------------------------------

# One installed case's fixture: the packaged world over the retail training
# mission with the local player in the M1A1 driver's seat.
class Installed:
	extends RefCounted
	var world: GameWorld
	var sim: Simulation
	var camera: Camera3D
	var entities: EntityPresenter
	var hull: EntityCard


func _frames(fixture: Installed, count: int) -> void:
	for _i in range(count):
		var frame_input := _installed_presenter.before_world_tick(Simulation.tick_dt(), false, true)
		# The real frame-leg table: the session leg presents the entity rows,
		# the local-view leg after it places the camera and feeds the display.
		fixture.world.tick(fixture.camera.global_position, fixture.camera.global_transform,
				Simulation.tick_dt(), frame_input)


# Null after pending() without the install (or after a failed assertion).
func _board_installed_m1a1() -> Installed:
	if RetailData.install().is_empty():
		pending("OPENNOVA_JO_DIR is required for the installed M1A1 driver display (%s)"
				% TRAINING_MISSION)
		return null
	var art := RetailData.mount_install_with(TRAINING_MISSION)
	if art == null:
		pending("OPENNOVA_JO_DIR carries no %s for the installed M1A1 driver display"
				% TRAINING_MISSION)
		return null
	var fixture := Installed.new()
	fixture.world = WorldFixture.make_world(self)
	fixture.world.set_resource_root(art)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(art, TRAINING_MISSION), OK)
	var tank_bms_id := -1
	for record: EntityRef in mission.get_all_entity_refs():
		if record.item_id == M1A1_ITEM:
			tank_bms_id = record.bms_id
			break
	assert_gte(tank_bms_id, 0, "the training mission places an M1A1")
	if tank_bms_id < 0:
		return null
	assert_eq(fixture.world.load_mission_data(mission, TRAINING_MISSION), OK)
	fixture.world.set_process(false)
	fixture.sim = fixture.world.get_sim()
	fixture.camera = Camera3D.new()
	add_child_autofree(fixture.camera)
	fixture.camera.make_current()
	_installed_presenter = LocalPlayerPresenter.new()
	add_child_autofree(_installed_presenter)
	_installed_presenter.setup(fixture.world, fixture.camera, null, ControlsModel.new())
	_installed_presenter.set_input_override(PlayerMoveIntent.new())
	fixture.entities = fixture.world.get_runtime().get_entity_presenter()
	_frames(fixture, 4)
	assert_false(fixture.world.local_player_view().virtual_display_active, "on foot: no display")
	assert_null(fixture.entities.virtual_display_node())

	# The driver's seat: the claimant of the hull.
	fixture.hull = fixture.sim.entity_card_by_net_id(tank_bms_id)
	assert_not_null(fixture.hull)
	if fixture.hull == null:
		return null
	var seat: EntityCardSeat = fixture.hull.get_seats()[0]
	var target := fixture.hull.get_mission_position() + seat.get_local().rotated(
			Vector3(0, 0, 1), deg_to_rad(-fixture.hull.get_yaw_deg()))
	MountLook.face(fixture.sim, target, target + Vector3(1, 0, 0))
	assert_true(fixture.sim.local_player_toggle_mount(), "board the training tank")
	_frames(fixture, 30)
	var view := fixture.world.local_player_view()
	assert_true(view.mounted)
	# A control seat opens on the chase camera (the retail default preference):
	# the hull draws and no display is even built.
	assert_true(view.third_person, "the driver's seat opens on the chase camera")
	assert_false(view.virtual_display_active)
	assert_null(fixture.entities.virtual_display_node(), "the chase camera builds no cockpit")
	return fixture


func test_installed_m1a1_driver_view_swaps_the_hull_for_its_cockpit() -> void:
	var fixture := _board_installed_m1a1()
	if fixture == null:
		return
	var entities := fixture.entities
	var hull_handle := fixture.hull.get_wire_handle()

	# First person: the engine frame swaps the driven hull for its display.
	_installed_presenter.apply_view_action(Simulation.VIEW_ACTION_FIRST_PERSON)
	_frames(fixture, 2)
	var view := fixture.world.local_player_view()
	assert_false(view.third_person)
	assert_true(view.virtual_display_active,
			"the engine frame swaps the driven tank's hull in first person")
	assert_eq(view.virtual_display_model, "tankdrvr", "the M1A1's authored display")
	assert_eq(view.virtual_display_carrier, hull_handle)

	var hull_node := entities.resolve_present_handle(hull_handle)
	assert_not_null(hull_node, "the hull's present node resolves by handle")
	var display := entities.virtual_display_node()
	assert_not_null(display, "the cockpit model is built")
	if hull_node == null or display == null:
		return
	assert_false(hull_node.visible, "the hull is local-view suppressed")
	assert_eq(display.get_graphic_name(), "tankdrvr")
	assert_gt(_surfaces(display).size(), 0, "the installed cockpit model built its surfaces")
	assert_true(display.is_visible_in_tree())
	assert_eq(display.get_parent(), hull_node.get_parent())
	assert_eq(display.global_transform, hull_node.global_transform,
			"the cockpit sits at the hull's presentation transform")
	# The engine seats the first-person eye on the display model's authored
	# `camera` userpoint, pulled 0.1875 back along the view: the drawn model
	# puts that point where the camera looks out from, so the driver sits
	# INSIDE the cockpit that is drawn.
	var data := display.get_object_data()
	var eye_local := Vector3.INF
	for i in range(data.get_user_point_count()):
		var info := data.get_user_point_info(i)
		if String(info.name).to_lower() == "camera":
			eye_local = info.position
	assert_true(eye_local.is_finite(), "the installed display authors its camera userpoint")
	# The eye rides the sim's fixed-point hull matrix; the drawn cockpit rides
	# the hull's present row, which carries the motor's BAM-precise pitch and
	# roll, so both are the one attitude and only float32 rounding separates
	# them: two world points some 840 units from the origin, where one float
	# step is 0.00006. Measured on this hull: 0.00002 parked (pitch -0.5, roll
	# -0.9 degrees) and at most 0.00004 over the drive below (pitch to -3.2).
	# EYE_TOLERANCE is half a millimetre: room for that rounding on any map
	# position, while a whole-degree attitude misses by centimetres (half a
	# degree swings this 1.85-unit arm 0.016) and a wrong axis by metres.
	const CAMERA_PULL_BACK := 0.1875
	const EYE_TOLERANCE := 0.0005
	assert_almost_eq(fixture.camera.global_position.distance_to(
			display.global_transform * eye_local), CAMERA_PULL_BACK, EYE_TOLERANCE,
			"the camera sits on the drawn cockpit's camera point (the 0.1875 pull-back away)")

	# Driving: every frame the display lands on THAT frame's hull stamp (the
	# local-view leg runs after the session leg's entity rows), and the eye
	# stays on its camera point while the accelerating hull pitches and rolls.
	var forward := PlayerMoveIntent.new()
	forward.forward = true
	_installed_presenter.set_input_override(forward)
	var start := hull_node.global_transform
	var steepest_tilt_deg := 0.0
	for _i in range(40):
		_frames(fixture, 1)
		assert_eq(display.global_transform, hull_node.global_transform)
		assert_almost_eq(fixture.camera.global_position.distance_to(
				display.global_transform * eye_local), CAMERA_PULL_BACK, EYE_TOLERANCE,
				"the eye keeps the drawn camera point on a pitched and rolled hull")
		steepest_tilt_deg = maxf(steepest_tilt_deg, rad_to_deg(
				hull_node.global_transform.basis.y.normalized().angle_to(Vector3.UP)))
	_installed_presenter.set_input_override(PlayerMoveIntent.new())
	assert_gt(steepest_tilt_deg, 1.5,
			"the drive tilts the hull off level (a level hull would prove nothing about attitude)")
	assert_ne(hull_node.global_transform, start, "the driven hull moved under the display")
	assert_eq(entities.virtual_display_node(), display, "one model for the whole drive")

	# The chase camera draws the hull again; first person swaps back.
	_installed_presenter.apply_view_action(Simulation.VIEW_ACTION_CHASE)
	_frames(fixture, 2)
	assert_false(display.visible, "the chase camera draws no cockpit")
	assert_true(hull_node.visible, "...and the hull again")
	_installed_presenter.apply_view_action(Simulation.VIEW_ACTION_FIRST_PERSON)
	_frames(fixture, 2)
	assert_true(display.visible)
	assert_false(hull_node.visible)
	assert_eq(entities.virtual_display_node(), display, "the same model shows again")

	# The presenter going away ends the display at once; a fresh setup over
	# the same seated player shows it again.
	_installed_presenter.teardown()
	assert_false(display.visible, "teardown hides the display")
	_installed_presenter.setup(fixture.world, fixture.camera, null, ControlsModel.new())
	_installed_presenter.set_input_override(PlayerMoveIntent.new())
	_frames(fixture, 2)
	assert_true(display.visible)
	assert_eq(entities.virtual_display_node(), display)

	# Death ends the driver's view.
	assert_eq(fixture.sim.debug_kill_player_entity(fixture.sim.get_local_player_wire_handle()), OK)
	_frames(fixture, 10)
	assert_false(fixture.world.local_player_view().virtual_display_active,
			"the dead driver's frame draws no display")
	assert_false(display.visible, "the display goes with the driver's life")
	assert_true(hull_node.visible, "the hull draws for the death camera")

	# Mission unload frees it with the mission container: no node is left.
	_installed_presenter.teardown()
	fixture.world.unload()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_false(is_instance_valid(display), "the unloaded mission leaves no display node")


func test_installed_m1a1_display_ends_when_the_driver_gives_up_the_claim() -> void:
	# The training mission belts the player in (its script sets the seatbelt, so
	# USE cannot leave the tank); moving to the turret seat is the way out of
	# the driver's claim that it allows.
	var fixture := _board_installed_m1a1()
	if fixture == null:
		return
	_installed_presenter.apply_view_action(Simulation.VIEW_ACTION_FIRST_PERSON)
	_frames(fixture, 2)
	var hull_node := fixture.entities.resolve_present_handle(fixture.hull.get_wire_handle())
	var display := fixture.entities.virtual_display_node()
	assert_not_null(hull_node)
	assert_not_null(display)
	if hull_node == null or display == null:
		return
	assert_true(display.visible)
	assert_false(hull_node.visible)

	assert_true(fixture.sim.local_player_select_seat(1), "move to the turret gunner's seat")
	_frames(fixture, 30)
	var view := fixture.world.local_player_view()
	assert_true(view.mounted, "still aboard")
	assert_false(view.virtual_display_active, "the gunner is not the hull's claimant")
	assert_false(display.visible, "the display ends with the claim")
	assert_true(hull_node.visible, "the gunner sees the hull")
