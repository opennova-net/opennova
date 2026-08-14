extends GutTest


class MinimapHudStub:
	extends RefCounted
	var calls: Array[Dictionary] = []

	func set_minimap_terrain(terrain: TerrainData, texture: Texture2D) -> void:
		calls.append({"terrain": terrain, "texture": texture})


func test_world_bake_publishes_even_an_empty_atlas() -> void:
	var world := GameWorld.new()
	watch_signals(world)

	world._bake_minimap_terrain_atlas()

	assert_signal_emitted_with_parameters(
			world, "minimap_terrain_changed", [null])
	world.free()


func test_presenter_refreshes_an_existing_hud_when_the_atlas_changes() -> void:
	var world := GameWorld.new()
	var presenter := GameHudPresenter.new()
	presenter.setup(world, null, null)
	var hud := MinimapHudStub.new()
	presenter._game_hud = hud
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	var texture := ImageTexture.create_from_image(image)

	world.emit_signal("minimap_terrain_changed", texture)

	assert_eq(hud.calls.size(), 1)
	assert_same(hud.calls[0]["texture"], texture)
	world.free()
	presenter.free()
