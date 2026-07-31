extends "res://modtools/mission/controller/controller_section.gd"

# Selection accessors (display names / summary / user points / focus / tile
# info), the per-entity + mission-header + loadout/group property setters,
# and the entity/object enumeration lists (object browser, scripting pickers).
# Moved verbatim from mission_controller.gd (F5); state stays on the
# controller, reached through `_c`.

# The items.def display name for an entity, or "" when it can't be resolved (no item
# database, unknown id, or blank name). Drives the inspector identity line and the
# delete / place status messages so the user reads a model name, not just an index.
func entity_display_name(kind: int, index: int) -> String:
	var entity = _c._viewport._find_entity(kind, index)
	if entity.is_empty():
		return ""
	var db = _c._placement._item_db()
	if db == null:
		return ""
	var item_id := int(entity.get("item_id", 0))
	if not db.has_item(item_id):
		return ""
	return db.get_display_name(item_id).strip_edges()


# The resolved model name of the current selection, or "" when nothing is selected /
# unresolvable. The inspector pairs this with the kind + index for the identity line.
func get_selected_display_name() -> String:
	if _c._selected_ref.is_empty():
		return ""
	return entity_display_name(int(_c._selected_ref["kind"]), int(_c._selected_ref["index"]))


# The items.def graphic basename for the current selection, or "" when nothing is
# selected / no item database is loaded / the item has no declared graphic.
func get_selected_graphic_name() -> String:
	if _c._selected_ref.is_empty():
		return ""
	var entity = _c._viewport._find_entity(int(_c._selected_ref["kind"]), int(_c._selected_ref["index"]))
	if entity.is_empty():
		return ""
	var db = _c._placement._item_db()
	if db == null:
		return ""
	var item_id := int(entity.get("item_id", 0))
	if not db.has_item(item_id):
		return ""
	return db.get_graphic(item_id).strip_edges()


# { kind, index, position (mission-space Vector3), animated } for the selected
# entity, or empty when nothing is selected. Drives the inspector's selection line.
func get_selection_summary() -> Dictionary:
	if _c._selected_ref.is_empty():
		return {}
	return {
		"kind": int(_c._selected_ref["kind"]),
		"index": int(_c._selected_ref["index"]),
		"position": _c.MissionObjectPlacer.godot_to_bms_position(_c._selected_xform.origin),
		"animated": _c._selected_node != null,
	}


func selected_has_user_points() -> bool:
	var data := _selected_object_data()
	return data != null \
		and data.has_method("get_user_point_count") \
		and data.get_user_point_count() > 0


func is_selected_user_points_visible() -> bool:
	return _c._selected_user_points_visible and selected_has_user_points()


func set_selected_user_points_visible(value: bool) -> void:
	_c._selected_user_points_visible = value and selected_has_user_points()
	_c._viewport._refresh_selected_user_points_overlay()
	_c._notify_changed()


func _selected_object_data() -> NovaObjectData:
	if _c._selected_ref.is_empty() or _c._selected_graphic.is_empty() or _c._placer == null:
		return null
	if int(_c._selected_ref.get("kind", -1)) == NovaMissionData.KIND_MARKER:
		return null
	if not _c._placer.has_method("object_data_for"):
		return null
	return _c._placer.object_data_for(_c._selected_graphic)


# Select an object from the inspector's "Placed objects" browser by kind + array index,
# then frame the editor camera on it so it is found in the viewport. This is the whole
# point of the list: on a large map a named unit can be located without hunting the world.
# Public (the viewport pick path uses the private _select); a missing entity is a no-op.
func select_object(kind: int, index: int) -> void:
	if _c._mission == null or _c._viewport._find_entity(kind, index).is_empty():
		return
	_c._viewport._select(kind, index)
	focus_selection_in_view()


# Orbit the editor camera onto the current selection's world AABB (falling back to its
# authored origin when the selection has no baked mesh). Keeps the current heading so the
# view does not spin. Returns false with no camera / nothing selected (e.g. headless tests).
func focus_selection_in_view() -> bool:
	if _c._selected_ref.is_empty() or _c.terrain_editor == null or not _c.terrain_editor.has_method("get_editor_camera"):
		return false
	var camera: Camera3D = _c.terrain_editor.get_editor_camera()
	if camera == null or not camera.has_method("frame_bounds_custom"):
		return false
	var aabb = _c._viewport._selected_world_aabb()
	var center: Vector3
	var radius: float
	if aabb.size != Vector3.ZERO:
		center = aabb.position + aabb.size * 0.5
		radius = maxf(aabb.size.length() * 0.5, 8.0)
	else:
		# Mesh-less / not-yet-baked: frame the authored origin, converted to world space.
		var container: Node3D = _c._objects_container()
		center = (container.global_transform * _c._selected_xform.origin) if container != null else _c._selected_xform.origin
		radius = 16.0
	camera.frame_bounds_custom(center, radius, 2.5, 1200.0, camera.rotation.y, -0.55)
	return true


func get_mission_title() -> String:
	if _c._mission == null:
		return "Mission"
	var mission_name: String = _c._mission.get_mission_name().strip_edges()
	if mission_name.is_empty():
		mission_name = _c._current_path.get_file().get_basename()
	if mission_name.is_empty():
		mission_name = "untitled"
	return "%s%s" % [mission_name, "*" if _c.is_dirty() else ""]


## The mounted resource root, via the bound terrain editor — the controller's one
## VFS seam. The controller runs headless in tests (no shell), so this reads the
## editor, not the workspace; the duck-type guard for bare doubles lives here.
func _resource_root() -> NovaResourceRoot:
	if _c.terrain_editor != null and _c.terrain_editor.has_method("get_resource_root"):
		return _c.terrain_editor.get_resource_root()
	return null


# Retail loads <mission>.til into one shared terrain tile array used by both
# terrain composition and foliage's radius-2 blocker. Keep the basename and
# VFS lookup identical to GameWorld._load_mission_tile_info.
# [orig: Terrain_LoadFoliageFile @ 0x60a740;
# Foliage_PathBlockedByPlacedTile @ 0x606490]
func _load_mission_tile_info(bms_name: String, resource_root: NovaResourceRoot) -> void:
	_clear_mission_tile_info()
	if resource_root == null:
		return
	var mission_name := bms_name.get_file()
	if mission_name.is_empty():
		mission_name = bms_name
	var til_name := mission_name.get_basename() + ".til"
	if not resource_root.has_file(til_name):
		return
	var til_bytes := resource_root.read_file(til_name)
	if til_bytes.is_empty():
		return
	var tile_info := NovaTerrainTileInfo.new()
	if tile_info.load_from_bytes(til_bytes) != OK:
		push_warning("MissionController: failed to parse mission tile file '%s'." % til_name)
		return
	_c._mission_tile_info = tile_info


func _clear_mission_tile_info() -> void:
	_c._mission_tile_info = null


# The full editable dictionary for the selected entity (see NovaMissionData entity
# fields: position is mission-space, rotation_deg is authored degrees, plus team /
# group), or {} when nothing is selected.
func get_selected_entity() -> Dictionary:
	if _c._selected_ref.is_empty():
		return {}
	return _c._viewport._find_entity(int(_c._selected_ref["kind"]), int(_c._selected_ref["index"]))


# The live selected position in mission (BMS) space, tracking any uncommitted drag.
func get_selected_position() -> Vector3:
	if _c._selected_ref.is_empty():
		return Vector3.ZERO
	return _c.MissionObjectPlacer.godot_to_bms_position(_c._selected_xform.origin)


# The live selected rotation as authored (pitch, yaw, roll) degrees.
func get_selected_rotation() -> Vector3:
	if _c._selected_ref.is_empty():
		return Vector3.ZERO
	return _c._selected_rotation_deg


func set_selected_position(bms_pos: Vector3) -> void:
	if _c._selected_ref.is_empty() or _c._mission == null:
		return
	# Open (or continue) one edit session so a run of axis edits on this entity coalesces
	# into a single undo step; it is pushed by the next action's flush. begin_edit is inert
	# if a session is already open, so X / Y / Z / pitch / yaw / roll share one step.
	_c.begin_edit()
	# entity_transform places objects at bms_to_godot_position(pos) in container-local
	# space (the drag path and get_selected_position both invert exactly that), so set
	# the local origin directly. Routing through the container's world transform would
	# double-apply it and shift the object whenever the container is not at the origin.
	_c._viewport._apply_selected_xform(Transform3D(_c._selected_xform.basis, _c.MissionObjectPlacer.bms_to_godot_position(bms_pos)))
	_c._viewport._commit_selected_transform()


func set_selected_rotation(rot_deg: Vector3) -> void:
	if _c._selected_ref.is_empty() or _c._mission == null:
		return
	_c.begin_edit()
	# Unlike a drag (position only), this rebuilds the basis from the authored degrees
	# and re-applies the full transform so the in-world object actually rotates. Round
	# to whole degrees first: the format (and set_entity_transform) stores integer
	# degrees, so keeping a fractional value would leave get_selected_rotation out of
	# step with the persisted record on the next axis edit.
	_c._selected_rotation_deg = rot_deg.round()
	var basis: Basis = _c.MissionObjectPlacer.bms_to_godot_basis(_c._selected_rotation_deg)
	_c._viewport._apply_selected_xform(Transform3D(basis, _c._selected_xform.origin))
	_c._viewport._commit_selected_transform()


func set_selected_team(value: int) -> void:
	# team / group are stored as uint8 by the format; clamp at this API boundary so an
	# out-of-range value cannot silently wrap (the SpinBoxes already cap 0..255, but
	# these methods are public). Surface the clamp so a corrected value is not a surprise.
	var clamped := clampi(value, 0, 255)
	if clamped != value:
		_c._report("Team clamped to the 0 to 255 range.")
	set_selected_property("team", clamped)


func set_selected_group(value: int) -> void:
	var clamped := clampi(value, 0, 255)
	if clamped != value:
		_c._report("Group clamped to the 0 to 255 range.")
	set_selected_property("group", clamped)


# Generic per-entity scalar property edit from the inspector: team / group plus the
# AI + waypoint fields the format carries (waypoint_id, wp_number, perception, accuracy,
# alert_state, the engagement / attack distances, spawn_count, max_simultaneous,
# ai_flags). `property` is the entity-dictionary key it edits. A property change is its
# own undo step: close any open transform session first, then bracket the write with
# begin_edit/commit_edit so it records one step only if the write actually changed the document
# (a same-value write is a no-op). The value range is governed by the inspector's SpinBoxes and the format's field
# widths, so this does not clamp; team / group clamp through their wrappers above.
func set_selected_property(property: String, value: int) -> void:
	if _c._selected_ref.is_empty():
		return
	# set_entity_property_int returns false only on rejection (bad index, unknown property, failed
	# write) -- never on a benign same-value write -- so a false return is a real error worth surfacing.
	_c._edit_step(func(): return _c._mission.set_entity_property_int(
			int(_c._selected_ref["kind"]), int(_c._selected_ref["index"]), property, value),
		"Could not set %s on the selected object." % property)
	# A group change moves which groups are "in use" (and which "New group N" the picker offers), so the
	# cached group dropdown must rebuild. Entity-set changes are covered by _rebake_objects; other
	# per-entity fields (waypoint_id, team, AI) don't affect any cached option list, so don't bump here.
	if property == "group":
		_c._membership_rev += 1


# Revision of the entity set + group membership; see _membership_rev. The inspector gates its
# (otherwise per-`changed`, ~1600-entity) rebuild of the group / waypoint-path / entity pickers on this.
func get_membership_revision() -> int:
	return _c._membership_rev


# String counterpart of set_selected_property, for the fixed-string entity fields
# "name1" (AI class) and "name2" (AI script). Same snapshot / one-undo-step model.
func set_selected_string_property(property: String, value: String) -> void:
	if _c._selected_ref.is_empty():
		return
	_c._edit_step(func(): return _c._mission.set_entity_property_string(
			int(_c._selected_ref["kind"]), int(_c._selected_ref["index"]), property, value),
		"Could not set %s on the selected object." % property)


# --- Authoring: mission-header editing ----------------------------------------
# Each setter snapshots, writes one header field through NovaMissionData, then pushes a
# single undo step. Field names match NovaMissionData::set_header_* and the inspector form.
func set_header_string(field: String, value: String) -> void:
	_c._edit_step(func(): return _c._mission.set_header_string(field, value),
		"Could not set mission %s." % field)


func set_header_int(field: String, value: int) -> void:
	_c._edit_step(func(): return _c._mission.set_header_int(field, value),
		"Could not set mission %s." % field)


func set_header_flag(bit: int, on: bool) -> void:
	_c._edit_step(func(): return _c._mission.set_header_flag(bit, on),
		"Could not set mission flag.")


# Single-select game mode (one attrib_flags mode bit, or 0 = Single Player). Mirrors set_header_*:
# one undo step + dirty. NovaMissionData.set_game_mode clears the other mode bits.
func set_game_mode(bit: int) -> void:
	_c._edit_step(func(): return _c._mission.set_game_mode(bit),
		"Could not set the game mode.")


# --- Weapon loadout + groups (mission-global) ---------------------------------
# Loadout entries are dictionaries { index, name, ammo_primary, ammo_secondary, flags }; groups are
# { index, field0(flags), field8(value), field12(constant 10) }. Edits use the one-step snapshot/undo recipe.

func get_weapon_loadout() -> Array:
	if _c._mission == null:
		return []
	return _c._mission.get_weapon_loadout()


func set_weapon_loadout(entries: Array) -> void:
	_c._edit_step(func(): return _c._mission.set_weapon_loadout(entries),
		"Could not update the weapon loadout.")


func get_group_count() -> int:
	if _c._mission == null:
		return 0
	return _c._mission.get_group_count()


func get_groups() -> Array:
	if _c._mission == null:
		return []
	return _c._mission.get_groups()


func get_group(index: int) -> Dictionary:
	if _c._mission == null:
		return {}
	return _c._mission.get_group(index)


func set_group(index: int, field0: int, field8: int, field12: int) -> void:
	_c._edit_step(func(): return _c._mission.set_group(index, field0, field8, field12),
		"Could not update group %d." % index)


# Flat list of every entity (all kinds), shaped for a scripting param picker: { value: bms_id, label }.
# Single/Player triggers and Single actions reference a unit by its BMS/net id, not an array index
# ([orig: EntityPool_FindByNetId @0x4f0a20]); an unmatched value still round-trips as a raw row.
func get_all_entities() -> Array:
	if _c._mission == null:
		return []
	var out: Array = []
	var id_counts: Dictionary = {}
	for kind in [NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC]:
		for e in _c._mission.get_entities(kind):
			var ed := e as Dictionary
			var bms_id := int(ed.get("bms_id", 0))
			var display := entity_display_name(kind, int(ed.get("index", 0)))
			var label := ("%s #%d" % [display, bms_id]) if display != "" else ("Unit #%d" % bms_id)
			out.append({ "value": bms_id, "label": label })
			id_counts[bms_id] = int(id_counts.get(bms_id, 0)) + 1
	# bms_id is not guaranteed unique on disk (many records default to 0), and the picker keys its
	# OptionButton items by value, so rows that share an id would be visually indistinguishable. Append a
	# 1-based ordinal to each member of a colliding id so the user can tell them apart. The committed
	# value stays the bms_id -- the engine resolves units by that id (FindByNetId), so same-id rows are
	# genuinely equivalent on disk; this only disambiguates the display.
	var seen: Dictionary = {}
	for row in out:
		var v := int(row["value"])
		if int(id_counts.get(v, 0)) > 1:
			var n := int(seen.get(v, 0)) + 1
			seen[v] = n
			row["label"] = "%s  (%d)" % [String(row["label"]), n]
	return out


# Human kind label for a "Placed objects" browser row. The list covers every entity kind the
# Objects mode renders + picks, markers included (they are placed / edited as general entities
# there, gizmo-picked via the always-on marker overlay; Waypoints mode is just a second view of them).
func _object_kind_label(kind: int) -> String:
	match kind:
		NovaMissionData.KIND_BUILDING:
			return "Building"
		NovaMissionData.KIND_ORGANIC:
			return "Person"
		NovaMissionData.KIND_MARKER:
			return "Marker"
		_:
			return "Item"


# Flat, ordered list of every placed object (items / buildings / people / markers) for the
# inspector's left-pane browser. One row per entity: { kind, index, item_id, name, category }.
# `name` is the resolved model name (or "" -> the inspector falls back to the item id); the row
# order is the on-disk array order, stable across edits, so duplicate-name ordinals stay put.
func get_object_list() -> Array:
	var out: Array = []
	if _c._mission == null:
		return out
	for kind in [NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC, NovaMissionData.KIND_MARKER]:
		var category := _object_kind_label(kind)
		for e in _c._mission.get_entities(kind):
			var ed := e as Dictionary
			var index := int(ed.get("index", 0))
			out.append({
				"kind": kind,
				"index": index,
				"item_id": int(ed.get("item_id", 0)),
				"name": entity_display_name(kind, index),
				"category": category,
			})
	return out


# Number of placed objects across every Objects-mode kind (markers included). Cheap (count fields,
# no record walk); the inspector gates its (potentially 1000+ row) list rebuild on this changing.
func get_object_count() -> int:
	if _c._mission == null:
		return 0
	return _c._mission.get_entity_count(NovaMissionData.KIND_ITEM) \
		+ _c._mission.get_entity_count(NovaMissionData.KIND_BUILDING) \
		+ _c._mission.get_entity_count(NovaMissionData.KIND_ORGANIC) \
		+ _c._mission.get_entity_count(NovaMissionData.KIND_MARKER)


# Whether item names are resolvable yet. A mission can open before its items.def is reachable
# (the resource directory is repointed afterwards); the inspector rebuilds its row labels once
# this flips true so the browser does not stay stuck on "Item <id>" placeholders.
func has_item_database() -> bool:
	return _c._placement._item_db() != null


# Options for the inspector's "Waypoint path" picker -- which path a unit follows (the waypoint_id /
# byte-79 field, [orig: Entity_SpawnFromBMSRecord @0x40f02f `if (record[79]) follow path record[79]`]).
# Shaped { id, label } for InspectorForms.populate_id_option. id 0 = "None": byte 79 == 0 means the
# unit follows no path, so path index 0 is unreachable as a follow target and is not offered. The
# inspector adds the unit's current value if it is not in this set, so an odd value still round-trips.
func get_waypoint_path_options() -> Array:
	var out: Array = [{ "id": 0, "label": "None" }]
	if _c._mission == null:
		return out
	for s in _c._mission.get_waypoint_summaries():
		var d := s as Dictionary
		var idx := int(d.get("index", 0))
		var count := int(d.get("marker_count", 0))
		if idx <= 0 or count <= 0:
			continue
		out.append({ "id": idx, "label": "Path %d  -  %d markers" % [idx, count] })
	return out


# Options for the inspector's "Group" picker -- which squad a unit belongs to (the group_id / byte-78
# field, [orig: Entity_SpawnFromBMSRecord @0x40ebb7]; the format carries 64 groups, 0..63). Shaped
# { id, label }: "Ungrouped" (0), every group already in use (annotated with its unit count so the
# user joins an existing squad), and a "New group N" entry for the first free group so a fresh squad
# can be started. The inspector adds the unit's current group if it is not already listed.
func get_group_options() -> Array:
	var out: Array = [{ "id": 0, "label": "Ungrouped" }]
	if _c._mission == null:
		return out
	var counts: Dictionary = {}
	for kind in [NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC]:
		for e in _c._mission.get_entities(kind):
			var g := int((e as Dictionary).get("group", 0))
			if g > 0:
				counts[g] = int(counts.get(g, 0)) + 1
	var used: Array = counts.keys()
	used.sort()
	for g in used:
		out.append({ "id": int(g), "label": "Group %d  (%d units)" % [int(g), int(counts[g])] })
	for candidate in range(1, 64):
		if not counts.has(candidate):
			out.append({ "id": candidate, "label": "New group %d" % candidate })
			break
	return out
