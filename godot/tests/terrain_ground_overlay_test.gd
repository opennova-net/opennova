extends GutTest

# The editor's ground overlay on the terrain (ADR 0046 DI-29: the mission view's Show > Surface classes and
# Foliage): Terrain.set_ground_overlay lays a picture over the terrain from above, its north-west corner and its
# span on the world's x/z plane, and the one value past it; the terrain's material carries it (u_overlay_on,
# u_overlay, u_overlay_rect, u_overlay_outside), set before the build or after it alike; clear_ground_overlay
# turns it off. The game never sets one: a terrain's material starts with it off.

var _root_dir := ""


func before_all() -> void:
	_root_dir = TestFs.stage_terrain_root("ground_overlay")


func after_all() -> void:
	TestFs.remove_dir_recursive(_root_dir)


func _terrain() -> Terrain:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root_dir), OK, "the staged terrain root mounts")
	var data := TerrainData.new()
	assert_eq(data.load_from_resource_root(root, TestFs.TMAP_TRN), OK, "the Tmap fixture loads")
	var view := SubViewport.new()
	view.size = Vector2i(32, 32)
	view.own_world_3d = true
	add_child_autofree(view)
	var terrain := Terrain.new()
	view.add_child(terrain)
	terrain.set_terrain_data(data)
	return terrain


func _picture() -> Image:
	var image := Image.create_empty(4, 2, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.0, 0.0, 1.0, 0.5))
	return image


func test_the_game_terrain_carries_no_overlay() -> void:
	var terrain := _terrain()
	terrain.build()
	assert_true(terrain.is_built())
	assert_false(terrain.has_ground_overlay())
	assert_false(bool(terrain.get_terrain_material().get_shader_parameter("u_overlay_on")))


func test_an_overlay_set_after_the_build_reaches_the_material() -> void:
	var terrain := _terrain()
	terrain.build()
	# Mission (-512, 512) north-west, 1024 east and 512 south: world x -512, z -512, 1024 by 512.
	terrain.set_ground_overlay(_picture(), Rect2(-512.0, -512.0, 1024.0, 512.0), Color(0.0, 0.0, 1.0, 0.6667), true)
	var material := terrain.get_terrain_material()
	assert_true(terrain.has_ground_overlay())
	assert_true(bool(material.get_shader_parameter("u_overlay_on")))
	var texture: Texture2D = material.get_shader_parameter("u_overlay")
	assert_not_null(texture)
	if texture:
		assert_eq(Vector2i(texture.get_width(), texture.get_height()), Vector2i(4, 2), "the picture as given")
	var rect: Vector4 = material.get_shader_parameter("u_overlay_rect")
	assert_almost_eq(rect.x, -512.0, 0.001)
	assert_almost_eq(rect.y, -512.0, 0.001)
	assert_almost_eq(rect.z, 1.0 / 1024.0, 1e-7, "the inverse of its span across")
	assert_almost_eq(rect.w, 1.0 / 512.0, 1e-7, "and down")
	var outside: Color = material.get_shader_parameter("u_overlay_outside")
	assert_almost_eq(outside.a, 0.6667, 0.001, "past it, the one value")
	terrain.clear_ground_overlay()
	assert_false(terrain.has_ground_overlay())
	assert_false(bool(material.get_shader_parameter("u_overlay_on")))


func test_an_overlay_set_before_the_build_is_kept_by_it() -> void:
	var terrain := _terrain()
	# No picture, one value everywhere (a terrain with no char map reads one class).
	terrain.set_ground_overlay(null, Rect2(), Color(0.6, 0.46, 0.24, 0.6667), true)
	terrain.build()
	var material := terrain.get_terrain_material()
	assert_true(bool(material.get_shader_parameter("u_overlay_on")))
	assert_null(material.get_shader_parameter("u_overlay"))
	var rect: Vector4 = material.get_shader_parameter("u_overlay_rect")
	assert_eq(rect, Vector4.ZERO, "no picture: every point is past it")
	var outside: Color = material.get_shader_parameter("u_overlay_outside")
	assert_almost_eq(outside.r, 0.6, 0.001)
	# Past it nothing, where the game reads no one value there.
	terrain.set_ground_overlay(_picture(), Rect2(0.0, 0.0, 16.0, 8.0), Color(1.0, 1.0, 1.0, 1.0), false)
	outside = material.get_shader_parameter("u_overlay_outside")
	assert_eq(outside.a, 0.0, "no tint past the picture")
