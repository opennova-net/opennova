extends Node

# Renders a NovaNetClient's decoded world. Spawns a NovaObjectModel per net entity
# (resolved by items.def type), updates each entity's transform + visibility every
# frame from the client's interpolated samples, and optionally follows the assigned
# player with a camera. The net analog of MissionObjectPlacer + MissionPresentPass:
# it reads the live wire-decoded world model instead of the AI sim's snapshot.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

var _client                # NovaNetClient
var _resolver              # NovaModelResolver
var _container: Node3D     # parent for spawned models
var _env_node: Node
var _camera: Camera3D
var _nodes := {}           # handle -> Node3D
var _unresolved := {}      # handle -> true (type didn't resolve; don't retry each frame)
var stats := { "spawned": 0, "unresolved": 0 }


func setup(client, resolver, container: Node3D, env_node: Node = null, camera: Camera3D = null) -> void:
	_client = client
	_resolver = resolver
	_container = container
	_env_node = env_node
	_camera = camera


func _process(_delta: float) -> void:
	if _client == null or _resolver == null or _container == null:
		return
	_sync_entities()
	_apply_transforms()
	_update_camera()


# Spawn newly-seen entities; despawn ones that left the world.
func _sync_entities() -> void:
	var ents: Array = _client.get_entities()
	var live := {}
	for e in ents:
		var h := int(e["handle"])
		live[h] = true
		if _nodes.has(h) or _unresolved.has(h):
			continue
		var node = _resolver.make_model(int(e["type_id"]) + 100000, _container, _env_node)
		if node == null:
			_unresolved[h] = true            # markers / graphicless types — skip
			stats.unresolved += 1
			continue
		node.name = "Net_%04x" % h
		_nodes[h] = node
		stats.spawned += 1
	for h in _nodes.keys():
		if not live.has(h):
			_nodes[h].queue_free()
			_nodes.erase(h)


# Pull each live entity's interpolated pose at the render head and apply it. Wire
# samples are mission-space meters + a yaw heading; the placement convention maps
# them to Godot space (same path a .bms-placed entity goes through).
func _apply_transforms() -> void:
	var frame := float(_client.get_latest_frame())
	for h in _nodes.keys():
		var s: Dictionary = _client.sample_at(h, frame)
		if not bool(s.get("found", false)):
			continue
		var node: Node3D = _nodes[h]
		var gpos := MissionObjectPlacer.bms_to_godot_position(s["pos"])
		var yaw := float(s.get("heading_deg", 0.0))
		node.transform = Transform3D(
			MissionObjectPlacer.bms_to_godot_basis(Vector3(0.0, yaw, 0.0)), gpos)
		node.visible = bool(s.get("alive", true))


# Chase the assigned player (client roles) from its DECODED pose — players carry no
# items.def model (the avatar system is separate), so we follow the wire position +
# heading directly rather than a spawned node. The host role spectates (no own
# player) and is left on the free-fly camera. On the first valid follow we silence
# the fly-camera's own movement so the two don't fight for the transform.
var _fly_silenced := false
var _host_framed := false

func _update_camera() -> void:
	if _camera == null:
		return
	var own = _client.get_own_player_handle()
	if own == 0xFFFF:
		_frame_host_once() # spectator: point the camera at the action once
		return
	var s: Dictionary = _client.sample_at(own, float(_client.get_latest_frame()))
	if not bool(s.get("found", false)):
		return
	if not _fly_silenced:
		_fly_silenced = true
		_camera.set_process(false)
		_camera.set_physics_process(false)
	var p := MissionObjectPlacer.bms_to_godot_position(s["pos"])
	var basis := MissionObjectPlacer.bms_to_godot_basis(Vector3(0.0, float(s.get("heading_deg", 0.0)), 0.0))
	var forward := -basis.z.normalized()           # the direction the player faces
	_camera.global_position = p + Vector3(0.0, 2.2, 0.0) - forward * 5.0  # behind + above
	_camera.look_at(p + Vector3(0.0, 1.3, 0.0) + forward * 2.0, Vector3.UP)


# Spectator framing (host role): once models exist, lift the camera to an overview
# of their centroid so the populated world is in view immediately, then hand back
# to the free-fly controller (no silencing — the user can fly from there).
func _frame_host_once() -> void:
	if _host_framed or _nodes.is_empty():
		return
	var centroid := Vector3.ZERO
	var n := 0
	for h in _nodes.keys():
		centroid += (_nodes[h] as Node3D).global_position
		n += 1
	if n == 0:
		return
	_host_framed = true
	centroid /= n
	_camera.global_position = centroid + Vector3(0.0, 90.0, 110.0)
	_camera.look_at(centroid, Vector3.UP)


func entity_count() -> int:
	return _nodes.size()
