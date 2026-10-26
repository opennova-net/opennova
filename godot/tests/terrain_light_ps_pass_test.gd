extends GutTest

## The terrain pool-light pass the reference adapter draws: the ps.1.1 program
## sat(dp3(cube-normalized light vector, detail coefficient normal)) x the
## texlight2d disc x the same texture at the height coordinate x c0, with no
## doubling anywhere (renderer/light_terrain_pass.h carries the witness:
## Light_SetupTerrainProjectedPassPS and the source at 0x7d9fa0). The lit-card
## pins need a RenderingDevice: windowed/terrain_light_ps_pass_device_test.gd.


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
