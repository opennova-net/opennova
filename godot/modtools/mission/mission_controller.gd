extends RefCounted

# Editor-side controller for the Mission workspace (the Phase 2 adapter).
#
# Holds the open mission (NovaMissionData) plus its document state (path /
# loaded / dirty) and drives the load: parse the .bms, resolve its referenced
# terrain + environment from the shared resource root, load them through the
# terrain editor (read-only viewport), then run the host-agnostic
# MissionObjectPlacer under the terrain editor's world root. The resolve + place
# logic is the same piece the runtime uses (NovaWorld.load_mission); this is the
# thin editor binding around it.
#
# Read-only for now: mission authoring (place / move / save entities) is deferred,
# so the document never goes dirty. The dirty/save hooks exist for when authoring
# lands. Referenced via preload (no class_name) so it resolves without an editor
# re-import, the same convention as the placer and veg_assets.gd.

signal changed  # Mission loaded or cleared; the inspector rebuilds on this.

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
# Must match MissionObjectPlacer.CONTAINER_NAME — that is where placed objects land.
const OBJECTS_CONTAINER := "MissionObjects"

var terrain_editor: Node

var _mission: NovaMissionData
var _current_path: String = ""
# The resolved .trn path the loaded mission mounted, so a later terrain swap in the
# Terrain workspace can be detected (see reconcile_with_terrain()).
var _loaded_trn_path: String = ""
var _last_open_dir: String = ""
var _is_dirty: bool = false
var _stats: Dictionary = {}
var _last_status: String = ""

# --- Authoring (Phase 1) state ------------------------------------------------
# Pickable index harvested from the placer (edit_mode): one record per (entity,
# static batch) or per animated entity. See MissionObjectPlacer.pickable_records.
var _pickable: Array = []
# The selected entity as { kind, index }, or empty when nothing is selected.
var _selected_ref: Dictionary = {}
# The selected entity's static batch records (its MultiMesh slots), or its animated
# node, plus its tracked container-local transform and authored rotation (degrees).
var _selected_records: Array = []
var _selected_node: Node3D
var _selected_xform: Transform3D = Transform3D.IDENTITY
var _selected_rotation_deg: Vector3 = Vector3.ZERO
# Drag session: _drag_active spans press..release; _drag_moved gates the commit so a
# plain click only selects.
var _drag_active: bool = false
var _drag_moved: bool = false
# Translucent box marking the selection in the viewport (lazily built under the
# objects container; freed with the container).
var _selection_box: MeshInstance3D


func _init(p_terrain_editor: Node = null) -> void:
	terrain_editor = p_terrain_editor


func set_terrain_editor(value: Node) -> void:
	terrain_editor = value


# --- State accessors ----------------------------------------------------------

func get_mission() -> NovaMissionData:
	return _mission


func is_loaded() -> bool:
	return _mission != null


func is_dirty() -> bool:
	return _is_dirty


func get_current_path() -> String:
	return _current_path


func get_last_open_dir() -> String:
	return _last_open_dir


func get_stats() -> Dictionary:
	return _stats


func get_last_status() -> String:
	return _last_status


# { kind, index, position (mission-space Vector3), animated } for the selected
# entity, or empty when nothing is selected. Drives the inspector's selection line.
func get_selection_summary() -> Dictionary:
	if _selected_ref.is_empty():
		return {}
	return {
		"kind": int(_selected_ref["kind"]),
		"index": int(_selected_ref["index"]),
		"position": MissionObjectPlacer.godot_to_bms_position(_selected_xform.origin),
		"animated": _selected_node != null,
	}


func get_mission_title() -> String:
	if _mission == null:
		return "Mission"
	var mission_name := _mission.get_mission_name().strip_edges()
	if mission_name.is_empty():
		mission_name = _current_path.get_file().get_basename()
	if mission_name.is_empty():
		mission_name = "untitled"
	return "%s%s" % [mission_name, "*" if _is_dirty else ""]


# --- Open ---------------------------------------------------------------------

## Open a .bms: parse it, resolve + load its referenced terrain and environment
## through the terrain editor, then place its objects under the shared world root.
## Returns OK, or an error code; get_last_status() carries a human-facing reason.
func open_mission(bms_path: String) -> Error:
	_last_status = ""
	if terrain_editor == null or not terrain_editor.has_method("get_resource_root"):
		_last_status = "No terrain editor is bound."
		return ERR_UNAVAILABLE
	var resource_root: NovaResourceRoot = terrain_editor.get_resource_root()
	if resource_root == null:
		_last_status = "Set a resource directory before opening a mission."
		return ERR_UNCONFIGURED

	var mission := NovaMissionData.new()
	if mission.open_file(bms_path) != OK:
		_last_status = "Could not read %s: %s" % [bms_path.get_file(), mission.get_last_error()]
		return ERR_CANT_OPEN

	# The mission header selects the world: resolve its terrain (required) and
	# environment (optional) from the user's resource directory, case-insensitive.
	var terrain_ref := mission.get_terrain_ref()
	var trn_path := resource_root.resolve_file(terrain_ref + ".trn")
	if trn_path.is_empty():
		_last_status = "%s.trn (referenced by the mission) was not found in the resource directory." % terrain_ref
		return ERR_FILE_NOT_FOUND

	# Loading the referenced terrain is an atomic dependency of opening the mission,
	# not a separate user action, so it goes straight to open_trn rather than the
	# terrain editor's dirty-guarded request_open_trn.
	var trn_err := int(terrain_editor.open_trn(trn_path))
	if trn_err != OK:
		# open_trn already replaced the editor's terrain with an empty one, so any
		# previously-loaded mission now describes a world that is gone. Drop it
		# rather than leaving stale objects / metadata over a blanked terrain.
		clear()
		_last_status = "Could not load %s.trn (error %d)." % [terrain_ref, trn_err]
		return trn_err as Error

	var env_note := _load_environment(mission, resource_root)
	_place_objects(mission, resource_root)

	_mission = mission
	_current_path = bms_path
	_loaded_trn_path = trn_path
	_last_open_dir = bms_path.get_base_dir()
	_is_dirty = false
	_last_status = _describe_load(mission, bms_path, env_note)
	changed.emit()
	return OK


func clear() -> void:
	_reset_selection_state()
	_pickable = []
	_clear_objects()
	_mission = null
	_current_path = ""
	_loaded_trn_path = ""
	_stats = {}
	_is_dirty = false
	changed.emit()


# If the terrain underneath was swapped out from under a loaded mission (the user
# opened a different terrain in the Terrain workspace), the placed objects no
# longer belong to the mounted world. Drop the mission so its objects / metadata
# stop describing a world that is no longer there; re-opening shows it on its own
# terrain again. Called when the Mission workspace regains focus.
func reconcile_with_terrain() -> void:
	if _mission == null or terrain_editor == null or not terrain_editor.has_method("get_current_trn_path"):
		return
	if String(terrain_editor.get_current_trn_path()) != _loaded_trn_path:
		clear()


# The MissionObjects container hangs under the shared terrain world root, so it is
# also under the terrain workspace's view. Hide it there, show it in the mission
# workspace (toggled from the workspace's activate / deactivate).
func set_objects_visible(value: bool) -> void:
	var container := _objects_container()
	if container != null:
		container.visible = value


func mark_dirty() -> void:
	if not _is_dirty:
		_is_dirty = true
	changed.emit()


# --- Save ---------------------------------------------------------------------
# Mirrors the editor save contract (see strings_editor.gd): save_current() writes
# back to the opened path and returns ERR_INVALID_PARAMETER when there is none (the
# shell then offers Save As); save_as() takes a directory and composes the filename.

func save_current() -> Error:
	if _mission == null:
		return ERR_UNAVAILABLE
	if _current_path.is_empty() or _current_path.get_extension().to_lower() != "bms":
		return ERR_INVALID_PARAMETER
	var err := int(_mission.save_file())
	if err == OK:
		_is_dirty = false
		_last_status = "Saved %s." % _current_path.get_file()
		changed.emit()
	else:
		_last_status = "Could not save %s: %s" % [_current_path.get_file(), _mission.get_last_error()]
	return err as Error


func save_as(dir_path: String) -> Error:
	if _mission == null:
		return ERR_UNAVAILABLE
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	var mkdir := DirAccess.make_dir_recursive_absolute(dir_path)
	if mkdir != OK:
		return mkdir
	var filename := _current_path.get_file()
	if filename.is_empty():
		filename = "mission.bms"
	var path := dir_path.path_join(filename)
	var err := int(_mission.save_as(path))
	if err == OK:
		_current_path = path
		_last_open_dir = dir_path
		_is_dirty = false
		_last_status = "Saved %s." % filename
		changed.emit()
	else:
		_last_status = "Could not save %s: %s" % [filename, _mission.get_last_error()]
	return err as Error


# --- Viewport authoring: select + terrain-plane drag --------------------------
# Driven by the viewport input router (set as its input_target by the workspace).
# Left-click picks an entity (analytic ray-vs-AABB over the pickable index); dragging
# re-grounds it on the terrain each motion; release writes the new position back to
# the mission record. Rotation is preserved (numeric/rotate editing is a later phase).

func handle_viewport_input(event: InputEvent) -> void:
	if _mission == null or terrain_editor == null:
		return
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index != MOUSE_BUTTON_LEFT:
			return
		if mb.pressed:
			_on_left_press(mb.position)
		else:
			_on_left_release()
	elif event is InputEventMouseMotion and _drag_active:
		var motion := event as InputEventMouseMotion
		# Defend against a missed button-up (e.g. the release landed on a different
		# control while switching workspaces): if the left button is no longer held,
		# the gesture was abandoned, not continuing, so end it without committing.
		if (motion.button_mask & MOUSE_BUTTON_MASK_LEFT) == 0:
			cancel_drag()
			return
		_on_drag(motion.position)


# End an in-progress drag without committing. The workspace calls this when it
# deactivates / unmounts so a half-finished gesture cannot silently resume (and
# relocate + dirty the selection) on a later bare hover after the user returns.
func cancel_drag() -> void:
	_drag_active = false
	_drag_moved = false


func _on_left_press(mouse_pos: Vector2) -> void:
	var ref := _pick_entity(mouse_pos)
	if ref.is_empty():
		_deselect()
		return
	_select(int(ref["kind"]), int(ref["index"]))
	_drag_active = true
	_drag_moved = false


func _on_drag(mouse_pos: Vector2) -> void:
	if _selected_ref.is_empty() or terrain_editor == null or not terrain_editor.has_method("raycast_terrain_at"):
		return
	var hit: Vector3 = terrain_editor.raycast_terrain_at(mouse_pos)
	if not terrain_editor.is_valid_terrain_hit(hit):
		return
	_drag_moved = true
	_move_selected_to_world(hit)


func _on_left_release() -> void:
	if _drag_active and _drag_moved:
		_commit_selected_transform()
	_drag_active = false
	_drag_moved = false


func _pick_entity(mouse_pos: Vector2) -> Dictionary:
	if not terrain_editor.has_method("get_editor_camera"):
		return {}
	var camera: Camera3D = terrain_editor.get_editor_camera()
	if camera == null:
		return {}
	var from := camera.project_ray_origin(mouse_pos)
	var dir := camera.project_ray_normal(mouse_pos)
	var best_t := INF
	var best: Dictionary = {}
	for rec in _pickable:
		var aabb := _record_world_aabb(rec)
		if aabb.size == Vector3.ZERO:
			continue
		var t := _ray_aabb_entry(aabb, from, dir)
		if t >= 0.0 and t < best_t:
			best_t = t
			best = { "kind": int(rec["kind"]), "index": int(rec["index"]) }
	return best


func _select(kind: int, index: int) -> void:
	_selected_ref = { "kind": kind, "index": index }
	_selected_records = []
	_selected_node = null
	for rec in _pickable:
		if int(rec["kind"]) == kind and int(rec["index"]) == index:
			if bool(rec.get("animated", false)):
				_selected_node = rec.get("node")
			else:
				_selected_records.append(rec)
	var entity := _find_entity(kind, index)
	_selected_rotation_deg = entity.get("rotation_deg", Vector3.ZERO)
	_selected_xform = MissionObjectPlacer.entity_transform(
		entity.get("position", Vector3.ZERO), _selected_rotation_deg)
	_update_selection_box()
	changed.emit()


func _deselect() -> void:
	if _selected_ref.is_empty():
		return
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_hide_selection_box()
	changed.emit()


# Move the selected entity so its origin sits at a world-space ground point: rewrite
# every static MultiMesh instance (slot) of the entity, or the animated node, plus the
# selection box. Rotation is kept; only the origin tracks the cursor.
func _move_selected_to_world(global_hit: Vector3) -> void:
	var container := _objects_container()
	if container == null:
		return
	var local := container.global_transform.affine_inverse() * global_hit
	_selected_xform = Transform3D(_selected_xform.basis, local)
	if _selected_node != null:
		_selected_node.transform = _selected_xform
	else:
		for rec in _selected_records:
			var mm: MultiMesh = rec["mm"]
			mm.set_instance_transform(int(rec["slot"]), _selected_xform * (rec["offset"] as Transform3D))
	_update_selection_box()


func _commit_selected_transform() -> void:
	if _selected_ref.is_empty() or _mission == null:
		return
	var bms_pos := MissionObjectPlacer.godot_to_bms_position(_selected_xform.origin)
	if _mission.set_entity_transform(int(_selected_ref["kind"]), int(_selected_ref["index"]), bms_pos, _selected_rotation_deg):
		mark_dirty()


# --- Selection geometry helpers -----------------------------------------------

func _find_entity(kind: int, index: int) -> Dictionary:
	if _mission == null:
		return {}
	for e in _mission.get_entities(kind):
		if int((e as Dictionary).get("index", -1)) == index:
			return e
	return {}


func _record_world_aabb(rec: Dictionary) -> AABB:
	if bool(rec.get("animated", false)):
		var node: Node3D = rec.get("node")
		return _node_world_aabb(node) if node != null and node.is_inside_tree() else AABB()
	var mmi: MultiMeshInstance3D = rec.get("mmi")
	var mm: MultiMesh = rec.get("mm")
	if mmi == null or mm == null or not mmi.is_inside_tree():
		return AABB()
	var inst := mmi.global_transform * mm.get_instance_transform(int(rec["slot"]))
	return inst * (rec["mesh_aabb"] as AABB)


func _node_world_aabb(node: Node3D) -> AABB:
	var result := AABB()
	var have := false
	for vi in _visual_instances(node):
		var world: AABB = (vi as VisualInstance3D).global_transform * (vi as VisualInstance3D).get_aabb()
		if not have:
			result = world
			have = true
		else:
			result = result.merge(world)
	return result


func _visual_instances(node: Node) -> Array:
	var out: Array = []
	if node == null:
		return out
	if node is VisualInstance3D:
		out.append(node)
	for child in node.get_children():
		out.append_array(_visual_instances(child))
	return out


# Slab-method ray/AABB: returns the entry distance along `dir` (>= 0), or -1 on a miss.
func _ray_aabb_entry(aabb: AABB, from: Vector3, dir: Vector3) -> float:
	var tmin := -INF
	var tmax := INF
	var mn := aabb.position
	var mx := aabb.position + aabb.size
	for axis in 3:
		var o: float = from[axis]
		var d: float = dir[axis]
		var lo: float = mn[axis]
		var hi: float = mx[axis]
		if absf(d) < 1e-8:
			if o < lo or o > hi:
				return -1.0
		else:
			var t1 := (lo - o) / d
			var t2 := (hi - o) / d
			if t1 > t2:
				var tmp := t1
				t1 = t2
				t2 = tmp
			tmin = maxf(tmin, t1)
			tmax = minf(tmax, t2)
			if tmin > tmax:
				return -1.0
	if tmax < 0.0:
		return -1.0
	return maxf(tmin, 0.0)


func _selected_world_aabb() -> AABB:
	if _selected_node != null:
		return _node_world_aabb(_selected_node)
	var result := AABB()
	var have := false
	for rec in _selected_records:
		var a := _record_world_aabb(rec)
		if a.size == Vector3.ZERO:
			continue
		if not have:
			result = a
			have = true
		else:
			result = result.merge(a)
	return result


func _update_selection_box() -> void:
	var aabb := _selected_world_aabb()
	if aabb.size == Vector3.ZERO:
		_hide_selection_box()
		return
	var box := _ensure_selection_box()
	if box == null:
		return
	# Pad slightly so the outline reads around the object rather than z-fighting it.
	var pad := Vector3.ONE * 0.25
	var size := aabb.size + pad * 2.0
	var center := aabb.position + aabb.size * 0.5
	box.global_transform = Transform3D(Basis().scaled(size), center)
	box.visible = true


func _ensure_selection_box() -> MeshInstance3D:
	if _selection_box != null and is_instance_valid(_selection_box):
		return _selection_box
	var container := _objects_container()
	if container == null:
		return null
	var mi := MeshInstance3D.new()
	mi.name = "MissionSelectionBox"
	var box_mesh := BoxMesh.new()
	box_mesh.size = Vector3.ONE
	mi.mesh = box_mesh
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.albedo_color = Color(0.25, 0.9, 1.0, 0.18)
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mi.material_override = mat
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	container.add_child(mi)
	_selection_box = mi
	return mi


func _hide_selection_box() -> void:
	if _selection_box != null and is_instance_valid(_selection_box):
		_selection_box.visible = false


# Clear selection refs without touching the scene. The selection box is a child of the
# objects container, so it is freed when the container is (re)built; here we only drop
# the dangling ref.
func _reset_selection_state() -> void:
	_selected_ref = {}
	_selected_records = []
	_selected_node = null
	_selected_xform = Transform3D.IDENTITY
	_selected_rotation_deg = Vector3.ZERO
	_drag_active = false
	_drag_moved = false
	_selection_box = null


# --- Internals ----------------------------------------------------------------

# Load the mission's environment into the shared editor environment. Returns a
# short note ("" when clean) describing any problem, so the open status can be
# honest about a partial load. Environment is best-effort: the open does not fail
# just because the atmosphere is missing, but the rendered world is always made to
# match the mission rather than carrying over the previously-open mission's
# environment — when the mission brings no usable env, reset to a neutral default.
func _load_environment(mission: NovaMissionData, resource_root: NovaResourceRoot) -> String:
	if not terrain_editor.has_method("get_environment_editor"):
		return ""
	var env_editor = terrain_editor.get_environment_editor()
	if env_editor == null:
		return ""

	var note := ""
	var env_ref := mission.get_environment_ref().strip_edges()
	if not env_ref.is_empty():
		var env_path := resource_root.resolve_file(env_ref + ".env")
		if env_path.is_empty():
			note = "environment %s was not found" % env_ref
		elif env_editor.has_method("open_env") and int(env_editor.open_env(env_path)) == OK:
			return ""  # loaded the mission's own environment; nothing to reset or note
		else:
			note = "environment %s could not be loaded" % env_ref

	# Blank, unresolved, or unreadable reference: reset to a neutral default so the
	# atmosphere matches the inspector instead of lingering from a prior mission.
	if env_editor.has_method("create_default_environment"):
		env_editor.create_default_environment(false)
	return note


func _place_objects(mission: NovaMissionData, resource_root: NovaResourceRoot) -> void:
	_stats = {}
	# A fresh placement replaces the container (and the old selection box with it), so
	# drop any stale selection refs before re-harvesting the pickable index.
	_reset_selection_state()
	_pickable = []
	if not terrain_editor.has_method("get_terrain_world_root"):
		return
	var world_root: Node3D = terrain_editor.get_terrain_world_root()
	if world_root == null:
		return
	var placer := MissionObjectPlacer.new(resource_root)
	placer.edit_mode = true
	var options: Dictionary = {}
	var env_node := _environment_node()
	if env_node != null:
		options["environment_node"] = env_node
	_stats = placer.place(mission, world_root, options)
	_pickable = placer.pickable_records


func _environment_node() -> Node:
	if terrain_editor != null and terrain_editor.has_method("get_environment_node"):
		return terrain_editor.get_environment_node()
	return null


func _objects_container() -> Node3D:
	if terrain_editor == null or not terrain_editor.has_method("get_terrain_world_root"):
		return null
	var world_root: Node3D = terrain_editor.get_terrain_world_root()
	if world_root == null:
		return null
	return world_root.get_node_or_null(NodePath(OBJECTS_CONTAINER)) as Node3D


func _clear_objects() -> void:
	var container := _objects_container()
	if container != null and container.get_parent() != null:
		container.get_parent().remove_child(container)
		container.queue_free()


func _describe_load(mission: NovaMissionData, bms_path: String, env_note: String = "") -> String:
	var mission_name := mission.get_mission_name().strip_edges()
	if mission_name.is_empty():
		mission_name = bms_path.get_file()
	var placed := int(_stats.get("placed", 0))
	var unresolved := int(_stats.get("unresolved", 0))
	var text := "Loaded %s: %d objects placed" % [mission_name, placed]
	if unresolved > 0:
		text += ", %d unresolved" % unresolved
	if not env_note.is_empty():
		text += " (%s)" % env_note
	return text + "."
