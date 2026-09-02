extends SimDebugView

# Draws the live collision world over the scene: every nearby object's collision
# volumes as type-colored wireframe boxes, plus the local player's capsule test
# points and the last measured ground gap. The 3D face of the dev tools'
# "Show collision" toggle -- a developer tool for eyeballing the movement
# resolver (wall push-out, zone volumes, foot clearance), not engine-witnessed
# behavior.
#
# The geometry comes from Simulation.get_collision_debug(): box corners are
# transformed in C++ through the SAME fixed-point matrix path the resolver
# queries use, so what is drawn IS what movement resolves against. The hull
# mesh rebuilds only when the nearby-instance set changes (static world
# geometry; moving vehicles change the signature and rebuild); the player
# capsule redraws every frame. Built / freed by GameWorld on the overlay's
# "Show collision" toggle, like the skeleton view.

# Volume type -> wireframe color. The type codes are the engine's bvol families
# (world/collision.h bvol_type, bound as Simulation.BVOL_*; the collidable-type
# table in docs/world/world-wac-ai-re.md section 15); the colors stay
# godot-side. Types 1 (generic solid) and 19 (CP player collision) have no
# engine bvol_type home yet and keep their raw ids.
static func type_color(volume_type: int) -> Color:
	match volume_type:
		1:
			return Color(0.78, 0.78, 0.78)   # solid walls/floors - gray
		Simulation.BVOL_LADDER_CL:
			return Color(0.30, 0.55, 1.0)    # CL ladder - blue
		Simulation.BVOL_ARMORY_CA:
			return Color(0.30, 1.0, 0.45)    # CA armory - green
		Simulation.BVOL_BLINK_BB:
			return Color(1.0, 0.9, 0.25)     # BB blink box - yellow
		Simulation.BVOL_DOOR_CD:
			return Color(0.8, 0.4, 0.0)      # CD door - brown
		Simulation.BVOL_CHANGE_TEAM_CT:
			return Color(1.0, 0.2, 0.2)      # CT change team - coral
		Simulation.BVOL_FLAG_CF:
			return Color(0.3, 1.0, 0.3)      # CF flag / special function - green
		Simulation.BVOL_DAMAGE_HIGH_DH, Simulation.BVOL_DAMAGE_MEDIUM_DM, Simulation.BVOL_DAMAGE_LOW_DL:
			return Color(1.0, 0.25, 0.2)     # DH/DM/DL damage - red
		19:
			return Color(0.75, 0.35, 1.0)    # CP player collision - purple
		_:
			return COLOR_OTHER
const COLOR_OTHER := Color(1.0, 0.55, 0.15)   # any other type - orange

# Contact-kind -> flash color (engine ContactDebugKind order; the F3 Physics
# window's kKindColors is the same table and doubles as the legend).
const HIT_KIND_COLORS: Array[Color] = [
	Color(1.0, 0.35, 0.15), # Projectile hit - red-orange
	Color(1.0, 0.4, 0.7),   # Knife hit - pink
	Color(1.0, 0.7, 0.2),   # Move contact - amber
	Color(0.9, 0.3, 1.0),   # Vehicle hull - magenta
	Color(0.75, 0.6, 0.4),  # Terrain hit - tan
	Color(0.3, 0.9, 1.0),   # Water hit - cyan
]
const HIT_MIN_ALPHA := 0.25   # the oldest in-window flash still reads
const HIT_STRIDE := 6         # [target, age, kind, x, y, z] per event
const COLOR_PROBE_BOX := Color(1.0, 0.4, 0.9)       # vehicle platform probe box
const COLOR_PROBE_FOOTPRINT := Color(0.6, 0.25, 0.55) # its ground footprint
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
var _hit_mesh: ImmediateMesh
var _gap_label: Label3D
var _hull_signature := 0        # hash of instance pose + emitted geometry
var _hull_has_surface := false
var _hit_signature := 0         # hash of the hits channel (ages shift per tick)
var _drawable_count := 0        # valid volumes plus the local-player capsule


func _build_view() -> void:
	_hull_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("CollisionHullLines", _hull_mesh))
	_player_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("CollisionPlayerLines", _player_mesh))
	_hit_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("CollisionHitLines", _hit_mesh))
	_gap_label = _make_overlay_label("CollisionGapLabel", 0.006, 48, 12)
	_gap_label.modulate = COLOR_CAPSULE
	add_child(_gap_label)


func _refresh_from_sim(sim: Simulation) -> void:
	render_report(sim.get_collision_debug())


## Render one collision snapshot (Simulation.get_collision_debug's shape).
## Split from the sim fetch so tests and probes can drive the view with
## report data directly.
func render_report(debug: CollisionDebugReport) -> void:
	if debug == null:
		debug = CollisionDebugReport.new()
	var instances := debug.instances
	var probe_boxes := debug.probe_boxes
	var player := debug.player
	_drawable_count = _count_drawables(instances, player) + probe_boxes.size()
	_update_hulls(instances, probe_boxes)
	_update_player(player)
	_update_hits(instances, debug.hits, maxi(1, debug.hit_ttl))


func _clear_all() -> void:
	_drawable_count = 0
	if _hull_has_surface:
		_hull_mesh.clear_surfaces()
		_hull_has_surface = false
		_hull_signature = 0
	_player_mesh.clear_surfaces()
	_hit_mesh.clear_surfaces()
	_hit_signature = 0
	if _gap_label != null:
		_gap_label.visible = false


## Number of logical collision shapes currently contributing overlay geometry.
func get_debug_drawable_count() -> int:
	return _drawable_count


func _count_drawables(instances: Array, player: CollisionDebugPlayer) -> int:
	var count := 1 if player != null and player.valid else 0
	for inst: CollisionDebugInstance in instances:
		for vol: CollisionDebugVolume in inst.volumes:
			if vol.corners.size() == 8:
				count += 1
	return count


# --- Hull volumes -------------------------------------------------------------

func _update_hulls(instances: Array, probe_boxes: Array = []) -> void:
	# Rebuild only when the nearby set, a member's full pose, or its emitted
	# geometry changed. Vehicles can rotate without translating, and mission
	# reloads can reuse entity handles at the same pose with different hulls.
	var sig_parts := []
	for inst: CollisionDebugInstance in instances:
		sig_parts.append(inst.entity_handle)
		sig_parts.append(inst.pos)
		sig_parts.append(inst.heading)
		# Volumes are fresh records every cadence: key on their values.
		for vol: CollisionDebugVolume in inst.volumes:
			sig_parts.append(vol.type)
			sig_parts.append(hash(vol.corners))
	for box: CollisionProbeBox in probe_boxes:
		sig_parts.append(box.entity_handle)
		sig_parts.append(hash(box.corners))
	var sig := hash(sig_parts)
	var has_geometry := not instances.is_empty() or not probe_boxes.is_empty()
	if sig == _hull_signature and _hull_has_surface == has_geometry:
		return
	_hull_signature = sig
	_hull_mesh.clear_surfaces()
	_hull_has_surface = false
	var segments: Array = []
	for inst: CollisionDebugInstance in instances:
		for vol: CollisionDebugVolume in inst.volumes:
			var corners := vol.corners
			if corners.size() != 8:
				continue
			var color: Color = type_color(vol.type)
			for edge in BOX_EDGES:
				segments.append({ "a": corners[edge[0]], "b": corners[edge[1]], "color": color })
	# The vehicle platform probe boxes — where the solver rests wheels, not a
	# BVOL family; drawn in their own colors so a floating hull reads at a
	# glance (probe pair magenta, ground footprint dimmed).
	for box: CollisionProbeBox in probe_boxes:
		var corners := box.corners
		if corners.size() != 8:
			continue
		var color := COLOR_PROBE_FOOTPRINT if box.kind == "footprint" else COLOR_PROBE_BOX
		for edge in BOX_EDGES:
			segments.append({ "a": corners[edge[0]], "b": corners[edge[1]], "color": color })
	if segments.is_empty():
		return
	MissionOverlayUtil.emit_line_segments(_hull_mesh, segments)
	_hull_has_surface = true


# --- Hit / contact flashes ----------------------------------------------------

## The contact-debug hits channel: stride-6 [target_handle, age_ticks, kind,
## x, y, z] events inside the flash TTL. Each draws a cross at its point; a
## hit whose target box is in the same report re-emits that box in the kind
## color, fading with age -- the classic "body lights up on contact" view.
## Pure overdraw: the signature-cached hull mesh and the drawable count stay
## untouched (hits are decoration, not shapes).
func _update_hits(instances: Array, hits: PackedFloat32Array, ttl: int) -> void:
	var sig := hash(hits)
	if sig == _hit_signature:
		return
	_hit_signature = sig
	_hit_mesh.clear_surfaces()
	if hits.is_empty():
		return
	# Box corners by target handle, from the same report the hulls drew.
	var boxes_by_handle := {}
	for inst: CollisionDebugInstance in instances:
		var handle := inst.entity_handle
		if handle >= 0:
			boxes_by_handle[handle] = inst.volumes
	var segments: Array = []
	var count := hits.size() / HIT_STRIDE
	for i in range(count):
		var base := i * HIT_STRIDE
		var target := int(hits[base])
		var age := hits[base + 1]
		var kind := int(hits[base + 2])
		var at := Vector3(hits[base + 3], hits[base + 4], hits[base + 5])
		var color: Color = HIT_KIND_COLORS[kind] \
				if kind >= 0 and kind < HIT_KIND_COLORS.size() else COLOR_OTHER
		var alpha := maxf(HIT_MIN_ALPHA, 1.0 - age / float(ttl))
		var faded := Color(color, alpha)
		_cross(segments, at, 0.25, faded)
		for vol: CollisionDebugVolume in boxes_by_handle.get(target, []):
			var corners := vol.corners
			if corners.size() != 8:
				continue
			for edge in BOX_EDGES:
				segments.append({ "a": corners[edge[0]], "b": corners[edge[1]], "color": faded })
	if not segments.is_empty():
		MissionOverlayUtil.emit_line_segments(_hit_mesh, segments)


# --- The local player's capsule ------------------------------------------------

func _update_player(player: CollisionDebugPlayer) -> void:
	_player_mesh.clear_surfaces()
	if player == null or not player.valid:
		if _gap_label != null:
			_gap_label.visible = false
		return
	var pos := player.position
	var bottom := player.capsule_bottom
	var top := player.capsule_top
	var gap := player.foot_clearance
	var points := player.points
	var radii := player.radii

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
