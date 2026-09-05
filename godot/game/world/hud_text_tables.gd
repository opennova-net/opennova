extends RefCounted

## The HUD string-table registration lane of GameHudPresenter (split out at the
## 1200-line ratchet): refresh the global Strings registry from the current
## world's resource root on every HUD build, and feed the host's medic
## broadcast format from the loaded gametext.
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
	Strings.register_table(Strings.TABLE_GAMETEXT, load_rtxt(root, "gametext.bin"))
	# The host's medic broadcast format [orig: Server_BroadcastMedicRequest @0x515390].
	var sim: Simulation = world.get_sim() if world != null else null
	var gametext: RtxtStringFile = Strings.get_table(Strings.TABLE_GAMETEXT)
	if sim != null and gametext != null \
			and gametext.has_string_in_section("Server", "STRSRV_MEDREQ"):
		sim.set_server_text(gametext.get_string_in_section("Server", "STRSRV_MEDREQ"))
	# The per-mission table: <mission>.bin, else medmssn.bin — the engine's
	# resolver (runtime_boot resolve_mission_text) owns the fallback rule.
	var base := ""
	if world != null:
		base = String(world.get_loaded_mission_file()).get_basename()
	Strings.register_table(Strings.TABLE_MISSION, RtxtStringFile.load_mission_table(root, base))


static func load_rtxt(root: ResourceRoot, name: String) -> RtxtStringFile:
	var bytes := root.read_file(name)
	if bytes.is_empty():
		return null
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(bytes) != OK:
		return null
	return table
