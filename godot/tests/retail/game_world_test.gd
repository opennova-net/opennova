extends GutTest

const WORLD_TEST_ROOT := "game_world_test"
const ArmoryPresenter := preload("res://game/world/armory_presenter.gd")


func after_each() -> void:
	for staged_dir in _staged_dirs:
		TestFs.remove_dir_recursive(staged_dir)
	_staged_dirs.clear()
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join(WORLD_TEST_ROOT))


# (The Node runtime/sim doubles that used to live here — TransportRuntimeStub,
# ProfilingRuntimeStub, FxRuntimeStub, ItemPoseRuntimeStub, the anchor/blink/
# occlusion/joiner stubs — are gone: GameWorld._runtime is typed MissionRoot
# and MissionRoot._sim is typed Simulation, so every runtime-consuming
# test now boots the REAL stack through the public load path. The presentation
# doubles followed (ADR 0043 rule 11): the viewmodel/prewarm placer stubs and
# their GameWorld harnesses, the FxWorldStub/ImpactAudioStub recording sinks,
# the ItemFxDirectorProbe/ItemFxGameWorldHarness pair and the warm-pass stubs
# are replaced by the packaged world booted over staged roots (WorldFixture)
# and read through EffectWorld.get_debug_group_report,
# MissionAudio.recent_fired_soundsets and the world's typed local-player seams.)

# The WorldFixture roots this file staged (removed in after_each).
var _staged_dirs: Array[String] = []


# Register a staged WorldFixture root for after_each cleanup.
func _staged(root_dir: String) -> String:
	_staged_dirs.append(root_dir)
	return root_dir


func test_armory_can_reuse_game_world_weapon_database_on_first_open() -> void:
	Strings.clear()
	# The retail weapon.mnu, weapon.def and string tables come from the
	# reference fixture set (docs/asset-gated-tests.md).
	var staged := {
		"mnu/jo_weapon.mnu": "weapon.mnu",
		"def/weapon.def": "weapon.def",
		"rtxt/menutxt.bin": "menutxt.BIN",
		"rtxt/gametext.bin": "gametext.bin",
	}
	for rel in staged:
		if RetailData.fixture(rel).is_empty():
			pending(RetailData.fixture_pending_text(rel))
			return
	var root_dir := _staged(WorldFixture.stage_minimal_root("first_armory_open"))
	for rel in staged:
		var target := root_dir.path_join(staged[rel])
		if FileAccess.file_exists(target):
			assert_eq(DirAccess.remove_absolute(target), OK)
		assert_eq(DirAccess.copy_absolute(RetailData.fixture(rel), target), OK)

	var world := WorldFixture.make_world(self)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	var loadout := PlayerSpawnLoadout.new()
	loadout.primary = "WPN_M4AUTO"
	loadout.accessory = "WPN_SATCHEL_CHARGE"
	loadout.player_class = 8
	world.set_local_player_spawn_loadout(loadout)
	assert_eq(world.load_mission("mnml.bms"), OK)

	var weapons: WeaponDatabase = world.get_weapon_database()
	assert_not_null(weapons, "first armory open lazily resolves weapon.def")
	if weapons == null:
		return
	assert_true(weapons.is_loaded())
	assert_gte(weapons.find_weapon("WPN_M4AUTO"), 0)
	assert_gte(weapons.find_weapon("WPN_SATCHEL_CHARGE"), 0)
	var sim := world.get_sim()
	var expected_names := ["WPN_M4AUTO", "WPN_SATCHEL_CHARGE"]
	var before_names: Array[String] = []
	for value in sim.get_local_player_loadout():
		before_names.append((value as WeaponKitEntry).name)
	assert_eq(before_names, expected_names,
		"the production world promoted the PLAYER_INFO-style canonical profile")

	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world.armory_view(), null, overlay)
	assert_true(presenter.open(), "the production world catalog reaches first armory open")
	assert_not_null(overlay.get_node_or_null("ArmoryMenu"))
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	assert_gt(driver.selected_row(driver.widget_id("PRIMARY")), 0,
		"the current primary is not NONE on first visit")
	assert_gt(driver.selected_row(driver.widget_id("ACCESSORY")), 0,
		"the current satchel is not NONE on first visit")

	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	var after_names: Array[String] = []
	for value in sim.get_local_player_loadout():
		after_names.append((value as WeaponKitEntry).name)
	assert_eq(after_names, expected_names,
		"accepting the untouched first-open rows preserves the exact canonical kit")
	world.unload()
	Strings.clear()
