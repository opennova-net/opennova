extends Node3D

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

const MissionOverlayUtil := preload("res://engine/mission/mission_overlay_util.gd")

# Volume type -> wireframe color (the collidable-type table in
# docs/world/world-wac-ai-re.md section 15 / libs/world/include/world/collision.h).
static func type_color(volume_type: int) -> Color:
	match volume_type:
		1:
			return Color(0.78, 0.78, 0.78)   # solid walls/floors - gray
		4:
			return Color(0.30, 0.55, 1.0)    # platform / stand-on surface - blue
		6:
			return Color(0.30, 1.0, 0.45)    # armory zone - green
		8:
			return Color(1.0, 0.9, 0.25)     # indoor (blink) box - yellow
		16, 17, 18:
			return Color(1.0, 0.25, 0.2)     # hurt volumes - red
		19:
			return Color(0.75, 0.35, 1.0)    # player-only solid - purple
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

var _world: Node                # duck-typed GameWorld (get_sim()); re-resolved every frame
var _hull_mesh: ImmediateMesh
var _player_mesh: ImmediateMesh
var _gap_label: Label3D
var _hull_signature := 0        # hash of instance pose + emitted geometry
var _hull_has_surface := false


# `world` is the node owning the running sim (the GameWorld); the sim is
# re-resolved through it every frame so mission reloads never leave this view
# pointing at a freed NovaSimulation.
func setup(world: Node) -> void:
	_world = world
	_hull_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("CollisionHullLines", _hull_mesh))
	_player_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("CollisionPlayerLines", _player_mesh))
	_gap_label = Label3D.new()
	_gap_label.name = "CollisionGapLabel"
	_gap_label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	_gap_label.fixed_size = false
	_gap_label.pixel_size = 0.006
	_gap_label.no_depth_test = true
	_gap_label.font_size = 48
	_gap_label.outline_size = 12
	_gap_label.modulate = COLOR_CAPSULE
	_gap_label.outline_modulate = Color(0.0, 0.0, 0.0, 0.85)
	_gap_label.visible = false
	add_child(_gap_label)


func _make_lines_node(node_name: String, mesh: ImmediateMesh) -> MeshInstance3D:
	var mi := MeshInstance3D.new()
	mi.name = node_name
	mi.mesh = mesh
	# Unshaded, vertex-colored, depth-test off so the wireframes read through the
	# object meshes they describe (the skeleton-view overlay recipe).
	var mat := MissionOverlayUtil.line_material()
	mat.no_depth_test = true
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.material_override = mat
	return mi


func _process(_delta: float) -> void:
	refresh_now()


## Immediately refresh the debug geometry from the current simulation.
## The process hook delegates here; tests and tooling can request a deterministic
## refresh without reaching into Godot's private frame callback.
func refresh_now() -> void:
	var sim := _resolve_sim()
	if sim == null:
		_clear_all()
		return
	var debug: Dictionary = sim.get_collision_debug()
	_update_hulls(debug.get("instances", []))
	_update_player(debug.get("player", {}))


func _resolve_sim() -> Object:
	if _world == null or not is_instance_valid(_world) or not _world.has_method("get_sim"):
		return null
	var sim: Variant = _world.get_sim()
	if sim == null or not is_instance_valid(sim) or not (sim as Object).has_method("get_collision_debug"):
		return null
	return sim


func _clear_all() -> void:
	if _hull_has_surface:
		_hull_mesh.clear_surfaces()
		_hull_has_surface = false
		_hull_signature = 0
	_player_mesh.clear_surfaces()
	if _gap_label != null:
		_gap_label.visible = false


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


func _cross(segments: Array, at: Vector3, arm: float, color: Color) -> void:
	segments.append({ "a": at - Vector3(arm, 0, 0), "b": at + Vector3(arm, 0, 0), "color": color })
	segments.append({ "a": at - Vector3(0, arm, 0), "b": at + Vector3(0, arm, 0), "color": color })
	segments.append({ "a": at - Vector3(0, 0, arm), "b": at + Vector3(0, 0, arm), "color": color })


# A horizontal diamond (4 segments) of radius `r` around `at` -- reads as the
# point's test radius without the vertex cost of a circle.
func _diamond(segments: Array, at: Vector3, r: float, color: Color) -> void:
	var px := at + Vector3(r, 0, 0)
	var nx := at + Vector3(-r, 0, 0)
	var pz := at + Vector3(0, 0, r)
	var nz := at + Vector3(0, 0, -r)
	segments.append({ "a": px, "b": pz, "color": color })
	segments.append({ "a": pz, "b": nx, "color": color })
	segments.append({ "a": nx, "b": nz, "color": color })
	segments.append({ "a": nz, "b": px, "color": color })
