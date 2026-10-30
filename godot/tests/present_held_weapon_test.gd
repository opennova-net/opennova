extends GutTest

# The THIRD-PERSON held weapon's attach transform (D-WPN-32).
#
# Retail draws this model RIGID — one matrix stamped into every bone slot — so the whole
# appearance is the transform the engine builds (world::held_weapon_attach; its frames and
# placement are pinned by ctest world_entity_pose_attach). Two independent things have to hold,
# and both were measured off the binary rather than tuned by eye:
#
#  * ORIENTATION is the entity attach basis and nothing else. The original writes the attach
#    triple onto the entity and calls the SAME builder that places an ordinary world object
#    [orig: Math_BuildFixedPointToFloatMatrix4x4 @0x612200, called from the attach build
#     @0x4b1bf8 and from the generic render callback BoneCallback_gnrc_World @0x4e2912], so a
#    gfx3 weapon must be posed exactly like a placed .3di at those angles. Weapon models are
#    authored barrel-along-+Z, so the basis must send model +Z to the aim direction.
#  * POSITION is bone 16's joint plus a fixed nudge that the original adds to the bone's
#    MODEL-space pivot before carrying the sum through the bone matrix
#    [orig: @0x4b2186..0x4b220b], so the nudge rides the bone's MODEL->WORLD rotation.
#
# What stays here pins the engine's bases against the original's own matrices.


const BONE := EntityPresenter.HELD_WEAPON_BONE_INDEX

# --- The basis IS the original's placement matrix -----------------------------------------
#
# The whole slice rests on `bms_to_godot_basis` being a faithful image of
# `Math_BuildFixedPointToFloatMatrix4x4 @0x612200` — the function the original calls both to
# build this attachment matrix `@0x4b1bf8` and to place an ordinary world object `@0x4e2912`.
# That claim was derived on paper; this pins it in code by rebuilding the original's matrix
# here (the same arithmetic `world::render_matrix_from_pose` ports) and conjugating it into
# Godot space, then demanding the two agree.

const BAM_PER_TURN := 4294967296.0


# One rotation block exactly as the original emits it: quantized cos/sin through a
# 4194304 fixed-point round trip, with the SIN NEGATED (`fsin * dbl_7C57B0`, -4194304.0).
static func _quantized_trig(bam: float) -> Vector2:
	var th: float = bam * (TAU / BAM_PER_TURN)
	return Vector2(
			float(int(cos(th) * 4194304.0)) / 4194304.0,
			float(int(sin(th) * -4194304.0)) / 4194304.0)


# Plain row-major 3x3 arithmetic, deliberately not Godot's Basis: Basis takes and returns
# COLUMNS, and silently transposing the original's row-major matrices is exactly the kind of
# error this test exists to rule out.
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


# The original's 3x3, row-major, applied as a ROW vector: v' = v * Mz(roll) * Mx(pitch) *
# My(yaw). Element placement copied straight off Math_BuildFixedPointToFloatMatrix4x4's
# stores (4x4 indices 0/1/4/5 for Z, 5/6/9/10 for X, 0/2/8/10 for Y).
static func _original_matrix(yaw_bam: float, pitch_bam: float, roll_bam: float) -> PackedFloat32Array:
	var z := _quantized_trig(roll_bam)
	var x := _quantized_trig(pitch_bam)
	var y := _quantized_trig(yaw_bam)
	var mz := PackedFloat32Array([z.x, z.y, 0.0, -z.y, z.x, 0.0, 0.0, 0.0, 1.0])
	var mx := PackedFloat32Array([1.0, 0.0, 0.0, 0.0, x.x, x.y, 0.0, -x.y, x.x])
	var my := PackedFloat32Array([y.x, 0.0, -y.y, 0.0, 1.0, 0.0, y.y, 0.0, y.x])
	return _mul3(_mul3(mz, mx), my)


static func _row_vec_mul(v: Vector3, m: PackedFloat32Array) -> Vector3:
	return Vector3(
			v.x * m[0] + v.y * m[3] + v.z * m[6],
			v.x * m[1] + v.y * m[4] + v.z * m[7],
			v.x * m[2] + v.y * m[5] + v.z * m[8])


func test_bms_basis_matches_the_original_placement_matrix() -> void:
	var placer := MissionObjectPlacer
	# The render frame is mission relabelled (renderX=-missionY, renderY=missionZ,
	# renderZ=missionX); Godot is (mX, mZ, -mY). So godot = (render.z, render.y, render.x):
	# the two differ by an X<->Z swap, which is its own inverse.
	var swap := func(v: Vector3) -> Vector3: return Vector3(v.z, v.y, v.x)
	for case in [Vector3(0.0, 0.0, 0.0), Vector3(17.95, 13.57, 0.0), Vector3(30.0, 120.0, 0.0),
			Vector3(-22.5, -63.0, 11.0), Vector3(5.0, 200.0, -40.0)]:
		var mission: Vector3 = case
		# The engine heading is (90 - mission yaw); pitch and roll go straight to BAM.
		var m := _original_matrix(
				(90.0 - mission.y) / 360.0 * BAM_PER_TURN,
				mission.x / 360.0 * BAM_PER_TURN,
				mission.z / 360.0 * BAM_PER_TURN)
		var ours: Basis = placer.bms_to_godot_basis(mission)
		# A .3di vertex `a` is authored in the RENDER frame. The original sends it to
		# world_render = a * M and the result relabels into Godot as swap(...). Our loader
		# instead stores it at flipX(a) in the model node's local Godot space (ADR 0007's
		# (-x, y, z)), so a Godot model axis corresponds to the render axis flipX(axis):
		#     bms_to_godot_basis  ==  swap . M^T . flipX
		# One swap, one flip -- both reflections, so the product is a proper rotation. (The
		# trailing Ry(+90) in bms_to_godot_basis is precisely swap . flipX factored out.)
		for axis in [Vector3(1.0, 0.0, 0.0), Vector3(0.0, 1.0, 0.0), Vector3(0.0, 0.0, 1.0)]:
			var render_axis := Vector3(-axis.x, axis.y, axis.z)
			var expected: Vector3 = swap.call(_row_vec_mul(render_axis, m))
			var got: Vector3 = ours * axis
			assert_true(got.is_equal_approx(expected),
					"angles %s axis %s: original gives %s, bms_to_godot_basis gives %s" % [
							mission, axis, expected, got])


# --- The HAND frame (the 0x80 branch) -------------------------------------------------------


func test_hand_frame_constants_match_the_original_composition() -> void:
	# Independent re-derivation of the calibration, from the ELEMENT PLACEMENT of
	# Math_BuildRotationMatrix4x4_ByAxis @0x611db0 rather than from the shipped constants'
	# signs. The original composes, row-major, `Ry_e * Rz_e * boneMatrix[16]`; the constant
	# part is therefore (Ry_e * Rz_e), and its Godot image is F * (Ry_e*Rz_e)^T * F with
	# F = diag(-1, 1, 1) the loader's X-negation.
	var zc := _quantized_trig(EntityPresenter.held_weapon_hand_frame_z_rad() / TAU * BAM_PER_TURN)
	var yc := _quantized_trig(EntityPresenter.held_weapon_hand_frame_y_rad() / TAU * BAM_PER_TURN)
	# axisIndex 0 (Z): [0]=cos, [1]=sin, [4]=-sin, [5]=cos.
	var rz := PackedFloat32Array([zc.x, zc.y, 0.0, -zc.y, zc.x, 0.0, 0.0, 0.0, 1.0])
	# axisIndex 2 (Y): [0]=cos, [2]=-sin, [8]=sin, [10]=cos.
	var ry := PackedFloat32Array([yc.x, 0.0, -yc.y, 0.0, 1.0, 0.0, yc.y, 0.0, yc.x])
	var prod := _mul3(ry, rz)
	# Transpose, then conjugate by F: negate every element with exactly one index == 0.
	var conj := PackedFloat32Array()
	conj.resize(9)
	for i in range(3):
		for j in range(3):
			var v: float = prod[j * 3 + i]              # transpose
			if (i == 0) != (j == 0):
				v = -v                                  # F * X * F
			conj[i * 3 + j] = v
	var expected := Basis(
			Vector3(conj[0], conj[3], conj[6]),
			Vector3(conj[1], conj[4], conj[7]),
			Vector3(conj[2], conj[5], conj[8]))   # Basis takes COLUMNS
	var got := EntityPresenter.held_weapon_hand_frame_basis(Basis.IDENTITY)
	assert_true(got.is_equal_approx(expected),
			"hand-frame calibration: derived %s, implementation %s" % [expected, got])


