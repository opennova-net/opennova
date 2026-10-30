extends GutTest

# The in-world vehicle screen (E-L-2): vehicle.mnu's VEHICLE screen on the compiled-menu
# surface over a loaded world, as the game opens it on the USE-ITEM vehicle-loadout arm
# [orig: Input_HandleActionBinding_0 @0x4E0AF1..0x4E0B05 - UI_OpenMenuScreen("vehicle.mnu",
# "VEHICLE") @0x4E0AFD]. The engine's VehicleMenu and the screen's controller own the rows,
# the pick, the poll and both sends (ctest npruntime_vehicle_menu); this pins the device half:
# the frame and the ITEM_DISPLAY pane mounted, the controller claimed on the driver, ACCEPT and
# CANCEL closing the frame, the Escape close CANCEL's, and a root without vehicle.mnu opening
# nothing.

# An authored VEHICLE screen of the game's shape: the list and its two buttons.
const VEHICLE_MNU := """<SCREEN>
  <NAME>VEHICLE</NAME>
  <WINDOW type="window" name="MAIN">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="list" name="ITEM_LIST">
      <POSITION><LEFT>240</LEFT><TOP>80</TOP><RIGHT>560</RIGHT><BOTTOM>440</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="window" name="ITEM_DISPLAY">
      <POSITION><LEFT>580</LEFT><TOP>80</TOP><RIGHT>780</RIGHT><BOTTOM>280</BOTTOM></POSITION>
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
	# The ITEM_DISPLAY pane over its widget's rect, cleared to the draw's colour, showing the
	# selected group's model [orig: Render_AvatarPreview3D @0x563C10, ITEM_DISPLAY's callback
	# @0x564233].
	var preview := presenter.get_preview()
	assert_not_null(preview, "the ITEM_DISPLAY pane")
	if preview != null:
		var rect := driver.widget_frame_rect(driver.widget_id("ITEM_DISPLAY"))
		assert_eq(preview.position, rect.position)
		assert_eq(preview.size, rect.size)
		assert_eq(preview.graphic(), presenter.get_controller().preview_graphic(),
				"the pane asks for the selected group's graphic")
		assert_eq(preview.preview_camera().fov, 30.0, "the 30 degree field")
		assert_eq(preview.preview_camera().keep_aspect, Camera3D.KEEP_WIDTH,
				"the field across the rect's width")
	driver.activate(driver.widget_id("CANCEL"))
	assert_false(presenter.is_open(), "CANCEL closes the screen")
	assert_signal_emit_count(presenter, "closed", 1)
	assert_true(presenter.open(), "it opens again")
	driver.activate(driver.widget_id("ACCEPT"))
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
