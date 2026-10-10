extends RefCounted

## The Tab player list lane of GameHudPresenter. The playerlist action's edge
## and the toggled panel-visible flag are the engine's (hud/hud_toggles.h,
## through the presenter's HudToggles: retail keeps the board up until the
## next press, and a respawn init clears it [orig: Scoreboard_TogglePlayerList
## @0x4244c0 from the action dispatch case @0x49bb68; the respawn clear
## @0x4993ae; the drawer gate HUD_DrawKillListIfVisible @0x424300]). This
## lane shows or hides the board for that flag.
##
## The shell owns the gametext table; the board composes its strings from it
## natively as the game's drawer does (the engine's
## hud::scoreboard_header_strings: the title, the game type's rung, the count
## lines, keyhelp's paging hint through the process's keyhelp table), its
## server and mission rungs and counts from the session decode
## (Simulation.get_scoreboard).

var _pushed := false      # so the board clears exactly once on close


func reset() -> void:
	_pushed = false


func update(hud: HudOverlay, world: GameWorld, open: bool, frame_counter: int) -> void:
	if hud == null or world == null:
		return
	if not open:
		if _pushed:
			hud.set_scoreboard(false, 0, null, null)
			_pushed = false
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	_pushed = true
	# Nothing round-trips through script: the overlay pulls the session header,
	# the rows and the team count the 4-team page reads natively from the sim
	# (HudOverlay.set_scoreboard -> get_scoreboard, fill_scoreboard), with the
	# strings and the drawers' own gametext lookups resolved natively off the
	# table; the frame counter is the HUD tick the engine's page alternates on
	# (hud_scoreboard.h).
	hud.set_scoreboard(true, frame_counter, sim, Strings.get_table(Strings.TABLE_GAMETEXT))
