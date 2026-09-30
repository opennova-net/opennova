extends GutTest

# The authority's server-status view over the REAL bindings (ADR 0043 rule
# 11): the HudToggles special-key legs (the quit dialog's keys, the status
# page's Enter / PgUp / PgDn), the session rule, and the HudOverlay page mode
# over a loaded world — the page replaces the scene frame (the viewport stops
# drawing 3D) and hands it back; the quit dialog setter; the consumed-key
# mask the special-key legs write into the one binding model.
# [orig: Server_DrawStatusScreen @0x50a2d0 from GameLoop_RenderFrame
#  @0x521cd6..0x521cef; Input_HandleSpecialKeys @0x49c5df..0x49c6d1 /
#  @0x49c960..0x49c9c7; Input_ProcessKeyboardEvents @0x49d2fb]

const VK_RETURN := 0x0D
const VK_PRIOR := 0x21
const VK_NEXT := 0x22

var _staged_dir := ""


func after_each() -> void:
	if not _staged_dir.is_empty():
		TestFs.remove_dir_recursive(_staged_dir)
		_staged_dir = ""


func test_quit_dialog_keys_and_status_page_keys() -> void:
	var toggles := HudToggles.new()
	var y := "Y".unicode_at(0)
	var n := "N".unicode_at(0)
	var r := "R".unicode_at(0)
	assert_eq(toggles.quit_dialog_key(y, true, y, n, r), 0,
			"no dialog: the chain goes on")
	# The listen host's ToggleServer row flips the view in a session.
	assert_eq(toggles.poll(1 << HudToggles.ROW_TOGGLE_SERVER, false, false, true, true,
			false, true, true, true), HudToggles.EVENT_SERVER_STATUS_VIEW_TOGGLED)
	assert_true(toggles.is_server_status_view())
	# Esc with nothing open opens the quit dialog on the view.
	var events := toggles.escape(true, false, true)
	assert_true((events & HudToggles.EVENT_QUIT_DIALOG_OPENED) != 0)
	assert_true((events & HudToggles.EVENT_ESCAPE_CLOSED_WINDOW) != 0)
	assert_true(toggles.is_quit_dialog_open())
	var taken := toggles.quit_dialog_key("5".unicode_at(0), true, y, n, r)
	assert_eq(taken, HudToggles.SPECIAL_KEY_CHAIN_TAKEN | HudToggles.SPECIAL_KEY_CONSUMED,
			"a digit is taken while the dialog is up")
	assert_eq(toggles.quit_dialog_key(r, true, y, n, r), HudToggles.SPECIAL_KEY_CHAIN_TAKEN,
			"the restart key is not taken in a session")
	var confirm := toggles.quit_dialog_key(y, true, y, n, r)
	assert_true((confirm & HudToggles.SPECIAL_KEY_QUIT_CONFIRMED) != 0, "Y runs Exit Mission")
	assert_eq(toggles.quit_dialog_key(n, true, y, n, r),
			HudToggles.SPECIAL_KEY_CHAIN_TAKEN | HudToggles.SPECIAL_KEY_CONSUMED)
	assert_false(toggles.is_quit_dialog_open(), "N closes the dialog")
	# The status page's keys: consumed on the listen host's view only.
	assert_true(toggles.server_status_page_key(VK_RETURN, true, true, true))
	assert_true(toggles.server_status_page_key(VK_PRIOR, true, true, true))
	assert_true(toggles.server_status_page_key(VK_NEXT, true, true, true))
	assert_false(toggles.server_status_page_key(VK_RETURN, true, false, true), "a joiner")
	# The session's end clears the view.
	toggles.session_init(false, true)
	assert_false(toggles.is_server_status_view())
	assert_false(toggles.server_status_page_key(VK_RETURN, true, true, true))


func test_playerlist_takes_the_score_list_on_the_view() -> void:
	var toggles := HudToggles.new()
	var row := 1 << HudToggles.ROW_PLAYER_LIST
	# Out of a session the playerlist action does nothing.
	assert_eq(toggles.poll(row, false, false, true, false, false, true, true, true), 0)
	assert_false(toggles.is_scoreboard_open())
	toggles.poll(0, false, false, true, true, false, true, true, true)
	toggles.poll(1 << HudToggles.ROW_TOGGLE_SERVER, false, false, true, true, false, true,
			true, true)
	toggles.poll(0, false, false, true, true, false, true, true, true)
	assert_eq(toggles.poll(row, false, false, true, true, false, true, true, true),
			HudToggles.EVENT_SERVER_STATUS_SCORE_LIST_TOGGLED)
	assert_true(toggles.is_server_status_score_list_open())
	assert_false(toggles.is_scoreboard_open())


func test_page_mode_replaces_the_scene_frame() -> void:
	_staged_dir = HudFixture.stage_root(true)
	var world := WorldFixture.boot_minimal(self, _staged_dir)
	var presenter := HudFixture.presenter_over(self, world)
	var hud: HudOverlay = presenter.get_game_hud()
	var sim: Simulation = world.get_sim() if world != null else null
	assert_not_null(sim, "the booted world carries its simulation")
	if sim == null:
		return
	var viewport := hud.get_viewport()
	var had_3d := viewport.disable_3d
	# A sim without a host context never draws the page.
	var bare := Simulation.new()
	hud.set_server_status_page(true, false, null, bare)
	assert_false(hud.is_server_status_page_shown(), "only an authority draws the page")
	# The booted world's authority draws it and the viewport stops its 3D.
	hud.set_server_status_page(true, true, null, sim)
	assert_true(hud.is_server_status_page_shown())
	assert_true(viewport.disable_3d, "the page stands in for the scene frame")
	hud.queue_redraw()
	await get_tree().process_frame
	hud.set_quit_dialog(true, true, true, null)
	await get_tree().process_frame
	hud.set_quit_dialog(false, true, true, null)
	hud.set_server_status_page(false, false, null, sim)
	assert_false(hud.is_server_status_page_shown())
	assert_eq(viewport.disable_3d, had_3d, "the scene frame comes back")


func test_consumed_key_reads_up_until_released() -> void:
	var model := ControlsModel.new()
	var vk := VK_RETURN
	# Nothing is held in a headless run: a consumed key reads up and the mask
	# clears on the (already) released key without leaking.
	model.consume_key_press(vk)
	assert_false(model.is_token_pressed("talk"))
	assert_false(model.is_token_pressed("talk"))
