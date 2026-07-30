extends "res://modtools/mission/controller/controller_section.gd"

# Open / new / clear / save plus the load internals (environment
# resolve, object placement, load description).
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

# --- Open ---------------------------------------------------------------------

# True when `trn_path` already IS the mounted terrain (case-insensitive,
# slash-normalized — resolve_file and a user's own open can disagree on form)
# and that terrain has no unsaved edits. Dirty never matches, so the reload
# there preserves today's semantics; the dirty read is duck-typed because the
# headless test stub carries no is_dirty.
func _is_same_clean_terrain(trn_path: String) -> bool:
	if not _c.terrain_editor.has_method("get_current_trn_path"):
		return false
	if bool(_c.terrain_editor.get("is_dirty")):
		return false
	var current := String(_c.terrain_editor.get_current_trn_path())
	if current.is_empty():
		return false
	return current.replace("\\", "/").to_lower() == trn_path.replace("\\", "/").to_lower()

## Open a .bms/.mis: parse it, resolve + load its referenced terrain and environment
## through the terrain editor, then place its objects under the shared world root.
## Returns OK, or an error code; get_last_status() carries a human-facing reason.
func open_mission(bms_path: String) -> Error:
	_c._last_status = ""
	if _c.terrain_editor == null:
		_c._last_status = "No terrain editor is bound."
		return ERR_UNAVAILABLE
	var resource_root: NovaResourceRoot = _c._resource_root()
	if resource_root == null:
		_c._last_status = "Set a resource directory before opening a mission."
		return ERR_UNCONFIGURED

	# Wall-clock attribution per load stage; the abandoned timeline of a failed
	# open never reaches the ring (only finish() retains it).
	var timeline := PerfTimeline.begin("Mission load %s" % bms_path.get_file())

	timeline.span("parse")
	var mission := NovaMissionData.new()
	if mission.open_file(bms_path) != OK:
		_c._last_status = "Could not read %s: %s" % [bms_path.get_file(), mission.get_last_error()]
		return ERR_CANT_OPEN
	timeline.end_span()

	# The mission header selects the world: resolve its terrain (required) and
	# environment (optional) from the user's resource directory, case-insensitive.
	var terrain_ref := mission.get_terrain_ref()
	var trn_path := resource_root.resolve_file(terrain_ref + ".trn")
	if trn_path.is_empty():
		_c._last_status = "%s.trn (referenced by the mission) was not found in the resource directory." % terrain_ref
		return ERR_FILE_NOT_FOUND

	# Loading the referenced terrain is an atomic dependency of opening the mission,
	# not a separate user action, so it goes straight to open_trn rather than the
	# terrain workspace's dirty-guarded open_file. When the resolved .trn is
	# already the mounted terrain and it carries no unsaved edits, the remount is
	# skipped — the dominant browse-missions-on-one-map flow pays the terrain build
	# once. A dirty terrain always reloads (predictable authoring semantics).
	timeline.span("terrain")
	if _is_same_clean_terrain(trn_path):
		# Adopt the editor's own path form so reconcile_with_terrain's exact
		# compare cannot mistake a case/slash difference for a terrain swap.
		trn_path = String(_c.terrain_editor.get_current_trn_path())
	else:
		var trn_err := int(_c.terrain_editor.open_trn(trn_path, timeline))
		if trn_err != OK:
			# open_trn already replaced the editor's terrain with an empty one, so any
			# previously-loaded mission now describes a world that is gone. Drop it
			# rather than leaving stale objects / metadata over a blanked terrain.
			clear()
			_c._last_status = "Could not load %s.trn (error %d)." % [terrain_ref, trn_err]
			return trn_err as Error
	timeline.end_span()

	_c._load_mission_tile_info(bms_path, resource_root)
	timeline.span("environment")
	var env_note := _load_environment(mission, resource_root)
	timeline.end_span()
	timeline.span("objects")
	_place_objects(mission, resource_root, timeline)
	timeline.end_span()

	_c._mission = mission
	_c._current_path = bms_path
	_c._loaded_trn_path = trn_path
	_c._reground._record_ground_state()
	_c._last_open_dir = bms_path.get_base_dir()
	# A fresh undo history for this document, and a clean baseline so the freshly-opened mission
	# is not dirty (and undoing back to it later clears the `*`).
	_c._clear_history()
	_c._mission.mark_clean()
	# Fresh document: drop any prior marker selection and focus a populated path (so the
	# waypoint panel is not empty) only if the user is already in waypoints mode.
	_c._selected_marker = {}
	_c._marker_place_armed = false
	_c._selected_path_index = _c._waypoints._first_nonempty_path() if _c._mode == _c.Mode.WAYPOINTS else -1
	# Focus the first zone when reopening already in area-trigger mode, mirroring set_mode (and the
	# waypoint branch above), so the Triggers panel is not empty after an open.
	_c._selected_zone_index = 0 if (_c._mode == _c.Mode.AREA_TRIGGERS and mission.get_area_trigger_count() > 0) else -1
	timeline.span("overlays")
	_c._refresh_active_overlay()
	timeline.end_span()
	timeline.finish()
	_c._last_status = "%s (%s)" % [_describe_load(mission, bms_path, env_note), timeline.brief(3)]
	_c._notify_changed()
	return OK


## Create a brand-new empty mission on the currently-loaded terrain. A mission needs a terrain
## both to place objects onto and to reference in its header, so this requires one to be loaded
## already (open or create a terrain first); it adopts that terrain's basename as the mission's
## terrain ref. The (empty) objects are placed so the placer + palette + picking are live, exactly
## as after an open. The mission has no file yet (Save routes to Save As) and is clean until the
## first edit. Returns OK, or an error; get_last_status() carries a human-facing reason.
func new_mission() -> Error:
	_c._last_status = ""
	if _c.terrain_editor == null:
		_c._last_status = "No terrain editor is bound."
		return ERR_UNAVAILABLE
	var resource_root: NovaResourceRoot = _c._resource_root()
	if resource_root == null:
		_c._last_status = "Set a resource directory before creating a mission."
		return ERR_UNCONFIGURED
	var trn_path := String(_c.terrain_editor.get_current_trn_path()) if _c.terrain_editor.has_method("get_current_trn_path") else ""
	var world_root: Node3D = _c.terrain_editor.get_terrain_world_root() if _c.terrain_editor.has_method("get_terrain_world_root") else null
	if trn_path.is_empty() or world_root == null:
		_c._last_status = "Open or create a terrain first, then start a new mission on it."
		return ERR_UNCONFIGURED

	var mission := NovaMissionData.new()
	if mission.create_default() != OK:
		_c._last_status = "Could not create a new mission: %s" % mission.get_last_error()
		return FAILED
	# Self-describe: adopt the loaded terrain's basename so a later reopen resolves the same world.
	_c._clear_mission_tile_info()
	var terrain_ref := trn_path.get_file().get_basename()
	mission.set_header_string("terrain", terrain_ref)

	# Reset to a neutral environment (a fresh mission carries no env ref), then build the empty
	# world so the placer + palette + picking are live, exactly as after an open.
	var env_note := _load_environment(mission, resource_root)
	_place_objects(mission, resource_root)

	_c._mission = mission
	_c._current_path = ""          # no file yet; Save routes through Save As
	_c._loaded_trn_path = trn_path
	_c._reground._record_ground_state()
	# Empty undo history and a clean baseline: the new mission is not dirty until the first edit
	# (Save As is always available regardless). mark_clean must follow create_default so the
	# baseline is the empty mission.
	_c._clear_history()
	_c._mission.mark_clean()
	_c._viewport._reset_selection_state()
	_c._selected_marker = {}
	_c._marker_place_armed = false
	_c._selected_path_index = -1
	_c._selected_zone_index = -1
	_c._selected_event_index = -1
	_c._refresh_active_overlay()
	if not env_note.is_empty():
		_c._last_status = "New mission on %s (%s)." % [terrain_ref, env_note]
	else:
		_c._last_status = "New mission on %s." % terrain_ref
	_c._notify_changed()
	return OK


func clear() -> void:
	_c._preview.stop_preview()
	_c._viewport._reset_selection_state()
	_c._pickable = []
	_c._place_item_id = 0
	_c._marker_place_armed = false
	_c._selected_path_index = -1
	_c._placer = null
	_c._clear_mission_tile_info()
	_clear_objects()
	# Dropping the document drops its undo history + clean baseline with it (they live on the
	# NovaMissionData), so there is nothing else to reset; is_dirty() reads false once _mission is null.
	_c._mission = null
	_c._current_path = ""
	_c._loaded_trn_path = ""
	_c._loaded_height_revision = -1
	_c._ground_baseline = {}
	_c._reground_requests_cache = []
	_c._reground_cache_token = []
	_c._stats = {}
	_c._notify_changed()


func save_current() -> Error:
	if _c._mission == null:
		return ERR_UNAVAILABLE
	var ext = _c._current_path.get_extension().to_lower()
	if _c._current_path.is_empty() or (ext != "bms" and ext != "mis"):
		return ERR_INVALID_PARAMETER
	if ext == "mis":
		_stage_mis_base_heights()
	var err := int(_c._mission.save_file())
	if err == OK:
		# The saved state is the new clean baseline; the undo history is kept so the user can
		# still undo across the save.
		_c._mission.mark_clean()
		_c._last_status = "Saved %s." % _c._current_path.get_file()
		_c._notify_changed()
	else:
		_c._last_status = "Could not save %s: %s" % [_c._current_path.get_file(), _c._mission.get_last_error()]
	return err as Error


func save_as(dir_path: String) -> Error:
	if _c._mission == null:
		return ERR_UNAVAILABLE
	if dir_path.is_empty():
		return ERR_INVALID_PARAMETER
	var filename = _c._current_path.get_file()
	if filename.is_empty():
		filename = "mission.bms"
	return save_as_file(dir_path.path_join(filename))


func save_as_file(path: String) -> Error:
	if _c._mission == null:
		return ERR_UNAVAILABLE
	if path.is_empty():
		return ERR_INVALID_PARAMETER
	var ext := path.get_extension().to_lower()
	if ext != "bms" and ext != "mis":
		_c._last_status = "Mission files must be saved as .bms or .mis."
		return ERR_INVALID_PARAMETER
	var dir_path := path.get_base_dir()
	if not dir_path.is_empty():
		var mkdir := DirAccess.make_dir_recursive_absolute(dir_path)
		if mkdir != OK:
			return mkdir
	var filename := path.get_file()
	if ext == "mis":
		_stage_mis_base_heights()
	var err := int(_c._mission.save_as(path))
	if err == OK:
		_c._current_path = path
		_c._last_open_dir = dir_path
		_c._mission.mark_clean()
		_c._last_status = "Saved %s." % filename
		_c._notify_changed()
	else:
		_c._last_status = "Could not save %s: %s" % [filename, _c._mission.get_last_error()]
	return err as Error


## Save to an explicit .bms file path (the MCP save seam). Mirrors save_as(),
## which takes a directory and composes the filename from the current path;
## this takes the full destination and adopts it as the current path.
func save_as_path(path: String) -> Error:
	if _c._mission == null:
		return ERR_UNAVAILABLE
	if path.is_empty() or path.get_extension().to_lower() != "bms":
		return ERR_INVALID_PARAMETER
	var mkdir := DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	if mkdir != OK:
		return mkdir
	var err := int(_c._mission.save_as(path))
	if err == OK:
		_c._current_path = path
		_c._last_open_dir = path.get_base_dir()
		_c._mission.mark_clean()
		_c._last_status = "Saved %s." % path.get_file()
		_c._notify_changed()
	else:
		_c._last_status = "Could not save %s: %s" % [path.get_file(), _c._mission.get_last_error()]
	return err as Error


# --- Authoring (Phase 5): undo / redo -----------------------------------------
# The history + dirty flag live on the document (NovaMissionData): in-memory bms::File snapshots,
# never serialized bytes. The controller drives them. A continuous gesture (a drag, a run of
# inspector edits) is bracketed by begin_edit/commit_edit so it becomes one step; one-shot
# mutations bracket the same way (commit pushes a step only if the document actually changed, so a
# plain click / same-value / failed edit pushes nothing). undo/redo swap the document in memory
# (O(1), cannot fail) and the controller re-bakes the world to match. Triggered by the viewport
# Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y; also exposed for the workspace's framework hooks. Selection is
# dropped on restore: structural edits reindex entities, and the re-bake resets selection anyway.

# --- Internals ----------------------------------------------------------------

# Stage terrain base heights on the document for a .mis save: one 16.16 fixed-point height per
# entity, FLAT in the .mis writer's order (items, buildings, markers, organics). The original
# editor subtracts extra_bheight from a height-locked item's absolute z to recover the
# terrain-relative offset [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll], so each
# entity's base height is the terrain height under its (x, y) plane position — sampled through
# the same transform the placer uses (bms_to_godot_position maps mission (x, y, z) to godot
# (x, z, -y)): the godot-world sample point for mission (x, y) is (x, -y), and the sampled godot
# Y IS the mission z-units height. Off-terrain samples (NAN) bake 0; with no terrain surface at
# all nothing is staged (extra_bheight stays 0 — positions remain absolute-declared either way,
# only the baked base is absent). Duck-typed like _build_reground_requests so headless stubs work.
func _stage_mis_base_heights() -> void:
	if _c._mission == null or _c.terrain_editor == null:
		return
	var batched: bool = _c.terrain_editor.has_method("sample_heights_world")
	if not batched and not _c.terrain_editor.has_method("sample_height_world"):
		return
	var points := PackedVector2Array()
	for kind in [NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ORGANIC]:
		for e in _c._mission.get_entities(kind):
			var pos: Vector3 = (e as Dictionary).get("position", Vector3.ZERO)
			points.append(Vector2(pos.x, -pos.y))
	if points.is_empty():
		return
	var heights: PackedFloat32Array
	if batched:
		heights = _c.terrain_editor.sample_heights_world(points)
	else:
		heights = PackedFloat32Array()
		heights.resize(points.size())
		for i in points.size():
			heights[i] = _c.terrain_editor.sample_height_world(points[i].x, points[i].y)
	if heights.size() != points.size():
		return
	var fixed := PackedInt32Array()
	fixed.resize(points.size())
	for i in points.size():
		var h := heights[i]
		fixed[i] = 0 if is_nan(h) else int(roundf(h * 65536.0))
	_c._mission.set_mis_base_heights(fixed)


# Load the mission's environment into the shared editor environment. Returns a
# short note ("" when clean) describing any problem, so the open status can be
# honest about a partial load. Environment is best-effort: the open does not fail
# just because the atmosphere is missing, but the rendered world is always made to
# match the mission rather than carrying over the previously-open mission's
# environment — when the mission brings no usable env, reset to a neutral default.
func _load_environment(mission: NovaMissionData, resource_root: NovaResourceRoot) -> String:
	if not _c.terrain_editor.has_method("get_environment_editor"):
		return ""
	var env_editor = _c.terrain_editor.get_environment_editor()
	if env_editor == null:
		return ""

	var note := ""
	var env_ref := mission.get_environment_ref().strip_edges()
	if not env_ref.is_empty():
		var env_path := resource_root.resolve_file(env_ref + ".env")
		if env_path.is_empty():
			note = "environment %s was not found" % env_ref
		elif env_editor.has_method("open_env") and int(env_editor.open_env(env_path)) == OK:
			# Game parity: the runtime layers the mission's attrib-gated fog/water
			# overrides on top of the .env (get_environment_overrides builds exactly
			# the apply_mission_overrides payload). Apply them to the preview too,
			# then re-fan-out — open_env already emitted with the bare .env values.
			var env_file: Variant = env_editor.get("env_file")
			var overrides: Dictionary = mission.get_environment_overrides()
			if env_file != null and not overrides.is_empty() and env_file.has_method("apply_mission_overrides"):
				env_file.apply_mission_overrides(overrides)
				if env_editor.has_method("_emit_all_changed"):
					env_editor._emit_all_changed()
			return ""  # loaded the mission's own environment; nothing to reset or note
		else:
			note = "environment %s could not be loaded" % env_ref

	# Blank, unresolved, or unreadable reference: reset to a neutral default so the
	# atmosphere matches the inspector instead of lingering from a prior mission.
	if env_editor.has_method("create_default_environment"):
		env_editor.create_default_environment(false)
	return note


## Re-apply the open mission's environment (ref + fog/water overrides) to the
## editor preview — the seam the MCP set_mission_header tool calls after the
## `environment` header changes, since open/new are otherwise the only times
## the preview tracks the mission. Returns the load note ("" on success).
func reload_environment() -> String:
	if _c._mission == null:
		return "no mission open"
	var resource_root: NovaResourceRoot = _c._resource_root()
	if resource_root == null:
		return "no resource root mounted"
	return _load_environment(_c._mission, resource_root)


func _place_objects(mission: NovaMissionData, resource_root: NovaResourceRoot, timeline: PerfTimeline = null) -> void:
	_c._stats = {}
	# A fresh placement replaces the container (and the old selection box with it), so
	# drop any stale selection refs before re-harvesting the pickable index.
	_c._viewport._reset_selection_state()
	_c._pickable = []
	_c._place_item_id = 0
	_c._placer = null
	if not _c.terrain_editor.has_method("get_terrain_world_root"):
		return
	var world_root: Node3D = _c.terrain_editor.get_terrain_world_root()
	if world_root == null:
		return
	_c._placer = _c.MissionObjectPlacer.new(resource_root)
	_c._placer.edit_mode = true
	var options: Dictionary = {}
	var env_node = _c._environment_node()
	if env_node != null:
		options["environment_node"] = env_node
	if timeline != null:
		options["timeline"] = timeline
	_c._stats = _c._placer.place(mission, world_root, options)
	_c._pickable = _c._placer.pickable_records
	# The placer created the pick colliders with the world; refresh the debug overlay if on.
	_c._viewport._refresh_pick_debug()


func _clear_objects() -> void:
	var container = _c._objects_container()
	if container != null and container.get_parent() != null:
		container.get_parent().remove_child(container)
		container.queue_free()


func _describe_load(mission: NovaMissionData, bms_path: String, env_note: String = "") -> String:
	var mission_name := mission.get_mission_name().strip_edges()
	if mission_name.is_empty():
		mission_name = bms_path.get_file()
	var placed := int(_c._stats.get("placed", 0))
	var unresolved := int(_c._stats.get("unresolved", 0))
	var text := "Loaded %s: %d objects placed" % [mission_name, placed]
	if unresolved > 0:
		text += ", %d unresolved" % unresolved
	if not env_note.is_empty():
		text += " (%s)" % env_note
	return text + "."
