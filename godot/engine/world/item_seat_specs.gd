class_name ItemSeatSpecs
extends RefCounted

# Builds the per-item SEAT SPECS the runtime feeds NovaSimulation.set_item_seat_specs
# (the libs-side ItemSeatSpec records): the seat/armory/emplacement userpoint walk over
# each mission item's .3di model + items.def row — seat typing by the witnessed name
# prefixes, retail slot layout, yaw-zero local frames, and the phrase_set mount config.
# The tail (command_rule / predict_best_seat) mirrors the witnessed attach-command seat
# selection for the MCP mission tools and probes; the sim performs the real selection.
# (Formerly "MissionSeatDiagnostics" — a misnomer: this is production extraction.)

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

# Local aliases of the binding's single-source codes — NovaSimulation /
# NovaMissionData pin these to libs/world + libs/mission by static_assert, so
# a native change can never silently drift past this file's seat walks.
const SEAT_NONE := NovaSimulation.SEAT_NONE
const SEAT_PASSENGER := NovaSimulation.SEAT_PASSENGER
const SEAT_CONTROLLER := NovaSimulation.SEAT_CONTROLLER
const SEAT_GUNNER := NovaSimulation.SEAT_GUNNER
const SEAT_DRIVER := NovaSimulation.SEAT_DRIVER

const COMMAND_PASSENGER_ONLY := NovaSimulation.MOUNT_COMMAND_PASSENGER_ONLY
const COMMAND_SKIP_CONTROLLER := NovaSimulation.MOUNT_COMMAND_SKIP_CONTROLLER
const COMMAND_ANY_SEAT := NovaSimulation.MOUNT_COMMAND_ANY_SEAT
const ITEM_ID_OFFSET := NovaMissionData.ITEM_ID_OFFSET


static func model_name_for_graphic(graphic: String) -> String:
	var basename := graphic.get_file().get_basename()
	return "" if basename.is_empty() else basename + ".3di"


static func build_item_seat_specs(mission, resource_root, item_db, include_raw := false) -> Array:
	var specs: Array = []
	if mission == null or resource_root == null or item_db == null:
		return specs
	var seen_types: Dictionary = {}
	var pending: Array = []
	for raw in mission.get_all_entities():
		var entity: Dictionary = raw
		var item_id := int(entity.get("item_id", 0))
		var type_id := int(entity.get("type_id", 0))
		if item_id == 0 or type_id == 0:
			continue
		pending.append({"item_id": item_id, "type_id": type_id})
	while not pending.is_empty():
		var next: Dictionary = pending.pop_front()
		var item_id := int(next.get("item_id", 0))
		var type_id := int(next.get("type_id", 0))
		if item_id == 0 or type_id == 0 or seen_types.has(type_id):
			continue
		seen_types[type_id] = true
		var resolved := seat_specs_for_item(resource_root, item_db, item_id, type_id, include_raw)
		var has_runtime_metadata := (
				not (resolved.get("seats", []) as Array).is_empty()
				or not (resolved.get("armory_points", []) as Array).is_empty()
				or not (resolved.get("emplacement_attachments", []) as Array).is_empty()
				or not String(resolved.get("primary_weapon", "")).is_empty()
				or bool(resolved.get("mount_config_valid", false)))
		if has_runtime_metadata:
			specs.append(resolved)
		for raw_attachment in resolved.get("emplacement_attachments", []):
			var attachment: Dictionary = raw_attachment
			var child_item_id := int(attachment.get("item_id", 0))
			var child_type_id := child_item_id - ITEM_ID_OFFSET
			if child_type_id > 0 and not seen_types.has(child_type_id):
				pending.append({"item_id": child_item_id, "type_id": child_type_id})
	return specs


static func seat_specs_for_item(resource_root, item_db, item_id: int, type_id := 0, include_raw := false) -> Dictionary:
	var out := {
		"item_id": item_id,
		"type_id": type_id,
		"display_name": "",
		"graphic": "",
		"model": "",
		"model_data": null,
		"seats": [],
		"armory_points": [],
		"emplacement_attachments": [],
		"primary_weapon": "",
		"mount_config_valid": false,
		"mount_config": 0,
		"error": "",
	}
	if resource_root == null or item_db == null:
		out["error"] = "missing_resource_root_or_item_db"
		return out
	if item_db.has_method("has_item") and not item_db.has_item(item_id):
		out["error"] = "item_not_found"
		return out
	# Mounted gunner overlay selection reads the TARGET item definition's
	# phrase_set dword at +0x86c. Carry presence independently because zero is a
	# witnessed retail configuration.
	# [orig: ItemDef_ParseProperty @0x49f9db..0x49fa0a; consumer @0x4b1884]
	if item_db.has_method("get_mount_config"):
		var mount_config: Dictionary = item_db.get_mount_config(item_id)
		out["mount_config_valid"] = bool(mount_config.get("valid", false))
		out["mount_config"] = (
			int(mount_config.get("value", 0))
			if bool(out["mount_config_valid"])
			else 0
		)
	out["display_name"] = String(item_db.get_display_name(item_id)) if item_db.has_method("get_display_name") else ""
	if item_db.has_method("get_primary_weapon"):
		out["primary_weapon"] = String(item_db.get_primary_weapon(item_id))
	var authored_attachments: Array = []
	if item_db.has_method("get_emplacement_attachments"):
		authored_attachments = item_db.get_emplacement_attachments(item_id)
	out["emplacement_attachments"] = emplacement_specs_from_model(
			null, authored_attachments, include_raw)
	var graphic := String(item_db.get_graphic(item_id))
	out["graphic"] = graphic
	if graphic.is_empty():
		out["error"] = "missing_graphic"
		return out
	var model_name := model_name_for_graphic(graphic)
	out["model"] = model_name
	if model_name.is_empty():
		out["error"] = "missing_model_name"
		return out
	var data := NovaObjectData.new()
	if data.open_from_resource_root(resource_root, model_name) != OK:
		out["error"] = "model_not_found"
		return out
	out["model_data"] = data
	out["seats"] = seat_specs_from_model(data, include_raw)
	out["emplacement_attachments"] = emplacement_specs_from_model(
			data, authored_attachments, include_raw)
	# "armory*" userpoints label/scan only on Armory-attrib items — the same attrib
	# gate the original applies before its userpoint walk
	# [orig: itemDef->attrib & 0x80000 @0x4361ee/@0x5a36f5; walk @0x436226/@0x5a372b].
	if item_db.has_method("get_attrib") and (int(item_db.get_attrib(item_id)) & NovaItemDatabase.ATTRIB_ARMORY) != 0:
		out["armory_points"] = armory_points_from_model(data)
	return out


static func emplacement_specs_from_model(
		data: NovaObjectData, authored: Array, include_raw := false) -> Array:
	var attachments: Array = []
	for raw in authored:
		var spec: Dictionary = (raw as Dictionary).duplicate(true)
		spec["anchor_found"] = false
		spec["bone_index"] = 0
		spec["subobject"] = -1
		spec["source_name"] = String(spec.get("userpoint", ""))
		spec["position"] = Vector3.ZERO
		spec["local"] = Vector3.ZERO
		spec["yaw_offset"] = 0
		if data != null:
			var wanted := String(spec.get("userpoint", "")).strip_edges()
			for i in range(data.get_user_point_count()):
				var up: Dictionary = data.get_user_point_info(i)
				if String(up.get("name", "")).strip_edges().nocasecmp_to(wanted) != 0:
					continue
				var position := seat_local_from_user_point_position(
						up.get("position", Vector3.ZERO))
				spec["anchor_found"] = true
				spec["bone_index"] = i + 1
				spec["subobject"] = int(up.get("subobject", -1))
				spec["source_name"] = String(up.get("name", ""))
				spec["position"] = position
				spec["local"] = position
				spec["yaw_offset"] = seat_yaw_offset_from_user_point_rotation(
						up.get("rotation", Vector3.ZERO))
				if include_raw:
					spec["raw_position"] = up.get("position", Vector3.ZERO)
					spec["raw_rotation"] = up.get("rotation", Vector3.ZERO)
				break
		attachments.append(spec)
	return attachments


static func seat_specs_from_model(data: NovaObjectData, include_raw := false) -> Array:
	var seats: Array = []
	if data == null:
		return seats
	var passenger_slot := 0
	for i in range(data.get_user_point_count()):
		var up: Dictionary = data.get_user_point_info(i)
		var source_name := String(up.get("name", ""))
		var canonical := canonical_seat_name(source_name)
		var seat_type := seat_type_for_user_point(source_name)
		if seat_type == SEAT_NONE:
			continue
		# Retail keeps the gameplay/userpoint list and the ten occupant handles in
		# different layouts: sitex rows fill 0..7, ctrlx/drvrx share 8, UseGun is 9.
		# Carry the fixed slot beside the dense presentation/gameplay record.
		# [orig: ItemDef seatBoneIndex/controlBone/useGunBone +0x25D..+0x266]
		var retail_slot := -1
		match seat_type:
			SEAT_PASSENGER:
				if passenger_slot < 8:
					retail_slot = passenger_slot
				passenger_slot += 1
			SEAT_CONTROLLER, SEAT_DRIVER:
				retail_slot = 8
			SEAT_GUNNER:
				retail_slot = 9
		var position: Vector3 = seat_local_from_user_point_position(up.get("position", Vector3.ZERO))
		var seat := {
			"type": seat_type,
			"type_label": seat_type_label(seat_type),
			"retail_slot": retail_slot,
			"position": position,
			"local": position,
			"bone_index": i + 1,
			"pose_index": seat_pose_index_for_user_point(source_name),
			"source_name": source_name,
			"canonical_name": canonical,
			"yaw_offset": seat_yaw_offset_from_user_point_rotation(up.get("rotation", Vector3.ZERO)),
		}
		if include_raw:
			seat["raw_position"] = up.get("position", Vector3.ZERO)
			seat["raw_rotation"] = up.get("rotation", Vector3.ZERO)
		seats.append(seat)
	return seats


static func seat_local_from_user_point_position(pos: Vector3) -> Vector3:
	return MissionObjectPlacer.godot_to_bms_position(
			MissionObjectPlacer.bms_to_godot_basis(Vector3.ZERO) * pos)


# The "armory*" userpoint locals (prefix match, first 6 chars case-insensitive) —
# the floating armory-label anchors on Armory-attrib items.
# [orig: strnicmp(name, "armory", 6) @0x436226/@0x5a372b]
static func armory_points_from_model(data: NovaObjectData) -> Array:
	var points: Array = []
	if data == null:
		return points
	for i in range(data.get_user_point_count()):
		var up: Dictionary = data.get_user_point_info(i)
		var source_name := String(up.get("name", "")).strip_edges().to_lower()
		if not source_name.begins_with("armory"):
			continue
		points.append(seat_local_from_user_point_position(up.get("position", Vector3.ZERO)))
	return points


static func seat_yaw_offset_from_user_point_rotation(direction: Vector3) -> int:
	if direction.length_squared() < 0.000001:
		return 0
	var local := MissionObjectPlacer.godot_to_bms_position(
			MissionObjectPlacer.bms_to_godot_basis(Vector3.ZERO) * direction.normalized())
	if absf(local.x) < 0.000001 and absf(local.y) < 0.000001:
		return 0
	return int(round(rad_to_deg(atan2(local.x, local.y))))


static func seat_type_for_user_point(name: String) -> int:
	var canonical := canonical_seat_name(name)
	if canonical.begins_with("sitex"):
		return SEAT_PASSENGER
	if canonical.begins_with("ctrlx"):
		return SEAT_CONTROLLER
	if canonical.begins_with("usegun"):
		return SEAT_GUNNER
	if canonical.begins_with("drvrx"):
		return SEAT_DRIVER
	return SEAT_NONE


static func seat_pose_index_for_user_point(name: String) -> int:
	var canonical := canonical_seat_name(name)
	if not (canonical.begins_with("sitex") or canonical.begins_with("ctrlx") or canonical.begins_with("drvrx")):
		return 0
	var digits := ""
	var suffix := canonical.substr(5)
	for i in range(suffix.length()):
		var c := suffix.unicode_at(i)
		if c < 48 or c > 57:
			break
		digits += suffix.substr(i, 1)
	if digits.is_empty():
		return 0
	return clampi(int(digits), 0, 30)


static func canonical_seat_name(name: String) -> String:
	# Entity_GetBoneSlotType performs a case-insensitive comparison at byte zero
	# of the model's USRP row name. Whitespace trimming is reimpl-side string hygiene;
	# embedded tokens are not seats.
	# [orig: strnicmp(name, "sitex"/"ctrlx"/"UseGun"/"drvrx", 5/6) @0x434ED0]
	return name.to_lower().strip_edges()


static func seat_type_label(seat_type: int) -> String:
	match seat_type:
		SEAT_PASSENGER:
			return "passenger"
		SEAT_CONTROLLER:
			return "controller"
		SEAT_GUNNER:
			return "gunner"
		SEAT_DRIVER:
			return "driver"
	return "none"


static func command_rule(command_id: int) -> Dictionary:
	match command_id:
		COMMAND_PASSENGER_ONLY:
			return {
				"id": command_id,
				"mode": "passenger_only",
				"description": "command 123 accepts sitex/passenger seats only",
			}
		COMMAND_SKIP_CONTROLLER:
			return {
				"id": command_id,
				"mode": "no_controller",
				"description": "command 124 rejects ctrlx/controller seats",
			}
		COMMAND_ANY_SEAT:
			return {
				"id": command_id,
				"mode": "any_seat",
				"description": "command 125 accepts passenger, controller, driver, and gunner seats",
			}
	return {
		"id": command_id,
		"mode": "not_mount_command",
		"description": "not an attach-to-seat command",
	}


static func command_allows_seat(command_id: int, seat_type: int) -> bool:
	if seat_type == SEAT_NONE:
		return false
	match command_id:
		COMMAND_PASSENGER_ONLY:
			return seat_type == SEAT_PASSENGER
		COMMAND_SKIP_CONTROLLER:
			return seat_type != SEAT_CONTROLLER
		COMMAND_ANY_SEAT:
			return true
	return false


static func seat_priority_weight(seat: Dictionary) -> int:
	match int(seat.get("type", SEAT_NONE)):
		SEAT_CONTROLLER, SEAT_DRIVER:
			return 0x2000
		SEAT_GUNNER:
			return 0x20000
		SEAT_PASSENGER:
			return 0x200000
	return 0x7fffffff


static func predict_best_seat(seats: Array, command_id: int) -> Dictionary:
	var candidates: Array = []
	var best_index := -1
	var best_weight := 0x7fffffff
	for i in range(seats.size()):
		var seat: Dictionary = seats[i]
		var seat_type := int(seat.get("type", SEAT_NONE))
		var occupied := bool(seat.get("occupied", false))
		var allowed := command_allows_seat(command_id, seat_type)
		var weight := seat_priority_weight(seat)
		var status := "eligible"
		var reason := ""
		if occupied:
			status = "skipped"
			reason = "occupied"
		elif not allowed:
			status = "skipped"
			reason = "command_filter"
		elif weight < best_weight:
			best_weight = weight
			best_index = i
		var candidate := seat.duplicate(true)
		candidate["index"] = i
		candidate["type_label"] = seat_type_label(seat_type)
		candidate["eligible"] = allowed and not occupied
		candidate["weight"] = weight
		candidate["status"] = status
		candidate["skip_reason"] = reason
		candidates.append(candidate)
	if best_index >= 0:
		var selected: Dictionary = candidates[best_index]
		selected["status"] = "selected"
		candidates[best_index] = selected
		return {
			"command": command_rule(command_id),
			"seat_index": best_index,
			"seat": selected,
			"candidates": candidates,
		}
	return {
		"command": command_rule(command_id),
		"seat_index": -1,
		"seat": {},
		"candidates": candidates,
	}
