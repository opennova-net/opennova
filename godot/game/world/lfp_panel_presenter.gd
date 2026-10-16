extends RefCounted

## The AAS zone status panel lane of GameHudPresenter: one marker per
## contested objective ("LFP" is the game's own name for an Objective Point),
## grouped by owning team, with the group's Under-Attack / Ready-for-Takeover
## text. The panel is the AAS branch of its drawer; the conquest arm
## (g_GameType == 0x50010) is unmodelled and the overlay draws nothing under
## it, so this lane shows the panel for AAS sessions only.
## [orig: HUD_DrawZoneStatusPanel @0x5a2480 -> HUD_DrawZoneMarker @0x5986f0;
##  the game-type test @0x5a24a1]
##
## The zone rows never round-trip through script (the overlay pulls them
## natively through Simulation.fill_lfp_zones); the two status strings resolve
## natively too, in the gametext table this lane hands over (engine
## hud_lfp_panel.h, retail's literal fallbacks).

var _pushed := false


func reset() -> void:
	_pushed = false


func update(hud: HudOverlay, sim: Simulation, frame_counter: int) -> void:
	if hud == null:
		return
	if sim == null:
		_hide(hud)
		return
	var game_type := int(sim.get_session_game_type())
	if game_type != NetProtocol.GAME_TYPE_ADVANCE_AND_SECURE:
		_hide(hud)
		return
	hud.set_lfp_panel(true, game_type, sim.get_local_player_team(), frame_counter,
			Strings.get_table(Strings.TABLE_GAMETEXT), sim)
	_pushed = true


func _hide(hud: HudOverlay) -> void:
	if _pushed:
		hud.set_lfp_panel(false, 0, 0, 0, null, null)
		_pushed = false
