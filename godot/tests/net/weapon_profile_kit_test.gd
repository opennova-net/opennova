extends GutTest

# The multiplayer loadout SOURCE (net-re §5.66, D-NET-183).
#
# In a live session retail does NOT use the mission's .bms kit: Mission_LoadBMSFile
# fseeks past both the loadout and availability chunks [orig: @0x40F4E0, gate @0x40f694],
# and Game_StartMission instead copies the assigned side's per-CLASS page out of the
# player profile into restrictionData and submits it with THAT page's class byte
# [orig: @0x525793..@0x525836]. One integer picks both, which is why a stock kit is
# class-appropriate at the host's accept gate.
#
# Two things are pinned here, both of which broke real sessions:
#   1. the weapon.sav offsets, read back through the sim (a wrong stride silently
#      hands the host somebody else's kit);
#   2. THE ONE-BUFFER RULE — retail has a single restrictionData that feeds BOTH the
#      local slot pool (Player_InitPlayer -> AvatarDef_BuildDisplayList) and the C2S
#      0x2F (NetPacket_SendLoadoutSubmit). Sourcing the wire from the profile while the
#      local pool still came from the mission kit reproduces the original defect
#      mirrored: we would tell the host one kit and hold another.

const HEADER_BYTES := 16
const SLOT_BYTES := 0x1080C   # 67596
const SIDE_BYTES := 0x8006    # 32774
const PAGE_BYTES := 2048
const FIRST_PAGE_OFFSET := 6

const BLUE_CLASS := 8
const RED_CLASS := 6
# Every name below is a row of ArmoryFixture's loadout catalog (the shipped
# weapon.def's rows and loadout fields, in the shipped order).
const BLUE_PAGE := ["WPN_KNIFE", "WPN_M4AUTO", "WPN_colt45"]
const RED_PAGE := ["WPN_KNIFE2", "WPN_DRAGUNOV", "WPN_357"]
# Deliberately absent from both pages: if it shows up in the local pool, the pool was
# built from something other than the profile page.
const OFF_PAGE := "WPN_M9Beretta"


func _def_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ArmoryFixture.stage_loadout_catalog("weapon_profile_kit")), OK)
	return root


func _page_blob(names: Array) -> PackedByteArray:
	var blob := PackedByteArray()
	for n in names:
		for token in [String(n), "-1", "-1", "-1"]:
			blob.append_array(token.to_ascii_buffer())
			blob.append(0)
	blob.append(0)  # the terminating empty string
	return blob


func _blit(buf: PackedByteArray, offset: int, src: PackedByteArray) -> void:
	assert_lt(offset + src.size(), buf.size(), "page blob overruns the record")
	for i in src.size():
		buf[offset + i] = src[i]


# A synthetic weapon.sav: 16-byte "FPBC"/"0211" header then five 0x1080C records, each
# [BLUE side 0x8006][RED side 0x8006][single-player page 2048].
func _make_weapon_sav() -> PackedByteArray:
	var buf := PackedByteArray()
	buf.resize(HEADER_BYTES + 5 * SLOT_BYTES)
	buf.fill(0)
	_blit(buf, 0, "FPBC0211".to_ascii_buffer())
	# Only slot 0 is populated — that is the record the sim keeps.
	var base := HEADER_BYTES
	buf[base] = BLUE_CLASS
	buf[base + SIDE_BYTES] = RED_CLASS
	_blit(buf, base + FIRST_PAGE_OFFSET + PAGE_BYTES * (BLUE_CLASS - 5),
			_page_blob(BLUE_PAGE))
	_blit(buf, base + SIDE_BYTES + FIRST_PAGE_OFFSET + PAGE_BYTES * (RED_CLASS - 5),
			_page_blob(RED_PAGE))
	return buf


# The profile over the bytes of one synthetic weapon.sav (no player.sav).
func _profiles(weapon_sav: PackedByteArray) -> PlayerProfiles:
	var profiles := PlayerProfiles.new()
	profiles.load_bytes(PackedByteArray(), false, weapon_sav, true, "")
	return profiles


func test_a_malformed_profile_leaves_the_defaults() -> void:
	# The load's header gate reads past a file that is not a profile and leaves
	# PlayerProfile_InitDefaults' records in place [orig: @0x54f6fd]: both sides
	# class 8, the default class pages (the playersav ctest pins them).
	var profiles := _profiles("NOTAPROFILE00000".to_ascii_buffer())
	var summary := profiles.character_summary()
	assert_true(summary.loaded)
	assert_eq(summary.blue.player_class, 8)
	assert_eq(summary.red.player_class, 8)
	assert_eq(String(summary.blue.kit[0]), "WPN_M4AUTO", "the rifleman's default page")
	var sim := Simulation.new()
	assert_eq(sim.use_player_profile(profiles), OK, "the defaults are a usable profile")


func test_an_unlatched_team_commits_no_page() -> void:
	# The side selector is the S2C 0x04 tail byte [orig: g_LocalTeam @0x425499], and
	# side_for_team maps anything that is not 1 or 3 to the RED block [orig: @0x525798].
	# Retail cannot reach the page copy before that byte is latched — admission delivers
	# 0x04 long before Game_StartMission runs — but our catalog can land first. Copying at
	# team 0 would therefore commit the WRONG side's page. This asserts we wait instead.
	#
	# This case caught a live defect: the first revision seeded unconditionally and a
	# blue-side joiner briefly held the red page.
	var sim := Simulation.new()
	assert_true(sim.enable_join("127.0.0.1", 32768, "ProfileKitTest"),
			"enable_join should arm the joiner role")
	assert_true(sim.is_joiner())
	assert_eq(sim.load_weapon_table(_def_root(), "weapon.def"), OK)
	# Loaded AFTER the catalog on purpose: the shell can only read the profile once the
	# sim exists, so this edge has to be handled too. An earlier revision seeded only at
	# catalog load and silently shipped the defaults.
	assert_eq(sim.use_player_profile(_profiles(_make_weapon_sav())), OK)

	var inv := sim.get_local_player_inventory()
	var held := []
	for row in inv.slots:
		held.append((row as PlayerInventorySlot).name)
	for name in RED_PAGE:
		assert_does_not_have(held, name,
				"%s is on the RED page; an unlatched team must not commit a side" % name)
	assert_does_not_have(held, OFF_PAGE,
			"%s is on no page at all" % OFF_PAGE)


func test_player_info_character_save_is_per_side_and_preserves_other_slots() -> void:
	var bytes := _make_weapon_sav()
	# Sentinel fields in slot 1: active slot-0 ACCEPT must not touch them.
	var slot1 := HEADER_BYTES + SLOT_BYTES
	bytes[slot1] = 0x44
	bytes[slot1 + 1] = 0x55
	bytes[slot1 + 2] = 0x66
	bytes.encode_u16(slot1 + 4, 0x9234)
	bytes[slot1 + SIDE_BYTES] = 0x33
	# Slot 0's red side carries a character the ACCEPT on blue must leave.
	bytes[HEADER_BYTES + SIDE_BYTES + 1] = 9
	bytes[HEADER_BYTES + SIDE_BYTES + 2] = 2
	bytes.encode_u16(HEADER_BYTES + SIDE_BYTES + 4, 0x8A49)
	var profiles := _profiles(bytes)

	var profile := {
		"team": 0,
		"player_class": 9,
		"side_profiles": [
			{"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0400},
			{"avatar_a": 7, "avatar_b": 0, "avatar_packed": 0x8407},
		],
	}
	assert_eq(profiles.apply_character_selection(profile), OK,
			"PLAYER_INFO ACCEPT writes the current record in memory")
	var summary := profiles.character_summary()
	assert_true(summary.loaded)
	assert_eq(summary.blue.player_class, 9)
	assert_eq(summary.red.player_class, 9,
			"retail ACCEPT writes the class to both 0x8006 side blocks")
	assert_eq(summary.blue.avatar_packed, 0x0400, "the shown side takes its character")
	assert_eq([summary.red.avatar_a, summary.red.avatar_b, summary.red.avatar_packed],
			[9, 2, 0x8A49], "the side not shown keeps its bytes, whatever the snapshot carries")

	profile["team"] = 1
	assert_eq(profiles.apply_character_selection(profile), OK)
	summary = profiles.character_summary()
	assert_eq(summary.red.avatar_packed, 0x8407, "shown, red takes its character")
	assert_eq(summary.blue.avatar_packed, 0x0400)
	profile.erase("team")
	assert_eq(profiles.apply_character_selection(profile), ERR_INVALID_PARAMETER,
			"a snapshot with no shown side is refused")

	var rewritten := profiles.weapon_sav_bytes()
	assert_eq(int(rewritten[slot1]), 0x44)
	assert_eq(int(rewritten[slot1 + 1]), 0x55)
	assert_eq(int(rewritten[slot1 + 2]), 0x66)
	assert_eq(int(rewritten.decode_u16(slot1 + 4)), 0x9234,
			"the other four profile slots are preserved")
	assert_eq(int(rewritten[slot1 + SIDE_BYTES]), 0x33,
			"saving slot 0 does not normalize another slot's class bytes")
	# [orig: PlayerInfo_SaveFromDialog @0x55EE3F-0x55EE6D class loop,
	#  @0x55EE93-0x55EF38 selected-side avatar fields, each indexed by
	#  g_PlayerInfoTeamIndex: @0x55EEAB, @0x55EEEE, @0x55EF31]


func test_a_corrupt_profile_is_replaced_at_the_save() -> void:
	# The save writes the image the load left, which for a file that was not a
	# profile is the defaults with the screen's edits [orig: PlayerProfile_SaveToFiles
	# @0x54be00 writes its records whatever the load read].
	var profiles := _profiles("NOTAPROFILE".to_ascii_buffer())
	var profile := {
		"team": 0,
		"player_class": 8,
		"side_profiles": [
			{"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0200},
			{},
		],
	}
	assert_eq(profiles.apply_character_selection(profile), OK)
	var saved := profiles.weapon_sav_bytes()
	assert_eq(saved.size(), HEADER_BYTES + 5 * SLOT_BYTES)
	assert_eq(saved.slice(0, 8).get_string_from_ascii(), "FPBC0211")
	assert_eq(int(saved.decode_u16(HEADER_BYTES + 4)), 0x0200)


func test_player_info_accept_writes_the_edited_sides_class_page() -> void:
	var profiles := _profiles(_make_weapon_sav())
	var kit := [
		{"name": "WPN_KNIFE2", "ammo_primary": -1, "ammo_secondary": -1, "flags": -1},
		{"name": "WPN_DRAGUNOV", "ammo_primary": 5, "ammo_secondary": -1, "flags": 0},
		{"name": "WPN_357", "ammo_primary": 3, "ammo_secondary": -1, "flags": 1},
		{"name": "WPN_M4AUTO", "ammo_primary": -1, "ammo_secondary": -1, "flags": -1},
	]
	var profile := {
		"team": 1,
		"player_class": RED_CLASS,
		"side_profiles": [
			{"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0400},
			{"avatar_a": 7, "avatar_b": 0, "avatar_packed": 0x8407},
		],
		"kit": kit,
	}
	assert_eq(profiles.apply_character_selection(profile), OK)

	var expected := PackedByteArray()
	for entry in kit:
		for token in [String(entry["name"]), str(entry["ammo_primary"]),
				str(entry["ammo_secondary"]), str(entry["flags"])]:
			expected.append_array(token.to_ascii_buffer())
			expected.append(0)
	expected.append(0)
	var bytes := profiles.weapon_sav_bytes()
	var page_at := HEADER_BYTES + SIDE_BYTES + FIRST_PAGE_OFFSET \
			+ PAGE_BYTES * (RED_CLASS - 5)
	assert_eq(bytes.slice(page_at, page_at + expected.size()), expected,
			"the RED class-%d page carries the serialized kit verbatim" % RED_CLASS)
	# The untouched BLUE page of the same class keeps the fixture's block.
	var blue_at := HEADER_BYTES + FIRST_PAGE_OFFSET + PAGE_BYTES * (BLUE_CLASS - 5)
	var blue_blob := _page_blob(BLUE_PAGE)
	assert_eq(bytes.slice(blue_at, blue_at + blue_blob.size()), blue_blob,
			"the other side's page is not rewritten by the kit")

	var bad := profile.duplicate(true)
	bad["kit"] = [{"name": "", "ammo_primary": -1, "ammo_secondary": -1, "flags": -1}]
	var before := profiles.weapon_sav_bytes()
	assert_eq(profiles.apply_character_selection(bad), ERR_INVALID_PARAMETER,
			"an entry without a weapon name is refused before the record is touched")
	assert_eq(profiles.weapon_sav_bytes(), before)
