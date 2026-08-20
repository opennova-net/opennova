class_name NovaAiProbe
extends RefCounted
## Env-gated sim-truth probe for the autonomous self-test loop (net-capture
## branch): with NW_AI_PROBE=<path> set, appends one JSON line every interval
## carrying a compact card for EVERY AI entity — position, anim, mount state —
## so a capture diff can separate "the sim never moved it" from "the 0x0A
## interest scheduler never resent it". Inert without the env var.

const INTERVAL_S := 5.0

var _path := OS.get_environment("NW_AI_PROBE")
var _accum := 0.0

func tick(world: GameWorld, delta: float) -> void:
	if _path.is_empty() or world == null:
		return
	_accum += delta
	if _accum < INTERVAL_S:
		return
	_accum = 0.0
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	var cards: Array = []
	for i in range(int(sim.get_entity_count())):
		var card: Dictionary = sim.get_entity_debug(i)
		if card.is_empty():
			continue
		var pos: Vector3 = card.get("position", Vector3.ZERO)
		cards.append({
			"i": i,
			"bms": int(card.get("bms_id", 0)),
			"item": int(card.get("item_id", 0)),
			"team": int(card.get("team", -1)),
			"wp": int(card.get("waypoint_id", 0)),
			"num": int(card.get("wp_number", 0)),
			"hp": int(card.get("health", 0)),
			"mounted": bool(card.get("mounted", false)),
			"pos": [pos.x, pos.y, pos.z],
			"yaw": float(card.get("yaw_deg", 0.0)),
			"anim": int(card.get("anim_state", -1)),
			# think state: why a soldier is (not) walking
			"mm": int(card.get("infantry_move_mode", -1)),
			"ch": int(card.get("wp_channel", -1)),
			"node": int(card.get("wp_node", -1)),
			"wpd": int(card.get("wp_distance", -1)),
			"spd": int(card.get("out_speed", -1)),
			"st": int(card.get("state", -1)),
			"adm": String(card.get("adm_name", "")),
			"inf": bool(card.get("infantry", false)),
			"flags": int(card.get("engine_flags", 0)),
			"bflags": int(card.get("bms_flags", 0)),
			"alert": int(card.get("alert", -1)),
			"tgt": bool(card.get("combat_target_valid", false)),
		})
	if cards.is_empty():
		return
	var f: FileAccess
	if FileAccess.file_exists(_path):
		f = FileAccess.open(_path, FileAccess.READ_WRITE)
		if f != null:
			f.seek_end()
	else:
		f = FileAccess.open(_path, FileAccess.WRITE)
	if f == null:
		return
	f.store_line(JSON.stringify({"ms": Time.get_ticks_msec(), "ai": cards}))
	f.close()
