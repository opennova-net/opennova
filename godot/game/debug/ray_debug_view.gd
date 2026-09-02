extends SimDebugView

# Draws the engine's ray-debug capture over the scene: every raycast the
# collision world ran recently (bullets, knife, throwables, AI / script /
# replication / explosion LOS, ground probes, camera iris, render occlusion,
# sun visibility, sound occlusion, precipitation, picks) as its segment,
# color-coded by category and fading with age. A hit gets a cross at the
# resolved point plus a faint clipped-remainder tail; a blocked boolean ray
# draws darkened; a clear ray draws dimmest. A developer window into OUR
# query traffic, not retail-mimicked UI.
#
# Data comes from Simulation.get_ray_debug(): a stride-12 float channel
# (category, age_ticks, result, start, end, hit — Godot space), already
# filtered by the category mask and TTL the Simulation owns (the F3 Rays
# window edits the same state). Built / freed by GameWorld on the overlay's
# "Show rays" toggle, which also arms engine recording.

const STRIDE := 12
const MIN_ALPHA := 0.15       # the oldest still-drawn event's floor
const BLOCKED_DIM := 0.55     # blocked boolean rays draw darkened
const CLEAR_DIM := 0.3        # clear rays draw dimmest
const TAIL_DIM := 0.18        # the clipped remainder past a hit point
const HIT_CROSS_ARM := 0.15

# RayDebugCategory -> color, enum order (collision.h).
const CATEGORY_COLORS: Array[Color] = [
	Color(0.7, 0.7, 0.7),    # Uncategorized - gray
	Color(1.0, 0.35, 0.15),  # Projectile - red-orange
	Color(1.0, 0.4, 0.7),    # Knife - pink
	Color(1.0, 0.7, 0.2),    # Throwable - amber
	Color(0.95, 0.95, 0.25), # AI LOS - yellow
	Color(0.7, 0.4, 1.0),    # Replication LOS - purple
	Color(0.2, 0.9, 0.75),   # Script LOS - teal
	Color(1.0, 0.5, 0.4),    # Explosion LOS - salmon
	Color(0.75, 0.6, 0.4),   # Ground probe - tan
	Color(0.4, 0.75, 1.0),   # Camera iris - light blue
	Color(0.25, 0.45, 1.0),  # Render occlusion - blue
	Color(1.0, 0.85, 0.3),   # Sun visibility - gold
	Color(0.35, 1.0, 0.45),  # Sound occlusion - green
	Color(0.3, 0.9, 1.0),    # Precipitation - cyan
	Color(1.0, 1.0, 1.0),    # Pick - white
]

var _mesh: ImmediateMesh
var _signature := 0
var _drawable_count := 0


func _build_view() -> void:
	_mesh = ImmediateMesh.new()
	add_child(_make_lines_node("RayDebugLines", _mesh))


func _refresh_from_sim(sim: Simulation) -> void:
	render_report(sim.get_ray_debug())


## Render one ray snapshot (Simulation.get_ray_debug's shape). Split from the
## sim fetch so tests and probes can drive the view with report data directly.
func render_report(report: RayDebugReport) -> void:
	if report == null:
		report = RayDebugReport.new()
	var events := report.events
	var ttl := maxi(1, report.ttl)
	# Ages shift every tick while recording, so the tick + payload size is the
	# change signature (a paused world stops rebuilding).
	var sig := hash([report.tick, events.size()])
	if sig == _signature:
		return
	_signature = sig
	_drawable_count = events.size() / STRIDE
	_mesh.clear_surfaces()
	if events.is_empty():
		return

	var segments: Array = []
	var count := events.size() / STRIDE
	for i in range(count):
		var base := i * STRIDE
		var category := int(events[base])
		var age := events[base + 1]
		var result := int(events[base + 2])
		var start := Vector3(events[base + 3], events[base + 4], events[base + 5])
		var end := Vector3(events[base + 6], events[base + 7], events[base + 8])
		var hit := Vector3(events[base + 9], events[base + 10], events[base + 11])
		var color := CATEGORY_COLORS[category] if category < CATEGORY_COLORS.size() \
				else Color.MAGENTA
		var alpha := maxf(MIN_ALPHA, 1.0 - age / float(ttl))
		match result:
			1:  # hit: bright to the resolved point, faint remainder past it
				segments.append({ "a": start, "b": hit,
						"color": Color(color, alpha) })
				if hit.distance_squared_to(end) > 0.0001:
					segments.append({ "a": hit, "b": end,
							"color": Color(color, alpha * TAIL_DIM) })
				_cross(segments, hit, HIT_CROSS_ARM, Color(color, alpha))
			2:  # blocked boolean ray: no resolved point, darkened
				segments.append({ "a": start, "b": end,
						"color": Color(color * BLOCKED_DIM, alpha) })
			_:  # clear: dimmest
				segments.append({ "a": start, "b": end,
						"color": Color(color, alpha * CLEAR_DIM) })
	MissionOverlayUtil.emit_line_segments(_mesh, segments)


func _clear_all() -> void:
	_drawable_count = 0
	_mesh.clear_surfaces()
	_signature = 0


## Number of ray events currently contributing segments.
func get_debug_drawable_count() -> int:
	return _drawable_count
