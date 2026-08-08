extends RefCounted

# Maps a loaded mission's live animated entity nodes back to the identities a mission ACTION targets:
# a single entity's SSN (bms_id), a group id, or an area-trigger zone. Built once after the placer runs.
#
# Only animatable entities (individual NovaObjectModel nodes, i.e. nodes exposing play_part_anim) are
# indexed: static entities are merged into MultiMesh batches with no per-entity node and cannot run a
# part animation, so they are out of scope as PLAYPARTANIM targets. Identity is read from each node's
# "entity_ref" meta, which MissionObjectPlacer tags in both runtime and editor.
#
# Shell-agnostic, RefCounted, referenced via preload() (same convention as MissionObjectPlacer) so it
# resolves without an editor re-import.

var _by_bms_id: Dictionary = {}     # bms_id (SSN) -> Node
var _by_kind_index: Dictionary = {} # packed (kind,index) -> Node (fallback when bms_id is 0)
var _by_group: Dictionary = {}      # group_id -> Array[Node]
var _nodes: Array = []              # [{ node, pos (mission-space Vector3), team }]
var _area_triggers: Array = []      # cached mission.get_area_triggers()
var _generation := 0                # invalidates presentation row plans after rebuild/clear


static func _origin_key(kind: int, index: int) -> int:
	# Keep both signed 32-bit inputs distinct without allocating a formatted
	# String in the per-frame present path.
	return (kind << 32) | (index & 0xffffffff)


## (Re)build the indexes from the animatable entity nodes under `container`, reading each node's
## "entity_ref" meta. `mission` supplies the area-trigger rects for zone resolution.
func build(container: Node, mission) -> void:
	clear()
	if mission != null and mission.has_method("get_area_triggers"):
		_area_triggers = mission.get_area_triggers()
	if container == null:
		return
	for child in container.get_children():
		# Only animatable models are valid targets; this skips editor pick colliders + static batches.
		if not child.has_method("play_part_anim") or not child.has_meta("entity_ref"):
			continue
		var ref: Dictionary = child.get_meta("entity_ref")
		var bms_id := int(ref.get("bms_id", 0))
		if bms_id != 0:
			_by_bms_id[bms_id] = child
		var kind := int(ref.get("kind", -1))
		var index := int(ref.get("index", -1))
		if kind >= 0 and index >= 0:
			_by_kind_index[_origin_key(kind, index)] = child
		var group := int(ref.get("group", -1))
		if group >= 0:
			if not _by_group.has(group):
				_by_group[group] = []
			(_by_group[group] as Array).append(child)
		_nodes.append({
			"node": child,
			"pos": ref.get("position", Vector3.ZERO),
			"team": int(ref.get("team", -1)),
		})


func clear() -> void:
	_generation += 1
	_by_bms_id.clear()
	_by_kind_index.clear()
	_by_group.clear()
	_nodes.clear()
	_area_triggers = []


func get_generation() -> int:
	return _generation


## The node column of the animatable set, for the F3 Animation & models page's
## per-model rows. Rows can hold freed instances after a reload — callers
## is_instance_valid-guard each entry, matching the resolver contract.
func get_animatable_nodes() -> Array:
	var out: Array = []
	for record in _nodes:
		out.append(record["node"])
	return out


## Resolve one entity's node for the present pass: by file id (bms_id) first -- stable for saved loose
## missions -- then by (kind, index), which also supports an in-memory bms::File with bms_id 0 in
## isolated tests/tooling previews. Returns the live node or null. This is THE resolver for the
## standalone GameWorld present path; resolve_single/group/zone below also serve live effect routing
## and non-gameplay authoring previews.
func resolve(bms_id: int, kind: int, index: int) -> Node:
	var node: Variant = null
	if bms_id != 0:
		node = _by_bms_id.get(bms_id, null)
	if (node == null or not is_instance_valid(node)) and kind >= 0 and index >= 0:
		node = _by_kind_index.get(_origin_key(kind, index), null)
	return node if (node != null and is_instance_valid(node)) else null


## Resolve a single entity by its SSN (bms_id). Returns the live node or null.
func resolve_single(bms_id: int) -> Node:
	if bms_id == 0:
		return null
	# Untyped: the stored value may be a previously-freed instance, which cannot be assigned to a
	# typed Node local (Godot throws); is_instance_valid() then filters it.
	var node: Variant = _by_bms_id.get(bms_id, null)
	return node if (node != null and is_instance_valid(node)) else null


## Resolve all live members of a group. Returns [] for an unknown/empty group.
func resolve_group(group_id: int) -> Array:
	if group_id < 0:
		return []
	var out: Array = []
	for node in _by_group.get(group_id, []):
		if is_instance_valid(node):
			out.append(node)
	return out


## Resolve all live entities whose mission-space position falls inside area-trigger `zone_index`
## (by array order -- the editor's zone list order). X/Y (the BMS horizontal plane) are always tested;
## the vertical Z is tested only when the trigger constrains it. Returns [] when out of range.
func resolve_zone(zone_index: int) -> Array:
	if zone_index < 0 or zone_index >= _area_triggers.size():
		return []
	var trig: Dictionary = _area_triggers[zone_index]
	var amin: Vector3 = trig.get("min", Vector3.ZERO)
	var amax: Vector3 = trig.get("max", Vector3.ZERO)
	var lo := Vector3(minf(amin.x, amax.x), minf(amin.y, amax.y), minf(amin.z, amax.z))
	var hi := Vector3(maxf(amin.x, amax.x), maxf(amin.y, amax.y), maxf(amin.z, amax.z))
	var check_z := bool(trig.get("constrain_z", false))
	var out: Array = []
	for entry in _nodes:
		var node: Variant = entry["node"]  # may be a freed instance; keep untyped, then validate
		if not is_instance_valid(node):
			continue
		var p: Vector3 = entry["pos"]
		if p.x < lo.x or p.x > hi.x or p.y < lo.y or p.y > hi.y:
			continue
		if check_z and (p.z < lo.z or p.z > hi.z):
			continue
		out.append(node)
	return out
