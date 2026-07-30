class_name NovaDebugEntities
## UI-free entity discovery shared by F3 and runtime MCP.
##
## The rendered client snapshot is the primary list: unlike the AI pool it
## includes vehicles and, for a joiner, the actual decoded remote view. On an
## authority-owning host we join those rows to AI cards by wire handle/SSN so
## edit actions receive the real AI-pool index. Host-only AI entries that are
## not currently presented are appended as diagnostics; joiners never mix
## their non-authoritative tooling pool into the decoded view.


static func list(sim: Object) -> Array[Dictionary]:
	var output: Array[Dictionary] = []
	if sim == null or not is_instance_valid(sim):
		return output

	var joiner := sim.has_method("is_joiner") and bool(sim.is_joiner())
	var ai_cards: Array[Dictionary] = []
	var ai_by_wire := {}
	var ai_by_net := {}
	if sim.has_method("get_entity_count") and sim.has_method("get_entity_debug"):
		for ai_index in range(maxi(0, int(sim.get_entity_count()))):
			var card_value: Variant = sim.get_entity_debug(ai_index)
			var card: Dictionary = card_value if card_value is Dictionary else {}
			ai_cards.append(card)
			if card.is_empty():
				continue
			if card.has("wire_handle"):
				ai_by_wire[int(card.get("wire_handle", 0))] = ai_index
			var net_id := int(card.get("net_id", 0))
			if net_id > 0:
				ai_by_net[net_id] = ai_index

	var seen_ai := {}
	var snapshot := PackedFloat32Array()
	var stride := 0
	if sim.has_method("get_present_snapshot") \
			and sim.has_method("get_present_stride"):
		snapshot = sim.get_present_snapshot()
		stride = int(sim.get_present_stride())
	if stride > 0 and snapshot.size() % stride == 0:
		for view_index in range(snapshot.size() / stride):
			var base := view_index * stride
			var type_id := int(snapshot[base + NovaSimulation.PF_TYPE_ID])
			# Joiner self-filter rows are deliberately retained as zero-filled
			# records by the presenter; they are not rendered entities.
			if type_id == 0:
				continue
			var net_id := int(snapshot[base + NovaSimulation.PF_NET_ID])
			var wire_handle := int(
					snapshot[base + NovaSimulation.PF_WIRE_HANDLE])
			var ai_index := -1
			if not joiner:
				if ai_by_wire.has(wire_handle):
					ai_index = int(ai_by_wire[wire_handle])
				elif net_id > 0 and ai_by_net.has(net_id):
					ai_index = int(ai_by_net[net_id])
			var detail: Dictionary = ai_cards[ai_index].duplicate(true) \
					if ai_index >= 0 and ai_index < ai_cards.size() else {}
			if detail.is_empty() and not joiner and net_id > 0 \
					and sim.has_method("get_world_entity_debug"):
				var world_value: Variant = sim.get_world_entity_debug(net_id)
				if world_value is Dictionary:
					detail = world_value
			var position := Vector3(
					snapshot[base + NovaSimulation.PF_POS_X],
					snapshot[base + NovaSimulation.PF_POS_Y],
					snapshot[base + NovaSimulation.PF_POS_Z])
			var row := _make_row(detail, position, true, ai_index)
			var row_detail: Dictionary = row["detail"]
			row["view_index"] = view_index
			row["kind"] = int(snapshot[base + NovaSimulation.PF_KIND])
			row["source_index"] = int(
					snapshot[base + NovaSimulation.PF_INDEX])
			row["bms_id"] = int(snapshot[base + NovaSimulation.PF_BMS_ID])
			row["net_id"] = net_id
			row["type_id"] = type_id
			row["wire_handle"] = wire_handle
			row["alive"] = snapshot[
					base + NovaSimulation.PF_ALIVE] != 0.0
			row["hidden"] = snapshot[
					base + NovaSimulation.PF_HIDDEN] != 0.0
			row_detail["position"] = position
			row_detail["net_id"] = net_id
			row_detail["wire_handle"] = wire_handle
			row_detail["type_id"] = type_id
			row_detail["alive"] = row["alive"]
			row_detail["hidden"] = row["hidden"]
			if ai_index >= 0:
				seen_ai[ai_index] = true
			output.append(row)

	# A bare tooling sim has no client snapshot. On a host, append AI cards
	# missing from the current presentation too (despawned/culled diagnostics).
	if not joiner:
		for ai_index in range(ai_cards.size()):
			if seen_ai.has(ai_index) or ai_cards[ai_index].is_empty():
				continue
			var ai_position: Vector3 = ai_cards[ai_index].get(
					"position", Vector3.ZERO)
			output.append(_make_row(
					ai_cards[ai_index],
					ai_position,
					false,
					ai_index))

	for debug_index in range(output.size()):
		output[debug_index]["index"] = debug_index
	return output


static func _make_row(
		detail_source: Dictionary,
		position: Vector3,
		presented: bool,
		ai_index: int) -> Dictionary:
	var detail := detail_source.duplicate(true)
	var registry_present := not detail.has("pool") \
			or int(detail.get("pool", -1)) >= 0
	detail["position"] = position
	detail["ai_index"] = ai_index
	detail["presented"] = presented
	detail["registry_present"] = registry_present
	return {
		"index": -1,
		"view_index": -1,
		"ai_index": ai_index,
		"editable": ai_index >= 0 and registry_present,
		"presented": presented,
		"registry_present": registry_present,
		"kind": int(detail.get("kind", -1)),
		"source_index": int(detail.get("index", -1)),
		"bms_id": int(detail.get("bms_id", 0)),
		"net_id": int(detail.get("net_id", 0)),
		"type_id": int(detail.get("item_id", 0)),
		"wire_handle": int(detail.get("wire_handle", 0)),
		"name": String(detail.get("name", "")),
		"state": String(detail.get("state_name", "")),
		"health": int(detail.get("health", 0)),
		"team": int(detail.get("team", -1)),
		"alive": bool(detail.get("alive", false)),
		"hidden": bool(detail.get("hidden", false)),
		"world_position": position,
		"mission_position": Vector3(position.x, -position.z, position.y),
		"detail": detail,
	}
