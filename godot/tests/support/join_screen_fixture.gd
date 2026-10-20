class_name JoinScreenFixture
extends RefCounted

## The join screen (pre.mnu PRE_GAME_MENU) the join-screen tests open: an authored
## pre.mnu carrying the screen's windows and controls (the choice panels'
## GAME_PASSWORD edit, SPECTATE radio and shared TEAM_PASSWORD edit, the MESSAGES
## box, ABORT_CANCEL, BACK and ACCEPT), staged under a scratch res:// directory.
## The shipped pre.mnu stays covered by the retail menu-corpus compilation test.

const STAGED := ["pre.mnu", "menutxt.bin"]


## Write the authored pre.mnu and an empty menutxt.bin under `res_dir`.
static func stage(test: GutTest, res_dir: String) -> void:
	var dir := ProjectSettings.globalize_path(res_dir)
	test.assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var w := MenuDriverFixture.wnd
	var body: String = (
			w.call("window", "GAME_PASSWORD_WRAPPER", 20,
					w.call("edit", "GAME_PASSWORD", 30), " HIDDEN")
			+ w.call("window", "SPECTATE_WRAPPER", 60,
					w.call("radio", "SPECTATE", 70), " HIDDEN")
			+ w.call("window", "TEAM_PASSWORD_WRAPPER", 100,
					w.call("window", "TEAM_STATIC_WRAPPER", 110, "", " HIDDEN")
					+ w.call("window", "SPECTATOR_STATIC_WRAPPER", 130, "", " HIDDEN")
					+ w.call("edit", "TEAM_PASSWORD", 150), " HIDDEN")
			+ w.call("window", "ERROR_WRAPPER", 200,
					w.call("multiline_edit", "MESSAGES", 210, "", " READONLY"), " HIDDEN")
			+ w.call("window", "ABORT_WRAPPER", 300,
					w.call("button", "ABORT_CANCEL", 310), " HIDDEN")
			+ w.call("window", "ABORTRETRY_WRAPPER", 400,
					w.call("button", "BACK", 410) + w.call("button", "ACCEPT", 440), " HIDDEN"))
	TestFs.write_bytes(test, dir.path_join("pre.mnu"),
			MenuDriverFixture.screen_xml("PRE_GAME_MENU", body).to_utf8_buffer())
	TestFs.write_bytes(test, dir.path_join("menutxt.bin"), RtxtStringFile.new().to_byte_array())


static func unstage(res_dir: String) -> void:
	PresenterFixture.unstage(res_dir, PackedStringArray(STAGED))


## A join screen opened over the staged directory (its own owner node), asserted
## on the test.
static func open(test: GutTest, res_dir: String) -> PreGameMenuPresenter:
	var presenter := PreGameMenuPresenter.new()
	test.add_child_autofree(presenter)
	var owner := Node.new()
	test.add_child_autofree(owner)
	test.assert_true(presenter.open(PresenterFixture.root_over(test, res_dir), owner),
			"the join screen opens over pre.mnu")
	return presenter


## Type into a control, check a radio, or press a button, as the player does.
static func set_text(presenter: PreGameMenuPresenter, control: String, text: String) -> void:
	var driver := presenter.get_menu_driver()
	driver.set_widget_text(driver.widget_id(control), text)


static func set_checked(presenter: PreGameMenuPresenter, control: String, checked: bool) -> void:
	var driver := presenter.get_menu_driver()
	driver.set_widget_checked(driver.widget_id(control), checked)


static func press(presenter: PreGameMenuPresenter, control: String) -> void:
	var driver := presenter.get_menu_driver()
	driver.activate(driver.widget_id(control))
