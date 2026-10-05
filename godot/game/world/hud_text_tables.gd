extends RefCounted

## The HUD string-table registration lane of GameHudPresenter (split out at the
## 1200-line ratchet): refresh the global Strings registry from the current
## world's resource root on every HUD build.
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
	# (The host's "Server" broadcast formats come from the same gametext.bin
	# inside the engine's host boot, so every host carries them, D-NET-344.)
	# The per-mission table: <mission>.bin, else medmssn.bin — the engine's
	# resolver (runtime_boot resolve_mission_text) owns the fallback rule.
	var base := ""
	if world != null:
		base = String(world.get_loaded_mission_file()).get_basename()
	Strings.register_table(Strings.TABLE_MISSION, RtxtStringFile.load_mission_table(root, base))
