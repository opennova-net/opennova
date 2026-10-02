extends GutTest

# The in-game armory seam: weapon.mnu's WEAPON screen driven by the ArmoryMenuCompanion
# companion (godot/game/world/armory_menu_companion.gd) riding a MenuDriver over a real
# MnuDocument (the compiled-menu surface; the Control tree is gone). Pins the witnessed
# wiring [orig: WeaponDef_RegisterUICallbacks @0x567020 registers PLAYER_CLASS / PRIMARY /
# SECONDARY / ACCESSORY / *_AMMO / ACCEPT / CANCEL on the "WEAPON" screen; population
# UI_PopulateThreeCategoryLists @0x566db0 (sorted rows, NONE at 0); class resolution
# Armory_ResolveSelectedClass @0x5642f0; weight UI_UpdateWeaponWeightDisplay @0x565640;
# ACCEPT WeaponLoadout_ApplyFromBuffer @0x565cd0]. The zone-gated OPEN path (the
# useitem key, action 177, on entity Flags 0x400000 @0x4e0b4d) lives in main_game +
# the collision resolver (ctest collision_test) — here the screen itself is the unit.

# The retail weapon.def, weapon.mnu and gametext.bin come from the reference
# fixture set (docs/asset-gated-tests.md); the whole script skips without it.
const WEAPON_FIXTURE_REL := "def/weapon.def"
const WEAPON_MNU_REL := "mnu/jo_weapon.mnu"
const GAMETEXT_REL := "rtxt/gametext.bin"


func should_skip_script():
	for rel in [WEAPON_FIXTURE_REL, WEAPON_MNU_REL, GAMETEXT_REL]:
		if RetailData.fixture(rel).is_empty():
			return RetailData.fixture_pending_text(rel)
	return false


func before_each() -> void:
	Strings.clear()


func after_all() -> void:
	Strings.clear()


func _load_weapons() -> WeaponDatabase:
	var wdb := WeaponDatabase.new()
	var path := RetailData.fixture(WEAPON_FIXTURE_REL)
	assert_eq(wdb.load(path), OK, "the reference weapon.def loads")
	return wdb


func _make_weapon_driver(with_frame := false) -> MenuDriver:
	var body := MenuDriverFixture.wnd("spinlist", "PLAYER_CLASS", 10)
	var y := 40
	for n in ["PRIMARY", "SECONDARY", "ACCESSORY",
			"PRIMARY_AMMO1", "SECONDARY_AMMO1", "ACCESSORY_AMMO1",
			"GRENADE_AMMO1", "GRENADE_AMMO2", "GRENADE_AMMO3"]:
		body += MenuDriverFixture.wnd("combobox", n, y)
		y += 24
	for n in ["PRIMARY_ICON", "SECONDARY_ICON", "ACCESSORY_ICON"]:
		body += MenuDriverFixture.wnd("window", n, y)
		y += 24
	body += MenuDriverFixture.wnd("button", "ACCEPT", y)
	body += MenuDriverFixture.wnd("button", "CANCEL", y + 24)
	body += MenuDriverFixture.wnd("static", "STATIC_TOTAL_WEIGHT", y + 48)
	var doc := MenuDriverFixture.doc_from_xml(self, MenuDriverFixture.screen_xml("WEAPON", body))
	if not with_frame:
		return MenuDriverFixture.driver_over(self, doc, "weapon.mnu")
	var driver := MenuDriver.new()
	var frame := MenuFrame.new()
	frame.size = Vector2(800, 600)
	add_child_autofree(frame)
	driver.attach(frame, null)
	assert_true(driver.open_document(doc, null, null, null, "weapon.mnu", "WEAPON"),
			"the framed WEAPON document opens on the driver")
	return driver


func test_ammo_rows_resolve_the_shipped_wepdes_labels() -> void:
	var gametext := RtxtStringFile.new()
	assert_eq(gametext.load_from_byte_array(
			FileAccess.get_file_as_bytes(RetailData.fixture(GAMETEXT_REL))), OK,
			"the shipped GameText fixture loads")
	Strings.register_table("gametext", gametext)

	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO")
	var driver := _make_weapon_driver()
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	assert_eq(driver.item_text(driver.widget_id("PRIMARY_AMMO1"), 0), "30 - 5.56x45",
			"parent ammo rows use the shipped WepDes round label")
	assert_eq(driver.item_text(driver.widget_id("GRENADE_AMMO1"), 0), "0 - Flashbang",
			"grenade zero rows use the shipped WepDes round label")
	assert_eq(driver.item_text(driver.widget_id("GRENADE_AMMO1"), 1), "1 - Flashbang",
			"grenade count rows keep the localized label")


func test_real_weapon_mnu_populates() -> void:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(
			FileAccess.get_file_as_bytes(RetailData.fixture(WEAPON_MNU_REL))), OK,
			"the shipped jo_weapon.mnu fixture loads")
	var driver := MenuDriverFixture.driver_over(self, doc, "weapon.mnu", "WEAPON")

	var companion := ArmoryMenuCompanion.new()
	assert_true(companion.owns_menu(driver), "the real weapon.mnu is claimed by the armory companion")
	companion.set_weapon_database(_load_weapons())
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	var primary := driver.widget_id("PRIMARY")
	assert_gte(primary, 0, "the real weapon.mnu authors a PRIMARY combobox")
	assert_gt(driver.item_count(primary), 1, "PRIMARY populates (NONE + weapons)")
	var spin := driver.widget_id("PLAYER_CLASS")
	assert_gte(spin, 0, "the real weapon.mnu authors the PLAYER_CLASS spinlist")
	assert_eq(driver.item_count(spin), 5, "the companion fills the authored-empty class spinlist")


# First-show regression against the real authored WEAPON screen and weapon.def:
# M4 (5.5 + 10 * 1.5) plus two satchels (2 * 17.6) is 55.7 lbs / Normal.
# Selecting row zero means one satchel and must re-render 38.1 lbs / Normal.
func test_real_weapon_mnu_weight_tracks_ammo_and_encumbrance_on_first_open() -> void:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(
			FileAccess.get_file_as_bytes(RetailData.fixture(WEAPON_MNU_REL))), OK)
	var driver := MenuDriverFixture.driver_over(self, doc, "weapon.mnu", "WEAPON")

	var companion := ArmoryMenuCompanion.new()
	companion.set_weapon_database(_load_weapons())
	companion.set_player_class(8)
	companion.set_current_loadout("WPN_M4AUTO", "", "WPN_SATCHEL_CHARGE")
	companion.on_menu_built(driver, "weapon.mnu", "WEAPON", null)

	assert_gte(driver.widget_id("STATIC_TOTAL_WEIGHT"), 0,
			"the real weapon.mnu authors the weight readout")
	assert_eq(companion.weight_line(), "Total Weight 55.7 lbs (Normal)",
			"first open weighs the selected M4 plus two satchels")

	var satchel_ammo := driver.widget_id("ACCESSORY_AMMO1")
	assert_gte(satchel_ammo, 0, "the real weapon.mnu authors the satchel ammo combo")
	assert_eq(driver.selected_row(satchel_ammo), 1,
			"two satchels preselect the last zero-based row")
	driver.select_row(satchel_ammo, 0)
	assert_eq(companion.weight_line(), "Total Weight 38.1 lbs (Normal)",
			"row zero means one satchel and updates both weight and encumbrance")
