extends Node3D

# Renders a NovaNetClient's decoded EVENT stream as transient world-space markers:
# weapon-fire tracers, hit bursts, kill marks, and capture-zone rings. The 3D
# analog of the markers the old standalone 2D replay viewer drew on its canvas;
# everything here is decoded from the wire (NovaNetClient.get_events()) — this only
# renders it. A sibling of NetWorldView (which renders the entity models): this
# draws "what is happening" over them.
#
# All markers are drawn into one ImmediateMesh rebuilt every frame (the canvas's
# redraw-from-the-event-list model), so there is no per-event node churn. Events
# fade by capture-frame age, mirroring the viewer's FADE / KILL_FADE windows.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

# ReplayEventKind — keep in sync with libs/npwire/replay_timeline.h.
const KIND_FIRE := 0
const KIND_HIT := 1
const KIND_KILL := 2
const KIND_GAMEEVENT := 3
const KIND_ZONE := 4

# Fade windows in capture frames (the recv_counter the client stamps each arrival
# with, also the clock get_latest_frame() reports). Mirrors the old 2D viewer.
const FADE := 18
const KILL_FADE := 120

const TRACER_LEN := 12.0    # metres a fire tracer extends along its direction
const ZONE_RADIUS := 18.0   # metres of a capture-zone ring
const KILL_SIZE := 2.0      # metres of a kill-X arm
const NONE_HANDLE := 65535  # the 0xFFFF "no entity" sentinel get_events() carries

# Zone-event aux bit: the zone has been cleared and stops drawing (mirrors the
# viewer's zoneStates()).
const ZONE_CLEARED_AUX_BIT := 0x20
# Capture-zone icon ids -> team (g_minimap_overlay_color_table, net-re §5.19).
const ZONE_ICON_RED := 0x09
const ZONE_ICON_BLUE := 0x0a

const COL_FIRE := Color(1.0, 0.82, 0.29)
const COL_HIT := Color(1.0, 0.35, 0.35)
const COL_KILL := Color(1.0, 0.48, 0.09)
const COL_NEUTRAL := Color(0.71, 0.74, 0.76)
const COL_RED := Color(1.0, 0.35, 0.35)
const COL_BLUE := Color(0.29, 0.56, 1.0)

var _client                # NovaNetClient
var _mesh: ImmediateMesh
var _segments: Array = []   # [a: Vector3, b: Vector3, col: Color] gathered per frame


func setup(client) -> void:
	_client = client
	_mesh = ImmediateMesh.new()
	var mi := MeshInstance3D.new()
	mi.name = "NetEventMarkers"
	mi.mesh = _mesh
	# Unshaded, vertex-coloured, alpha-blended, depth-test off so the markers read
	# as a debug overlay over the terrain + models (the viewer's flat top-down view
	# showed everything; in 3D, drawing on top keeps that visibility).
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.vertex_color_use_as_albedo = true
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.no_depth_test = true
	mi.material_override = mat
	add_child(mi)


func _process(_delta: float) -> void:
	if _client == null or _mesh == null:
		return
	_mesh.clear_surfaces()
	var events: Array = _client.get_events()
	if events.is_empty():
		return
	var frame := int(_client.get_latest_frame())
	_segments.clear()
	# Capture zones are persistent state (latest per source), not a fading burst.
	_draw_zones(events, frame)
	for e in events:
		var age := frame - int(e["frame"])
		if age < 0:
			continue
		match int(e["kind"]):
			KIND_FIRE:
				if age <= FADE:
					_draw_fire(e, 1.0 - float(age) / FADE)
			KIND_HIT:
				if age <= FADE:
					_draw_hit(e, age, 1.0 - float(age) / FADE)
			KIND_KILL:
				if age <= KILL_FADE:
					_draw_kill(e, frame, 0.35 + (1.0 - float(age) / KILL_FADE) * 0.65)
	if _segments.is_empty():
		return  # nothing live this frame — leave the mesh cleared (no empty surface)
	_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	for s in _segments:
		_mesh.surface_set_color(s[2])
		_mesh.surface_add_vertex(s[0])
		_mesh.surface_set_color(s[2])
		_mesh.surface_add_vertex(s[1])
	_mesh.surface_end()


func _line(a: Vector3, b: Vector3, col: Color) -> void:
	_segments.append([a, b, col])


# A horizontal ring in mission space (x=east, y=north plane) at the point's height,
# converted to Godot space per vertex so it lies flat on the world.
func _ring(center_m: Vector3, radius: float, col: Color, segments: int = 24) -> void:
	var prev := Vector3.ZERO
	for i in segments + 1:
		var a := TAU * float(i) / float(segments)
		var pm := center_m + Vector3(cos(a) * radius, sin(a) * radius, 0.0)
		var pg := MissionObjectPlacer.bms_to_godot_position(pm)
		if i > 0:
			_line(prev, pg, col)
		prev = pg


func _draw_fire(e: Dictionary, alpha: float) -> void:
	if not bool(e.get("has_pos", false)) or not bool(e.get("has_dir", false)):
		return
	var dir: Vector2 = e["dir"]
	if dir.length() < 0.0001:
		return
	dir = dir.normalized()
	var origin: Vector3 = e["pos"]                      # mission metres (x=E, y=N, z=up)
	var endm := origin + Vector3(dir.x, dir.y, 0.0) * TRACER_LEN
	var col := COL_FIRE
	col.a = alpha
	_line(MissionObjectPlacer.bms_to_godot_position(origin),
		MissionObjectPlacer.bms_to_godot_position(endm), col)


func _draw_hit(e: Dictionary, age: int, alpha: float) -> void:
	if not bool(e.get("has_pos", false)):
		return
	var col := COL_HIT
	col.a = alpha
	_ring(e["pos"], 0.6 + float(age) * 0.25, col, 16)


func _draw_kill(e: Dictionary, frame: int, alpha: float) -> void:
	var pm = _event_or_entity_pos(e, e.get("target", NONE_HANDLE), frame)
	if pm == null:
		return
	var col := COL_KILL
	col.a = alpha
	var c: Vector3 = MissionObjectPlacer.bms_to_godot_position(pm)
	# An X in the Godot horizontal plane + a short vertical stem so it reads from a
	# spectator's oblique angle (the viewer's flat X was enough top-down).
	var d := KILL_SIZE
	_line(c + Vector3(-d, 0.0, -d), c + Vector3(d, 0.0, d), col)
	_line(c + Vector3(d, 0.0, -d), c + Vector3(-d, 0.0, d), col)
	_line(c, c + Vector3(0.0, d * 2.0, 0.0), col)


# Latest capture-zone state per source at/under `frame`, drawn as a coloured ring;
# a cleared zone draws nothing. Mirrors the viewer's zoneStates().
func _draw_zones(events: Array, frame: int) -> void:
	var latest := {}
	for e in events:
		if int(e["kind"]) != KIND_ZONE or int(e["frame"]) > frame:
			continue
		latest[int(e.get("source", NONE_HANDLE))] = e
	for src in latest:
		var e: Dictionary = latest[src]
		if int(e.get("aux", 0)) & ZONE_CLEARED_AUX_BIT:
			continue
		var pm = _event_or_entity_pos(e, src, frame)
		if pm == null:
			continue
		_ring(pm, ZONE_RADIUS, _zone_color(int(e.get("event_type", 0))))


# The event's own world position when it carries one, else the position of entity
# `handle` interpolated at `frame` (the viewer's BYH.get(...) fallback). Returns a
# mission-space Vector3, or null when neither resolves.
func _event_or_entity_pos(e: Dictionary, handle, frame: int):
	if bool(e.get("has_pos", false)):
		return e["pos"]
	var h := int(handle)
	if h == NONE_HANDLE:
		return null
	var s: Dictionary = _client.sample_at(h, float(frame))
	if not bool(s.get("found", false)):
		return null
	return s["pos"]


func _zone_color(icon: int) -> Color:
	match icon:
		ZONE_ICON_RED: return COL_RED
		ZONE_ICON_BLUE: return COL_BLUE
		_: return COL_NEUTRAL
