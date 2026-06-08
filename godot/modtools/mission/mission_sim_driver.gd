extends Node
# Drives a live mission simulation over the editor's already-placed entity nodes.
#
# The mission editor renders each placed entity as a node (MissionObjectPlacer); this driver owns
# a NovaSimulation (the libs/world World + AI promoted from the loaded mission), ticks it, and each
# frame copies every AI entity's transform onto its rendered node so the NPCs walk their authored
# routes in the viewport. It is a Node (so it gets _process) parented under the objects container;
# the NovaSimulation itself is held off-tree so only this driver advances it. Stop restores the
# authored transforms (the simulation never touches the mission data).

var _sim: NovaSimulation
var _nodes: Array = []       # parallel to the sim's entity index -> the rendered Node3D (or null)
var _orig: Array = []        # snapshot of each node's transform at play, for restore-on-stop
var _playing: bool = false
var loco_scale: int = 4096   # AI-speed -> world-units pace (see AiSystem::loco_scale)

# Promote `mission` (a NovaMissionData) and map each AI entity to its rendered node via the
# placer's pickable records (keyed by mission (kind, index)). Returns the AI-entity count.
func setup(mission, pickable: Array) -> int:
	_sim = NovaSimulation.new() # off-tree: only this driver ticks it
	if mission == null or not _sim.load_from_mission_data(mission):
		_sim.free() # NovaSimulation is a Node (not RefCounted); free the orphan on load failure
		_sim = null
		return 0
	_sim.set_tick_mode(NovaSimulation.TICK_EVERY_PROCESS) # snappy preview: one logic tick per frame
	_sim.set_loco_scale(loco_scale)

	var node_by_key := {}
	for rec in pickable:
		var node = rec.get("node", null) # animated entities carry their Node3D directly
		if node != null:
			node_by_key["%d:%d" % [int(rec.get("kind", -1)), int(rec.get("index", -1))]] = node

	_nodes.clear()
	_orig.clear()
	for i in range(_sim.get_entity_count()):
		var key := "%d:%d" % [_sim.get_entity_kind(i), _sim.get_entity_index(i)]
		var n = node_by_key.get(key, null)
		_nodes.append(n)
		_orig.append((n.transform) if (n != null and is_instance_valid(n)) else Transform3D())
	return _sim.get_entity_count()

func set_playing(p: bool) -> void:
	_playing = p

func is_playing() -> bool:
	return _playing

func entity_count() -> int:
	return _sim.get_entity_count() if _sim != null else 0

# One simulation tick (AI decision + locomotion) + push transforms to the nodes.
func step() -> void:
	if _sim == null:
		return
	_sim.step()
	_apply()

func _process(_delta: float) -> void:
	if _playing:
		step()

func _apply() -> void:
	for i in range(min(_nodes.size(), _sim.get_entity_count())):
		var n = _nodes[i]
		if n != null and is_instance_valid(n):
			n.position = _sim.get_entity_position(i)
			# Match the placer's yaw convention (bms_to_godot_rotation: -yaw + 180deg).
			n.rotation.y = deg_to_rad(-_sim.get_entity_yaw_deg(i)) + PI

# Restore the authored node transforms (called before teardown so stopping the sim leaves the
# placed world exactly as it was).
func restore() -> void:
	if _sim != null:
		_sim.restart() # rewind the world + AI to the play-start baseline (World::restore)
	for i in range(min(_nodes.size(), _orig.size())):
		var n = _nodes[i]
		if n != null and is_instance_valid(n):
			n.transform = _orig[i]
	_playing = false


# The driver holds its NovaSimulation off-tree, so Godot won't free it when the driver is freed
# (Nodes aren't reference-counted). Free it explicitly when the driver leaves the tree (the
# controller queue_free()s the driver on stop).
func _exit_tree() -> void:
	if _sim != null:
		_sim.free()
		_sim = null
