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
# Every name below exists in the reference fixture set's def/weapon.def (94 rows,
# 1-based ADM space; staged through RetailData.def_root()).
const BLUE_PAGE := ["WPN_KNIFE", "WPN_M4AUTO", "WPN_colt45"]
const RED_PAGE := ["WPN_KNIFE2", "WPN_DRAGUNOV", "WPN_357"]
# Deliberately absent from both pages: if it shows up in the local pool, the pool was
# built from something other than the profile page.
const OFF_PAGE := "WPN_M9Beretta"

var _sav_path := ""


func after_each() -> void:
	if not _sav_path.is_empty() and FileAccess.file_exists(_sav_path):
		DirAccess.remove_absolute(_sav_path)
	_sav_path = ""


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


func _write_weapon_sav() -> String:
	var path := ProjectSettings.globalize_path("user://weapon_profile_kit_test.sav")
	var f := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(f, "could not open %s for writing" % path)
	f.store_buffer(_make_weapon_sav())
	f.close()
	_sav_path = path
	return path


func test_a_malformed_profile_is_reported() -> void:
	# Retail's miss leaves PlayerProfile_InitDefaults' values installed [orig: @0x54bb40]:
	# both sides class 8, one weapon name per page (the playersav ctest pins the
	# shipped defaults and the page pick). A bad header must not be fatal.
	var path := ProjectSettings.globalize_path("user://weapon_profile_kit_test.sav")
	var f := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(f)
	f.store_buffer("NOTAPROFILE00000".to_ascii_buffer())
	f.close()
	_sav_path = path

	var sim := Simulation.new()
	assert_ne(sim.load_weapon_profile(path), OK, "a bad header must be reported")


func test_player_info_character_save_is_per_side_and_preserves_other_slots() -> void:
	var path := ProjectSettings.globalize_path("user://weapon_profile_kit_test.sav")
	var bytes := _make_weapon_sav()
	# Sentinel fields in slot 1: active slot-0 ACCEPT must not touch them.
	var slot1 := HEADER_BYTES + SLOT_BYTES
	bytes[slot1] = 0x44
	bytes[slot1 + 1] = 0x55
	bytes[slot1 + 2] = 0x66
	bytes.encode_u16(slot1 + 4, 0x9234)
	bytes[slot1 + SIDE_BYTES] = 0x33
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	file.store_buffer(bytes)
	file.close()
	_sav_path = path

	var profile := {
		"team": 0,
		"player_class": 9,
		"side_profiles": [
			{"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0400},
			{"avatar_a": 7, "avatar_b": 0, "avatar_packed": 0x8407},
		],
	}
	assert_eq(Simulation.save_weapon_profile_selection(path, profile), OK,
			"PLAYER_INFO ACCEPT atomically rewrites the active profile")
	var summary := Simulation.read_weapon_profile_summary(path)
	assert_true(summary.loaded)
	var blue := summary.blue
	var red := summary.red
	assert_eq(blue.player_class, 9)
	assert_eq(red.player_class, 9,
			"retail ACCEPT writes the class to both 0x8006 side blocks")
	assert_eq(blue.avatar_packed, 0x0400)
	assert_eq(red.avatar_packed, 0x8407,
			"the two side-specific packed character ids survive together")

	var rewritten := FileAccess.get_file_as_bytes(path)
	assert_eq(int(rewritten[slot1]), 0x44)
	assert_eq(int(rewritten[slot1 + 1]), 0x55)
	assert_eq(int(rewritten[slot1 + 2]), 0x66)
	assert_eq(int(rewritten.decode_u16(slot1 + 4)), 0x9234,
			"the other four profile slots are preserved")
	assert_eq(int(rewritten[slot1 + SIDE_BYTES]), 0x33,
			"saving slot 0 does not normalize another slot's class bytes")
	assert_false(FileAccess.file_exists("%s.tmp.%d" % [path, OS.get_process_id()]),
			"the atomic temp sibling is renamed away")
	# [orig: PlayerInfo_SaveFromDialog @0x55EE3F-0x55EE6D class loop,
	#  @0x55EE93-0x55EF38 selected-side avatar fields]


func test_character_save_refuses_to_replace_a_corrupt_existing_profile() -> void:
	var path := ProjectSettings.globalize_path("user://weapon_profile_kit_test.sav")
	var original := "NOTAPROFILE".to_ascii_buffer()
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	file.store_buffer(original)
	file.close()
	_sav_path = path
	var profile := {
		"team": 0,
		"player_class": 8,
		"side_profiles": [
			{"avatar_a": 0, "avatar_b": 0, "avatar_packed": 0x0200},
			{},
		],
	}
	assert_eq(Simulation.save_weapon_profile_selection(path, profile), ERR_FILE_CORRUPT)
	assert_eq(FileAccess.get_file_as_bytes(path), original,
			"a rejected profile remains recoverable and byte-identical")


func test_player_info_accept_writes_the_edited_sides_class_page() -> void:
	var path := _write_weapon_sav()
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
	assert_eq(Simulation.save_weapon_profile_selection(path, profile), OK)

	var expected := PackedByteArray()
	for entry in kit:
		for token in [String(entry["name"]), str(entry["ammo_primary"]),
				str(entry["ammo_secondary"]), str(entry["flags"])]:
			expected.append_array(token.to_ascii_buffer())
			expected.append(0)
	expected.append(0)
	var bytes := FileAccess.get_file_as_bytes(path)
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
	assert_eq(Simulation.save_weapon_profile_selection(path, bad), ERR_INVALID_PARAMETER,
			"an entry without a weapon name is refused before the file is touched")
