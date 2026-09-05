extends GutTest

# Pins the NetProtocol bound constants to the witnessed hex (the retail
# LTGT_* code words, docs/interface/loading-screen-re.md). The engine home
# (engine/base/gameprofile/game_type.h) carries the witnesses and the
# C++ static_asserts pin the binding against it — this leg proves the values
# actually reach GDScript through the re-export chain, and that the
# HostSessionConfig aliases serve the same values.


func test_witnessed_code_words() -> void:
	assert_eq(NetProtocol.GAME_TYPE_DEATHMATCH, 0x00000, "LTGT_DM")
	assert_eq(NetProtocol.GAME_TYPE_KING_OF_THE_HILL, 0x00001, "LTGT_KOTH")
	assert_eq(NetProtocol.GAME_TYPE_FLAG_ME, 0x00008, "LTGT_FM")
	assert_eq(NetProtocol.GAME_TYPE_TEAM_DEATHMATCH, 0x10000, "LTGT_TDM")
	assert_eq(NetProtocol.GAME_TYPE_TEAM_KING_OF_THE_HILL, 0x10001, "LTGT_TKOTH")
	assert_eq(NetProtocol.GAME_TYPE_ATTACK_AND_DEFEND, 0x10002, "LTGT_AD")
	assert_eq(NetProtocol.GAME_TYPE_CAPTURE_THE_FLAG, 0x10004, "LTGT_CTF")
	assert_eq(NetProtocol.GAME_TYPE_FLAGBALL, 0x10008, "LTGT_FB")
	assert_eq(NetProtocol.GAME_TYPE_ADVANCE_AND_SECURE, 0x10010, "LTGT_AAS")
	assert_eq(NetProtocol.GAME_TYPE_TRAINING_COOP, 0x10020, "LTGT_COOP, stock/non-objective")
	assert_eq(NetProtocol.GAME_TYPE_SEARCH_AND_DESTROY, 0x90002, "LTGT_SD")
	assert_eq(NetProtocol.GAME_TYPE_CONQUER_AND_CONTROL, 0x50010, "LTGT_CAC")


func test_objective_bit_composition() -> void:
	assert_eq(NetProtocol.GAME_TYPE_OBJECTIVE_BIT, 0x20000,
			"the 0x0A sub-block-3 gate bit")
	assert_eq(NetProtocol.GAME_TYPE_COOP,
			NetProtocol.GAME_TYPE_TRAINING_COOP | NetProtocol.GAME_TYPE_OBJECTIVE_BIT,
			"objective Co-op composes from stock Co-op + the objective bit (0x30020)")


func test_host_session_config_re_exports() -> void:
	assert_eq(HostSessionConfig.GAME_TYPE_FLAG_ME, NetProtocol.GAME_TYPE_FLAG_ME,
			"HostSessionConfig aliases the bound Flag Me word")
	assert_eq(HostSessionConfig.GAME_TYPE_COOP, NetProtocol.GAME_TYPE_COOP,
			"HostSessionConfig aliases the bound objective Co-op word")
	assert_eq(HostSessionConfig.GAME_TYPE_TRAINING_COOP,
			NetProtocol.GAME_TYPE_TRAINING_COOP,
			"HostSessionConfig aliases the bound stock Co-op word")
	assert_eq(HostSessionConfig.GAME_TYPE_OBJECTIVE_BIT,
			NetProtocol.GAME_TYPE_OBJECTIVE_BIT,
			"HostSessionConfig aliases the bound objective bit")
	assert_eq(HostSessionConfig.DEFAULT_LAN_PORT, NetProtocol.DEFAULT_LAN_PORT,
			"HostSessionConfig aliases the bound LAN default port")
	assert_eq(HostSessionConfig.DEFAULT_GATE_PORT, NetProtocol.DEFAULT_GATE_PORT,
			"HostSessionConfig aliases the bound gate port")


func test_mission_mode_selection() -> void:
	assert_eq(HostSessionConfig.game_type_for_mission_mode(MissionData.ATTRIB_COOP),
			NetProtocol.GAME_TYPE_COOP,
			"an ATTRIB_COOP mission derives the OBJECTIVE Co-op word")
	assert_eq(HostSessionConfig.game_type_for_mission_mode(0),
			NetProtocol.GAME_TYPE_TRAINING_COOP,
			"no multiplayer attrib resolves to stock/training Co-op")
