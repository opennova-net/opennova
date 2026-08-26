extends RefCounted

## The SP "Show Score" statistics panel lane of GameHudPresenter — retail's
## toggled end-round statistics overlay: one titled label box with four
## label/value rows (subgoals won/defined, enemy units killed/total, team-unit
## and friendly-unit kills). The ShowScore action is an EDGE that TOGGLES the
## panel flag, settable only OUTSIDE a session (SP); each flip runs the
## respawn-init wrapper, which clears every other overlay toggle and keeps
## this one, and the respawn init clears the flag itself.
## [orig: HUD_DrawEndRoundStatistics @0x5b7600, drawn from the frame drawer
##  HUD_DrawOverlayPanels @0x5c0092 while dword_24C18AC; the toggle is
##  Input_HandleActionBinding case 422 @0x49bd29 (gated !is_in_session) ->
##  Game_InitRespawnStateKeepingToggle @0x4993c0 (&toggle) = Game_InitRespawnState keeping *ptr; cleared by
##  Game_InitRespawnState @0x499381. Controls catalog row 99 "ShowScore"
##  ("!Show Score"), default VK 0x74 = F5 — nothing new is bound.]
##
## The shell resolves the title (gametext "Score"/SCORE_TITLE) and the four
## Epilog labels and formats the values, because it owns the string tables;
## the engine compiler (hud/end_round_statistics.h) owns the layout.

var _open := false      # the toggled panel flag [orig: dword_24C18AC]
var _was_down := false  # the toggle's down-edge latch
var _pushed := false    # so the panel clears exactly once on close


## The flag clears with the respawn init, as retail's does [orig: @0x499381].
func reset() -> void:
	_open = false
	_was_down = false
	_pushed = false


func is_open() -> bool:
	return _open


## `sp` mirrors retail's `!is_in_session` gate on the toggle edge
## [orig: @0x49bd29 — the case jumps to default while in a session].
## `close_siblings` is the shell's reach into the other overlay toggles the
## respawn-init wrapper clears (the sim-owned ones — map overlay, emote/radio
## menus — clear through the sim's own respawn init) [orig: Game_InitRespawnStateKeepingToggle @0x4993c0].
func update(hud: HudOverlay, sim: Simulation, down: bool, chorded: bool,
		active: bool, sp: bool, close_siblings: Callable) -> void:
	if hud == null:
		return
	if down and not _was_down and active and not chorded and sp:
		_open = not _open
		if close_siblings.is_valid():
			close_siblings.call()
	_was_down = down
	if _open:
		_push(hud, sim)
		_pushed = true
	elif _pushed:
		hud.set_end_round_statistics(false, false, "", PackedStringArray(),
				PackedStringArray())
		_pushed = false


## Values re-push every frame while open — the counters are live
## [orig: the drawer reads the 0xC846xx block per frame]. The rows arrive
## engine-composed (hud/end_round_statistics.h owns the "%d/%d" forms and the
## enemy-kill clamp); the shell only resolves the Epilog label keys.
func _push(hud: HudOverlay, sim: Simulation) -> void:
	if sim == null:
		return
	var stats: Dictionary = sim.get_end_round_statistics()
	if stats.is_empty():
		return
	var labels := PackedStringArray()
	var values := PackedStringArray()
	for row in stats.get("rows", []):
		labels.push_back(_gametext("Epilog", String(row.get("label_key", ""))))
		values.push_back(String(row.get("value", "")))
	hud.set_end_round_statistics(true, bool(stats.get("raised", false)),
			_gametext("Score", "SCORE_TITLE"), labels, values)


## Missing strings resolve empty, as retail's GameText_GetString does.
func _gametext(section: String, key: String) -> String:
	var table: RtxtStringFile = Strings.get_table("gametext")
	if table != null and table.has_string_in_section(section, key):
		return table.get_string_in_section(section, key)
	return ""
