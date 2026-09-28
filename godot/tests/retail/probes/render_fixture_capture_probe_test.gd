extends GutTest

# The render-fixture capture contract (RenderFixtureContract) and its probe's
# transactions. The contract's live legs are typed to their production owners
# (GameShell, GameWorld, Simulation, Terrain, Weather, LocalPlayerPresenter;
# ADR 0043 d12), so this file drives them over REAL fixtures (the packaged
# world, a bare Terrain, the null GameShell) and pins the rule they refuse
# with; the positive comparison legs (a spawned local player carrying the
# retail kit) run in the live render_fixture_capture probe over the retail
# fixture, never against a double.


func test_comparison_contract_uses_production_spawn_profile_and_presentation_witness() -> void:
	var contract := {
		"weapon": "WPN_M16BURST",
		"retail_hud_weapon_label": "M16 - Burst",
		"hud_enabled": true,
		"viewmodel_enabled": true,
		"terrain_enabled": true,
		"player_position_bms": Vector3(283.129364, -357.799469, 27.0),
		"yaw_deg": -90.0,
		"pitch_deg": -6.5,
	}

	assert_eq(RenderFixtureContract.comparison_spawn_profile(contract), {
		"player_class": 9,
		"primary": "WPN_M16BURST",
		"primary_clips": 9,
		"secondary": "",
		"secondary_clips": -1,
		"accessory": "",
		"accessory_clips": -1,
	}, "legacy full_frame keeps its committed spawn-profile shape")
	var hidden_profile_contract := {
		"capture_mode": "hud_hidden",
		"equipped_weapon": "WPN_M16BURST",
	}
	assert_eq(RenderFixtureContract.comparison_spawn_profile(hidden_profile_contract), {
		"team": 0,
		"player_class": 9,
		"primary": "WPN_M16BURST",
		"primary_clips": 9,
		"secondary": "",
		"secondary_clips": -1,
		"accessory": "",
		"accessory_clips": -1,
		"side_profiles": [
			{
				"team": 0,
				"nationality": 2,
				"division": 0,
				"combo": 1,
				"player_class": 9,
			},
			{
				"team": 1,
				"nationality": 7,
				"division": 0,
				"combo": 0,
				"player_class": 9,
			},
		],
	})
	var avatar_db := AvatarDatabase.new()
	assert_eq(avatar_db.load(ProjectSettings.globalize_path(
			"res://../fixtures/avatars/synth_avatars.def")), OK)
	var join_profile := avatar_db.character_join_profile(
			RenderFixtureContract.comparison_spawn_profile(hidden_profile_contract))
	assert_eq([join_profile.get_character_id(0), join_profile.get_character_id(1)],
			[0x0402, 0x8207],
			"the staged tree selections resolve to retail slot 0's character IDs")
	assert_eq([join_profile.get_player_class(0), join_profile.get_player_class(1)], [9, 9])
	# The reference leg: in the shipped table blue's 0x0402 wears the arms the retail
	# capture shows (IndoArms.3di, camo 1).
	var retail_avatars := RetailData.fixture("avatars/Avatars.def")
	if retail_avatars.is_empty():
		pending(RetailData.fixture_pending_text("avatars/Avatars.def"))
	else:
		var retail_db := AvatarDatabase.new()
		assert_eq(retail_db.load(retail_avatars), OK)
		var combo := retail_db.resolve_character_id(0x0402, 0)
		assert_not_null(combo)
		var arms := combo.get_arms()
		assert_not_null(arms, "blue 0x0402 authors an arms part")
		assert_eq(arms.graphic, "IndoArms.3di")
		assert_eq(arms.camo, Vector3i(1, 0, 0))


# The typed comparison legs over real owners that carry no matched spawn: the
# unloaded packaged world (no Simulation), the null shell (no presenter, no
# HUD witness) and a loaded minimal world whose spawn is not the retail kit.
# Every leg names what is missing instead of passing; the positive path (a
# spawned local player carrying WPN_M16BURST at 30/270, the retail arms) is
# the live probe's over the retail fixture.
