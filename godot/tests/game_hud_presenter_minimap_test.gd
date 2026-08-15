extends GutTest


class MinimapHudStub:
	extends RefCounted
	var calls: Array[Dictionary] = []

	func set_minimap_terrain(terrain: TerrainData, texture: Texture2D) -> void:
		calls.append({"terrain": terrain, "texture": texture})


class MinimapWorldHarness:
	extends GameWorld

	func rebuild_minimap_water_mask_for_test() -> void:
		_build_minimap_water_mask()


class MinimapPresenterHarness:
	extends GameHudPresenter

	func install_hud_fixture(hud) -> void:
		_game_hud = hud


func test_world_build_publishes_even_an_empty_water_mask() -> void:
	var world := MinimapWorldHarness.new()
	watch_signals(world)

	world.rebuild_minimap_water_mask_for_test()

	assert_signal_emitted_with_parameters(
			world, "minimap_water_changed", [null])
	world.free()


func test_presenter_refreshes_an_existing_hud_when_the_water_mask_changes() -> void:
	var world := GameWorld.new()
	var presenter := MinimapPresenterHarness.new()
	presenter.setup(world, null, null)
	var hud := MinimapHudStub.new()
	presenter.install_hud_fixture(hud)
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	var texture := ImageTexture.create_from_image(image)

	world.emit_signal("minimap_water_changed", texture)

	assert_eq(hud.calls.size(), 1)
	assert_same(hud.calls[0]["texture"], texture)
	world.free()
	presenter.free()
