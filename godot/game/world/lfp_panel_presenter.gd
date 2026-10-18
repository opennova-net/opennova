extends RefCounted

## The zone status panel lane of GameHudPresenter: one marker per contested
## objective ("LFP" is the game's own name for an Objective Point), grouped by
## owning team, with the group's Under-Attack / Ready-for-Takeover text. Retail
## calls the drawer every HUD frame whatever the game type; its one game-type
## test picks the conquest arm (g_GameType == 0x50010), which the engine
## element draws too. So this lane pushes the panel with the session's game
## type and the engine decides; with no session the zone fill finds nothing
## and the panel stays hidden.
## [orig: HUD_RenderAllOverlays @0x5a8530 -> HUD_DrawZoneStatusPanel @0x5a2480
##  -> HUD_DrawZoneMarker @0x5986f0; the game-type test @0x5a248d..0x5a24a1]
##
## The zone rows never round-trip through script (the overlay pulls them
## natively through Simulation.fill_lfp_zones); the two status strings resolve
## natively too, in the gametext table this lane hands over (engine
## hud_lfp_panel.h, retail's literal fallbacks); the blink reads the HUD frame
## counter the overlay holds (HudOverlay.set_frame_clocks).

var _pushed := false


func reset() -> void:
	_pushed = false


## Whether the lane's last update pushed the panel (ADR 0018 read seam for
## tests and diagnostics).
func is_pushed() -> bool:
	return _pushed


func update(hud: HudOverlay, sim: Simulation) -> void:
	if hud == null:
		return
	if sim == null:
		_hide(hud)
		return
	hud.set_lfp_panel(true, int(sim.get_session_game_type()), sim.get_local_player_team(),
			Strings.get_table(Strings.TABLE_GAMETEXT), sim)
	_pushed = true


func _hide(hud: HudOverlay) -> void:
	if _pushed:
		hud.set_lfp_panel(false, 0, 0, null, null)
		_pushed = false
