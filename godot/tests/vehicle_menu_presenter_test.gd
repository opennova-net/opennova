extends GutTest

# The in-world vehicle screen (E-L-2): vehicle.mnu's VEHICLE screen on the compiled-menu
# surface over a loaded world, as the game opens it on the USE-ITEM vehicle-loadout arm
# [orig: Input_HandleActionBinding_0 @0x4E0AF1..0x4E0B05 - UI_OpenMenuScreen("vehicle.mnu",
# "VEHICLE") @0x4E0AFD]. The engine's VehicleMenu owns the rows, the poll and both sends
# (ctest npruntime_vehicle_menu); this pins the screen: its ITEM_LIST shows the engine's
# rows, ACCEPT and CANCEL close it [orig: VehicleMenu_OnButton @0x563B60], the Escape close
# is CANCEL's, and a root without vehicle.mnu opens nothing.

# An authored VEHICLE screen of the game's shape: the list and its two buttons.
const VEHICLE_MNU := """<SCREEN>
  <NAME>VEHICLE</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="list" name="ITEM_LIST">
      <POSITION><LEFT>240</LEFT><TOP>80</TOP><RIGHT>560</RIGHT><BOTTOM>440</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="button" name="ACCEPT">
      <POSITION><LEFT>240</LEFT><TOP>470</TOP><RIGHT>390</RIGHT><BOTTOM>504</BOTTOM></POSITION>
      <STRING justify="CENTER" vjustify="CENTER">ACCEPT</STRING>
    </WINDOW>
    <WINDOW type="button" name="CANCEL">
      <POSITION><LEFT>410</LEFT><TOP>470</TOP><RIGHT>560</RIGHT><BOTTOM>504</BOTTOM></POSITION>
      <STRING justify="CENTER" vjustify="CENTER">CANCEL</STRING>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


func after_all() -> void:
	Strings.clear()


func _presenter(world: GameWorld) -> VehicleMenuPresenter:
	var parent := Control.new()
	parent.size = Vector2(800, 600)
	add_child_autofree(parent)
	var presenter: VehicleMenuPresenter = add_child_autofree(VehicleMenuPresenter.new())
	presenter.setup(world.world_view(), parent)
	return presenter


func test_the_screen_opens_from_vehicle_mnu_and_its_buttons_close_it() -> void:
	var root_dir := WorldFixture.stage_minimal_root("vehicle_menu", false,
			{"vehicle.mnu": VEHICLE_MNU})
	var world := WorldFixture.boot_minimal(self, root_dir)
	var presenter := _presenter(world)
	watch_signals(presenter)
	assert_true(presenter.open(), "the screen opens from the world's vehicle.mnu")
	assert_true(presenter.is_open())
	assert_signal_emitted(presenter, "opened")
	assert_false(presenter.open(), "an open screen does not open again")
	var driver := presenter.get_menu_driver()
	assert_not_null(driver)
	if driver == null:
		return
	assert_eq(driver.get_current_screen(), "VEHICLE")
	var list := driver.widget_id("ITEM_LIST")
	assert_gte(list, 0, "the screen's ITEM_LIST")
	assert_eq(driver.get_widget_items(list), world.get_sim().get_vehicle_menu_rows(),
			"the ITEM_LIST shows the engine's rows")
	driver.widget_activated.emit(driver.widget_id("CANCEL"), "CANCEL")
	assert_false(presenter.is_open(), "CANCEL closes the screen")
	assert_signal_emit_count(presenter, "closed", 1)
	assert_true(presenter.open(), "it opens again")
	driver.widget_activated.emit(driver.widget_id("ACCEPT"), "ACCEPT")
	assert_false(presenter.is_open(), "ACCEPT closes it")
	assert_signal_emit_count(presenter, "closed", 2)
	assert_true(presenter.open())
	presenter.close()
	assert_false(presenter.is_open(), "the Escape close is CANCEL's")
	assert_signal_emit_count(presenter, "closed", 3)
	presenter.teardown()
	TestFs.remove_dir_recursive(root_dir)


func test_a_root_without_vehicle_mnu_opens_nothing() -> void:
	var world := WorldFixture.boot_minimal(self)
	var presenter := _presenter(world)
	assert_false(presenter.open(), "no vehicle.mnu: no screen")
	assert_false(presenter.is_open())
	assert_null(presenter.get_menu_driver())
