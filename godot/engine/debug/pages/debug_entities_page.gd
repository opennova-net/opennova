class_name DebugEntitiesPage
extends VBoxContainer
## The present-snapshot entity list + a scalar detail card for the selection.
## The list reads ONE packed snapshot per refresh and only the selected entity
## pays for the detail card.

var _resolve_sim := Callable()
var _entity_list: ItemList
var _entity_detail: Label
var _selected_entity := -1


func _init() -> void:
	name = "Entities"
	add_theme_constant_override("separation", 6)

	_entity_list = ItemList.new()
	_entity_list.name = "EntityList"
	_entity_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_entity_list.item_selected.connect(_on_entity_selected)
	add_child(_entity_list)

	_entity_detail = Label.new()
	_entity_detail.name = "EntityDetail"
	_entity_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_entity_detail.text = "Select a unit to see its details."
	add_child(_entity_detail)


## `resolve_sim` returns the live validated sim (or null); re-resolved on every
## selection because mission reloads recreate it.
func setup(resolve_sim: Callable) -> void:
	_resolve_sim = resolve_sim


func refresh(sim: Object) -> void:
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var stride: int = sim.get_present_stride()
	var count := 0 if stride <= 0 else snap.size() / stride
	# Rebuild only on count change; steady-state refreshes update text in place.
	if _entity_list.item_count != count:
		_entity_list.clear()
		for i in range(count):
			_entity_list.add_item("")
		if _selected_entity >= count:
			_selected_entity = -1
	for i in range(count):
		var base := i * stride
		var flags := ""
		if snap[base + NovaSimulation.PF_ALIVE] == 0.0:
			flags += "  [down]"
		if snap[base + NovaSimulation.PF_HIDDEN] != 0.0:
			flags += "  [hidden]"
		_entity_list.set_item_text(i, "#%d  ssn %d  (%.0f, %.0f)%s" % [
			i,
			int(snap[base + NovaSimulation.PF_NET_ID]),
			snap[base + NovaSimulation.PF_POS_X],
			snap[base + NovaSimulation.PF_POS_Z],
			flags,
		])
	_refresh_entity_detail(sim)


func clear_live() -> void:
	if _entity_list.item_count > 0:
		_entity_list.clear()
	_selected_entity = -1
	_entity_detail.text = "Select a unit to see its details."


func _refresh_entity_detail(sim: Object) -> void:
	if _selected_entity < 0:
		_entity_detail.text = "Select a unit to see its details."
		return
	var card: Dictionary = sim.get_entity_debug(_selected_entity)
	if card.is_empty():
		_entity_detail.text = "Select a unit to see its details."
		return
	var name := String(card.get("name", ""))
	var pos: Vector3 = card.get("position", Vector3.ZERO)
	var lines := PackedStringArray()
	lines.append("%s  (ssn %d)" % [name if not name.is_empty() else "unnamed", int(card.get("net_id", 0))])
	lines.append("state: %s (%d)  alert %d" % [
		String(card.get("state_name", "?")), int(card.get("state", 0)), int(card.get("alert", 0))])
	lines.append("health: %d (ai %d)  team %d" % [
		int(card.get("health", 0)), int(card.get("ai_health", 0)), int(card.get("team", 0))])
	lines.append("position: (%.1f, %.1f, %.1f)  facing %.0f°" % [
		pos.x, pos.y, pos.z, float(card.get("yaw_deg", 0.0))])
	lines.append("route %d  node %d  distance %d  speed %d" % [
		int(card.get("waypoint_id", 0)), int(card.get("wp_node", 0)),
		int(card.get("wp_distance", 0)), int(card.get("out_speed", 0))])
	if bool(card.get("mounted", false)):
		var seat_local: Vector3 = card.get("mount_seat_local", Vector3.ZERO)
		lines.append("mounted: target ssn %d  seat %d/%d  %s  type %d  bone %d" % [
			int(card.get("mount_target_net_id", 0)), int(card.get("mount_seat", -1)),
			int(card.get("mount_target_seat_count", 0)),
			String(card.get("mount_seat_source_name", "")),
			int(card.get("mount_type", 0)), int(card.get("mount_seat_bone", 0))])
		lines.append("seat local: (%.2f, %.2f, %.2f)  pose %d  yaw %+d  anim %s (%d)" % [
			seat_local.x, seat_local.y, seat_local.z,
			int(card.get("mount_seat_pose_index", 0)),
			int(card.get("mount_seat_yaw_offset", 0)),
			String(card.get("anim_key", "")), int(card.get("anim_state", -1))])
	var traits := PackedStringArray()
	if bool(card.get("infantry", false)):
		traits.append("on foot")
	if bool(card.get("hidden", false)):
		traits.append("hidden")
	if bool(card.get("held", false)):
		traits.append("held")
	if bool(card.get("disabled", false)):
		traits.append("disabled")
	if not traits.is_empty():
		lines.append(", ".join(traits))
	_entity_detail.text = "\n".join(lines)


func _on_entity_selected(index: int) -> void:
	_selected_entity = index
	var sim: Object = _resolve_sim.call() if _resolve_sim.is_valid() else null
	if sim != null:
		_refresh_entity_detail(sim)
