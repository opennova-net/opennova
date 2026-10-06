extends GutTest

# DeployScreenPresenter on the typed surfaces (ADR 0034): host and joiner
# Simulations communicate over real UDP. Initial admission and an alive
# player's deploy-map selection are separate steps. Death re-arms the same
# request/release path through begin_redeployment.
# The world seam is a WorldView interface harness. A real C2S 0x0E reaches the
# host and releases its pending hold for both the initial and death selections.
#
# (The old value-double's host-side zone-mutation refresh leg — a zone turning
# contested between refreshes — has no typed equivalent without host-side zone
# capture orchestration; the param-keyed selection survival across the periodic
# rebuild is pinned below on unchanged zones.)

const DeployPresenter := preload("res://game/world/deploy_screen_presenter.gd")
const TMP_DIR := "res://.godot/deploy_screen_presenter_test"

# DeployMenuFixture authors the death-screen controls and string tables.
const STAGED_FIXTURES: Array[String] = ["death.mnu", "menutxt.BIN", "gametext.bin"]


const AI_TYPE := 0x14BF        # Generic Soldier (items.def id 105311)
const SPAWN_ZONE_TYPE := 1359  # pool-1 fixture; ItemDef supplies SpawnPoint
const ZONE_ITEMS_FILE := "deploy_spawn_zone_items.def"


var _overlay: Control = null


func before_each() -> void:
	Strings.clear()
	DeployMenuFixture.stage(self, TMP_DIR, STAGED_FIXTURES)


func after_each() -> void:
	_overlay = null
	Strings.clear()


func after_all() -> void:
	PresenterFixture.unstage(TMP_DIR, STAGED_FIXTURES)
	ItemDbFixture.release(ZONE_ITEMS_FILE)


# The fixture items.def plus one deploy-selectable SpawnPoint row (mission id
# 1359 promotes to ItemDef 101359).
func _spawn_zone_item_db() -> ItemDatabase:
	return ItemDbFixture.with_rows(self, ZONE_ITEMS_FILE, ItemDbFixture.SPAWN_ZONE_ROW)


func _spawn_zone_mission() -> MissionData:
	var md := MissionData.new()
	assert_eq(md.create_default(), OK)
	md.add_entity(3, AI_TYPE, Vector3(0, 0, 0), Vector3.ZERO)
	var zone := md.add_entity(
			MissionData.KIND_ITEM, SPAWN_ZONE_TYPE, Vector3(40, 0, 0), Vector3.ZERO)
	assert_not_null(zone)
	if zone != null:
		assert_true(md.set_entity_property_int(
				MissionData.KIND_ITEM, zone.index, "team", 1))
		# A numbered team zone starts secured and is selectable immediately.
		assert_true(md.set_entity_property_int(
				MissionData.KIND_ITEM, zone.index, "lfp_group", 1))
	return md


# A real loopback join completes admission without forcing a pick. The
# spawn-zone host retains its pending hold until a user selects a spawn.
# `host_traits_first` installs the host's item table ahead of its boot, as
# the game's load does, so the spawn zone exists when the host's own player
# joins and holds it too. Returns {host, joiner}; both are autofreed
# Simulations.
func _join_pair_in_match(host_traits_first := false) -> Dictionary:
	var mission := _spawn_zone_mission()
	var item_db := _spawn_zone_item_db()
	assert_not_null(item_db)

	var host := Simulation.new()
	autofree(host)
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	# No configured respawn timeout: the deploy pick's penalty is the stock
	# three-second floor (the host screen's default RESPAWN would hold it five).
	host_options.respawn_timeout = 0
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0), "host bound an OS-assigned UDP port")
	if host_traits_first:
		host.resolve_item_traits(item_db)
	assert_true(host.load_from_mission_data(mission))
	assert_true(host.spawn_local_player(Vector3(5, 0, 5), 0.0, 1))
	var host_anim := PresenterFixture.anim_root(self)
	assert_gt(host.set_infantry_anim_map(host_anim, "soldier.adm"), 0)
	host.resolve_item_traits(item_db)
	host.resolve_infantry_adm_ids(host_anim, item_db)
	var port: int = host.get_host_listen_port()
	assert_gt(port, 0)

	var joiner := Simulation.new()
	autofree(joiner)
	assert_true(joiner.enable_join("127.0.0.1", port, "DeployJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	var joiner_anim := PresenterFixture.anim_root(self)
	assert_gt(joiner.set_infantry_anim_map(joiner_anim, "soldier.adm"), 0)
	joiner.resolve_item_traits(item_db)
	joiner.resolve_infantry_adm_ids(joiner_anim, item_db)

	var reached := false
	for _i in range(800):
		host.step()
		joiner.step()
		if joiner.is_joined_in_match():
			reached = true
			break
		OS.delay_msec(2)
	assert_true(reached, "the joiner reached in-match over real loopback UDP")
	assert_false(joiner.is_join_deploy_pick_pending(),
			"initial admission does not force a C2S 0x0E")
	return {"host": host, "joiner": joiner}


# The in-match pair driven onward to the DEATH deploy pick: the authority kills
# the joiner and the death edge re-arms the pick (begin_redeployment). Returns
# {host, joiner}; both are autofreed Nodes.
func _join_pair_with_pending_pick() -> Dictionary:
	var pair := _join_pair_in_match()
	var host: Simulation = pair.host
	var joiner: Simulation = pair.joiner
	# The kill targets the joiner's wire handle: wait for the 0x0C name-match to
	# bind it, then the authority's real death transaction re-arms the pick (the
	# DEATH screen).
	var self_bound := false
	for _i in range(400):
		host.step()
		joiner.step()
		if joiner.get_joiner_self_handle() > 0:
			self_bound = true
			break
		OS.delay_msec(2)
	assert_true(self_bound, "the joiner bound its wire handle before the kill")
	assert_eq(host.debug_kill_player_entity(joiner.get_joiner_self_handle()), OK,
			"the host queued the joiner's death")
	var pick_pending := false
	for _i in range(240):
		host.step()
		joiner.step()
		if joiner.is_join_deploy_pick_pending():
			pick_pending = true
			break
		OS.delay_msec(2)
	assert_true(pick_pending,
			"the death edge holds the deploy pick (begin_redeployment)")
	# A death within 620 ticks of the deployment arms the host's 3-second pick
	# penalty (silently dropped picks). Settle past it so each test's single
	# pick/release leg operates in the clean accepting window.
	for _i in range(260):
		host.step()
		joiner.step()
	assert_true(joiner.is_join_deploy_pick_pending(),
			"nothing auto-picks while the death pick stays owed")
	return {"host": host, "joiner": joiner}


func _make_presenter(sim: Simulation) -> DeployPresenter:
	var view := PresenterFixture.FakeWorldView.new()
	view.root = PresenterFixture.root_over(self, TMP_DIR)
	view.sim_value = sim
	_overlay = Control.new()
	_overlay.size = Vector2(800, 600)
	add_child_autofree(_overlay)
	var presenter := DeployPresenter.new()
	presenter.setup(view, _overlay)
	add_child_autofree(presenter)
	return presenter


# The show hides DEATH_SHROUD and fills nothing: the per-frame reveal (the
# death is over 240 ticks old here, so at once) runs the content refresh on
# the next 16-tick boundary [orig: DeathScreen_UpdateShroudReveal @0x554730].
func _await_refresh(pair: Dictionary) -> void:
	for _i in range(20):
		pair.host.step()
		pair.joiner.step()
		OS.delay_msec(2)
	await get_tree().process_frame


# The visible list row carrying a node PARAM (the sorted list's row order is
# retail's text sort, so tests never assume fixed indices).
func _row_index_for_param(presenter: DeployPresenter, param: int) -> int:
	var rows := presenter.get_spawn_rows()
	for row in rows.size():
		if rows[row].param == param:
			return row
	return -1


# The presenter's row PARAM at a visible list row (the old ItemList metadata's
# successor: the presenter row model carries the node parameter per row).
func _row_param(presenter: DeployPresenter, row: int) -> int:
	var rows := presenter.get_spawn_rows()
	if row < 0 or row >= rows.size():
		return -1
	return rows[row].param


# The shell leaves State.WORLD on `opened` and returns on `closed`; without those
# two signals firing, LocalPlayerPresenter re-captures the mouse every frame and the
# SPAWNPOINTS_LIST rows cannot be clicked at all.
func test_open_and_close_emit_the_shell_state_signals() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)

	assert_false(presenter.is_open(), "the screen starts closed")
	assert_true(presenter.open(), "a pending pick opens the screen")
	assert_signal_emitted(presenter, "opened", "opening tells the shell to release the cursor")
	assert_true(presenter.is_open())

	presenter.close()
	assert_signal_emitted(presenter, "closed", "closing hands gameplay input back to the world")
	assert_false(presenter.is_open())

	# close() is idempotent: a second call must not re-emit and strand the shell.
	assert_signal_emit_count(presenter, "closed", 1)
	presenter.close()
	assert_signal_emit_count(presenter, "closed", 1, "closing an already-closed screen is a no-op")


# The MAP window (hud-re D-HUD-19): the presenter mounts the windowed map view
# over the authored MAP widget inside DEATH_SHROUD, follows its frame rect, and
# the show event fits the zoom; the window's model takes the mouse-class events.
func test_map_window_mounts_over_the_map_widget() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	assert_true(presenter.open())
	var map_window := presenter.get_map_window()
	assert_not_null(map_window, "the MAP widget hosts the map window")
	if map_window == null:
		return
	var driver := presenter.get_menu_driver()
	await _await_refresh(pair)
	var rect := driver.widget_frame_rect(driver.widget_id("MAP"))
	assert_eq(map_window.position, rect.position, "placed at the widget's frame rect")
	assert_eq(map_window.size, rect.size)
	assert_eq(map_window.get_widget_design_rect(), Rect2i(310, 30, 460, 460),
			"the absolute authored rect (DEATH_SHROUD origin + MAP position)")
	assert_true(map_window.visible)
	var zoom := map_window.get_zoom()
	assert_between(zoom, 0.1, 10.0, "the show-time fit lands inside the clamp")
	map_window.push_map_event(MapViewWindow.MAP_EVENT_WHEEL, Vector2i(400, 200), 0, 1)
	assert_almost_eq(map_window.get_zoom(), maxf(zoom * 0.85, 0.1), 0.0001)
	presenter.close()
	assert_false(presenter.is_open())


# DEATH_SHROUD (the window around the map, the list and the statics): the
# show hides it; it reveals 240 ticks after the death (or at once under the
# deploy overlay), and only then does the content refresh fill the list.
# [orig: DeathScreen_UpdateUI @0x553150 (the hide); DeathScreen_UpdateShroudReveal
#  @0x554730 (the reveal, UI_UpdateDeathScreenContent on (tick & 0xF) == 0)]
func test_the_shroud_hides_on_show_and_reveals_after_the_death_delay() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	assert_true(presenter.open())
	var driver := presenter.get_menu_driver()
	var shroud := driver.widget_id("DEATH_SHROUD")
	assert_gte(shroud, 0, "death.mnu authors the shroud")
	assert_false(driver.is_widget_shown(shroud), "the show hides the shroud")
	assert_eq(presenter.get_spawn_rows().size(), 0, "the show fills nothing")
	assert_true(pair.joiner.is_death_shroud_revealed(),
			"the death is past the 240-tick delay")
	await _await_refresh(pair)
	assert_true(driver.is_widget_shown(shroud), "the per-frame reveal shows it")
	assert_gt(presenter.get_spawn_rows().size(), 0, "the refresh fills the list")


# The DEATH map's pan/zoom state is the presenter's (the process's), not the
# menu's: the window a menu rebuild mounts drives the same state (the show's
# zoom fit re-runs on it; the load seed does not).
func test_the_map_view_state_survives_the_menu_rebuild() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	assert_true(presenter.open())
	var first := presenter.get_map_window()
	if first == null:
		fail_test("no map window")
		return
	var state := first.get_view_state()
	presenter.teardown()
	assert_true(presenter.open(), "the rebuilt screen opens")
	var second := presenter.get_map_window()
	assert_not_null(second)
	if second != null:
		assert_ne(second, first, "the menu rebuild mounts a new window")
		assert_eq(second.get_view_state(), state, "the view state carries over")


func test_open_refuses_when_no_pick_is_owed() -> void:
	# A spawned OFFLINE player owes no deployment pick — the real
	# is_join_deploy_pick_pending is false outside a held join.
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	autofree(sim)
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var presenter := _make_presenter(sim)
	watch_signals(presenter)
	assert_false(presenter.open(), "no pending pick means no screen")
	assert_signal_emit_count(presenter, "opened", 0, "a refused open must not move the shell state")
	assert_false(presenter.is_open())


func _assert_deployment_released(pair: Dictionary) -> void:
	var released := false
	for _i in range(400):
		pair.host.step()
		pair.joiner.step()
		if pair.joiner.is_joined_in_match() \
				and not pair.joiner.is_join_deploy_pick_pending() \
				and not pair.joiner.is_deploy_overlay_active():
			released = true
			break
		OS.delay_msec(2)
	assert_true(released,
			"the real host clears deployment-pending and releases the selection")


# An alive, admitted player may still be held on the authority's deploy map.
# Initial map selection must send C2S 0x0E, not just dismiss the local screen.
# [orig: Input_HandleActionBinding @0x49AD40, case 12 @0x49B0C5..0x49B17B; Server_OnPlayerJoin @0x51A680, hold @0x51A6F2]
func test_initial_overlay_row_selection_releases_authority_deployment() -> void:
	var pair := _join_pair_in_match()
	var overlay := false
	for _i in range(240):
		pair.host.step()
		pair.joiner.step()
		if pair.joiner.is_deploy_overlay_active():
			overlay = true
			break
		OS.delay_msec(2)
	assert_true(overlay, "the held joiner's per-frame flags1 bit1 arms the overlay")
	assert_false(pair.joiner.is_join_deploy_pick_pending(), "no selection has been sent yet")
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)
	assert_true(presenter.open(), "the overlay opens the deploy screen without a pick")
	assert_signal_emitted(presenter, "opened")
	for _i in range(20):
		pair.host.step()
		pair.joiner.step()
		OS.delay_msec(2)
	await get_tree().process_frame
	assert_true(presenter.is_open(), "the host-held overlay keeps the screen open")
	var rows := presenter.get_spawn_rows()
	var row := -1
	for i in rows.size():
		if rows[i].param != -1:
			row = i
			break
	assert_gte(row, 0, "the spawn list carries a selectable row")
	presenter.select_spawn_row(row)
	assert_false(presenter.is_open(), "the overlay row click closes the screen locally")
	assert_signal_emitted(presenter, "closed")
	_assert_deployment_released(pair)


# The initial overlay's default-spawn key must send the same request as a row
# click; with a pick owed, a key pick keeps the screen until the host releases
# (the engine's deploy-key rule, hud::hud_deploy_key_pick).
func test_initial_overlay_space_sends_default_spawn_selection() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)
	assert_true(presenter.open(), "the pending deploy UI opens")
	assert_true(pair.joiner.is_join_deploy_pick_pending(), "a pick is owed")
	var key := InputEventKey.new()
	key.keycode = KEY_X
	key.pressed = true
	presenter.get_viewport().push_input(key)
	await get_tree().process_frame
	assert_true(presenter.is_open(),
			"with a pick owed, X never closes the screen ahead of the host's release")

	var pair2 := _join_pair_in_match()
	var overlay := false
	for _i in range(240):
		pair2.host.step()
		pair2.joiner.step()
		if pair2.joiner.is_deploy_overlay_active():
			overlay = true
			break
		OS.delay_msec(2)
	assert_true(overlay, "the held joiner's overlay arms")
	assert_false(pair2.joiner.is_join_deploy_pick_pending(), "no selection has been sent yet")
	var overlay_presenter := _make_presenter(pair2.joiner)
	watch_signals(overlay_presenter)
	assert_true(overlay_presenter.open(), "the overlay opens without a pick")
	var space := InputEventKey.new()
	space.keycode = KEY_SPACE
	space.pressed = true
	overlay_presenter.get_viewport().push_input(space)
	await get_tree().process_frame
	assert_false(overlay_presenter.is_open(),
			"SPACE selects the default spawn and closes the initial overlay")
	assert_signal_emitted(overlay_presenter, "closed")
	_assert_deployment_released(pair2)


# The active session keeps running under the death screen — per-frame S2C 0x0A
# traffic continues while the pick is owed. The deploy UI is driven by the
# independent authoritative pending bit and must survive live session frames.
func test_live_session_frames_do_not_close_a_still_pending_deploy_screen() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)

	assert_true(presenter.open(), "the pending deploy UI opens in an active session")
	# Live traffic with no pick sent: the authority keeps the hold.
	for _i in range(30):
		pair.host.step()
		pair.joiner.step()
		OS.delay_msec(2)
	await get_tree().process_frame

	assert_true(presenter.is_open(), "live session frames do not dismiss a pending deploy UI")
	assert_signal_emit_count(presenter, "closed", 0,
			"only authority clearing deployment-pending closes a healthy screen")


func test_refresh_preserves_selected_spawn_identity_by_param() -> void:
	# Retail rebuilds the row set every 16 ticks and preserves the selected
	# node/param, not the visual row number. The periodic rebuild runs against
	# the REAL zone rows here; selection must survive the clear+repopulate.
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)

	assert_true(presenter.open(), "the pending death pick opens death.mnu")
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	if driver == null:
		return
	var list_id := driver.widget_id("SPAWNPOINTS_LIST")
	assert_gte(list_id, 0, "death.mnu authors the spawn list")
	await _await_refresh(pair)
	var zone_rows: Array = pair.joiner.get_deploy_spawn_zones()
	assert_eq(zone_rows.size(), 1,
			"the fixture exposes one team-owned non-default spawn-zone row")
	assert_eq(driver.get_widget_items(list_id).size(), 1 + 2 * zone_rows.size(),
			"default plus the secured zone and its blank separator")
	assert_eq(presenter.get_spawn_rows().size(), 1 + 2 * zone_rows.size(),
			"the presenter row model aligns with the compiled list")
	var zone_param := (zone_rows[0] as DeployZoneRow).param
	assert_gt(zone_param, 0)
	# The whole-list text sort orders "'A' zone" before "'D' Home Base": find
	# the zone row by its param, never by a fixed index.
	var zone_row := _row_index_for_param(presenter, zone_param)
	assert_gte(zone_row, 0, "the zone row is in the compiled list")
	driver.select_row(list_id, zone_row, false)  # highlight only; picks ride user clicks
	assert_eq(_row_param(presenter, zone_row), zone_param,
			"the zone row is selected by deploy param")

	await _await_refresh(pair)

	var reselected := driver.selected_row(list_id)
	assert_gte(reselected, 0, "the selected spawn survives the periodic rebuild")
	assert_eq(_row_param(presenter, reselected), zone_param,
			"selection follows the spawn param across the clear+repopulate")


# Death mid-match re-arms the pick and the shell reopens THIS SAME presenter: the menu is
# configured once (_ensure_menu reuses the frame+driver) and close() only hides it, so the
# driver's list state — and the row the player picked last time — survive into the next
# deploy screen, where _populate_spawn_list re-selects that row by param. The driver
# re-emits widget_value_changed on every list click (the old ItemList allow_reselect=true
# successor semantics), so a click on the already-selected row still queues a pick.
# [orig: DeathScreen_OnSpawnListSelect @0x553630 -> Input_QueueEvent(12, node) @0x55364d]
# The tail is the REAL deployment release: the default-spawn pick rides C2S 0x0E to the
# live host and the falling pending bit closes the screen through `closed`.
# The populate's second loop lands wave occupants + a blank separator at node
# -1; the select callback guards node != -1, so a click on such a row queues
# NO C2S 0x0E — the pick stays owed. [orig: UI_UpdateDeathScreenContent
#  @0x553c5f..0x553de3; DeathScreen_OnSpawnListSelect @0x55364d]
func test_occupant_rows_carry_node_minus_one_and_never_pick() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	assert_true(presenter.open(), "the join deploy screen opens")
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	if driver == null:
		return
	await _await_refresh(pair)
	var list_id := driver.widget_id("SPAWNPOINTS_LIST")
	# Even an empty team zone contributes a non-selectable blank separator.
	var separator_count := 0
	for row in presenter.get_spawn_rows():
		if row.param == -1:
			separator_count += 1
		else:
			assert_gte(row.param, 0, "spawn choices carry a nonnegative param")
	assert_eq(separator_count, 1, "the empty fixture zone retains its separator")
	presenter.select_spawn_row(_row_index_for_param(presenter, -1))
	# The engine builder's row model IS what a wave group would insert: a
	# synthetic occupant row at the presenter seam proves the guard.
	presenter.append_spawn_row("Ace", -1)
	presenter.select_spawn_row(presenter.get_spawn_rows().size() - 1)
	for _i in range(40):
		pair.host.step()
		pair.joiner.step()
		OS.delay_msec(2)
	assert_true(pair.joiner.is_join_deploy_pick_pending(),
			"a node -1 row never sends the deploy pick")
	# The statics follow the witnessed gates: the pick penalty lapsed and no wave
	# lists the player (no status line, no hold), while the other-player kill
	# opened the 120-second revive window -> the MEDIC pair shows, the
	# RESPAWN/PSPRESPAWN pair stays hidden, the list title shows.
	var status: DeployStatus = pair.joiner.get_deploy_status(Strings.get_table(Strings.TABLE_GAMETEXT), "")
	assert_eq(status.queued_kind, 0, "no penalty or wave line")
	assert_true(status.show_medic,
			"the open revive window shows the medic pair")
	for control_name in ["STATIC_RESPAWN_MSG1", "STATIC_PSPRESPAWN_MSG1"]:
		var id := driver.widget_id(control_name)
		if id >= 0:
			assert_false(driver.is_widget_shown(id), control_name + " hidden")
	for control_name in ["STATIC_MEDIC_MSG1", "STATIC_CALLMEDIC_MSG"]:
		var id := driver.widget_id(control_name)
		if id >= 0:
			assert_true(driver.is_widget_shown(id), control_name + " shown")
	var title_id := driver.widget_id("STATIC_LIST_TITLE")
	if title_id >= 0:
		assert_true(driver.is_widget_shown(title_id), "the list title shows with the list")
	assert_eq(driver.get_widget_items(list_id).size(), presenter.get_spawn_rows().size() - 1,
			"the compiled list mirrors the real row model")


func test_a_reopened_death_screen_repicks_and_the_release_closes_it() -> void:
	var pair := _join_pair_with_pending_pick()
	var presenter := _make_presenter(pair.joiner)
	watch_signals(presenter)

	assert_true(presenter.open(), "the join deploy screen opens")
	var driver: MenuDriver = presenter.get_menu_driver()
	assert_not_null(driver)
	if driver == null:
		return
	var list_id := driver.widget_id("SPAWNPOINTS_LIST")
	assert_gte(list_id, 0, "death.mnu authors the spawn list")
	await _await_refresh(pair)

	# Close and reopen WITHOUT a release: the once-configured menu and its
	# selected row must come back (the death-edge reopen shape).
	var default_row := _row_index_for_param(presenter, 0)
	assert_gte(default_row, 0, "the Default Spawn row is in the compiled list")
	driver.select_row(list_id, default_row, false)  # the player's pick, highlight only
	presenter.close()
	assert_false(presenter.is_open())
	assert_true(pair.joiner.is_join_deploy_pick_pending(),
			"nothing was released; the pick is still owed")
	assert_true(presenter.open(), "the death edge reopens the deploy screen")
	assert_eq(presenter.get_menu_driver(), driver,
			"the menu is configured once; close() only hides it")
	var selected := driver.selected_row(list_id)
	assert_gte(selected, 0, "the reopened screen restores the previous pick")
	assert_eq(_row_param(presenter, selected), 0,
			"and the restored row is the default spawn")

	# The re-pick CLICK on the already-selected row queues the REAL C2S 0x0E
	# (the driver re-emits on every click — allow_reselect successor); the
	# host's release clears the pending bit, and the presenter's poll closes
	# the screen through `closed` (the deployment release, not a teardown).
	driver.select_row(list_id, selected)
	var released := false
	for _i in range(800):
		pair.host.step()
		pair.joiner.step()
		if not pair.joiner.is_join_deploy_pick_pending():
			released = true
			break
		OS.delay_msec(2)
	assert_true(released, "the picked deployment releases on the live host")
	await get_tree().process_frame
	assert_false(presenter.is_open(), "the release closes the screen")
	# One `closed` from the manual death-edge close above, one from the release.
	assert_signal_emit_count(presenter, "closed", 2,
			"the release hands gameplay input back through `closed`")


func test_instruction_widgets_follow_retained_death_text() -> void:
	var pair := _join_pair_with_pending_pick()
	pair.joiner.retain_feed_announcement("DeployJoiner was killed.", 1)
	assert_eq(pair.joiner.get_kill_announcement_tick(188), 0)
	var presenter := _make_presenter(pair.joiner)
	assert_true(presenter.open())
	var driver := presenter.get_menu_driver()
	await _await_refresh(pair)
	var first := driver.widget_id("STATIC_INSTRUCTIONS_MSG")
	var second := driver.widget_id("STATIC_INSTRUCTIONS2_MSG")
	assert_gte(first, 0)
	assert_gte(second, 0)
	assert_false(driver.is_widget_shown(first), "AAS hides the first instruction")
	var status: DeployStatus = pair.joiner.get_deploy_status(Strings.get_table(Strings.TABLE_GAMETEXT), "")
	assert_eq(driver.is_widget_shown(second), status.show_instruction2)
	assert_eq(driver.get_widget_text(second), Strings.get_table(Strings.TABLE_GAMETEXT)
			.get_string_in_section("Overlays", "STROVER_RESPAWN1"),
			"the registered spawn zone selects RESPawn1")
	if status.replace_instruction:
		assert_eq(driver.get_widget_text(first), "DeployJoiner was killed.",
				"the hidden instruction retains the expired kill message")


# The listen host's own player holds on the same deploy map as a joiner
# (D-NET-339): its own client folds the per-frame overlay bit, the frame loop
# opens DEATH once off it, and its SPACE pick rides its loopback as the C2S
# 0x0E its server releases. A later death re-arms the screen through the
# frame loop's dead-bit trigger; the X re-pick keeps the screen up until that
# release, and nothing redeploys the host on its own.
# [orig: Server_OnPlayerJoin @0x51a6f2 (no host exemption); Input_HandleActionBinding
#  case 12 @0x49b17b; Render_ProcessMainSceneFrame @0x5CAB39..0x5CAB8B]
func test_the_listen_host_deploys_its_own_player_through_the_deploy_map() -> void:
	var pair := _join_pair_in_match(true)
	var host: Simulation = pair.host
	var overlay := false
	for _i in range(240):
		host.step()
		pair.joiner.step()
		if host.is_deploy_overlay_active():
			overlay = true
			break
		OS.delay_msec(2)
	assert_true(overlay, "the host's own client folds its held overlay")
	assert_true(host.is_death_menu_held(), "the overlay holds the host's screen")
	assert_false(host.take_death_menu_open(true), "another screen holds the open off")
	assert_true(host.take_death_menu_open(false), "the frame loop opens the screen once")
	assert_false(host.take_death_menu_open(false), "the open latch holds")
	var presenter := _make_presenter(host)
	assert_true(presenter.open(), "the overlay opens the host's deploy screen")
	var space := InputEventKey.new()
	space.keycode = KEY_SPACE
	space.pressed = true
	presenter.get_viewport().push_input(space)
	await get_tree().process_frame
	assert_false(presenter.is_open(), "SPACE sends the auto pick and closes the overlay")
	var released := false
	for _i in range(240):
		host.step()
		pair.joiner.step()
		if not host.is_deploy_overlay_active() and not host.is_death_menu_held():
			released = true
			break
		OS.delay_msec(2)
	assert_true(released, "the host's own looped-back 0x0E releases its hold")
	assert_false(host.take_death_menu_open(false), "the cleared triggers re-arm nothing")

	# A death past the 620-tick spawn window: the pick penalty is the stock
	# three-second floor.
	for _i in range(640):
		host.step()
		pair.joiner.step()
	assert_eq(host.debug_kill_player_entity(host.get_local_player_wire_handle()), OK,
			"the host queued its own player's death")
	var dead := false
	for _i in range(120):
		host.step()
		pair.joiner.step()
		if host.is_local_player_dead():
			dead = true
			break
		OS.delay_msec(2)
	assert_true(dead, "the host's own player died")
	assert_true(host.is_death_menu_held(), "the in-session death holds the screen")
	assert_true(host.take_death_menu_open(false), "the dead bit re-arms the open")
	# Past the pick penalty, still dead: nothing redeploys the host by itself.
	for _i in range(260):
		host.step()
		pair.joiner.step()
	assert_true(host.is_local_player_dead(), "the expired hold redeploys nothing")
	var death_presenter := _make_presenter(host)
	assert_true(death_presenter.open(), "the death opens the host's deploy screen")
	var x_key := InputEventKey.new()
	x_key.keycode = KEY_X
	x_key.pressed = true
	death_presenter.get_viewport().push_input(x_key)
	await get_tree().process_frame
	assert_true(death_presenter.is_open(), "a death re-pick keeps the screen until the release")
	var respawned := false
	for _i in range(240):
		host.step()
		pair.joiner.step()
		await get_tree().process_frame
		if not host.is_local_player_dead() and not death_presenter.is_open():
			respawned = true
			break
	assert_true(respawned, "the host's X pick redeploys it and the release closes the screen")
