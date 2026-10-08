extends GutTest

# The player profile (docs/playerinfo/player-sav-re.md): the shell's one in-memory
# profile over player.sav and weapon.sav, saved beside the game, its name the
# callsign, and single player's session words (D-NET-375). The format and the
# load/save rules are the ctests' (playersav_player_sav, profile_player_profiles);
# this pins the binding and the shell's seams over them.

const PLAYER_SAV_BYTES := 77464
const WEAPON_SAV_BYTES := 337996

var _run_dir := ""


func before_each() -> void:
	_run_dir = OS.get_cache_dir().path_join("opennova_player_profile_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_run_dir), OK)
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir]))


func after_each() -> void:
	LaunchFlags.clear_args_override()
	TestFs.remove_dir_recursive(_run_dir)


func test_the_profile_saves_both_files_beside_the_game() -> void:
	# A first run: no files, record 0 named for the account.
	assert_eq(PlayerProfile.load_for(null), OK)
	assert_true(PlayerProfile.store().is_first_run())
	# GetUserNameA's 16-byte buffer: a longer account name leaves the field empty.
	var account := PlayerProfiles.account_name()
	assert_eq(PlayerProfile.store().get_player_name(), account if account.length() < 16 else "")
	assert_eq(PlayerProfile.save(), OK)
	var player := FileAccess.get_file_as_bytes(_run_dir.path_join("player.sav"))
	var weapon := FileAccess.get_file_as_bytes(_run_dir.path_join("weapon.sav"))
	assert_eq(player.size(), PLAYER_SAV_BYTES)
	assert_eq(weapon.size(), WEAPON_SAV_BYTES)
	assert_eq(player.slice(0, 8).get_string_from_ascii(), "FPBC0211")
	assert_eq(player.slice(PLAYER_SAV_BYTES - 8).get_string_from_ascii(), "fsasyaof",
			"player.sav ends with the program's eight-byte trailer")
	assert_eq(weapon.slice(0, 16), player.slice(0, 16), "both files carry the same header")


func test_the_accept_names_the_record_and_the_name_is_the_callsign() -> void:
	PlayerProfile.load_for(null)
	var profile := {
		"name": "Sandman",
		"player_class": 8,
		"side_profiles": [{"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0200}, {}],
	}
	assert_eq(PlayerProfile.accept_player_info(profile), OK)
	assert_eq(PlayerProfile.load_callsign(), "Sandman")
	assert_false(FileAccess.file_exists(_run_dir.path_join("player.sav")),
			"the ACCEPT writes the record in memory; a save point writes the file")
	assert_eq(PlayerProfile.save(), OK)
	PlayerProfile.load_for(null)
	assert_eq(PlayerProfile.load_callsign(), "Sandman", "the name reads back from player.sav")
	assert_false(PlayerProfile.store().is_first_run())
	# A name that starts with white space empties the name and flags the record.
	PlayerProfile.accept_player_info({"name": " Sandman", "player_class": 8,
			"side_profiles": [{"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0200}, {}]})
	assert_eq(PlayerProfile.store().get_player_name(), "")
	assert_true(PlayerProfile.store().is_nameless())


func test_the_record_reads_every_named_word() -> void:
	PlayerProfile.load_for(null)
	var profiles := PlayerProfile.store()
	var words := PlayerProfiles.word_names()
	assert_eq(words.size(), 40)
	assert_eq(words[0], "last_campaign", "in offset order")
	assert_eq(profiles.get_word("mouse_sensitivity"), 128)
	assert_eq(profiles.get_word("sp_gps_icons"), 1)
	assert_eq(profiles.get_word("intro_pending"), -1, "a fresh record's intro word")
	assert_eq(profiles.get_binding_count(), 109)
	assert_eq(profiles.get_macros().size(), 10)
	assert_eq(profiles.get_flags(), 0, "record 0 is named for the account")


# D-NET-375: single player's charattr restriction words are the current record's
# [orig: Game_ApplySessionSettingsToGlobals @0x551F15..0x551F3F]; the host boot's
# restriction step disables ATTRIBUTES and SCOPE_MUTE for them (the S2C 0x42
# packing: 0x02 ATTRIBUTES, 0x04 XHAIR_MUTE, 0x08 RECOIL_MUTE, 0x10 SCOPE_MUTE).
func test_single_player_takes_the_profile_restriction_words() -> void:
	var profiles := PlayerProfiles.new()
	profiles.load_bytes(PackedByteArray(), false, PackedByteArray(), false, "")
	assert_true(profiles.set_word("sp_no_char_abilities", 1))
	assert_true(profiles.set_word("sp_no_scope_drift", 1))
	assert_false(profiles.set_word("not_a_word", 1))
	var world := WorldFixture.make_world(self)
	world.set_player_profiles(profiles)
	assert_eq(WorldFixture.load_mission(world, ProjectSettings.globalize_path(
			WorldFixture.BOOT_FIXTURE_DIR)), OK)
	var sim := world.get_sim()
	assert_not_null(sim)
	if sim == null:
		return
	assert_eq(sim.get_charattr_disabled_word() & 0x12, 0x12,
			"ATTRIBUTES and SCOPE_MUTE are disabled for the single-player session")
	assert_eq(sim.get_charattr_disabled_word() & 0x0C, 0,
			"the words the record leaves 0 disable nothing")


func test_a_fresh_profile_restricts_nothing() -> void:
	var profiles := PlayerProfiles.new()
	profiles.load_bytes(PackedByteArray(), false, PackedByteArray(), false, "")
	var world := WorldFixture.make_world(self)
	world.set_player_profiles(profiles)
	assert_eq(WorldFixture.load_mission(world, ProjectSettings.globalize_path(
			WorldFixture.BOOT_FIXTURE_DIR)), OK)
	var sim := world.get_sim()
	assert_not_null(sim)
	if sim != null:
		assert_eq(sim.get_charattr_disabled_word(), 0)


# The mission start's session copy of the record's input words (docs/playerinfo/
# player-sav-re.md "The controls words"): the look's sensitivity and Y invert
# and the auto-reload global, from the record the world hands the sim; the
# in-game Accept's live writes of the two mouse words.
# [orig: Game_ApplySessionSettingsToGlobals @0x551612, @0x55161e, @0x551a40;
#  UI_IngameOptionsDialogEventHandler @0x55525e, @0x555271]
func test_the_session_copy_takes_the_profile_input_words() -> void:
	var profiles := PlayerProfiles.new()
	profiles.load_bytes(PackedByteArray(), false, PackedByteArray(), false, "")
	assert_true(profiles.set_word("invert_mouse", 1))
	assert_true(profiles.set_word("mouse_sensitivity", 511))
	assert_true(profiles.set_word("auto_reload", 0))
	var world := WorldFixture.make_world(self)
	world.set_player_profiles(profiles)
	assert_eq(WorldFixture.load_mission(world, ProjectSettings.globalize_path(
			WorldFixture.BOOT_FIXTURE_DIR)), OK)
	var sim := world.get_sim()
	assert_not_null(sim)
	if sim == null:
		return
	assert_eq(sim.get_session_mouse_sensitivity(), 511)
	assert_true(sim.is_session_mouse_inverted())
	assert_false(sim.is_session_auto_reload(), "the record's auto-reload word")
	# The in-game Accept writes the two mouse words at once; the auto-reload
	# global waits for the next mission start.
	assert_true(profiles.set_word("invert_mouse", 0))
	assert_true(profiles.set_word("mouse_sensitivity", 64))
	assert_true(profiles.set_word("auto_reload", 1))
	assert_eq(sim.apply_ingame_options(profiles), OK)
	assert_eq(sim.get_session_mouse_sensitivity(), 64)
	assert_false(sim.is_session_mouse_inverted())
	assert_false(sim.is_session_auto_reload(), "auto-reload waits for the session copy")


# /noreload forces the auto-reload global off whatever the record holds
# [orig: dword_B4C4F4 @0x4A76E9; @0x551a48].
func test_noreload_forces_auto_reload_off() -> void:
	LaunchFlags.set_args_override(PackedStringArray(["--working-dir", _run_dir, "/noreload"]))
	var profiles := PlayerProfiles.new()
	profiles.load_bytes(PackedByteArray(), false, PackedByteArray(), false, "")
	var world := WorldFixture.make_world(self)
	world.set_player_profiles(profiles)
	assert_eq(WorldFixture.load_mission(world, ProjectSettings.globalize_path(
			WorldFixture.BOOT_FIXTURE_DIR)), OK)
	var sim := world.get_sim()
	assert_not_null(sim)
	if sim != null:
		assert_false(sim.is_session_auto_reload())


# PLAYER_INFO's two boxes over the record: the auto-medic box is the INVERSE of
# the +1660 word [orig: PlayerInfo_PopulateAllControls @0x56074c;
# PlayerInfo_SaveFromDialog @0x55ef5f, @0x55ef92].
func test_the_auto_boxes_read_and_write_the_record() -> void:
	PlayerProfile.load_for(null)
	var store := PlayerProfile.store()
	assert_true(store.is_auto_reload_checked() and store.is_auto_medic_checked(),
			"a fresh record has both boxes checked")
	store.set_auto_reload_checked(false)
	store.set_auto_medic_checked(false)
	assert_eq(store.get_word("auto_reload"), 0)
	assert_eq(store.get_word("auto_medic_off"), 1, "a clear auto-medic box stores 1")
	assert_false(store.is_auto_reload_checked() or store.is_auto_medic_checked())
	store.set_auto_medic_checked(true)
	assert_eq(store.get_word("auto_medic_off"), 0)
