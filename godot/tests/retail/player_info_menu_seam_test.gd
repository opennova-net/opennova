extends GutTest

# The player.mnu PLAYER_INFO screen seam: the companion (player_info_menu_companion.gd)
# drives the JO character screen by control NAME -- fills NATIONALITY / DIVISION /
# COMBO_LIST / PLAYERVOICE from Avatars.def, runs the nationality->division->combo
# cascade, and re-filters by the SIDE_BLUE/SIDE_RED team -- riding a MenuDriver over
# a real MnuDocument (the compiled-menu surface; the Control tree is gone). This pins
# the wiring against the witnessed original (docs/playerinfo/avatars-re.md,
# D-PLAYERINFO-5/7); the live in-engine render is the manual smoke. The table is the minted
# synth_avatars.def fixture (tests/fixtures/minimal_avatars_gen.cpp), injected directly
# (root-less) like the avatar data tests.

const AVATARS_FIXTURE := "res://../fixtures/avatars/synth_avatars.def"


# Strings is a shared autoload; keep the registry clean so the raw-key tests below see
# no gametext table and the resolved-names test starts from a known state.
func before_each() -> void:
	Strings.clear()


func after_all() -> void:
	Strings.clear()


func _load_db() -> AvatarDatabase:
	var db := AvatarDatabase.new()
	var path := ProjectSettings.globalize_path(AVATARS_FIXTURE)
	assert_eq(db.load(path), OK, "the minted avatar table loads")
	return db


# A resource root whose Avatars.def is the minted table: the companion reads the
# table under the name the engine binds [orig: CAvatarDefs_Init @ 0x57b180], so the
# fixture is staged under that name in the cache dir (ResourceRoot.set_root_dir
# rejects user://). Null when the fixture cannot be staged.
func _staged_avatars_root() -> ResourceRoot:
	var bytes := FileAccess.get_file_as_bytes(AVATARS_FIXTURE)
	if bytes.is_empty():
		return null
	var dir := OS.get_cache_dir().path_join("opennova_player_info_seam_test")
	DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(dir.path_join("Avatars.def"), FileAccess.WRITE)
	if f == null:
		return null
	f.store_buffer(bytes)
	f.close()
	var root := ResourceRoot.new()
	return root if root.set_root_dir(dir) == OK else null


# --- Driver harness (the compiled-menu seam) ----------------------------------

# A synthetic PLAYER_INFO screen: the cascade combos, the team radios (SIDE_BLUE
# ships CHECKED, both share the authored GROUP), the name edit and ACCEPT --
# exactly the control names player.mnu authors.
func test_voice_preview_requests_selected_avatar_voice() -> void:
	var root := _staged_avatars_root()
	assert_not_null(root, "the minted table stages as the root's Avatars.def")
	if root == null:
		return

	var player_mnu := RetailData.fixture("mnu/jo_player.mnu")
	if player_mnu.is_empty():
		pending(RetailData.fixture_pending_text("mnu/jo_player.mnu"))
		return
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(player_mnu)), OK,
		"the retail player menu fixture loads")
	var driver := MenuDriver.new()
	assert_true(driver.open_document(doc, root, null, null, "player.mnu", "PLAYER_INFO"),
		"the retail player menu opens on the driver")

	var companion := PlayerInfoMenuCompanion.new()
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", root)
	watch_signals(driver)
	assert_true(driver.has_widget("TESTPLAYERVOICE"),
		"the retail PLAYER_INFO screen authors its voice-preview button")
	driver.widget_activated.emit(driver.widget_id("TESTPLAYERVOICE"), "TESTPLAYERVOICE")

	# The table's initially selected N00/D00 head (SYN_HEAD_BOONIE) carries voice 1.
	# Retail formats that avatar-derived fallback as VOICE_1 and plays it from the
	# dedicated menu.lwf bank. [orig: PlayerInfo_PreviewVoice @ 0x55ff70]
	assert_signal_emitted_with_parameters(driver, "sound_requested", ["menu.lwf", "VOICE_1"])


# --- Loadout (PRIMARY/SECONDARY/ACCESSORY) ------------------------------------

# The shipped weapon.def from the reference fixture set; the loadout legs pend
# without it.
func _load_weapons() -> WeaponDatabase:
	var wdb := WeaponDatabase.new()
	assert_eq(wdb.load(RetailData.fixture("def/weapon.def")), OK, "the shipped weapon.def loads")
	return wdb


# A synthetic PLAYER_INFO loadout screen: the three weapon combos, PLAYERCLASS
# carrying the CHARTYPE values 5..9 as authored `value=` items (as the .mnu's
# static items do), the ammo/type/grenade combos, STATIC_TOTAL_WEIGHT, and the
# bare *_ICON windows.
func _loadout_screen_xml() -> String:
	var type_items := ('<ITEMS><ITEM value="0">FMJ</ITEM><ITEM value="1">AP</ITEM>'
			+ '<ITEM value="2">SP</ITEM></ITEMS>')
	var body := ""
	var y := 10
	for n in ["PRIMARY", "SECONDARY", "ACCESSORY"]:
		body += MenuDriverFixture.wnd("combobox", n, y)
		body += MenuDriverFixture.wnd("combobox", n + "_AMMO1", y + 24)
		body += MenuDriverFixture.wnd("combobox", n + "_AMMO2", y + 48)
		body += MenuDriverFixture.wnd("window", n + "_ICON", y + 72)
		y += 100
	for n in ["PRIMARY", "SECONDARY"]:
		# player.mnu authors the TYPE statics (FMJ/AP/SP, values 0/1/2); the
		# companion selects/locks them but never refills.
		body += MenuDriverFixture.wnd("combobox", n + "_AMMO1_TYPE", y, type_items)
		y += 24
	for n in ["GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]:
		body += MenuDriverFixture.wnd("combobox", n, y)
		y += 24
	body += MenuDriverFixture.wnd("static", "STATIC_TOTAL_WEIGHT", y)
	var class_items := ""
	for v in range(5, 10):  # Medic..Engineer = values 5..9
		class_items += '<ITEM value="%d">class %d</ITEM>' % [v, v]
	body += MenuDriverFixture.wnd("combobox", "PLAYERCLASS", y + 24, "<ITEMS>%s</ITEMS>" % class_items)
	return MenuDriverFixture.screen_xml("PLAYER_INFO", body)


var _ammo_driver: MenuDriver = null  # the driver behind the current _make_ammo_companion


func _make_loadout_driver(with_frame := false) -> MenuDriver:
	var driver := MenuDriver.new()
	if with_frame:
		# The icon mounts are frame children placed by widget_frame_rect.
		var frame := MenuFrame.new()
		frame.size = Vector2(800, 600)
		add_child_autofree(frame)
		driver.attach(frame, null)
	assert_true(driver.open_document(MenuDriverFixture.doc_from_xml(self, _loadout_screen_xml()),
			null, null, null, "player.mnu"), "the loadout document opens on the driver")
	return driver


# A wired companion over the loadout screen (medic/blue), for the ammo cases —
# built through the public seams only (set_weapon_database + on_menu_built).
func _make_ammo_companion(wdb: WeaponDatabase = null, with_frame := false) -> PlayerInfoMenuCompanion:
	var companion := PlayerInfoMenuCompanion.new()
	companion.set_weapon_database(wdb if wdb != null else _load_weapons())
	_ammo_driver = _make_loadout_driver(with_frame)
	companion.on_menu_built(_ammo_driver, "player.mnu", "PLAYER_INFO", null)
	return companion


func _ammo_id(name: String) -> int:
	return _ammo_driver.widget_id(name)


func _weight_text() -> String:
	return _ammo_driver.get_widget_text(_ammo_id("STATIC_TOTAL_WEIGHT"))


# Select the named weapon in a parent slot combo and fire the selection relay the
# way a user pick would (select_row emits the "combo" value change). The row model
# is public: row = position in the filtered slot list + 1 (row 0 is NONE)
# [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430].
func _select_weapon(wdb: WeaponDatabase, control: String, slot: int,
		class_mask: int, weapon_name: String) -> WeaponDef:
	var combo := _ammo_id(control)
	var defs := wdb.get_slot_weapons(slot, class_mask, 2)  # blue
	for i in defs.size():
		var w := defs[i] as WeaponDef
		if w.name.nocasecmp_to(weapon_name) == 0:
			_ammo_driver.select_row(combo, i + 1)  # emits -> the witnessed refill
			return w
	assert_true(false, "%s offers %s" % [control, weapon_name])
	return null


# The expected *_AMMO2 sub-weapon, computed from the public table walk the companion
# mirrors [orig: the stricmp walk in PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0].
func _expected_sub(wdb: WeaponDatabase, parent: WeaponDef) -> WeaponDef:
	var parent_round := parent.round_type
	for k in range(1, parent.loadout_subclasses + 1):
		var cand := wdb.get_weapon(parent.index + k)
		if cand == null:
			return null
		if cand.round_type.nocasecmp_to(parent_round) != 0:
			return cand
	return null


func _items(name: String) -> Array:
	return Array(_ammo_driver.get_widget_items(_ammo_id(name)))


func test_populates_loadout_slots_filtered_by_class_and_team() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _companion := _make_ammo_companion(wdb)

	var primary := _ammo_id("PRIMARY")
	# Medic (class mask 1), blue (team mask 2): the DB's filtered set plus a leading NONE row.
	var expected := wdb.get_slot_weapons(WeaponDatabase.SLOT_PRIMARY, 1, 2)
	assert_gt(expected.size(), 0, "the fixture has medic/blue primary weapons")
	assert_eq(_ammo_driver.item_count(primary), expected.size() + 1, "PRIMARY = NONE + the filtered weapons")
	assert_eq(_ammo_driver.item_text(primary, 0), "None", "NONE leads the slot (fallback with no string table)")


func test_loadout_class_filter_includes_and_excludes() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var _companion := _make_ammo_companion(_load_weapons())

	# Medic (value 5): WPN_M4AUTO (charfilter medic|rifleman|engineer, blue) is a primary -> present.
	assert_true(_items("PRIMARY").has("WPN_M4AUTO"), "M4 shows for Medic")

	# Sniper (value 6): M4's charfilter excludes sniper -> absent after re-fill.
	# select_row emits the "combo" value change — the class-mask refilter relay.
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 1)
	assert_false(_items("PRIMARY").has("WPN_M4AUTO"), "M4 is hidden for Sniper")


func test_snapshot_carries_the_selected_loadout_weapon_ids() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var companion := _make_ammo_companion(wdb)

	var primary_defs := wdb.get_slot_weapons(
			WeaponDatabase.SLOT_PRIMARY, 1, 2)
	var selected_primary := ""
	var selected_row := -1
	for i in primary_defs.size():
		var weapon: WeaponDef = primary_defs[i]
		if weapon.name.nocasecmp_to("WPN_M4AUTO") == 0:
			selected_primary = weapon.name
			selected_row = i + 1 # row 0 is NONE
			break
	assert_false(selected_primary == null, "the fixture offers M4AUTO for medic/blue")
	_ammo_driver.select_row(_ammo_id("PRIMARY"), selected_row, false)  # silent reselect

	var profile := companion.snapshot()
	assert_eq(String(profile.get("primary", "")), selected_primary,
		"PLAYER_INFO ACCEPT preserves the selected primary's weapon.def id")
	assert_eq(int(profile.get("player_class", 0)), 5,
		"the selected class travels with the spawn loadout")


func test_real_player_mnu_loadout_populates() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var player_mnu := RetailData.fixture("mnu/jo_player.mnu")
	if player_mnu.is_empty():
		pending(RetailData.fixture_pending_text("mnu/jo_player.mnu"))
		return
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(FileAccess.get_file_as_bytes(player_mnu)), OK,
			"the shipped jo_player.mnu fixture loads")
	var driver := MenuDriverFixture.driver_over(self, doc, "player.mnu", "PLAYER_INFO")

	assert_true(driver.has_widget("PRIMARY"), "the real player.mnu authors a PRIMARY combobox")
	var pclass := driver.widget_id("PLAYERCLASS")
	assert_gte(pclass, 0, "the real player.mnu authors a PLAYERCLASS combobox")
	assert_gt(driver.item_count(pclass), 0, "PLAYERCLASS carries its static class items")

	var companion := PlayerInfoMenuCompanion.new()
	companion.set_database(_load_db())
	companion.set_weapon_database(_load_weapons())  # injected (root-less unit), as if weapon.def had loaded
	companion.on_menu_built(driver, "player.mnu", "PLAYER_INFO", null)

	assert_gt(driver.item_count(driver.widget_id("PRIMARY")), 1,
		"PRIMARY populates (NONE + weapons) through the real menu's authored tree")
	# The ammo/weight tail fills through the same document: the default NONE
	# selection hides PRIMARY_AMMO1, and the weight readout renders the witnessed
	# "%s %.1f %s (%s)" shape (fallback strings, no tables registered).
	var primary_ammo := driver.widget_id("PRIMARY_AMMO1")
	assert_gte(primary_ammo, 0, "the real player.mnu authors PRIMARY_AMMO1")
	assert_false(driver.is_widget_shown(primary_ammo), "NONE selected -> the ammo combo hides")
	var weight := driver.widget_id("STATIC_TOTAL_WEIGHT")
	assert_gte(weight, 0, "the real player.mnu authors STATIC_TOTAL_WEIGHT")
	# The built PLAYERCLASS carries a default selection, so default grenades may
	# already weigh in — pin the witnessed "%s %.1f %s (%s)" shape, not the sum.
	assert_true(driver.get_widget_text(weight).begins_with("Total Weight "),
		"the weight readout renders through the compiled static")
	assert_true(driver.get_widget_text(weight).contains(" lbs ("),
		"the readout carries the witnessed format tail")


# --- Ammo combos + weight + icons (D-PLAYERINFO-11) -----------------------------

func test_primary_ammo_rows_follow_selected_weapon() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	var ammo := _ammo_id("PRIMARY_AMMO1")
	var maxclips := w.maxclips
	assert_true(_ammo_driver.is_widget_shown(ammo), "a clip-carrying weapon shows its ammo combo")
	assert_eq(_ammo_driver.item_count(ammo), maxclips,
		"rows 1..maxclips [orig: PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0]")
	assert_eq(_ammo_driver.item_text(ammo, 0),
		"%d - %s" % [w.clipsize, w.round_type],
		"row labels are the witnessed \"%d - %s\" rounds + round type")
	assert_eq(_ammo_driver.selected_row(ammo), maxclips - 1,
		"the untouched default selects the full (maxclips) row")


func test_none_selection_hides_ammo_and_clears_icon() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	# The icon mounts are frame children, so this case runs with a real MenuFrame.
	var _presenter := _make_ammo_companion(wdb, true)
	_select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	_ammo_driver.select_row(_ammo_id("PRIMARY"), 0)  # back to NONE
	assert_false(_ammo_driver.is_widget_shown(_ammo_id("PRIMARY_AMMO1")), "NONE hides the ammo combo")
	var rect := _ammo_driver.get_frame().find_child("PRIMARYLoadoutIcon", true, false) as TextureRect
	assert_not_null(rect, "the companion mounts an icon rect over PRIMARY_ICON")
	if rect != null:
		assert_null(rect.texture, "NONE clears the icon texture")


func test_m203_subweapon_fills_ammo2_from_the_differing_round_entry() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	# The M203 carbines are rifleman-filtered; switch PLAYERCLASS to Rifleman
	# (value 8, row 3) so the slot list offers them.
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 3)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 8, "WPN_M4M203AUTO")
	# The witnessed walk skips same-round WPN_M4M203 and lands on WPN_M4M203HE
	# (AMMO_M203_40MM_NADE) within loadout_subclasses = 2.
	var sub := _expected_sub(wdb, w)
	assert_eq(sub.name, "WPN_M4M203HE",
		"the sub walk lands on the first DIFFERING round_type entry")
	var ammo2 := _ammo_id("PRIMARY_AMMO2")
	assert_true(_ammo_driver.is_widget_shown(ammo2), "a live sub-weapon shows *_AMMO2")
	assert_eq(_ammo_driver.item_count(ammo2), sub.maxclips,
		"*_AMMO2 rows come from the SUB-weapon's maxclips")
	assert_eq(_ammo_driver.item_text(ammo2, 0),
		"%d - %s" % [sub.clipsize, sub.round_type],
		"*_AMMO2 labels use the sub-weapon's clipsize + round type")


func test_grenade_combos_fill_in_table_order_with_zero_row() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var expected: Array = wdb.get_slot_weapons(
			WeaponDatabase.SLOT_GRENADE, 1, 2)  # medic/blue
	assert_gt(expected.size(), 0, "the fixture carries medic/blue grenades")
	for i in mini(expected.size(), 3):
		var combo := _ammo_id("GRENADE_AMMO%d" % (i + 1))
		var w := expected[i] as WeaponDef
		assert_true(_ammo_driver.is_widget_shown(combo), "an owned grenade control shows")
		assert_eq(_ammo_driver.item_count(combo), w.maxclips + 1,
			"grenade rows are 0..maxclips INCLUDING the zero row [orig: @ 0x55def0]")
		assert_eq(_ammo_driver.item_text(combo, 0), "0 - %s" % w.round_type,
			"row 0 is the zero-rounds row")
		assert_eq(_ammo_driver.selected_row(combo), w.maxclips,
			"the untouched default selects the full row")
	for i in range(expected.size(), 3):
		assert_false(_ammo_driver.is_widget_shown(_ammo_id("GRENADE_AMMO%d" % (i + 1))),
			"unowned grenade controls hide")


func test_weight_label_renders_witnessed_format_and_band() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	# Expected parent term [orig: PlayerInfo_CalculateLoadoutWeight @ 0x55f1f0]:
	# weight + maxclips*clip_weight (untouched default), plus the sub-weapon and
	# full-grenade clip-only terms the fill selects by default.
	var expected := w.weight \
			+ w.maxclips * w.clip_weight
	var sub := _expected_sub(wdb, w)
	if sub != null and sub.clipsize > 0:
		expected += sub.maxclips * sub.clip_weight
	for g in wdb.get_slot_weapons(WeaponDatabase.SLOT_GRENADE, 1, 2).slice(0, 3):
		expected += g.maxclips * g.clip_weight
	var band := "Light"
	if expected >= 66.6:
		band = "Heavy"
	elif expected >= 33.3:
		band = "Normal"
	assert_eq(_weight_text(), "Total Weight %.1f lbs (%s)" % [expected, band],
		"the witnessed \"%s %.1f %s (%s)\" readout over the native weight math")
	assert_eq(wdb.encumbrance_class(0.0), WeaponDatabase.ENCUMBRANCE_LIGHT)
	assert_eq(wdb.encumbrance_class(33.3), WeaponDatabase.ENCUMBRANCE_NORMAL)
	assert_eq(wdb.encumbrance_class(66.6), WeaponDatabase.ENCUMBRANCE_HEAVY)


func test_ammo_selection_recomputes_weight_and_snapshot() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var companion := _make_ammo_companion(wdb)
	var w := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1, "WPN_M4AUTO")
	assert_eq(companion.selected_clips("PRIMARY"), -1,
		"untouched ammo reports the -1 default [orig: the '-1' kit filler]")
	var before := _weight_text()
	_ammo_driver.select_row(_ammo_id("PRIMARY_AMMO1"), 0)  # one clip
	assert_eq(companion.selected_clips("PRIMARY"), 1,
		"the pick records row+1 [orig: @ 0x55f730]")
	assert_ne(_weight_text(), before, "an ammo pick recomputes the weight readout")
	var profile := companion.snapshot()
	assert_eq(int(profile.get("primary_clips", -99)), 1,
		"snapshot carries the recorded clip pick")
	assert_eq(int(profile.get("secondary_clips", -99)), -1,
		"untouched slots stay at the -1 default")
	assert_eq(int(profile.get("primary_ammo_type", -99)), 0,
		"the type byte rides the snapshot [orig: the kit tuple flags field]")
	# Icon side of the same selection: the M4's authored icon texture name is
	# non-empty, but with no resource root the rect stays cleared (root-less unit).
	assert_false(w.icon.is_empty(), "the fixture authors an icon")


func test_grenade_zero_pick_stays_zero_in_the_weight() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	# The witnessed asymmetry [orig: PlayerInfo_CalculateLoadoutWeight @ 0x55f1f0]:
	# grenades default -1 -> maxclips, but a PICKED 0 stays 0 (the zero row) —
	# unlike the parents' <=0 -> maxclips rule.
	var wdb := _load_weapons()
	var _presenter := _make_ammo_companion(wdb)
	var g: WeaponDef = wdb.get_slot_weapons(WeaponDatabase.SLOT_GRENADE, 1, 2)[0]
	assert_gt(g.clip_weight * g.maxclips, 0.0,
		"the first grenade def carries weighable clips")
	var default_text := _weight_text()  # -1 default = full grenades weighed in
	_ammo_driver.select_row(_ammo_id("GRENADE_AMMO1"), 0)  # the zero row
	assert_ne(_weight_text(), default_text,
		"picking the zero row removes that grenade's clip term")
	var zero_total := float(_weight_text().get_slice(" ", 2))
	var default_total := float(default_text.get_slice(" ", 2))
	assert_almost_eq(default_total - zero_total,
		g.maxclips * g.clip_weight, 0.06,
		"the delta is exactly the grenade's maxclips*clip_weight term")


func test_weapon_dict_carries_loadout_subclasses() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var idx := wdb.find_weapon("WPN_M4M203AUTO")
	assert_gt(idx, 0)
	assert_eq(wdb.get_weapon(idx).loadout_subclasses, 2,
		"loadout_subclasses (+36) rides the transport dict")


# --- PLAYERVOICE (the voice-definition table walk) -----------------------------

# The list is DEFAULT_VOICE plus every ENABLED voice-table row whose sex matches
# the selected head's sex byte, not the head's own voice id. The synth table's
# N00/D00 combos are all male heads, so the male set (ids 1..6 and 10 -- id 9 is
# the table's one disabled row) shows; N00/D03's second combo is a female head
# and swaps the list to the female set (7, 8, 11).
# docs/playerinfo/avatars-re.md "PLAYERVOICE list" (the table at 0x83C7A8).
func test_kit_page_carries_knife_medpack_slots_and_three_grenades() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var wdb := _load_weapons()
	var companion := _make_ammo_companion(wdb)  # the screen opens on class 5 (medic)

	var kit: Array = companion.snapshot().get("kit", [])
	assert_eq(kit.size(), 8, "knife + medpack + three slots + three grenade slots")
	var knife: Dictionary = kit[0]
	assert_eq(String(knife.get("name", "")), "WPN_KNIFE",
		"the blue side's knife leads the page")
	for key in ["ammo_primary", "ammo_secondary", "flags"]:
		assert_eq(int(knife.get(key, 0)), -1,
			"the knife entry's three other values are the -1 filler")
	assert_eq(String((kit[1] as Dictionary).get("name", "")), "WPN_MEDPACK",
		"class 5 adds the medpack entry")

	# Slot index 2 = PRIMARY. Nothing picked yet, so retail's NONE row value (0)
	# serializes weapon-table entry 0.
	assert_eq(String((kit[2] as Dictionary).get("name", "")), wdb.get_weapon(0).name,
		"a NONE slot serializes weapon-table entry 0")

	var picked := _select_weapon(wdb, "PRIMARY", WeaponDatabase.SLOT_PRIMARY, 1,
			"WPN_M4AUTO")
	_ammo_driver.select_row(_ammo_id("PRIMARY_AMMO1"), 0)  # clip pick stores row + 1
	kit = companion.snapshot().get("kit", [])
	var primary: Dictionary = kit[2]
	assert_eq(String(primary.get("name", "")), picked.name)
	assert_eq(int(primary.get("ammo_primary", 0)), 1,
		"the recorded clip pick rides the entry's second value")
	assert_eq(int(primary.get("flags", -99)), companion.selected_ammo_type("PRIMARY"),
		"PRIMARY's fourth value is its team ammo-type byte")
	assert_eq(int((kit[4] as Dictionary).get("flags", 0)), -1,
		"ACCESSORY's fourth value is always the filler")
	assert_eq(kit.size() - 5, 3, "exactly three grenade entries close the page")
	for i in 3:
		assert_false(String((kit[5 + i] as Dictionary).get("name", "")).is_empty(),
			"every grenade slot serializes a name, filled or not")


# Class 8 (rifleman) writes no medpack entry; the page then has seven entries.
func test_kit_page_medpack_is_class_five_only() -> void:
	if RetailData.def_root().is_empty():
		pending(RetailData.fixture_pending_text("def/weapon.def"))
		return
	var companion := _make_ammo_companion(_load_weapons())
	_ammo_driver.select_row(_ammo_id("PLAYERCLASS"), 3)  # values 5..9 -> row 3 = class 8

	var kit: Array = companion.snapshot().get("kit", [])
	assert_eq(kit.size(), 7, "no medpack for a rifleman")
	assert_eq(String((kit[0] as Dictionary).get("name", "")), "WPN_KNIFE",
		"the blue side keeps WPN_KNIFE")
	for entry in kit:
		assert_ne(String((entry as Dictionary).get("name", "")), "WPN_MEDPACK")
