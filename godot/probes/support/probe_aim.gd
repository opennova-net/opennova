class_name ProbeAim
extends RefCounted

## The aim lock the shooting probes share: the local player's view set straight
## onto an entity's body plus `height` m (the debug teleport at the body's own
## position, the yaw/pitch solved from the camera: Godot (x, y up, z) ->
## mission (x east, y north = -z, z up)). Call `at` every frame to hold it.


## Aim at the entity `handle` names; false when it no longer resolves.
static func at(sim: Simulation, camera: Camera3D, handle: int, height: float) -> bool:
	if sim == null or camera == null:
		return false
	var target := row(sim, handle)
	if target == null:
		return false
	var point: Vector3 = target.get_world_position() + Vector3(0.0, height, 0.0)
	var to := point - camera.global_position
	var yaw := rad_to_deg(atan2(to.x, -to.z))
	var pitch := rad_to_deg(atan2(to.y, Vector2(to.x, to.z).length()))
	var body := sim.get_local_player_position()
	sim.debug_teleport_local_player(Vector3(body.x, -body.z, body.y), yaw, pitch)
	return true


## The entity row a wire handle names this frame, or null.
static func row(sim: Simulation, handle: int) -> EntityRow:
	for row_v in sim.entity_directory():
		var entry := row_v as EntityRow
		if entry != null and entry.get_wire_handle() == handle:
			return entry
	return null


## The entity row an ai_index (game_entities' edit target) names, or null.
static func row_by_ai_index(sim: Simulation, ai_index: int) -> EntityRow:
	for row_v in sim.entity_directory():
		var entry := row_v as EntityRow
		if entry != null and entry.get_ai_index() == ai_index:
			return entry
	return null
