extends GutTest

const DeployPresenter := preload("res://engine/world/deploy_screen_presenter.gd")
const TMP_DIR := "res://.godot/deploy_screen_presenter_test"


class FakeSim:
	extends RefCounted
	var zones: Array[Dictionary] = []
	var picks: Array[int] = []
	var pick_pending := true
	var in_match := false

	func is_join_deploy_pick_pending() -> bool:
		return pick_pending

	func is_joined_in_match() -> bool:
		return in_match

	func get_join_assigned_team() -> int:
		return 1

	func get_deploy_spawn_zones() -> Array[Dictionary]:
		return zones

	func send_deployment_pick(param: int) -> bool:
		picks.append(param)
		return true


class FakeWorld:
	extends Node
	var root: NovaResourceRoot
	var sim: FakeSim

	func get_resource_root() -> NovaResourceRoot:
		return root

	func get_sim() -> FakeSim:
		return sim


func before_each() -> void:
	NovaStrings.clear()
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	if not DirAccess.dir_exists_absolute(dir):
		assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_copy_fixture("res://../fixtures/mnu/jo_death.mnu", dir.path_join("death.mnu"))
	_copy_fixture("res://../fixtures/rtxt/menutxt.bin", dir.path_join("menutxt.BIN"))
	_copy_fixture("res://../fixtures/rtxt/gametext.bin", dir.path_join("gametext.bin"))


func after_each() -> void:
	NovaStrings.clear()


func after_all() -> void:
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	for name in ["death.mnu", "menutxt.BIN", "gametext.bin"]:
		var path := dir.path_join(name)
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	DirAccess.remove_absolute(dir)


func _copy_fixture(source: String, target: String) -> void:
	var output := FileAccess.open(target, FileAccess.WRITE)
	assert_not_null(output, "temporary deploy-screen fixture opens for write")
	if output != null:
		output.store_buffer(FileAccess.get_file_as_bytes(source))
		output.close()


func _make_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(TMP_DIR)), OK)
	return root


func _open_presenter(sim: FakeSim):
	var world := FakeWorld.new()
	world.root = _make_root()
	world.sim = sim
	add_child_autofree(world)
	var overlay := Control.new()
	overlay.size = Vector2(800, 600)
	add_child_autofree(overlay)
	var presenter := DeployPresenter.new()
	presenter.setup(world, overlay)
	add_child_autofree(presenter)
	return presenter


# The shell leaves State.WORLD on `opened` and returns on `closed`; without those
# two signals firing, LocalPlayerPresenter re-captures the mouse every frame and the
# SPAWNPOINTS_LIST rows cannot be clicked at all.
func test_open_and_close_emit_the_shell_state_signals() -> void:
	var sim := FakeSim.new()
	sim.zones = [{"param": 1, "letter": "A", "name_key": "STRWPNAME001"}]
	var presenter = _open_presenter(sim)
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


func test_open_refuses_when_no_pick_is_owed() -> void:
	var sim := FakeSim.new()
	sim.pick_pending = false
	var presenter = _open_presenter(sim)
	watch_signals(presenter)
	assert_false(presenter.open(), "no pending pick means no screen")
	assert_signal_emit_count(presenter, "opened", 0, "a refused open must not move the shell state")
	assert_false(presenter.is_open())


func test_refresh_preserves_selected_spawn_identity_across_live_zone_changes() -> void:
	var sim := FakeSim.new()
	sim.zones = [
		{"param": 1, "letter": "A", "name_key": "STRWPNAME001"},
		{"param": 2, "letter": "B", "name_key": "STRWPNAME002"},
	]
	var world := FakeWorld.new()
	world.root = _make_root()
	world.sim = sim
	add_child_autofree(world)
	var overlay := Control.new()
	overlay.size = Vector2(800, 600)
	add_child_autofree(overlay)
	var presenter := DeployPresenter.new()
	presenter.setup(world, overlay)
	add_child_autofree(presenter)

	assert_true(presenter.open(), "the player-paced join opens death.mnu")
	var menu := overlay.get_node_or_null("DeployScreenMenu") as NovaMnuMenu
	assert_not_null(menu)
	if menu == null:
		return
	var list := menu.find_child("SPAWNPOINTS_LIST", true, false) as ItemList
	assert_not_null(list)
	if list == null:
		return
	assert_eq(list.item_count, 3, "default plus both secured zones")
	list.select(2)
	assert_eq(int(list.get_item_metadata(2)), 2, "zone B is selected by deploy param")

	# Zone A becomes contested. Retail rebuilds the row set every 16 ticks but
	# preserves the selected node/param, not the old visual row number.
	sim.zones = [
		{"param": 2, "letter": "B", "name_key": "STRWPNAME002"},
	]
	await get_tree().create_timer(DeployPresenter.REFRESH_INTERVAL_S + 0.05).timeout

	assert_eq(list.item_count, 2, "the contested zone was removed from the live list")
	var selected := list.get_selected_items()
	assert_eq(selected.size(), 1, "the surviving selected spawn remains highlighted")
	if selected.size() == 1:
		assert_eq(int(list.get_item_metadata(selected[0])), 2,
				"selection follows the spawn param rather than the former row index")
		list.item_selected.emit(selected[0])
		assert_eq(sim.picks, [2], "the refreshed row still queues zone B's pick")


# Death mid-match re-arms the pick and the shell reopens THIS SAME presenter: the menu is
# built once (_ensure_menu reuses it) and close() only hides it, so the ItemList — and
# the row the player picked last time — survive into the next deploy screen, where
# _populate_spawn_list re-selects that row by param. Godot's SELECT_SINGLE ItemList does
# not emit item_selected when an already-selected row is clicked unless allow_reselect is
# set, so without it the reopened screen can never send a second C2S 0x0E: on a co-op map
# the list is a single "Home Base" row, leaving the player stuck on the death screen for
# the rest of the match. The original has no such gate — its select callback queues a
# fresh pick on every click, which is also what makes a re-pick possible after the presenter
# silently drops one (an invalid/contested zone, or its post-death respawn timer).
# [orig: DeathScreen_OnSpawnListSelect @0x553630 -> Input_QueueEvent(12, node) @0x55364d]
func test_a_reopened_death_screen_can_still_pick_the_row_it_is_already_on() -> void:
	var sim := FakeSim.new()
	var world := FakeWorld.new()
	world.root = _make_root()
	world.sim = sim
	add_child_autofree(world)
	var overlay := Control.new()
	overlay.size = Vector2(800, 600)
	add_child_autofree(overlay)
	var presenter := DeployPresenter.new()
	presenter.setup(world, overlay)
	add_child_autofree(presenter)

	# Join-time deploy: no zones, so the co-op shape — one Home Base row.
	assert_true(presenter.open(), "the join deploy screen opens")
	var menu := overlay.get_node_or_null("DeployScreenMenu") as NovaMnuMenu
	assert_not_null(menu)
	if menu == null:
		return
	var list := menu.find_child("SPAWNPOINTS_LIST", true, false) as ItemList
	assert_not_null(list)
	if list == null:
		return
	assert_eq(list.item_count, 1, "a co-op map offers only the default spawn row")
	assert_true(list.allow_reselect,
			"clicking the already-selected spawn row must still queue a pick")

	# The player deploys: click row 0, the presenter releases, the screen closes.
	list.select(0)
	list.item_selected.emit(0)
	assert_eq(sim.picks, [0], "the first deploy sends the parameter-0 pick")
	sim.pick_pending = false
	sim.in_match = true
	presenter.close()
	assert_false(presenter.is_open())

	# Death: the runtime re-arms the pick and the shell reopens the same presenter.
	sim.pick_pending = true
	sim.in_match = false
	assert_true(presenter.open(), "the death edge reopens the deploy screen")
	var reopened := menu.find_child("SPAWNPOINTS_LIST", true, false) as ItemList
	assert_eq(reopened, list, "the menu and its list survive close(); they are rebuilt once")
	var selected := reopened.get_selected_items()
	assert_eq(selected.size(), 1, "the reopened screen restores the previous pick")
	assert_eq(int(reopened.get_item_metadata(selected[0])), 0,
			"and that restored row is the only row there is")
	assert_true(reopened.allow_reselect,
			"so the respawn pick depends entirely on re-selection being allowed")

	reopened.item_selected.emit(selected[0])
	assert_eq(sim.picks, [0, 0], "the respawn queues a second pick from the same row")
