class_name DebugOcclusionPage
extends NovaDebugPage
## The render-occlusion decision inspector. The summary names what happened to
## the frame, while the attention-ordered building list explains one decision
## at a time instead of exposing a raw mask/weld table. "Show portal faces"
## rides the NovaDebugOptions registry.

var _occ_status_label: Label
var _occ_list: ItemList
var _occ_detail: Label
var _selected_building_key := ""
var _has_selected_building := false
var _building_keys := PackedStringArray()
var _buildings_by_key: Dictionary = {}
var _welds: Array[Dictionary] = []


func page_id() -> StringName:
	return &"Occlusion"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_occ_status_label = Label.new()
	_occ_status_label.name = "OcclusionStatus"
	_occ_status_label.text = "No occlusion data."
	_occ_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_occ_status_label)

	_occ_list = ItemList.new()
	_occ_list.name = "OcclusionBuildings"
	_occ_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_occ_list.allow_reselect = true
	_occ_list.item_selected.connect(_on_building_selected)
	add_child(_occ_list)

	_occ_detail = Label.new()
	_occ_detail.name = "OcclusionBuildingDetail"
	_occ_detail.text = "Select a building to inspect its occlusion decision."
	_occ_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_occ_detail)

	add_option_check(&"show_portal_faces")


func refresh() -> void:
	var sim := _ctx.sim()
	if sim == null or not sim.has_method("get_occlusion_debug"):
		_clear_pane("No occlusion data.")
		return
	var occ: Dictionary = sim.get_occlusion_debug()
	if not bool(occ.get("active", false)):
		_clear_pane("No portal-carrying buildings in this mission.")
		return
	var counts: Dictionary = occ.get("counts", {})
	var buildings: Array[Dictionary] = []
	var drawn := 0
	var culled := 0
	var out_of_batch := 0
	var open_now := 0
	var identity_counts := {}
	for building_v in occ.get("buildings", []):
		var building: Dictionary = (building_v as Dictionary).duplicate(true)
		var key_base := _building_key_base(building)
		var key_ordinal := int(identity_counts.get(key_base, 0))
		identity_counts[key_base] = key_ordinal + 1
		building["_debug_key"] = (
				key_base
				if key_ordinal == 0
				else "%s:%d" % [key_base, key_ordinal])
		var state := _building_state(building)
		building["_state"] = state
		building["_open_now"] = bool(building.get("open_flagged", false))
		match state:
			"DRAWN":
				drawn += 1
			"CULLED":
				culled += 1
			"OUT OF BATCH":
				out_of_batch += 1
		if bool(building["_open_now"]):
			open_now += 1
		buildings.append(building)
	buildings.sort_custom(_sort_building_attention)
	_welds.clear()
	for weld_v in occ.get("welds", []):
		_welds.append((weld_v as Dictionary).duplicate(true))

	_occ_status_label.text = "\n".join(PackedStringArray([
		"Camera: %s   Exterior: %s   Water: %s   Blink: 0x%02X" % [
			"INDOORS" if bool(occ.get("camera_indoors", false)) else "OUTDOORS",
			"VISIBLE" if bool(occ.get("exterior_visible", false)) else "HIDDEN",
			"VISIBLE" if bool(occ.get("water_visible", false)) else "HIDDEN",
			int(occ.get("local_blink_flags", 0))],
		"Frame: %d drawn   %d culled   %d out of batch   %d open now   %d entities hidden" % [
			drawn, culled, out_of_batch, open_now,
			int(counts.get("culled_entities", 0))],
	]))
	_refresh_buildings(buildings)


func _refresh_buildings(buildings: Array[Dictionary]) -> void:
	var retained_key := _selected_building_key if _has_selected_building else ""
	_occ_list.clear()
	_building_keys.clear()
	_buildings_by_key.clear()
	var selected_index := -1
	for building in buildings:
		var bms_id := int(building.get("bms_id", 0))
		var pos: Vector3 = building.get("pos", Vector3.ZERO)
		var building_key := String(building.get(
				"_debug_key", _building_key_base(building)))
		var state := String(building.get("_state", "DRAWN"))
		var state_label := _building_state_label(building)
		var building_label := (
				"#%d" % bms_id
				if bms_id != 0
				else "@ %.1f, %.1f, %.1f" % [pos.x, pos.y, pos.z])
		var row := "%s  %s  |  %d records  |  sections %s" % [
			state_label, building_label, int(building.get("records", 0)),
			_visible_sections(int(building.get("mask", 0)), true)]
		var idx := _occ_list.add_item(row)
		_occ_list.set_item_metadata(idx, bms_id)
		_occ_list.set_item_custom_fg_color(
				idx, _state_color(state, bool(building.get("_open_now", false))))
		_building_keys.append(building_key)
		_buildings_by_key[building_key] = building
		if building_key == retained_key:
			selected_index = idx

	if buildings.is_empty():
		_selected_building_key = ""
		_has_selected_building = false
		_occ_detail.text = "No buildings were reported for this frame."
		return
	if selected_index < 0:
		selected_index = 0
	_selected_building_key = _building_keys[selected_index]
	_has_selected_building = true
	_occ_list.select(selected_index)
	_show_building_detail(_buildings_by_key.get(_selected_building_key, {}))


func _on_building_selected(index: int) -> void:
	if index < 0 or index >= _building_keys.size():
		return
	_selected_building_key = _building_keys[index]
	_has_selected_building = true
	_show_building_detail(_buildings_by_key.get(_selected_building_key, {}))


func _show_building_detail(building: Dictionary) -> void:
	if building.is_empty():
		_occ_detail.text = "Select a building to inspect its occlusion decision."
		return
	var bms_id := int(building.get("bms_id", 0))
	var state := String(building.get("_state", "DRAWN"))
	var state_label := _building_state_label(building)
	var pos: Vector3 = building.get("pos", Vector3.ZERO)
	var records := int(building.get("records", 0))
	var windows := int(building.get("windows", 0))
	var portals := int(building.get("portals", 0))
	var links := int(building.get("links", 0))
	var building_name := (
			"Building #%d" % bms_id
			if bms_id != 0
			else "Runtime building (no BMS id)")
	var lines := PackedStringArray([
		"%s - %s" % [building_name, state_label],
		_state_explanation(state),
		"Position: %.1f, %.1f, %.1f" % [pos.x, pos.y, pos.z],
		"Visible sections: %s (mask 0x%08X)" % [
			_visible_sections(int(building.get("mask", 0))),
			int(building.get("mask", 0)) & 0xFFFFFFFF],
		"Portal model: %d %s - %d %s, %d %s, %d %s" % [
			records, _plural(records, "record"),
			windows, _plural(windows, "window"),
			portals, _plural(portals, "portal"),
			links, _plural(links, "welded link")],
	])
	if bool(building.get("_open_now", false)):
		lines.insert(2, "Open state: the building's open path is active now.")
	if bms_id == 0:
		lines.append(
				"Welded connections: unavailable for a runtime building without a BMS id.")
		_occ_detail.text = "\n".join(lines)
		return
	var related_welds := _related_welds(bms_id)
	if related_welds.is_empty():
		lines.append("Welded connections: none.")
	else:
		lines.append("Welded connections: %d" % related_welds.size())
		for weld in related_welds:
			lines.append("  Section %d -> building #%d section %d" % [
				int(weld.get("own_section", 0)),
				int(weld.get("other_bms", 0)),
				int(weld.get("other_section", 0))])
	_occ_detail.text = "\n".join(lines)


func _related_welds(bms_id: int) -> Array[Dictionary]:
	var related: Array[Dictionary] = []
	for weld in _welds:
		var own_bms := int(weld.get("own_bms", 0))
		var other_bms := int(weld.get("other_bms", 0))
		if own_bms == bms_id:
			related.append({
				"own_section": int(weld.get("own_section", 0)),
				"other_bms": other_bms,
				"other_section": int(weld.get("other_section", 0)),
			})
		elif other_bms == bms_id:
			related.append({
				"own_section": int(weld.get("other_section", 0)),
				"other_bms": own_bms,
				"other_section": int(weld.get("own_section", 0)),
			})
	return related


func _building_key_base(building: Dictionary) -> String:
	var bms_id := int(building.get("bms_id", 0))
	if bms_id != 0:
		return "bms:%d" % bms_id
	var pos: Vector3 = building.get("pos", Vector3.ZERO)
	return "runtime:%.6f,%.6f,%.6f" % [pos.x, pos.y, pos.z]


func _building_state(building: Dictionary) -> String:
	if not bool(building.get("batched", false)):
		return "OUT OF BATCH"
	if not bool(building.get("visible", false)):
		return "CULLED"
	return "DRAWN"


func _sort_building_attention(a: Dictionary, b: Dictionary) -> bool:
	var a_attention := _building_attention(a)
	var b_attention := _building_attention(b)
	if a_attention != b_attention:
		return a_attention > b_attention
	return int(a.get("bms_id", 0)) < int(b.get("bms_id", 0))


func _building_attention(building: Dictionary) -> int:
	if bool(building.get("_open_now", false)):
		return 300
	var state := String(building.get("_state", "DRAWN"))
	match state:
		"CULLED":
			return 200
		"OUT OF BATCH":
			return 100
	return 0


func _building_state_label(building: Dictionary) -> String:
	var state := String(building.get("_state", "DRAWN"))
	return "OPEN NOW · %s" % state \
			if bool(building.get("_open_now", false)) else state


func _state_color(state: String, open_now: bool = false) -> Color:
	if open_now:
		return Color(1.0, 0.75, 0.3)
	match state:
		"CULLED":
			return Color(1.0, 0.5, 0.35)
		"OUT OF BATCH":
			return Color(0.65, 0.65, 0.65)
	return Color(0.55, 1.0, 0.6)


func _state_explanation(state: String) -> String:
	match state:
		"CULLED":
			return "The occluder pass rejected this building after it entered the frame batch."
		"OUT OF BATCH":
			return "The building did not enter the distance/frustum batch this frame."
	return "The building entered the frame batch and reached the renderer."


func _visible_sections(mask: int, compact: bool = false) -> String:
	var unsigned_mask := mask & 0xFFFFFFFF
	if unsigned_mask == 0:
		return "none"
	if unsigned_mask == 0xFFFFFFFF:
		return "all"
	var sections := PackedStringArray()
	for section in range(32):
		if unsigned_mask & (1 << section):
			sections.append(str(section))
	return ",".join(sections) if compact else ", ".join(sections)


func _plural(value: int, singular: String) -> String:
	return singular if value == 1 else singular + "s"


func _clear_pane(message: String) -> void:
	_occ_status_label.text = message
	if _occ_list.item_count > 0:
		_occ_list.clear()
	_building_keys.clear()
	_buildings_by_key.clear()
	_welds.clear()
	_selected_building_key = ""
	_has_selected_building = false
	_occ_detail.text = "Select a building to inspect its occlusion decision."
