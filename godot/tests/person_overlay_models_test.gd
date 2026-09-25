extends GutTest

# The person render callback's item overlays beside the held weapon: the parachute
# canopy, the night-vision goggles, the binoculars and the carried object
# (PersonOverlayModels; the gates and calibration are engine world/person_overlays.h).
#
# Each overlay is drawn RIGID [retail BoneCallback_org0_World @0x4e3940 — one matrix
# stamped into every bone slot], so its whole appearance is the transform built here and
# the draw context it inherits from the body. The placement tests use a synthetic
# skeleton whose bones carry deliberately non-identity rests (an identity rest would pass
# a wrong model->world term), and the calibration tests re-derive every constant basis
# from the original's ELEMENT PLACEMENT rather than from the shipped signs.

const BAM_PER_TURN := 4294967296.0
const HEAD := 14
const LEFT_HAND := 15
const TYPE_BODY := 6100      # -> item 106100, pump (the wire body; its rig is scaffolded)
const TYPE_FLAG := 4093      # -> item 104093, the carried flag

var _root: Node3D = null
static var _flat_dir := ""


func before_all() -> void:
	# A flat resource root of committed fixtures: the fixture items.def plus the four
	# overlay items, every graphic backed by the synthetic gun model.
	_flat_dir = OS.get_temp_dir().replace("\\", "/") + "/opennova_person_overlay_root"
	DirAccess.make_dir_recursive_absolute(_flat_dir)
	var items := FileAccess.get_file_as_string("res://../fixtures/def/items.def")
	items += "\r\n".join(PackedStringArray([
		"",
		"begin \"Parachute\"", "  id 100185", "  type object", "  graphic parachut", "end",
		"begin \"Night Vision Goggles\"", "  id 101904", "  type object", "  graphic nvg", "end",
		"begin \"Binoculars\"", "  id 101424", "  type object", "  graphic Bino_3rd", "end",
		"begin \"Flag (Red)\"", "  id 104093", "  type object", "  graphic flagred", "end",
		""]))
	var out := FileAccess.open(_flat_dir + "/items.def", FileAccess.WRITE)
	out.store_string(items)
	out.close()
	var gun := ProjectSettings.globalize_path("res://../fixtures/threedi/synth/gun.3di")
	for model in ["parachut.3di", "nvg.3di", "Bino_3rd.3di", "flagred.3di"]:
		assert(DirAccess.copy_absolute(gun, _flat_dir + "/" + model) == OK)
	assert(DirAccess.copy_absolute(
			ProjectSettings.globalize_path("res://../fixtures/threedi/synth/pump.3di"),
			_flat_dir + "/pump.3di") == OK)


func before_each() -> void:
	_root = Node3D.new()
	add_child_autofree(_root)


# --- placement --------------------------------------------------------------------------

# A skeleton with bones 0..16 whose rests are deliberately non-identity.
func _make_skeleton() -> Skeleton3D:
	var skel := Skeleton3D.new()
	for i in range(17):
		skel.add_bone("BN%d" % (i + 1))
	skel.set_bone_rest(0, Transform3D(Basis(Vector3.UP, deg_to_rad(21.0)), Vector3(0.0, 0.9, 0.0)))
	skel.set_bone_rest(HEAD, Transform3D(Basis(Vector3.RIGHT, deg_to_rad(33.0)),
			Vector3(0.0, 1.6, 0.05)))
	skel.set_bone_rest(LEFT_HAND, Transform3D(Basis(Vector3.FORWARD, deg_to_rad(-48.0)),
			Vector3(0.35, 1.1, 0.2)))
	_root.add_child(skel)
	skel.reset_bone_poses()
	return skel


func test_canopy_rides_bone_zero_translation_with_a_heading_only_turn() -> void:
	var skel := _make_skeleton()
	var xf: Transform3D = PersonOverlayModels.canopy_transform(skel, 37.0)
	# At rest the hips bone moves nothing: the model origin stays at the skeleton origin,
	# NOT at the hip joint (retail copies the matrix's translation row).
	assert_true(xf.origin.is_equal_approx(Vector3.ZERO),
			"at rest the canopy sits at the model origin, got %s" % xf.origin)
	assert_true(xf.basis.is_equal_approx(
			MissionObjectPlacer.bms_to_godot_basis(Vector3(0.0, 37.0, 0.0))),
			"the canopy turns about the up axis alone")
	# Lift the hips: the canopy follows the bone's model->world translation.
	skel.set_bone_pose_position(0, Vector3(0.0, 2.9, 0.0))
	xf = PersonOverlayModels.canopy_transform(skel, 37.0)
	assert_true(xf.origin.is_equal_approx(Vector3(0.0, 2.0, 0.0)),
			"a posed hips bone carries the canopy, got %s" % xf.origin)


func test_goggles_ride_the_head_bone_with_the_nudge() -> void:
	var skel := _make_skeleton()
	var xf: Transform3D = PersonOverlayModels.nvg_transform(skel)
	var rest_origin := Vector3(0.0, 1.6, 0.05)
	assert_true(xf.origin.is_equal_approx(rest_origin + PersonOverlayModels.nvg_nudge()),
			"at rest the nudge lands unrotated on the head joint, got %s" % xf.origin)
	assert_true(xf.basis.is_equal_approx(Basis.IDENTITY),
			"at rest the goggles take the head bone's model->world rotation (identity)")
	var turn := Basis(Vector3.UP, deg_to_rad(90.0))
	var rest := skel.get_bone_rest(HEAD).basis
	skel.set_bone_pose_rotation(HEAD, (turn * rest).get_rotation_quaternion())
	xf = PersonOverlayModels.nvg_transform(skel)
	assert_true(xf.basis.is_equal_approx(turn), "posing the head carries the goggles")
	assert_true(xf.origin.is_equal_approx(rest_origin + turn * PersonOverlayModels.nvg_nudge()),
			"the nudge rides the head's model->world rotation")


func test_binoculars_ride_the_left_hand_with_the_calibration() -> void:
	var skel := _make_skeleton()
	var xf: Transform3D = PersonOverlayModels.binoculars_transform(skel)
	assert_true(xf.origin.is_equal_approx(
			Vector3(0.35, 1.1, 0.2) + PersonOverlayModels.binoculars_nudge()),
			"at rest the nudge lands unrotated on the left-hand joint")
	assert_true(xf.basis.is_equal_approx(PersonOverlayModels.binoculars_frame_basis(Basis.IDENTITY)),
			"at rest the binoculars take the calibration alone")


func test_carried_object_takes_the_carrier_triple_at_the_left_hand() -> void:
	var skel := _make_skeleton()
	var euler := Vector3(12.0, -63.0, 4.0)
	var xf: Transform3D = PersonOverlayModels.carried_transform(skel, euler)
	var hand: Transform3D = PersonOverlayModels.binoculars_transform(skel)
	assert_true(xf.origin.is_equal_approx(hand.origin),
			"the carried object sits at the binocular frame's point")
	assert_true(xf.basis.is_equal_approx(PersonOverlayModels.carried_frame_basis(euler)),
			"the carried object takes the carrier's own orientation, not a bone's")


func test_placement_needs_the_bones() -> void:
	assert_null(PersonOverlayModels.canopy_transform(null, 0.0))
	var short_skel := Skeleton3D.new()
	short_skel.add_bone("BN1")
	_root.add_child(short_skel)
	assert_not_null(PersonOverlayModels.canopy_transform(short_skel, 0.0),
			"bone 0 alone places the canopy")
	assert_null(PersonOverlayModels.nvg_transform(short_skel), "no head bone, no goggles")
	assert_null(PersonOverlayModels.binoculars_transform(short_skel), "no hand, no binoculars")
	assert_null(PersonOverlayModels.carried_transform(short_skel, Vector3.ZERO),
			"no hand, no carried object")


# --- the calibration, re-derived from the original's element placement -----------------

# One Math_BuildRotationMatrix4x4_ByAxis block exactly as the original emits it: quantized
# cos/sin through a 4194304 fixed-point round trip, the SIN negated (`fsin * dbl_7C57B0`).
static func _quantized_trig(rad: float) -> Vector2:
	return Vector2(float(int(cos(rad) * 4194304.0)) / 4194304.0,
			float(int(sin(rad) * -4194304.0)) / 4194304.0)


# Row-major 3x3 blocks as the builder places them (axis 0 = Z: [0]=c [1]=s [4]=-s [5]=c;
# axis 1 = X: [5]=c [6]=s [9]=-s [10]=c; axis 2 = Y: [0]=c [2]=-s [8]=s [10]=c), in 3x3
# indices. [retail Math_BuildRotationMatrix4x4_ByAxis @0x611db0]
static func _block(axis: int, t: Vector2) -> PackedFloat32Array:
	match axis:
		0: return PackedFloat32Array([t.x, t.y, 0.0, -t.y, t.x, 0.0, 0.0, 0.0, 1.0])
		1: return PackedFloat32Array([1.0, 0.0, 0.0, 0.0, t.x, t.y, 0.0, -t.y, t.x])
	return PackedFloat32Array([t.x, 0.0, -t.y, 0.0, 1.0, 0.0, t.y, 0.0, t.x])


static func _mul3(a: PackedFloat32Array, b: PackedFloat32Array) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(9)
	for r in range(3):
		for c in range(3):
			var s := 0.0
			for k in range(3):
				s += a[r * 3 + k] * b[k * 3 + c]
			out[r * 3 + c] = s
	return out


# The Godot image of a MODEL-LOCAL row-major calibration C: F * C^T * F with F the
# loader's X negation (negate every element with exactly one index 0), as COLUMNS.
static func _model_local_basis(m: PackedFloat32Array) -> Basis:
	var conj := PackedFloat32Array()
	conj.resize(9)
	for i in range(3):
		for j in range(3):
			var v: float = m[j * 3 + i]
			if (i == 0) != (j == 0):
				v = -v
			conj[i * 3 + j] = v
	return Basis(Vector3(conj[0], conj[3], conj[6]), Vector3(conj[1], conj[4], conj[7]),
			Vector3(conj[2], conj[5], conj[8]))


# The Godot image of a WORLD row-major matrix M: swap . M^T . flipX per axis (the render
# frame is (-missionY, missionZ, missionX); Godot is (mX, mZ, -mY)).
static func _world_basis(m: PackedFloat32Array) -> Basis:
	var cols := []
	for axis in [Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)]:
		var r := Vector3(-axis.x, axis.y, axis.z)
		var w := Vector3(r.x * m[0] + r.y * m[3] + r.z * m[6],
				r.x * m[1] + r.y * m[4] + r.z * m[7],
				r.x * m[2] + r.y * m[5] + r.z * m[8])
		cols.append(Vector3(w.z, w.y, w.x))
	return Basis(cols[0], cols[1], cols[2])


func test_binocular_calibration_matches_the_original_composition() -> void:
	# Row-major Ry(dbl_7C9B78) * Rx(dbl_7C9B80) * Rz(dbl_7C9B88), left of M15
	# [retail Entity_BuildBoneTransformMatrices @0x4b2394..0x4b2486].
	var rz := _block(0, _quantized_trig(PersonOverlayModels.binoculars_frame_z_rad()))
	var rx := _block(1, _quantized_trig(PersonOverlayModels.binoculars_frame_x_rad()))
	var ry := _block(2, _quantized_trig(PersonOverlayModels.binoculars_frame_y_rad()))
	var expected := _model_local_basis(_mul3(_mul3(ry, rx), rz))
	var got := PersonOverlayModels.binoculars_frame_basis(Basis.IDENTITY)
	assert_true(got.is_equal_approx(expected),
			"binocular calibration: derived %s, implementation %s" % [expected, got])
	# And it rides the bone's model->world basis on the left.
	var bone := Basis(Vector3(0.3, 0.9, -0.2).normalized(), 0.8)
	assert_true(PersonOverlayModels.binoculars_frame_basis(bone).is_equal_approx(bone * got))


# The original's placement matrix (Math_BuildFixedPointToFloatMatrix4x4 @0x612200):
# v' = v * Mz(roll) * Mx(pitch) * My(yaw), quantized, from BAM angles.
static func _placement(yaw_bam: float, pitch_bam: float, roll_bam: float) -> PackedFloat32Array:
	return _mul3(_mul3(_block(0, _quantized_trig(roll_bam * TAU / BAM_PER_TURN)),
			_block(1, _quantized_trig(pitch_bam * TAU / BAM_PER_TURN))),
			_block(2, _quantized_trig(yaw_bam * TAU / BAM_PER_TURN)))


func test_carried_frame_matches_the_original_product() -> void:
	# E(carrier+4) * RotZ(flt_7CD438): a plain float Z block ([0]=c [1]=+s [4]=-s [5]=c)
	# [retail Math_BuildRotationMatrixZ_Float @0x613010; BoneCallback_org0_World @0x4e3df1].
	var k := PersonOverlayModels.carried_frame_z_rad()
	var rz := PackedFloat32Array([cos(k), sin(k), 0.0, -sin(k), cos(k), 0.0, 0.0, 0.0, 1.0])
	for mission in [Vector3.ZERO, Vector3(12.0, -63.0, 4.0), Vector3(-30.0, 170.0, 0.0)]:
		var e := _placement((90.0 - mission.y) / 360.0 * BAM_PER_TURN,
				mission.x / 360.0 * BAM_PER_TURN, mission.z / 360.0 * BAM_PER_TURN)
		var expected := _world_basis(_mul3(e, rz))
		var got := PersonOverlayModels.carried_frame_basis(mission)
		assert_true(got.is_equal_approx(expected),
				"carried frame %s: derived %s, implementation %s" % [mission, expected, got])


func test_canopy_turn_matches_the_inverted_heading_feed() -> void:
	# Axis 2 fed sinFixed = sin(h) * +2^22, cosFixed = cos(h) * -2^22 of the body heading
	# [retail BoneCallback_org0_World @0x4e39d6..0x4e3a0a]; the engine publishes the
	# mission yaw of h + pi, which the canopy turns by.
	var skel := _make_skeleton()
	for heading_deg in [0.0, 37.0, -120.0, 200.0]:
		var h := deg_to_rad(heading_deg)
		var fed := Vector2(float(int(cos(h) * -4194304.0)) / 4194304.0,
				float(int(sin(h) * 4194304.0)) / 4194304.0)
		var expected := _world_basis(_block(2, fed))
		var yaw := fposmod(90.0 - (heading_deg + 180.0), 360.0)
		var got: Transform3D = PersonOverlayModels.canopy_transform(skel, yaw)
		assert_true(got.basis.is_equal_approx(expected),
				"heading %s: derived %s, implementation %s" % [heading_deg, expected, got.basis])


# --- the wire walk ----------------------------------------------------------------------

var _sims: Array[Simulation] = []


func after_each() -> void:
	_sims.clear()


func _placer() -> MissionObjectPlacer:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_flat_dir), OK, "the flat fixture root mounts")
	var item_db := ItemDatabase.new()
	assert_eq(item_db.load(_flat_dir + "/items.def"), OK)
	return MissionObjectPlacer.create(root, item_db)


func _wire_presenter(container: Node3D) -> EntityPresenter:
	var sim := Simulation.new()
	_sims.append(sim)
	var presenter := EntityPresenter.new()
	add_child_autofree(presenter)
	presenter.setup_wire(sim, _placer(), container, null)
	return presenter


# One wire body row at handle 0x0004 with the given overlay fields.
func _row(fields: Dictionary) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(Simulation.PF_STRIDE)
	out[Simulation.PF_TYPE_ID] = float(TYPE_BODY)
	out[Simulation.PF_WIRE_HANDLE] = float(0x0004)
	out[Simulation.PF_KIND] = -1.0
	out[Simulation.PF_INDEX] = -1.0
	out[Simulation.PF_ALIVE] = 1.0
	out[Simulation.PF_BODY_ANIM_SLOT] = -1.0
	for field in [Simulation.PF_ANIM_STATE, Simulation.PF_ANIM_PHASE_TICKS,
			Simulation.PF_ANIM_SOURCE_STATE, Simulation.PF_ANIM_SOURCE_PHASE_TICKS,
			Simulation.PF_ANIM_STATE_PULSE, Simulation.PF_ANIM_PULSE_TICKS,
			Simulation.PF_WPN_ANIM_STATE, Simulation.PF_WPN_PHASE_TICKS,
			Simulation.PF_WPN_SOURCE_STATE, Simulation.PF_WPN_SOURCE_PHASE_TICKS,
			Simulation.PF_CARRIER_HANDLE]:
		out[field] = -1.0
	out[Simulation.PF_ANIM_BLEND_WEIGHT] = 1.0
	out[Simulation.PF_WPN_BLEND_WEIGHT] = 1.0
	out[Simulation.PF_ANIM_REMOTE_REQUEST] = 1.0
	for key in fields.keys():
		out[int(key)] = float(fields[key])
	return out


func test_wire_body_draws_its_overlays_in_its_own_context() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var p := _wire_presenter(container)
	p.present_wire_snapshot(_row({}), Simulation.PF_STRIDE, 1)
	assert_null(p.person_overlays_for(0x0004),
			"a body that never published an overlay allocates nothing")
	var body: ObjectModel = p.resolve_wire_handle(0x0004)
	assert_not_null(body, "the wire body is built")
	# The pump has no rig; scaffold the bones the overlays ride (as the held weapon's
	# wire test does).
	var skel := Skeleton3D.new()
	for i in range(17):
		skel.add_bone("BN%d" % (i + 1))
	body.add_child(skel)

	var fields := {
		Simulation.PF_CANOPY_PARA: 1536, Simulation.PF_CANOPY_PARA_O: 1024,
		Simulation.PF_CANOPY_YAW_DEG: 30.0,
		Simulation.PF_NVG_WORN: 1, Simulation.PF_NVG_FLIP: 65535,
		Simulation.PF_BINOCULARS_RAISED: 1,
		Simulation.PF_CARRIED_TYPE_ID: TYPE_FLAG,
		Simulation.PF_CARRIED_YAW_DEG: -40.0,
	}
	p.present_wire_snapshot(_row(fields), Simulation.PF_STRIDE, 1)
	var overlays: PersonOverlayModels = p.person_overlays_for(0x0004)
	assert_not_null(overlays, "the first published overlay allocates the body's set")
	var canopy := overlays.get_node(PersonOverlayModels.KIND_CANOPY, PersonOverlayModels.PASS_FIRST) as ObjectModel
	var nvg := overlays.get_node(PersonOverlayModels.KIND_NVG, PersonOverlayModels.PASS_FIRST) as ObjectModel
	var binoc := overlays.get_node(PersonOverlayModels.KIND_BINOCULARS, PersonOverlayModels.PASS_FIRST) as ObjectModel
	var carried := overlays.get_node(PersonOverlayModels.KIND_CARRIED, PersonOverlayModels.PASS_FIRST) as ObjectModel
	for model in [canopy, nvg, binoc, carried]:
		assert_not_null(model, "every published overlay builds its item model")
		if model == null:
			return
		assert_true(model.visible, "%s draws with its body" % model.name)
		assert_eq(model.get_authored_lod_owner(), body,
				"%s draws at its body's RLOD level, never its own walk" % model.name)
		assert_eq(model.get_parent(), container, "%s is a sibling of its body" % model.name)
	assert_eq(canopy.get_graphic_name(), "parachut", "the canopy is item 185's model")
	assert_eq(nvg.get_graphic_name(), "nvg", "the goggles are item 1904's model")
	assert_eq(binoc.get_graphic_name(), "Bino_3rd", "the binoculars are item 1424's model")
	assert_eq(carried.get_graphic_name(), "flagred", "the carried object is its own type's model")
	var ctrl := canopy.get_ctrl_values()
	assert_eq(int(ctrl.get("PARA", -1)), 1536, "the canopy carries PARA")
	assert_eq(int(ctrl.get("PARA_O", -1)), 1024, "the canopy carries PARA_O")
	assert_eq(int(nvg.get_ctrl_values().get("NVG_FLIP", -1)), 65535, "the goggles carry NVG_FLIP")
	assert_true(canopy.global_transform.is_equal_approx(
			PersonOverlayModels.canopy_transform(body, 30.0)), "the canopy sits at its frame")
	assert_true(carried.global_transform.is_equal_approx(
			PersonOverlayModels.carried_transform(body, Vector3(0.0, -40.0, 0.0))),
			"the carried object sits at its frame")
	assert_null(overlays.get_node(PersonOverlayModels.KIND_CANOPY, PersonOverlayModels.PASS_SECOND),
			"a single-part body draws each overlay once")

	# A hidden body hides its overlays; a cleared state hides that overlay alone.
	var hidden := fields.duplicate()
	hidden[Simulation.PF_HIDDEN] = 1
	p.present_wire_snapshot(_row(hidden), Simulation.PF_STRIDE, 1)
	for model in [canopy, nvg, binoc, carried]:
		assert_false(model.visible, "%s hides with its body" % model.name)
	var no_canopy := fields.duplicate()
	no_canopy[Simulation.PF_CANOPY_PARA] = 0
	no_canopy[Simulation.PF_CANOPY_PARA_O] = 0
	p.present_wire_snapshot(_row(no_canopy), Simulation.PF_STRIDE, 1)
	assert_false(canopy.visible, "empty canopy words draw no canopy")
	assert_true(nvg.visible, "the other overlays keep drawing")

	# Drawn inside the owner's bone callback, an overlay goes with its owner's
	# sub-pixel return (render_sector_entity @0x5c42d8..0x5c42de), as the held weapon.
	p.present_wire_snapshot(_row(fields), Simulation.PF_STRIDE, 1)
	assert_true(nvg.visible)
	var far := Transform3D(Basis.IDENTITY, Vector3(0.0, 0.0, 1000000.0))
	ObjectModel.update_authored_lods(far, 60.0, 640.0, 480.0)
	if body.is_subpixel_hidden():
		# The next present pass writes only its intent; the verdict holds.
		p.present_wire_snapshot(_row(fields), Simulation.PF_STRIDE, 1)
		assert_false(nvg.visible, "a sub-pixel owner drops its overlays")
		assert_true(nvg.is_subpixel_hidden(), "the overlay inherits the owner's verdict")
		var near := Transform3D(Basis.IDENTITY, Vector3(0.0, 0.0, 10.0))
		ObjectModel.update_authored_lods(near, 60.0, 640.0, 480.0)
		assert_true(nvg.visible, "and returns with it")
	else:
		fail_test("the far camera should drop the fixture body below 0.75 px")

	# The body leaving the walk frees its overlays.
	p.present_wire_snapshot(PackedFloat32Array(), Simulation.PF_STRIDE, 2)
	assert_null(p.person_overlays_for(0x0004), "a retired body drops its overlay set")
	assert_true(canopy.is_queued_for_deletion(), "its overlay models are freed with it")
