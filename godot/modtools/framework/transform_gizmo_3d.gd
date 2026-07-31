class_name TransformGizmo3D
extends Node3D

# Framework in-world transform gizmo (translate + rotate), domain-agnostic.
#
# An ImGuizmo-style "universal" gizmo: three world-aligned translate arrows (X/Y/Z) and
# three rotate rings (one per authored angle component). It owns only geometry,
# screen-space hit-testing, and PURE drag-delta math -- it knows nothing about any
# document. The owner picks a handle, feeds mouse rays in, and applies the returned
# delta through its own commit spine; selection/picking stays owner policy (mission
# keeps its physics-pick bodies, see editor-runtime parity rule 3: editor interaction
# attaches AROUND shared nodes).
#
# Rotation frame: the owner's authored angles may be nested in an euler convention with
# no closed-form inverse (mission's bms_to_godot_basis is
# RotY(90-yaw)*RotZ(pitch)*RotX(roll)*RotY(90)), so each ring's world rotation axis is
# derived numerically (finite difference) from the current authored degrees through the
# injected `basis_builder` -- see _compute_ring_axes. A ring drag maps its swept angle
# back to a delta on exactly that one authored component, so the owner's commit path
# needs no new code. The default builder is a plain euler Basis (authored X/Y/Z degrees
# about the world axes).
#
# Constant on-screen size: _process scales the node by camera distance, so the gizmo stays
# a roughly fixed pixel size. Geometry constants below are in BASE units (pre-scale).

# authored degrees (Vector3) -> Basis. Owners with a domain rotation convention inject
# theirs (mission: MissionObjectPlacer.bms_to_godot_basis); unset falls back to the
# plain euler default.
var basis_builder: Callable = Callable()

# Optional snapping, off by default (0.0): translate deltas snap to multiples of
# translate_snap (world units along the grabbed axis); ring deltas snap to multiples
# of rotate_snap_deg (authored degrees).
var translate_snap: float = 0.0
var rotate_snap_deg: float = 0.0

# Geometry (BASE units; node scale renders these at a constant pixel size).
const ARROW_LEN := 1.0
const CONE_H := 0.26
const CONE_R := 0.085
const RING_RADIUS := 1.28
const RING_SEGMENTS := 48
# Node scale = camera_distance * SCALE_K. Linear in distance keeps the angular (pixel) size
# constant for a fixed-fov perspective camera.
const SCALE_K := 0.10
# Screen-space grab tolerance (pixels) for hit-testing arrows + rings.
const PICK_PIXELS := 11.0

# Axis colours (X red, Y green, Z blue); rings use a lighter tint of the same hue.
const AXIS_COLORS := [Color(0.95, 0.26, 0.24), Color(0.36, 0.86, 0.30), Color(0.28, 0.52, 0.96)]
const RING_COLORS := [Color(0.96, 0.5, 0.47), Color(0.55, 0.88, 0.5), Color(0.46, 0.64, 0.97)]
const HILITE := Color(1.0, 0.95, 0.3)

# World directions for the three translate axes (X / Y / Z).
const AXES := [Vector3.RIGHT, Vector3.UP, Vector3.BACK]

# Current constant-size scale, recomputed per query from the camera so hit-test + drag math
# never depend on _process having run (headless tests drive begin/update directly).
var _scale := 1.0
# Built once in _ready.
var _arrow_lines: Array = []   # MeshInstance3D (shaft)
var _arrow_cones: Array = []   # MeshInstance3D (head)
var _arrow_mats: Array = []    # StandardMaterial3D
var _ring_nodes: Array = []    # MeshInstance3D
var _ring_mats: Array = []     # StandardMaterial3D
# Ring world rotation axes + per-degree sensitivity, recomputed in show_for from rot_deg.
var _ring_axis: Array = [Vector3.RIGHT, Vector3.UP, Vector3.BACK]
var _ring_sens: Array = [1.0, 1.0, 1.0]
# Active drag: { part, axis } plus the captured drag plane + anchors (frozen at grab time).
var _drag: Dictionary = {}
var _drag_plane: Plane = Plane(Vector3.UP, 0.0)
var _drag_axis_world: Vector3 = Vector3.ZERO
# The gizmo origin captured at grab time. The controller moves the gizmo node to ride the object
# every motion frame (set_origin), so the delta math must measure against this FROZEN origin, not
# the live global_position -- otherwise each frame's feedback cancels the last and the drag stalls.
var _drag_o0: Vector3 = Vector3.ZERO
var _drag_t0: float = 0.0
var _drag_v0: Vector3 = Vector3.ZERO
var _drag_sens: float = 1.0
# Unwrapped cumulative rotation (radians) since grab, so a single drag can sweep past +/-180 deg
# without atan2 wrapping the sign. _drag_prev_ang is the previous frame's raw (wrapped) angle.
var _drag_angle: float = 0.0
var _drag_prev_ang: float = 0.0


func _ready() -> void:
	for i in 3:
		var mat := _make_mat(AXIS_COLORS[i])
		_arrow_mats.append(mat)
		var line := MeshInstance3D.new()
		line.material_override = mat
		line.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		var im := ImmediateMesh.new()
		im.surface_begin(Mesh.PRIMITIVE_LINES)
		im.surface_add_vertex(Vector3.ZERO)
		im.surface_add_vertex(AXES[i] * ARROW_LEN)
		im.surface_end()
		line.mesh = im
		add_child(line)
		_arrow_lines.append(line)
		var cone := MeshInstance3D.new()
		var cm := CylinderMesh.new()
		cm.top_radius = 0.0
		cm.bottom_radius = CONE_R
		cm.height = CONE_H
		cm.radial_segments = 12
		cm.rings = 1
		cone.mesh = cm
		cone.material_override = mat
		cone.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		# CylinderMesh points up +Y by default; align +Y to the axis and sit at the tip.
		cone.transform = Transform3D(_aligned_basis(AXES[i]), AXES[i] * ARROW_LEN)
		add_child(cone)
		_arrow_cones.append(cone)
	for i in 3:
		var rmat := _make_mat(RING_COLORS[i])
		_ring_mats.append(rmat)
		var ring := MeshInstance3D.new()
		ring.material_override = rmat
		ring.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(ring)
		_ring_nodes.append(ring)
	_rebuild_rings()


func _process(_dt: float) -> void:
	if not visible:
		return
	var cam := _viewport_camera()
	if cam == null:
		return
	_scale = _scale_for(cam)
	scale = Vector3.ONE * _scale


# Place the gizmo at the selection's container-local origin and orient its rotate rings to
# the entity's current authored degrees. Arrows are world-aligned, so they need no rebuild.
func show_for(local_origin: Vector3, rot_deg: Vector3) -> void:
	position = local_origin
	_compute_ring_axes(rot_deg)
	_rebuild_rings()


# Lightweight reposition during a live drag (keeps the captured drag plane + ring orientation
# stable while the object slides / spins under it).
func set_origin(local_origin: Vector3) -> void:
	position = local_origin


# Nearest grabbable handle under the cursor, or {}. Arrows are tested first (they sit inside
# the rings); { part: "translate"|"rotate", axis: 0/1/2 }. Translate axis = world X/Y/Z;
# rotate axis = authored component (0 pitch, 1 yaw, 2 roll).
func pick_handle(cam: Camera3D, mouse_pos: Vector2) -> Dictionary:
	if cam == null:
		return {}
	var sc := _scale_for(cam)
	var o := global_position
	var best := {}
	var best_d := PICK_PIXELS
	# Arrows are tested first so that on a near-tie the inner translate handles win; the rings then
	# compete on true screen distance against the same best_d (strict <), so a clearly-closer ring in
	# an edge-on pose is still reachable rather than always shadowed by an overlapping arrow.
	for i in 3:
		var tip: Vector3 = o + AXES[i] * ARROW_LEN * sc
		if cam.is_position_behind(o) or cam.is_position_behind(tip):
			continue
		var dist := _dist_point_seg(mouse_pos, cam.unproject_position(o), cam.unproject_position(tip))
		if dist < best_d:
			best_d = dist
			best = { "part": "translate", "axis": i }
	for i in 3:
		var axis: Vector3 = _ring_axis[i]
		var u := _perp(axis)
		var v := axis.cross(u).normalized()
		var prev := Vector2.ZERO
		var have_prev := false
		for s in RING_SEGMENTS + 1:
			var t := TAU * float(s) / float(RING_SEGMENTS)
			var wp := o + (u * cos(t) + v * sin(t)) * RING_RADIUS * sc
			if cam.is_position_behind(wp):
				have_prev = false
				continue
			var sp := cam.unproject_position(wp)
			if have_prev:
				var dist := _dist_point_seg(mouse_pos, prev, sp)
				if dist < best_d:
					best_d = dist
					best = { "part": "rotate", "axis": i }
			prev = sp
			have_prev = true
	return best


# Capture the drag plane + anchor for `handle` at the press position.
func begin(handle: Dictionary, cam: Camera3D, mouse_pos: Vector2) -> void:
	_drag = handle
	# Freeze the grab-time origin; all delta math measures against this, never the live (moved) one.
	_drag_o0 = global_position
	var from := cam.project_ray_origin(mouse_pos)
	var dir := cam.project_ray_normal(mouse_pos)
	if String(handle.get("part", "")) == "translate":
		var a: Vector3 = AXES[int(handle["axis"])]
		_drag_axis_world = a
		# Drag plane contains the axis and faces the camera as much as possible.
		var camf := -cam.global_transform.basis.z
		var n := camf - a * a.dot(camf)
		if n.length() < 1e-4:
			n = _perp(a)
		n = n.normalized()
		_drag_plane = Plane(n, n.dot(_drag_o0))
		var hit = _drag_plane.intersects_ray(from, dir)
		_drag_t0 = (hit - _drag_o0).dot(a) if hit != null else 0.0
	else:
		var i := int(handle["axis"])
		var a: Vector3 = _ring_axis[i]
		_drag_axis_world = a
		_drag_sens = float(_ring_sens[i])
		_drag_plane = Plane(a, a.dot(_drag_o0))
		var hit2 = _drag_plane.intersects_ray(from, dir)
		_drag_v0 = (hit2 - _drag_o0) if hit2 != null else Vector3.ZERO
		_drag_angle = 0.0
		_drag_prev_ang = 0.0


# Drag delta since begin(): { "translate": Vector3 (world) } or { "rotate_deg": float } (a
# delta on the captured ring's authored component). {} when the ray misses the drag plane.
func update(cam: Camera3D, mouse_pos: Vector2) -> Dictionary:
	if _drag.is_empty() or cam == null:
		return {}
	var from := cam.project_ray_origin(mouse_pos)
	var dir := cam.project_ray_normal(mouse_pos)
	var hit = _drag_plane.intersects_ray(from, dir)
	if hit == null:
		return {}
	# Measure against the FROZEN grab origin (_drag_o0), not the live global_position: the controller
	# moves the gizmo to ride the object each frame, and reading the moved origin would cancel the
	# delta and stall the drag.
	if String(_drag.get("part", "")) == "translate":
		var t: float = (hit - _drag_o0).dot(_drag_axis_world)
		return { "translate": _drag_axis_world * _snap_translate(t - _drag_t0) }
	var v: Vector3 = hit - _drag_o0
	if v.length() < 1e-4 or _drag_v0.length() < 1e-4:
		return { "rotate_deg": _snap_rotate(rad_to_deg(_drag_angle) / _drag_sens) }
	# Signed sweep about the ring axis, accumulated + unwrapped so a single drag can pass +/-180 deg
	# without atan2 flipping the sign, then mapped back to authored degrees via the sensitivity.
	var ang := atan2(_drag_axis_world.dot(_drag_v0.cross(v)), _drag_v0.dot(v))
	var step := ang - _drag_prev_ang
	if step > PI:
		step -= TAU
	elif step < -PI:
		step += TAU
	_drag_angle += step
	_drag_prev_ang = ang
	return { "rotate_deg": _snap_rotate(rad_to_deg(_drag_angle) / _drag_sens) }


func _snap_translate(t: float) -> float:
	return snappedf(t, translate_snap) if translate_snap > 0.0 else t


func _snap_rotate(deg: float) -> float:
	return snappedf(deg, rotate_snap_deg) if rotate_snap_deg > 0.0 else deg


func end_drag() -> void:
	_drag = {}


# Tint the matching handle bright (hover or active), the rest to their base colour. Pass {} to
# clear all highlights.
func set_highlight(handle: Dictionary) -> void:
	var part := String(handle.get("part", ""))
	var axis := int(handle.get("axis", -1))
	for i in 3:
		(_arrow_mats[i] as StandardMaterial3D).albedo_color = HILITE if (part == "translate" and axis == i) else AXIS_COLORS[i]
		(_ring_mats[i] as StandardMaterial3D).albedo_color = HILITE if (part == "rotate" and axis == i) else RING_COLORS[i]


# --- Internals ----------------------------------------------------------------

func _scale_for(cam: Camera3D) -> float:
	return maxf(cam.global_position.distance_to(global_position) * SCALE_K, 0.001)


func _viewport_camera() -> Camera3D:
	var vp := get_viewport()
	return vp.get_camera_3d() if vp != null else null


# The injected authored-degrees -> Basis convention, defaulting to plain euler.
func _build_basis(rot_deg: Vector3) -> Basis:
	if basis_builder.is_valid():
		return basis_builder.call(rot_deg)
	return Basis.from_euler(Vector3(deg_to_rad(rot_deg.x), deg_to_rad(rot_deg.y), deg_to_rad(rot_deg.z)))


# Derive each rotate ring's world axis + per-degree sensitivity from the authored degrees by
# finite difference, so the rings stay correct for any orientation without an euler inverse.
func _compute_ring_axes(rot_deg: Vector3) -> void:
	var b0 := _build_basis(rot_deg)
	var b0i := b0.inverse()
	var delta := 1.0  # degrees
	for i in 3:
		var r := rot_deg
		if i == 0:
			r.x += delta
		elif i == 1:
			r.y += delta
		else:
			r.z += delta
		var db := _build_basis(r) * b0i
		var q := db.get_rotation_quaternion()
		var ang := q.get_angle()
		var ax := q.get_axis()
		if ang < 1e-6 or not ax.is_finite():
			ax = AXES[i]
			ang = deg_to_rad(delta)
		_ring_axis[i] = ax.normalized()
		# Signed: get_axis()/get_angle() pack the sign of "+component -> rotation about +axis"
		# into the axis direction, so sens stays positive (~1) and a +sweep about _ring_axis
		# maps to a +component delta.
		_ring_sens[i] = ang / deg_to_rad(delta)


func _rebuild_rings() -> void:
	for i in 3:
		var axis: Vector3 = _ring_axis[i]
		var u := _perp(axis)
		var v := axis.cross(u).normalized()
		var im := ImmediateMesh.new()
		im.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
		for s in RING_SEGMENTS + 1:
			var t := TAU * float(s) / float(RING_SEGMENTS)
			im.surface_add_vertex((u * cos(t) + v * sin(t)) * RING_RADIUS)
		im.surface_end()
		(_ring_nodes[i] as MeshInstance3D).mesh = im


func _make_mat(c: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.albedo_color = c
	# Always grabbable: draw on top of the world so a handle behind terrain/another object
	# is still visible + pickable.
	m.no_depth_test = true
	return m


# A stable unit vector perpendicular to `a`.
func _perp(a: Vector3) -> Vector3:
	var an := a.normalized()
	var ref := Vector3.UP if absf(an.dot(Vector3.UP)) < 0.95 else Vector3.RIGHT
	return ref.cross(an).normalized()


# A basis whose +Y maps to `y_dir` (for aligning the default-up cone mesh to an axis).
func _aligned_basis(y_dir: Vector3) -> Basis:
	var y := y_dir.normalized()
	var ref := Vector3.RIGHT if absf(y.dot(Vector3.RIGHT)) < 0.95 else Vector3.UP
	var x := ref.cross(y).normalized()
	var z := x.cross(y).normalized()
	return Basis(x, y, z)


# 2D distance from point `p` to segment `a`-`b`.
func _dist_point_seg(p: Vector2, a: Vector2, b: Vector2) -> float:
	var ab := b - a
	var l2 := ab.length_squared()
	if l2 < 1e-6:
		return p.distance_to(a)
	var t := clampf((p - a).dot(ab) / l2, 0.0, 1.0)
	return p.distance_to(a + ab * t)
