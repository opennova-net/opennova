extends RefCounted

## The Tab player list lane of GameHudPresenter. Like every other HUD key this
## is an EDGE: the playerlist action TOGGLES the panel-visible flag — retail
## keeps the board up until the next press (or a respawn init clears it)
## [orig: Scoreboard_TogglePlayerList @0x4244c0 from the action dispatch case
## @0x49bb68; the respawn clear @0x4993ae; the drawer gate
## HUD_DrawKillListIfVisible @0x424300]. The binding already ships in the
## controls catalog as "playerlist_alt" (vk 0x09), so nothing new is bound.
##
## The shell owns the strings because it owns the string tables: the title
## from gametext Overlays (with retail's literal fallback), the game-type
## label from the witnessed Overlays row map, the two count lines from
## Client, and the paging hint from Text. Server name and mission title ride
## the session decode through get_scoreboard.

var _open := false        # the toggled panel-visible flag [orig: g_scoreboardPanelVisible]
var _was_down := false    # the toggle's down-edge latch
var _pushed := false      # so the board clears exactly once on close


## The board flag clears with the mission, as retail's respawn/mission init
## clears its global [orig: @0x4993ae].
func reset() -> void:
	_open = false
	_was_down = false
	_pushed = false


func update(hud: HudOverlay, world: GameWorld, chorded: bool, active: bool,
		frame_counter: int) -> void:
	if hud == null or world == null:
		return
	var down := ControlsBindings.pressed("playerlist_alt")
	if down and not _was_down and active and not chorded:
		_open = not _open
	_was_down = down
	if not _open:
		if _pushed:
			hud.set_scoreboard(false, 0, 0, {}, null)
			_pushed = false
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	var board := sim.get_scoreboard()
	_pushed = true
	var table: RtxtStringFile = Strings.get_table(Strings.TABLE_GAMETEXT)
	var strings := {
		# [orig: GameText_GetStringWithFallback(Strings.SECTION_OVERLAYS,
		#  "STROVER_KILLLIST", "!Kill List") @0x423a75]
		"title": "!Kill List",
		# [orig: KeyHelp_GetStringWithFallback("Text", "CHANGE_SCREEN",
		#  "!PgUp and PgDn to change pages") @0x424272]
		"footer": "!PgUp and PgDn to change pages",
		"server": board.server,
		"mission": board.mission,
	}
	var game_type := board.game_type
	if table != null:
		if table.has_string_in_section(Strings.SECTION_OVERLAYS, "STROVER_KILLLIST"):
			strings["title"] = table.get_string_in_section(Strings.SECTION_OVERLAYS, "STROVER_KILLLIST")
		# The key map is retail's own, engine-owned (npwire game_type.h
		# overlay_label_key via NetProtocol) — this lane only looks it up.
		var label_key := NetProtocol.game_type_overlay_label_key(game_type)
		if label_key != "" and table.has_string_in_section(Strings.SECTION_OVERLAYS, label_key):
			strings["game_type"] = table.get_string_in_section(Strings.SECTION_OVERLAYS, label_key)
		# "<label> <count>": the counts are engine-computed — the players
		# count is netsim's witnessed rows-minus-spectators header arithmetic
		# (scoreboard_header); this lane only pairs them with the strings.
		var spectators := board.spectators
		if table.has_string_in_section("Client", "STRCLI04"):
			strings["players"] = "%s %d" % [
					table.get_string_in_section("Client", "STRCLI04"),
					board.players]
		if spectators > 0 and table.has_string_in_section("Client", "STRCLI23"):
			strings["spectators"] = "%s %d" % [
					table.get_string_in_section("Client", "STRCLI23"), spectators]
		if table.has_string_in_section("Text", "CHANGE_SCREEN"):
			strings["footer"] = table.get_string_in_section("Text", "CHANGE_SCREEN")
	# The rows never round-trip through script: the overlay pulls them (and the
	# team count the 4-team page reads) natively from the sim
	# (HudOverlay.set_scoreboard -> fill_scoreboard_rows); the frame counter is
	# the HUD tick the engine's page alternates on (hud_scoreboard.h).
	hud.set_scoreboard(true, game_type, frame_counter, strings, sim)
