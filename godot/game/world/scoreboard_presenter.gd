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


func update(hud, world, chorded: bool, active: bool) -> void:
	if hud == null or world == null:
		return
	var down := ControlsBindings.pressed("playerlist_alt")
	if down and not _was_down and active and not chorded:
		_open = not _open
	_was_down = down
	if not _open:
		if _pushed:
			hud.set_scoreboard(false, 0, {}, [])
			_pushed = false
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	var board: Dictionary = sim.get_scoreboard()
	_pushed = true
	var table: RtxtStringFile = Strings.get_table("gametext")
	var strings := {
		# [orig: GameText_GetStringWithFallback("Overlays",
		#  "STROVER_KILLLIST", "!Kill List") @0x423a75]
		"title": "!Kill List",
		# [orig: KeyHelp_GetStringWithFallback("Text", "CHANGE_SCREEN",
		#  "!PgUp and PgDn to change pages") @0x424272]
		"footer": "!PgUp and PgDn to change pages",
		"server": str(board.get("server", "")),
		"mission": str(board.get("mission", "")),
	}
	var game_type := int(board.get("game_type", 0))
	if table != null:
		if table.has_string_in_section("Overlays", "STROVER_KILLLIST"):
			strings["title"] = table.get_string_in_section("Overlays", "STROVER_KILLLIST")
		var label_key := _game_type_label_key(game_type)
		if label_key != "" and table.has_string_in_section("Overlays", label_key):
			strings["game_type"] = table.get_string_in_section("Overlays", label_key)
		# "<label> <count>": the HUD count is the accepted rows MINUS the
		# trailer's spectator count [orig: the subtraction @0x4231dd inside
		# the header block @0x423060].
		var rows: Array = board.get("rows", [])
		var spectators := int(board.get("spectators", 0))
		if table.has_string_in_section("Client", "STRCLI04"):
			strings["players"] = "%s %d" % [
					table.get_string_in_section("Client", "STRCLI04"),
					max(0, rows.size() - spectators)]
		if spectators > 0 and table.has_string_in_section("Client", "STRCLI23"):
			strings["spectators"] = "%s %d" % [
					table.get_string_in_section("Client", "STRCLI23"), spectators]
		if table.has_string_in_section("Text", "CHANGE_SCREEN"):
			strings["footer"] = table.get_string_in_section("Text", "CHANGE_SCREEN")
	hud.set_scoreboard(true, game_type, strings, board.get("rows", []))


## The game-type label row of the header ladder — retail's own key map
## [orig: HUD_GetGameTypeOverlayLabel @0x5b8680; the co-op mask arm
## @0x5b8692]. An unlisted type draws no label (the rung stays blank).
func _game_type_label_key(game_type: int) -> String:
	if (game_type & ~0x20000) == 0x10020:
		return "STROVER28"          # co-op (both 0x10020 and 0x30020)
	match game_type:
		0, 8: return "STROVER29"    # DM / the type-8 non-team mode
		0x10000: return "STROVER64" # TDM
		1: return "STROVER30"       # KOTH
		0x10001: return "STROVER48" # team KOTH
		0x10004: return "STROVER31" # CTF
		0x90002: return "STROVER56" # SD
		0x10002: return "STROVER57" # AD
		0x10008: return "STROVER58" # FB
		0x10010: return "STROVER92" # A&S
		0x50010: return "STROVER93" # CAC
	return ""
