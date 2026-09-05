extends GutTest

# The minimap water-mask lane of GameHudPresenter over a REAL HudOverlay (the
# HudFixture staged root) and the persisted HUD color-scheme edge machine.

var _staged_dir := ""


func after_each() -> void:
	if not _staged_dir.is_empty():
		TestFs.remove_dir_recursive(_staged_dir)
		_staged_dir = ""


func test_world_build_publishes_even_an_empty_water_mask() -> void:
	var world: GameWorld = autofree(GameWorld.new())
	watch_signals(world)

	world.build_minimap_water_mask()

	assert_signal_emitted_with_parameters(
			world, "minimap_water_changed", [null])


func test_presenter_refreshes_an_existing_hud_when_the_water_mask_changes() -> void:
	# A REAL loaded world: the overlay installs a water mask only beside the
	# mission's terrain routing (set_minimap_terrain drops both without it).
	_staged_dir = HudFixture.stage_root(true)
	var world := WorldFixture.boot_minimal(self, _staged_dir)
	var presenter := HudFixture.presenter_over(self, world)
	assert_true(world.is_loaded())
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	var texture := ImageTexture.create_from_image(image)
	assert_not_same(presenter.get_game_hud().get_minimap_water_mask(), texture)

	world.emit_signal("minimap_water_changed", texture)

	assert_same(presenter.get_game_hud().get_minimap_water_mask(), texture,
			"the existing overlay adopts the rebuilt mask")


func test_hud_color_poll_edges_gate_and_chord() -> void:
	var presenter: GameHudPresenter = autofree(GameHudPresenter.new())
	var start := presenter.hud_color_index()

	# A plain down edge cycles once; holding the key does not repeat.
	presenter.poll_hud_color_edge(true, false, true)
	assert_eq(presenter.hud_color_index(), (start + 1) % 6,
			"the down edge cycles the scheme once")
	presenter.poll_hud_color_edge(true, false, true)
	assert_eq(presenter.hud_color_index(), (start + 1) % 6,
			"holding the key does not re-cycle")

	# Held across a closed gate window: the latch rides the UNGATED state,
	# so reopening the gate with the key still down cannot re-fire.
	presenter.poll_hud_color_edge(true, false, false)
	presenter.poll_hud_color_edge(true, false, true)
	assert_eq(presenter.hud_color_index(), (start + 1) % 6,
			"a press held across a gate window cannot re-fire")

	# A chorded press (our debug picks ride Shift+F6) never cycles.
	presenter.poll_hud_color_edge(false, false, true)
	presenter.poll_hud_color_edge(true, true, true)
	assert_eq(presenter.hud_color_index(), (start + 1) % 6,
			"a chorded press never cycles")

	# Wrap the persisted config token back to its starting value (six
	# cycles are the identity), so the shared user:// settings survive.
	for i in range(5):
		presenter.cycle_hud_color()
	assert_eq(presenter.hud_color_index(), start,
			"the test leaves the persisted scheme where it started")
