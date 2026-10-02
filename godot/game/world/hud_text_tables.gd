extends RefCounted

## The HUD string-table registration lane of GameHudPresenter (split out at the
## 1200-line ratchet): refresh the global Strings registry from the current
## world's resource root on every HUD build, and feed the host's broadcast
## formats (the medic call, the team change's system line) from the loaded
## gametext.
## [orig: Game_InitSubsystems @0x4a6cd0 —
##  TextResource_LoadFromArchive("gametext.bin") -> g_TextGameText;
##  TextResource_LoadMissionTextBin @0x51ed90]


static func register(root: ResourceRoot, world: GameWorld) -> void:
	if root == null:
		return
	# Refresh this global registry from the current world's root every build.
	# Otherwise a second runtime or direct-mount test can silently reuse the first root's
	# strings. The gametext table IS gametext.bin [orig: Game_InitSubsystems
	# @0x4a6cd0 — TextResource_LoadFromArchive("gametext.bin") -> g_TextGameText;
	# Game.bin is the SEPARATE menu resource (@0x552510) and carries no WepDes].
	Strings.register_table(Strings.TABLE_GAMETEXT, Strings.load_rtxt(root, "gametext.bin"))
	# The host's broadcast formats; a missing key is GameText_GetString's ""
	# miss, which the host sends nothing for [orig: Server_BroadcastMedicRequest
	# @0x515390; NapiNPServerMsg_0x04D_ChangeTeam @0x51902E / @0x51909C].
	var sim: Simulation = world.get_sim() if world != null else null
	var gametext: RtxtStringFile = Strings.get_table(Strings.TABLE_GAMETEXT)
	if sim != null and gametext != null:
		sim.set_server_text(_server_text(gametext, "STRSRV_MEDREQ"),
				_server_text(gametext, "C2Blue"), _server_text(gametext, "C2Red"))
	# The per-mission table: <mission>.bin, else medmssn.bin — the engine's
	# resolver (runtime_boot resolve_mission_text) owns the fallback rule.
	var base := ""
	if world != null:
		base = String(world.get_loaded_mission_file()).get_basename()
	Strings.register_table(Strings.TABLE_MISSION, RtxtStringFile.load_mission_table(root, base))


static func _server_text(gametext: RtxtStringFile, key: String) -> String:
	if not gametext.has_string_in_section("Server", key):
		return ""
	return gametext.get_string_in_section("Server", key)
