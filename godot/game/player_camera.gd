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
func apply_from_sim(sim) -> bool:
	if sim == null or not sim.has_method("get_player_camera"):
		return false
	var c: PackedFloat32Array = sim.get_player_camera()
	if c.size() < 6:
		return false # no local player designated
	var eye := Vector3(c[0], c[1], c[2]) # already Godot-space (x, z, -y)
	var rot_deg := Vector3(c[4], c[3], c[5]) # (pitch, yaw, roll) mission degrees
	global_transform = Transform3D(MissionObjectPlacer.bms_to_godot_basis(rot_deg), eye)
	return true
