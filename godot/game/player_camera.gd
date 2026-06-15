extends Camera3D
## First-person camera driven from the simulation's local-player eye transform.
##
## [orig: Player_UpdateFirstPersonCamera @0x4dd380 output, seeded by
## Camera_ComputeThirdPersonView @0x437d10.] The sim computes the eye in the engine frame;
## NovaSimulation.get_player_camera() hands back [eye_x, eye_y, eye_z (already Godot world
## units), yaw_deg, pitch_deg, roll_deg (mission degrees)]. We build the basis through the
## single-source placer (MissionObjectPlacer.bms_to_godot_basis), the same handoff the
## present pass uses for entities, so the camera and the world agree by construction.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

## Drive this camera from the sim. Returns false (leaving the camera untouched) when there
## is no local player, so the caller can keep the free-fly camera current.
##
## The view angles arrive verbatim from the entity (yaw = 90 - heading, pitch, roll), as the
## original takes them [orig: Camera_ComputeThirdPersonView @0x437d10 sets g_view_rot_* directly].
## We build a FIRST-PERSON basis here, NOT the placer's entity basis: a Camera3D looks down its
## local -Z, while bms_to_godot_basis orients a model's +X forward AND folds "pitch" into a roll-like
## axis (fine for yaw-dominant entity placement, wrong for a look-up/down camera). So we take the
## yaw-only heading forward through the ONE placer convention (so the camera agrees with the world's
## facing), then pitch it about the camera's right axis and aim -Z along it.
func apply_from_sim(sim) -> bool:
	if sim == null or not sim.has_method("get_player_camera"):
		return false
	var c: PackedFloat32Array = sim.get_player_camera()
	if c.size() < 6:
		return false # no local player designated
	var eye := Vector3(c[0], c[1], c[2]) # already Godot-space (x, z, -y)
	# Heading forward (yaw only) via the shared placer convention: the same direction the soldier
	# model faces (its +X), flattened to the ground plane.
	var fwd_flat := MissionObjectPlacer.bms_to_godot_basis(Vector3(0.0, c[3], 0.0)) * Vector3(1.0, 0.0, 0.0)
	fwd_flat.y = 0.0
	if fwd_flat.length_squared() < 0.0000001:
		return false
	fwd_flat = fwd_flat.normalized()
	# Pitch about the camera's right axis (look up/down). c[4] is mission pitch degrees.
	var right := fwd_flat.cross(Vector3.UP).normalized()
	var fwd := fwd_flat.rotated(right, deg_to_rad(c[4]))
	# Aim the camera's -Z along the look direction (Camera3D convention). Roll (c[5]) is ~0 on foot.
	global_transform = Transform3D(Basis.IDENTITY, eye).looking_at(eye + fwd, Vector3.UP)
	return true
