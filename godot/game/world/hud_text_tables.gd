extends RefCounted

## The HUD string-table registration lane of GameHudPresenter (split out at the
## 1200-line ratchet): refresh the global Strings registry from the current
## world's resource root on every HUD build, and feed the host's medic
## broadcast format from the loaded gametext.
## [orig: Game_InitSubsystems @0x4a6cd0 —
##  TextResource_LoadFromArchive("gametext.bin") -> g_TextGameText;
##  TextResource_LoadMissionTextBin @0x51ed90]


static func register(root: ResourceRoot, world) -> void:
	if root == null:
		return
	# Refresh this global registry from the current world's root every build.
	# Otherwise a second runtime or direct-mount test can silently reuse the first root's
	# strings. The gametext table IS gametext.bin [orig: Game_InitSubsystems
	# @0x4a6cd0 — TextResource_LoadFromArchive("gametext.bin") -> g_TextGameText;
	# Game.bin is the SEPARATE menu resource (@0x552510) and carries no WepDes].
	Strings.register_table("gametext", load_rtxt(root, "gametext.bin"))
	# The host's medic broadcast format [orig: Server_BroadcastMedicRequest @0x515390].
	var sim = world.get_sim() if world != null else null
	var gametext: RtxtStringFile = Strings.get_table("gametext")
	if sim != null and gametext != null \
			and gametext.has_string_in_section("Server", "STRSRV_MEDREQ"):
		sim.set_server_text(gametext.get_string_in_section("Server", "STRSRV_MEDREQ"))
	# The medmssn fallback fires only when the mission .bin does not EXIST — a
	# present-but-unparseable file loads to nothing with no fallback.
	# [orig: TextResource_LoadMissionTextBin @0x51ede3 — FileSystem_FileExists picks
	# the filename; the load result is stored either way]
	var mission_table: RtxtStringFile = null
	var mission_bin := ""
	if world != null:
		var base: String = String(world.get_loaded_mission_file()).get_basename()
		if not base.is_empty():
			mission_bin = base + ".bin"
	if not mission_bin.is_empty() and root.has_file(mission_bin):
		mission_table = load_rtxt(root, mission_bin)
	else:
		mission_table = load_rtxt(root, "medmssn.bin")
	Strings.register_table("mission", mission_table)


static func load_rtxt(root: ResourceRoot, name: String) -> RtxtStringFile:
	var bytes := root.read_file(name)
	if bytes.is_empty():
		return null
	var table := RtxtStringFile.new()
	if table.load_from_byte_array(bytes) != OK:
		return null
	return table
