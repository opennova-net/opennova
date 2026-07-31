extends SimDebugView

# Draws the live collision world over the scene: every nearby object's collision
# volumes as type-colored wireframe boxes, plus the local player's capsule test
# points and the last measured ground gap. The 3D face of the F3 overlay's
# "Show collision" toggle -- a developer tool for eyeballing the movement
# resolver (wall push-out, zone volumes, foot clearance), not engine-witnessed
# behavior.
#
# The geometry comes from NovaSimulation.get_collision_debug(): box corners are
# transformed in C++ through the SAME fixed-point matrix path the resolver
# queries use, so what is drawn IS what movement resolves against. The hull
# mesh rebuilds only when the nearby-instance set changes (static world
# geometry; moving vehicles change the signature and rebuild); the player
# capsule redraws every frame. Built / freed by GameWorld on the overlay's
# "Show collision" toggle, like the skeleton view.

const TYPE_CD := 9
const TYPE_CT := 10
const TYPE_CF := 13

# Volume type -> wireframe color (the collidable-type table in
# docs/world/world-wac-ai-re.md section 15 / libs/world/include/world/collision.h).
static func type_color(volume_type: int) -> Color:
	match volume_type:
		1:
			return Color(0.78, 0.78, 0.78)   # solid walls/floors - gray
		4:
			return Color(0.30, 0.55, 1.0)    # CL ladder - blue
		6:
			return Color(0.30, 1.0, 0.45)    # CA armory - green
		8:
			return Color(1.0, 0.9, 0.25)     # BB blink box - yellow
		TYPE_CD:
			return Color(0.8, 0.4, 0.0)      # CD door - brown
		TYPE_CT:
			return Color(1.0, 0.2, 0.2)      # CT change team - coral
		TYPE_CF:
			return Color(0.3, 1.0, 0.3)      # CF flag / special function - green
		16, 17, 18:
			return Color(1.0, 0.25, 0.2)     # DH/DM/DL damage - red
		19:
			return Color(0.75, 0.35, 1.0)    # CP player collision - purple
		_:
			return COLOR_OTHER
const COLOR_OTHER := Color(1.0, 0.55, 0.15)   # any other type - orange
const COLOR_CAPSULE := Color(0.2, 1.0, 1.0)   # player test points / capsule span
const COLOR_GROUNDED := Color(0.3, 1.0, 0.5)  # foot ray while standing
const COLOR_AIRBORNE := Color(1.0, 0.45, 0.3) # foot ray while off the ground

# The 12 box edges over the corner order the sim emits (index bit0 = max x,
# bit1 = max y, bit2 = max z in mission axes).
const BOX_EDGES := [
	[0, 1], [2, 3], [4, 5], [6, 7],
	[0, 2], [1, 3], [4, 6], [5, 7],
	[0, 4], [1, 5], [2, 6], [3, 7],
]

var _hull_mesh: ImmediateMesh
var _player_mesh: ImmediateMesh
var _gap_label: Label3D
var _hull_signature := 0        # hash of instance pose + emitted geometry
var _hull_has_surface := false
var _drawable_count := 0        # valid volumes plus the local-player capsule


func _sim_debug_method() -> String:
	return "get_collision_debug"


func _build_view() -> void:
	_hull_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("CollisionHullLines", _hull_mesh))
	_player_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("CollisionPlayerLines", _player_mesh))
	_gap_label = _make_overlay_label("CollisionGapLabel", 0.006, 48, 12)
	_gap_label.modulate = COLOR_CAPSULE
	add_child(_gap_label)


func _refresh_from_sim(sim: Object) -> void:
	var debug: Dictionary = sim.get_collision_debug()
	var instances: Array = debug.get("instances", [])
	var player: Dictionary = debug.get("player", {})
	_drawable_count = _count_drawables(instances, player)
	_update_hulls(instances)
	_update_player(player)


func _clear_all() -> void:
	_drawable_count = 0
	if _hull_has_surface:
		_hull_mesh.clear_surfaces()
		_hull_has_surface = false
		_hull_signature = 0
	_player_mesh.clear_surfaces()
	if _gap_label != null:
		_gap_label.visible = false


## Number of logical collision shapes currently contributing overlay geometry.
func get_debug_drawable_count() -> int:
	return _drawable_count


func _count_drawables(instances: Array, player: Dictionary) -> int:
	var count := 1 if bool(player.get("valid", false)) else 0
	for inst_v in instances:
		var inst: Dictionary = inst_v
		for vol_v in inst.get("volumes", []):
			var vol: Dictionary = vol_v
			if (vol.get("corners", PackedVector3Array()) as PackedVector3Array).size() == 8:
				count += 1
	return count


# --- Hull volumes -------------------------------------------------------------

func _update_hulls(instances: Array) -> void:
	# Rebuild only when the nearby set, a member's full pose, or its emitted
	# geometry changed. Vehicles can rotate without translating, and mission
	# reloads can reuse entity handles at the same pose with different hulls.
	var sig_parts := []
	for inst in instances:
		sig_parts.append(inst.get("entity_handle", -1))
		sig_parts.append(inst.get("pos", Vector3.ZERO))
		sig_parts.append(inst.get("heading", 0.0))
		sig_parts.append(hash(inst.get("volumes", [])))
	var sig := hash(sig_parts)
	if sig == _hull_signature and _hull_has_surface == (not instances.is_empty()):
		return
	_hull_signature = sig
	_hull_mesh.clear_surfaces()
	_hull_has_surface = false
	var segments: Array = []
	for inst in instances:
		for vol in inst.get("volumes", []):
			var corners: PackedVector3Array = vol.get("corners", PackedVector3Array())
			if corners.size() != 8:
				continue
			var color: Color = type_color(int(vol.get("type", 0)))
			for edge in BOX_EDGES:
				segments.append({ "a": corners[edge[0]], "b": corners[edge[1]], "color": color })
	if segments.is_empty():
		return
	MissionOverlayUtil.emit_line_segments(_hull_mesh, segments)
	_hull_has_surface = true


# --- The local player's capsule ------------------------------------------------

func _update_player(player: Dictionary) -> void:
	_player_mesh.clear_surfaces()
	if not bool(player.get("valid", false)):
		if _gap_label != null:
			_gap_label.visible = false
		return
	var pos: Vector3 = player.get("position", Vector3.ZERO)
	var bottom := float(player.get("capsule_bottom", 0.0))
	var top := float(player.get("capsule_top", 0.0))
	var gap := float(player.get("foot_clearance", 0.0))
	var points: PackedVector3Array = player.get("points", PackedVector3Array())
	var radii: PackedFloat32Array = player.get("radii", PackedFloat32Array())

	var segments: Array = []
	# The 3 resolver test points (head / eye stand-in / feet): a cross at each,
	# ringed by a horizontal diamond of its test radius.
	for i in range(points.size()):
		var p := points[i]
		var r := radii[i] if i < radii.size() else 0.0
		_cross(segments, p, 0.12, COLOR_CAPSULE)
		if r > 0.0:
			_diamond(segments, p, r, COLOR_CAPSULE)

	# Capsule span: feet sit capsule_bottom below the entity origin; the top
	# extent reaches capsule_top above the feet.
	var feet := pos + Vector3(0.0, -bottom, 0.0)
	var head := feet + Vector3(0.0, top, 0.0)
	segments.append({ "a": feet, "b": head, "color": COLOR_CAPSULE })
	_diamond(segments, feet, 0.2, COLOR_CAPSULE)
	_diamond(segments, head, 0.2, COLOR_CAPSULE)

	# The foot ray: feet down to the resolved ground (gap = feet - ground; a gap
	# <= 0 means grounded/embedded). Green when standing, orange-red when airborne.
	var grounded := gap <= 0.0
	var ground := feet + Vector3(0.0, -gap, 0.0)
	var ray_color := COLOR_GROUNDED if grounded else COLOR_AIRBORNE
	segments.append({ "a": feet, "b": ground, "color": ray_color })
	_diamond(segments, ground, 0.3, ray_color)

	MissionOverlayUtil.emit_line_segments(_player_mesh, segments)

	if _gap_label != null:
		_gap_label.visible = true
		_gap_label.position = head + Vector3(0.0, 0.35, 0.0)
		_gap_label.text = "ground gap %.2f\ncapsule %.2f / %.2f" % [gap, bottom, top]
		_gap_label.modulate = ray_color
