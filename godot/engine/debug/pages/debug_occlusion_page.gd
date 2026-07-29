class_name DebugOcclusionPage
extends VBoxContainer
## The render-occlusion inspector: what the portal/section-mask engine decided
## this frame — the camera's blink state, the frame-wide latches, the batch
## split, and the per-building section masks (docs/render/render-occlusion-re.md
## §3/§5) — plus the "Show portal faces" world-view toggle. Row states name the
## culling stage: "not batched" (distance/frustum), "occluder-culled"
## (render_TOC), "drawn". Developer window into our port, not a mimicked retail
## debug page.

## Fired when "Show portal faces" is toggled. The host builds/frees the
## OcclusionDebugView (type-colored portal-face outlines + section labels over
## the world), the collision-view contract.
signal occlusion_debug_toggled(enabled: bool)

var _occ_status_label: Label
var _occ_list: ItemList
var _occ_portals_check: CheckBox


func _init() -> void:
	name = "Occlusion"
	add_theme_constant_override("separation", 6)

	_occ_status_label = Label.new()
	_occ_status_label.name = "OcclusionStatus"
	_occ_status_label.text = "No occlusion data."
	_occ_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_occ_status_label)

	_occ_list = ItemList.new()
	_occ_list.name = "OcclusionBuildings"
	_occ_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_occ_list.focus_mode = Control.FOCUS_NONE
	add_child(_occ_list)

	_occ_portals_check = CheckBox.new()
	_occ_portals_check.name = "OcclusionShowPortals"
	_occ_portals_check.text = "Show portal faces"
	_occ_portals_check.tooltip_text = "Draw every nearby building's occlusion faces over the world — windows, portals and welded links as colored outlines with section labels, plain occluder faces in gray."
	_occ_portals_check.button_pressed = false
	_occ_portals_check.toggled.connect(_on_occlusion_debug_toggled)
	add_child(_occ_portals_check)


func refresh(sim: Object) -> void:
	if sim == null or not sim.has_method("get_occlusion_debug"):
		_clear_pane("No occlusion data.")
		return
	var occ: Dictionary = sim.get_occlusion_debug()
	if not bool(occ.get("active", false)):
		_clear_pane("No portal-carrying buildings in this mission.")
		return
	var counts: Dictionary = occ.get("counts", {})
	var lines := PackedStringArray()
	lines.append("Camera: %s   blink letters 0x%02X" % [
		"indoors" if bool(occ.get("camera_indoors", false)) else "outdoors",
		int(occ.get("local_blink_flags", 0))])
	lines.append("Exterior visible: %s   water visible: %s" % [
		"yes" if bool(occ.get("exterior_visible", false)) else "no",
		"yes" if bool(occ.get("water_visible", false)) else "no"])
	lines.append("Buildings: %d tracked   %d batched   %d drawn   %d occluder-culled" % [
		int(counts.get("instances", 0)), int(counts.get("batched", 0)),
		int(counts.get("visible", 0)), int(counts.get("toc_culled", 0))])
	lines.append("Portal slots %d   window wedges %d   see-through wedges %d" % [
		int(counts.get("slots", 0)), int(counts.get("window_groups", 0)),
		int(counts.get("viewthru_groups", 0))])
	lines.append("Cross-building welds %d   entities hidden %d" % [
		int(counts.get("welds", 0)), int(counts.get("culled_entities", 0))])
	_occ_status_label.text = "\n".join(lines)

	_occ_list.clear()
	for b_v in occ.get("buildings", []):
		var b: Dictionary = b_v
		var state := "drawn"
		var row_color := Color(0.55, 1.0, 0.6)
		if not bool(b.get("batched", false)):
			state = "not batched"
			row_color = Color(0.6, 0.6, 0.6)
		elif not bool(b.get("visible", false)):
			state = "occluder-culled"
			row_color = Color(1.0, 0.5, 0.35)
		var traits := ""
		if bool(b.get("has_open", false)):
			traits += "O"
		if bool(b.get("has_windows", false)):
			traits += "W"
		if bool(b.get("has_links", false)):
			traits += "L"
		var row := "#%d  mask %08X  %s" % [
			int(b.get("bms_id", 0)), int(b.get("mask", 0)) & 0xFFFFFFFF, state]
		if not traits.is_empty():
			row += "  [%s]" % traits
		var windows := int(b.get("windows", 0))
		var portals := int(b.get("portals", 0))
		var links := int(b.get("links", 0))
		if windows + portals + links > 0:
			row += "  win %d por %d link %d" % [windows, portals, links]
		var idx := _occ_list.add_item(row, null, false)
		_occ_list.set_item_custom_fg_color(idx, row_color)
	for w_v in occ.get("welds", []):
		var w: Dictionary = w_v
		var widx := _occ_list.add_item("weld  #%d s%d <-> #%d s%d" % [
			int(w.get("own_bms", 0)), int(w.get("own_section", 0)),
			int(w.get("other_bms", 0)), int(w.get("other_section", 0))], null, false)
		_occ_list.set_item_custom_fg_color(widx, Color(1.0, 0.4, 1.0))


func _clear_pane(message: String) -> void:
	_occ_status_label.text = message
	if _occ_list.item_count > 0:
		_occ_list.clear()


func _on_occlusion_debug_toggled(pressed: bool) -> void:
	occlusion_debug_toggled.emit(pressed)
