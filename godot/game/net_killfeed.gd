extends CanvasLayer

# The NovaWorld spectator HUD: the kill feed + environment readout the old 2D
# replay viewer showed on its side rails, ported into the in-engine spectator.
# Reads a NovaNetClient's decoded streams (get_events / get_entities / env_at) and
# posts a top-right kill / objective feed plus a top-left env readout. Weapon fire +
# hits are drawn in the 3D world by NetEventView, so the feed stays the kill feed
# proper.
#
# Kill / event messages are the engine's canned "Canned Msg"/STRCNDnn strings
# formatted with the killer/victim names [orig: HUD_FormatKillEventMessage @
# 0x422DA0 -> Chat_FormatMessage @ 0x422C60, $A/$B token substitution]. When the
# gametext table is not loaded into NovaStrings (the in-game wiring of GameText_-
# GetString @ 0x51EBD0 + the canned-message section is a follow-up), the line falls
# back to "<attacker> x <victim>" — the same information the 2D viewer showed.
#
# The env readout mirrors the viewer's Environment panel (time-of-day / fog /
# clouds / overcast / quake) straight from the wire env snapshot. Driving the live
# 3D scene from that snapshot (the witnessed Env_FogDistTarget / Env_CurTimeFixed24
# targets, §5.x, [orig: 0x430253..0x430341]) faithfully needs the engine's
# target-interpolation model ported into the env subsystem — a separate follow-up.

const REFRESH_INTERVAL := 0.25
const MAX_LINES := 8
const NONE_HANDLE := 65535

# ReplayEventKind — keep in sync with libs/npwire/replay_timeline.h.
const KIND_KILL := 2
const KIND_GAMEEVENT := 3

const COL_KILL := Color(1.0, 0.48, 0.09)
const COL_EVENT := Color(0.29, 0.56, 1.0)
const COL_ENV := Color(0.71, 0.74, 0.76)

var _client
var _rows: VBoxContainer
var _env_label: Label
var _timer: Timer


func _init() -> void:
	layer = 80
	_build()
	_timer = Timer.new()
	_timer.wait_time = REFRESH_INTERVAL
	_timer.autostart = true
	_timer.timeout.connect(_refresh)
	add_child(_timer)


## The net spectator client to read events from. Pass null to blank the feed.
func set_client(client) -> void:
	_client = client
	_refresh()


func _build() -> void:
	_rows = VBoxContainer.new()
	_rows.name = "Feed"
	_rows.anchor_left = 1.0
	_rows.anchor_right = 1.0
	_rows.anchor_top = 0.0
	_rows.anchor_bottom = 0.0
	_rows.offset_left = -480.0
	_rows.offset_right = -16.0
	_rows.offset_top = 16.0
	_rows.offset_bottom = 320.0
	_rows.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	_rows.alignment = BoxContainer.ALIGNMENT_BEGIN
	add_child(_rows)

	_env_label = Label.new()
	_env_label.name = "Env"
	_env_label.offset_left = 16.0
	_env_label.offset_top = 16.0
	_env_label.modulate = COL_ENV
	add_child(_env_label)


func _refresh() -> void:
	if _rows == null:
		return
	for c in _rows.get_children():
		c.queue_free()
	if _client == null:
		if _env_label != null:
			_env_label.text = ""
		return
	if _env_label != null:
		_env_label.text = _format_env(_client.env_at(_client.get_latest_frame()))
	var names := {}
	for e in _client.get_entities():
		names[int(e["handle"])] = String(e["name"])
	# The kill feed proper: kills + objective/game events (fire + hits draw in 3D).
	var feed: Array = []
	for ev in _client.get_events():
		var k := int(ev["kind"])
		if k == KIND_KILL or k == KIND_GAMEEVENT:
			feed.append(ev)
	for i in range(max(0, feed.size() - MAX_LINES), feed.size()):
		var ev: Dictionary = feed[i]
		var label := Label.new()
		label.text = _format(ev, names)
		label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		label.modulate = COL_KILL if int(ev["kind"]) == KIND_KILL else COL_EVENT
		_rows.add_child(label)


# One feed line for an event. Faithful canned message when the template resolves,
# else the viewer-equivalent fallback so the line still reads.
func _format(ev: Dictionary, names: Dictionary) -> String:
	var attacker := _name_of(int(ev.get("source", NONE_HANDLE)), names)
	var victim := _name_of(int(ev.get("target", NONE_HANDLE)), names)
	var key := String(ev.get("label", ""))
	if key != "":
		var tmpl := _canned(key)
		if tmpl != "":
			return _apply_tokens(tmpl, attacker, victim)
	if int(ev["kind"]) == KIND_KILL:
		return "%s ✖ %s" % [attacker, victim]
	return key if key != "" else "event %d" % int(ev.get("event_type", 0))


# [orig: Chat_FormatMessage @ 0x422C60] — $A = attacker, $B = victim. (The full
# token grammar + clan-tag <ch>..<co> colouring is a documented refinement.)
func _apply_tokens(template: String, attacker: String, victim: String) -> String:
	return template.replace("$A", attacker).replace("$B", victim)


# The viewer's Environment panel, from one wire env snapshot. tod_fixed is a 16-bit
# fraction of a day (the engine's Env_CurTimeFixed24 = tod_fixed << 13); for a human
# readout it reads as hours = tod_fixed / 65536 * 24. Empty when none seen yet.
func _format_env(env: Dictionary) -> String:
	if not bool(env.get("found", false)):
		return ""
	var hours := float(int(env.get("tod_fixed", 0))) / 65536.0 * 24.0
	var hh := int(hours)
	var mm := int((hours - float(hh)) * 60.0)
	return "TOD %02d:%02d · fog %d · clouds %d · overcast %d · quake %d" % [
		hh, mm, int(env.get("fog_dist", 0)), int(env.get("cloud_scroll", 0)),
		int(env.get("overcast", 0)), int(env.get("quake_ticks", 0))]


# The engine resolves canned messages via GameText_GetString (@ 0x51EBD0). When the
# gametext table is loaded into NovaStrings the template resolves; otherwise "" so
# the caller falls back. No section is guessed here (the witness is a follow-up).
func _canned(key: String) -> String:
	return NovaStrings.get_display_string(key) if NovaStrings.has_string(key) else ""


func _name_of(handle: int, names: Dictionary) -> String:
	if handle == NONE_HANDLE:
		return "?"
	if names.has(handle) and String(names[handle]) != "":
		return String(names[handle])
	var pool := WireHandle.pool(handle)
	var slot := WireHandle.slot(handle)
	return "s%d" % slot if pool == 0 else "s%d·p%d" % [slot, pool]
