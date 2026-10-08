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


func _def_root() -> ResourceRoot:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(RetailData.def_root()), OK)
	return root


# One kit page: NUL-separated (name, ammoP, ammoS, flags) quads closed by a double NUL
# [orig: Buffer_CopyUntilDoubleNull @0x562f30].
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


func test_an_unlatched_team_commits_no_page() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The side selector is the S2C 0x04 tail byte [orig: byte_A85B48 @0x425499], and
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
	var profiles := PlayerProfiles.new()
	profiles.load_bytes(PackedByteArray(), false, _make_weapon_sav(), true, "")
	assert_eq(sim.use_player_profile(profiles), OK)

	var inv := sim.get_local_player_inventory()
	var held := []
	for row in inv.slots:
		held.append((row as PlayerInventorySlot).name)
	for name in RED_PAGE:
		assert_does_not_have(held, name,
				"%s is on the RED page; an unlatched team must not commit a side" % name)
	assert_does_not_have(held, OFF_PAGE,
			"%s is on no page at all" % OFF_PAGE)


# The kit page reaches the profile record. The PLAYER screen serializes the edited
# side's page as consecutive (name, primary, secondary, flags) entries (knife first,
# then the class-5 medpack, the three category picks, three grenade slots) and retail
# writes that block into the side's CLASS page before PlayerProfile_SaveToFiles.
# retail: PlayerInfo_SerializeWeaponLoadout @ 0x55e4b0; see docs/playerinfo/avatars-re.md.
