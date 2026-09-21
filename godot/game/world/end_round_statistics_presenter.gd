extends RefCounted

## The SP "Show Score" statistics panel lane of GameHudPresenter — retail's
## toggled end-round statistics overlay: one titled label box with four
## label/value rows (subgoals won/defined, enemy units killed/total, team-unit
## and friendly-unit kills). The ShowScore edge, its SP-only gate, the
## sibling close and the respawn clear are the engine's (hud/hud_toggles.h,
## through the presenter's HudToggles) [orig: HUD_DrawEndRoundStatistics
## @0x5b7600, drawn from the frame drawer HUD_DrawOverlayPanels @0x5c0092
## while dword_24C18AC; the toggle is Input_HandleActionBinding case 422
## @0x49bd29 (gated !is_in_session) -> Game_InitRespawnStateKeepingToggle
## @0x4993c0; cleared by Game_InitRespawnState @0x499381. Controls catalog
## row 99 "ShowScore" ("!Show Score"), default VK 0x74 = F5]. This lane shows
## or hides the panel for that flag.
##
## The shell resolves the title (gametext "Score"/SCORE_TITLE) and the four
## Epilog labels and formats the values, because it owns the string tables;
## the engine compiler (hud/end_round_statistics.h) owns the layout.

var _pushed := false    # so the panel clears exactly once on close


func reset() -> void:
	_pushed = false


func update(hud: HudOverlay, sim: Simulation, open: bool) -> void:
	if hud == null:
		return
	if open:
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
	var stats := sim.get_end_round_statistics()
	if stats == null:
		return
	var labels := PackedStringArray()
	for label_key in stats.label_keys:
		labels.push_back(_gametext("Epilog", label_key))
	hud.set_end_round_statistics(true, stats.raised,
			_gametext("Score", "SCORE_TITLE"), labels, stats.values)


## Missing strings resolve empty, as retail's GameText_GetString does.
func _gametext(section: String, key: String) -> String:
	return Strings.lookup_or(Strings.TABLE_GAMETEXT, section, key, "")
