class_name DebugNetPage
extends NovaDebugPage
## The net-session inspector: which net role this sim runs (every session is a
## listen server — ADR 0011), the host's session row + bound port + peer count,
## and the joiner's connect phase / self wire handle / error. Read-only, no
## toggles.

# npruntime JoinerConnection::Phase, surfaced by NovaSimulation.get_joiner_phase().
const JOINER_PHASE_NAMES := ["Idle", "Hello", "Auth", "Driving", "InMatch", "Error"]

var _net_status_label: Label
var _net_list: ItemList
var _net_rows_signature := ""


func page_id() -> StringName:
	return &"Net"


func page_category() -> StringName:
	return CATEGORY_SIM


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_net_status_label = Label.new()
	_net_status_label.name = "NetStatus"
	_net_status_label.text = "No net session."
	_net_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_net_status_label)

	_net_list = ItemList.new()
	_net_list.name = "NetDetails"
	_net_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_net_list.focus_mode = Control.FOCUS_NONE
	add_child(_net_list)


func refresh() -> void:
	if _net_status_label == null:
		return
	var sim := _ctx.sim()
	if sim == null or not sim.has_method("is_joiner"):
		_clear_pane("No net session.")
		return
	var rows := PackedStringArray()
	if bool(sim.is_joiner()):
		var phase := int(sim.get_joiner_phase())
		var phase_name: String = JOINER_PHASE_NAMES[phase] \
				if phase >= 0 and phase < JOINER_PHASE_NAMES.size() else str(phase)
		_net_status_label.text = "Joiner — connect phase %s" % phase_name
		rows.append("in match: %s" % ("yes" if bool(sim.is_joined_in_match()) else "no"))
		rows.append("self wire handle: %d" % int(sim.get_joiner_self_handle()))
		var join_error := String(sim.get_join_error())
		if not join_error.is_empty():
			rows.append("error: %s" % join_error)
		rows.append("server: %s" % String(sim.get_join_server_name()))
		rows.append("mission: %s (%s)" % [String(sim.get_join_mission_name()),
				String(sim.get_join_mission_file())])
		if sim.has_method("get_joiner_network_diagnostics"):
			var diagnostics: Dictionary = sim.get_joiner_network_diagnostics()
			rows.append("sequence: inbound %d  outbound %d  retained %d" % [
				int(diagnostics.get("frontier_seq", 0)),
				int(diagnostics.get("outbound_seq", 0)),
				int(diagnostics.get("retained_outbound", 0))])
			rows.append("records applied: %d  gap depth: %d" % [
				int(diagnostics.get("records_applied", 0)),
				int(diagnostics.get("gap_depth", 0))])
			rows.append("traffic flat: %.1f s%s" % [
				float(diagnostics.get("flat_seconds", 0.0)),
				"  [freeze suspected]"
				if bool(diagnostics.get("freeze_suspected", false)) else ""])
			rows.append("diagnostic capture: %s" % [
				"enabled" if bool(diagnostics.get("enabled", false)) else "off"])
		if sim.has_method("get_session_loss_reason"):
			var loss_reason := String(sim.get_session_loss_reason())
			if not loss_reason.is_empty():
				rows.append("session ended: %s" % loss_reason)
		if sim.has_method("is_join_deploy_pick_pending") \
				and bool(sim.is_join_deploy_pick_pending()):
			var team := int(sim.get_join_assigned_team()) \
					if sim.has_method("get_join_assigned_team") else 0
			var zone_count := 0
			if sim.has_method("get_deploy_spawn_zones"):
				zone_count = sim.get_deploy_spawn_zones().size()
			rows.append("deployment pending: team %d  %d spawn zone(s)" % [
				team, zone_count])
	elif bool(sim.is_host_listening()):
		_net_status_label.text = "Host — listening on UDP %d, %d peer(s)" % [
				int(sim.get_host_listen_port()), int(sim.get_host_peer_count())]
		# Keys as get_host_session_config() emits them (nova_simulation_net.cpp).
		var config: Dictionary = sim.get_host_session_config()
		for key in ["server_name", "mission_name", "mission_file", "gametype",
				"max_players", "serve_and_play", "expansion", "player_name",
				"bind_port"]:
			if config.has(key):
				rows.append("%s: %s" % [key, str(config[key])])
	else:
		_net_status_label.text = "Local listen server (no bound socket) — SP session."
	# Rebuild the rows only when they change (ItemList rebuilds are the cost).
	var signature := "\n".join(rows)
	if signature != _net_rows_signature:
		_net_rows_signature = signature
		_net_list.clear()
		for row in rows:
			_net_list.add_item(row, null, false)


func _clear_pane(message: String) -> void:
	_net_status_label.text = message
	_net_rows_signature = ""
	if _net_list != null and _net_list.item_count > 0:
		_net_list.clear()
