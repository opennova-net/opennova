extends GutTest

# Pins the GDScript g_GameType constants to the witnessed hex (the retail
# LTGT_* code words, docs/interface/loading-screen-re.md). The C++ twin
# (libs/npwire/include/npwire/game_type.h) static_asserts the same values —
# this is the GDScript leg of that agreement.


func test_witnessed_code_words() -> void:
	assert_eq(HostSessionConfig.GAME_TYPE_DEATHMATCH, 0x00000, "LTGT_DM")
	assert_eq(HostSessionConfig.GAME_TYPE_KING_OF_THE_HILL, 0x00001, "LTGT_KOTH")
	assert_eq(HostSessionConfig.GAME_TYPE_TEAM_DEATHMATCH, 0x10000, "LTGT_TDM")
	assert_eq(HostSessionConfig.GAME_TYPE_TEAM_KING_OF_THE_HILL, 0x10001, "LTGT_TKOTH")
	assert_eq(HostSessionConfig.GAME_TYPE_ATTACK_AND_DEFEND, 0x10002, "LTGT_AD")
	assert_eq(HostSessionConfig.GAME_TYPE_CAPTURE_THE_FLAG, 0x10004, "LTGT_CTF")
	assert_eq(HostSessionConfig.GAME_TYPE_FLAGBALL, 0x10008, "LTGT_FB")
	assert_eq(HostSessionConfig.GAME_TYPE_ADVANCE_AND_SECURE, 0x10010, "LTGT_AAS")
	assert_eq(HostSessionConfig.GAME_TYPE_TRAINING_COOP, 0x10020, "LTGT_COOP, stock/non-objective")
	assert_eq(HostSessionConfig.GAME_TYPE_SEARCH_AND_DESTROY, 0x90002, "LTGT_SD")
	assert_eq(HostSessionConfig.GAME_TYPE_CONQUER_AND_CONTROL, 0x50010, "LTGT_CAC")


func test_objective_bit_composition() -> void:
	assert_eq(HostSessionConfig.GAME_TYPE_OBJECTIVE_BIT, 0x20000,
			"the 0x0A sub-block-3 gate bit")
	assert_eq(HostSessionConfig.GAME_TYPE_COOP,
			HostSessionConfig.GAME_TYPE_TRAINING_COOP | HostSessionConfig.GAME_TYPE_OBJECTIVE_BIT,
			"objective Co-op composes from stock Co-op + the objective bit (0x30020)")
