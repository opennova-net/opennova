extends GutTest


class MinimapHudStub:
	extends RefCounted
	var calls: Array[Dictionary] = []

	func set_minimap_terrain(terrain: TerrainData, texture: Texture2D) -> void:
		calls.append({"terrain": terrain, "texture": texture})


class MinimapWorldHarness:
	extends GameWorld

	func rebuild_minimap_water_mask_for_test() -> void:
		load_stages().build_minimap_water_mask()


class MinimapPresenterHarness:
	extends GameHudPresenter

	func install_hud_fixture(hud) -> void:
		_game_hud = hud

	func color_index() -> int:
		return _hud_color_index


func test_world_build_publishes_even_an_empty_water_mask() -> void:
	var world: MinimapWorldHarness = autofree(MinimapWorldHarness.new())
	watch_signals(world)

	world.rebuild_minimap_water_mask_for_test()

	assert_signal_emitted_with_parameters(
			world, "minimap_water_changed", [null])


func test_presenter_refreshes_an_existing_hud_when_the_water_mask_changes() -> void:
	var world: GameWorld = autofree(GameWorld.new())
	var presenter: MinimapPresenterHarness = autofree(
			MinimapPresenterHarness.new())
	presenter.setup(world, null, null)
	var hud := MinimapHudStub.new()
	presenter.install_hud_fixture(hud)
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	var texture := ImageTexture.create_from_image(image)

	world.emit_signal("minimap_water_changed", texture)

	assert_eq(hud.calls.size(), 1)
	assert_same(hud.calls[0]["texture"], texture)


func test_hud_color_poll_edges_gate_and_chord() -> void:
	var presenter: MinimapPresenterHarness = autofree(
			MinimapPresenterHarness.new())
	var start := presenter.color_index()

	# A plain down edge cycles once; holding the key does not repeat.
	presenter.poll_hud_color_edge(true, false, true)
	assert_eq(presenter.color_index(), (start + 1) % 6,
			"the down edge cycles the scheme once")
	presenter.poll_hud_color_edge(true, false, true)
	assert_eq(presenter.color_index(), (start + 1) % 6,
			"holding the key does not re-cycle")

	# Held across a closed gate window: the latch rides the UNGATED state,
	# so reopening the gate with the key still down cannot re-fire.
	presenter.poll_hud_color_edge(true, false, false)
	presenter.poll_hud_color_edge(true, false, true)
	assert_eq(presenter.color_index(), (start + 1) % 6,
			"a press held across a gate window cannot re-fire")

	# A chorded press (our debug picks ride Shift+F6) never cycles.
	presenter.poll_hud_color_edge(false, false, true)
	presenter.poll_hud_color_edge(true, true, true)
	assert_eq(presenter.color_index(), (start + 1) % 6,
			"a chorded press never cycles")

	# Wrap the persisted config token back to its starting value (six
	# cycles are the identity), so the shared user:// settings survive.
	for i in range(5):
		presenter.cycle_hud_color()
	assert_eq(presenter.color_index(), start,
			"the test leaves the persisted scheme where it started")
