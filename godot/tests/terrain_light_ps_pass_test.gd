extends GutTest

## The terrain pool-light pass the reference adapter draws: the ps.1.1 program
## sat(dp3(cube-normalized light vector, detail coefficient normal)) x the
## texlight2d disc x the same texture at the height coordinate x c0, with no
## doubling anywhere (renderer/light_terrain_pass.h carries the witness:
## Light_SetupTerrainProjectedPassPS and the source at 0x7d9fa0). The shipped
## terrain shader draws a lit card here from one hand-built light row, the
## engine's textures (LightScene) and a uniform coefficient texel, over a 64/255
## page with the ordinary pass and the fog blacked out, so a pixel is the
## witnessed composite 2 * sat(2 * page) * pool alone.
## A 64 x 64 view straight down from 6 u over an 8 x 8 card; pixel (32, 32)
## lies within 0.06 u of the origin.

const CENTRE := Vector2i(32, 32)
const PAGE_BYTE := 64
const LIGHT_RADIUS := 4.0
# 0.4 / r: the ps.1.1 pass's projection scale.
const INV_SCALE := 0.4 / LIGHT_RADIUS
# A light this far above the card puts the height coordinate on the centre of
# texlight2d column 20: u = 0.5 - h * inv = 20.5 / 64.
const LIGHT_HEIGHT := 1.796875


func _rgba8_texture(width: int, height: int, bytes: PackedByteArray) -> ImageTexture:
	return ImageTexture.create_from_image(
			Image.create_from_data(width, height, false, Image.FORMAT_RGBA8, bytes))


func _cube_texture() -> Cubemap:
	var size: int = LightScene.terrain_light_cube_size()
	var faces: Array[Image] = []
	for face in 6:
		faces.append(Image.create_from_data(size, size, false, Image.FORMAT_RGBA8,
				LightScene.terrain_light_cube_face_rgba8(face)))
	var cube := Cubemap.new()
	assert_eq(cube.create_from_images(faces), OK, "the six cube faces load")
	return cube


func _normal_texture(normal: Color) -> ImageTexture:
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(normal)
	return ImageTexture.create_from_image(image)


## One light row in slot 0: (position, inv_scale) then (c0, count 1).
func _rows_texture(light: Vector3, c0: float) -> ImageTexture:
	var floats := PackedFloat32Array()
	floats.resize(32 * 4)
	floats[0] = light.x
	floats[1] = light.y
	floats[2] = light.z
	floats[3] = INV_SCALE
	floats[4] = c0
	floats[5] = c0
	floats[6] = c0
	floats[7] = 1.0
	return ImageTexture.create_from_image(Image.create_from_data(32, 1, false,
			Image.FORMAT_RGBAF, floats.to_byte_array()))


## The lit value at the centre pixel, back in the scene target's domain.
func _centre_value(light: Vector3, normal: Color, c0: float) -> float:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var clear := WorldEnvironment.new()
	clear.environment = Environment.new()
	clear.environment.background_mode = Environment.BG_COLOR
	clear.environment.background_color = Color.BLACK
	viewport.add_child(clear)
	var camera := Camera3D.new()
	camera.fov = 60.0
	viewport.add_child(camera)
	camera.look_at_from_position(Vector3(0.0, 6.0, 0.0), Vector3.ZERO, Vector3.FORWARD)
	camera.make_current()
	var page := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	page.fill(Color8(PAGE_BYTE, PAGE_BYTE, PAGE_BYTE, 255))
	var page_images: Array[Image] = [page]
	var pages := Texture2DArray.new()
	assert_eq(pages.create_from_images(page_images), OK)
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/terrain.gdshader") as Shader
	material.set_shader_parameter("u_has_tile_cache", true)
	material.set_shader_parameter("u_tile_cache", pages)
	# The ordinary pass and the fog contribute nothing.
	material.set_shader_parameter("u_sun_light", Vector3.ZERO)
	material.set_shader_parameter("u_sky_ambient", Vector3.ZERO)
	material.set_shader_parameter("u_fog_color", Vector3.ZERO)
	material.set_shader_parameter("u_fog_type", 1)
	material.set_shader_parameter("u_fog_start", 0.0)
	material.set_shader_parameter("u_fog_end", 1.0e6)
	material.set_shader_parameter("u_terrain_light_enabled", true)
	material.set_shader_parameter("u_terrain_light_rows", _rows_texture(light, c0))
	material.set_shader_parameter("u_terrain_light_disc", _rgba8_texture(
			LightScene.terrain_light_texture_size(), LightScene.terrain_light_texture_size(),
			LightScene.terrain_light_disc_rgba8()))
	material.set_shader_parameter("u_terrain_light_cube", _cube_texture())
	material.set_shader_parameter("u_terrain_light_normal", _normal_texture(normal))
	var card := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(8.0, 8.0)
	card.mesh = plane
	card.material_override = material
	viewport.add_child(card)
	card.set_instance_shader_parameter("u_instance_tile_cache_ready", true)
	card.set_instance_shader_parameter("u_instance_tile_cache_layer", 0.0)
	card.set_instance_shader_parameter("u_instance_tile_cache_projection",
			Vector4(-4.0, -4.0, 1.0 / 8.0, 8.0))
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var pixel := viewport.get_texture().get_image().get_pixel(CENTRE.x, CENTRE.y)
	return pixel.srgb_to_linear().r


func _disc_byte(column: int, row: int) -> int:
	var size: int = LightScene.terrain_light_texture_size()
	return LightScene.terrain_light_disc_rgba8()[(row * size + column) * 4]


func test_windowed_pool_is_the_unfolded_ps_product() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	# A flat coefficient texel under a light straight above: N.L = 1, the disc
	# samples its 254 centre, the height coordinate texlight2d column 20, and
	# c0 multiplies in once — no 2 x 2, no 0.66, no 0.5.
	var c0 := 0.25
	var value: float = await _centre_value(Vector3(0.0, LIGHT_HEIGHT, 0.0),
			Color8(128, 128, 255), c0)
	var page_gain := 2.0 * minf(1.0, 2.0 * float(PAGE_BYTE) / 255.0)
	var disc := float(_disc_byte(32, 32)) / 255.0
	var height := float(_disc_byte(20, 31)) / 255.0
	var expected := page_gain * disc * height * c0
	assert_almost_eq(value, expected, 0.006,
			"pool = N.L x disc x height (texlight2d) x c0 (%s vs %s)" % [value, expected])
	# The former device law over the same row (texlightspot1d on the height
	# stage and both MODULATE2X doublings) lands elsewhere.
	var spot1d_height := float(((20 * 4) >> 1) + ((20 * 5) >> 1)) * 0.5 / 255.0
	var fixed_function := page_gain * 4.0 * c0 * disc * spot1d_height
	assert_gt(absf(value - fixed_function), 0.02,
			"not the 2 x 2 x row x disc x texlightspot1d product (%s)" % fixed_function)


func test_windowed_a_light_below_the_surface_draws_no_pool() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	# The same distance below the card: disc and height are unchanged (the
	# height falloff is symmetric), but dp3_sat of the downward light vector
	# with the upward coefficient normal is 0.
	var value: float = await _centre_value(Vector3(0.0, -LIGHT_HEIGHT, 0.0),
			Color8(128, 128, 255), 1.0)
	assert_lt(value, 0.01, "a light under the ground leaves it dark (%s)" % value)


func test_windowed_the_coefficient_normal_steers_the_pool() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	# The cube lookup carries the light vector in D3D (z, x, y) order, which
	# pairs the coefficient texel's red with world x and its green with world
	# z (renderer::terrain_light_cube_vector): a texel leaning toward the light
	# brightens the pool, one leaning away blacks it out.
	var east := Vector3(1.5, 1.5, 0.0)
	var flat: float = await _centre_value(east, Color8(128, 128, 255), 1.0)
	var toward: float = await _centre_value(east, Color8(226, 128, 180), 1.0)
	var away: float = await _centre_value(east, Color8(30, 128, 180), 1.0)
	assert_gt(flat, 0.05, "an oblique light still pools (%s)" % flat)
	assert_gt(toward, flat + 0.02,
			"red leaning to +x brightens a light at +x (%s vs %s)" % [toward, flat])
	assert_lt(away, 0.005, "red leaning to -x turns a +x light away (%s)" % away)
	# The same elevation toward +z: the flat texel pools alike by symmetry.
	var south := Vector3(0.0, 1.5, 1.5)
	var toward_z: float = await _centre_value(south, Color8(128, 226, 180), 1.0)
	var away_z: float = await _centre_value(south, Color8(128, 30, 180), 1.0)
	assert_gt(toward_z, flat + 0.02,
			"green leaning to +z brightens a light at +z (%s vs %s)" % [toward_z, flat])
	assert_lt(away_z, 0.005, "green leaning to -z turns it away (%s)" % away_z)


func test_the_cube_faces_carry_the_generated_normals() -> void:
	# Face centres are the six axes packed trunc(n * 127.5 + 128), in the
	# +X, -X, +Y, -Y, +Z, -Z layer order (renderer::cube_normalize_texel_argb).
	var size: int = LightScene.terrain_light_cube_size()
	assert_eq(size, 256)
	var centre := (128 * size + 128) * 4
	var expected := [[255, 128, 128], [0, 128, 128], [128, 255, 128],
			[128, 0, 128], [128, 128, 255], [128, 128, 0]]
	for face in 6:
		var bytes: PackedByteArray = LightScene.terrain_light_cube_face_rgba8(face)
		assert_eq(bytes.size(), size * size * 4)
		assert_eq([bytes[centre], bytes[centre + 1], bytes[centre + 2]], expected[face],
				"face %d centre" % face)
	assert_eq(LightScene.terrain_light_cube_face_rgba8(6).size(), 0, "no seventh face")
