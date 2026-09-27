extends GutTest

## The terrain leg of the EffectWorld light pool at the device seam: per
## terrain patch (an AABB), the pool lights whose volume overlaps it, gated by
## the authored terrain-disable flag, as the rows the terrain shader re-draws
## the patch with — the terrain twin of per_model_light_isolation_test
## [orig: the per-light arm of render_terrain_sector_batch @0x6092A0
## (the <= 16 collect @0x609658, both light groups cleared @0x60967c /
## @0x609685, LightInstance_IsAliveAndLightsTerrain @0x609880) ->
## Light_SetupTerrainProjectedPassPS @0x5aab30, the reference adapter's
## ps.1.1 pass].

# The synthetic Tmap terrain (fixtures/terrain/tmap) staged over the minimal
# assets it names; one root per test file (TestFs.staged_tmap), removed at the end.
const TMAP_STAGE := "light_pass"


func after_all() -> void:
	TestFs.release_staged_tmap(TMAP_STAGE)


const NEAR_PATCH := AABB(Vector3(-8.0, -2.0, -8.0), Vector3(16.0, 6.0, 16.0))
const FAR_PATCH := AABB(Vector3(4088.0, -2.0, -8.0), Vector3(16.0, 6.0, 16.0))


func _rows(scene: LightScene, patches: Array) -> Array:
	var boxes: Array[AABB] = []
	for box in patches:
		boxes.append(box)
	return scene.collect_terrain_light_rows_for_bounds(boxes, Vector3.ONE, 0,
			null, 0x808080)


func test_a_world_light_reaches_only_the_patch_it_overlaps() -> void:
	var scene := LightScene.new()
	assert_gt(scene.spawn_glow(GlowSpawn.make(Vector3(0.0, 1.0, 0.0), 8.0, Color.WHITE)), 0)
	var rows := _rows(scene, [NEAR_PATCH, FAR_PATCH])
	assert_eq(rows.size(), 2, "one row list per patch")
	assert_eq((rows[0] as Array).size(), 1,
			"the patch under the light re-draws with it")
	assert_eq((rows[1] as Array).size(), 0,
			"a patch outside the light's volume draws no pool")
	var row: Dictionary = rows[0][0]
	var position: Vector3 = row.get("position", Vector3.ZERO)
	assert_almost_eq(position.x, 0.0, 0.01)
	assert_almost_eq(position.y, 1.0, 0.01, "the row position is Godot world")
	assert_almost_eq(position.z, 0.0, 0.01)
	# The ps.1.1 pass projects at 0.4 / radius: the disc's 0..1 span covers
	# 2.5 radii [orig: 26214.4 / range @0x5aab76].
	assert_almost_eq(float(row.get("inv_scale", 0.0)), 0.4 / 8.0, 0.0001)
	# c0 = rgb (255/256) x blend 1 x ambient 1 x recip factor 1 (0x808080
	# divides to exactly one), with no 0.66 / 0.5 fold [orig: @0x5aad93..0x5aae8c].
	var color: Vector3 = row.get("color", Vector3.ZERO)
	var expected := 255.0 / 256.0
	assert_almost_eq(color.x, expected, 0.002)
	assert_almost_eq(color.y, expected, 0.002)
	assert_almost_eq(color.z, expected, 0.002)


func test_the_authored_terrain_disable_flag_keeps_a_light_off_the_ground() -> void:
	var scene := LightScene.new()
	# Flag 0x400 (disable_lightterrain) — the light still lights objects.
	assert_gt(scene.spawn_glow(GlowSpawn.make(Vector3(0.0, 1.0, 0.0), 8.0, Color.WHITE)
			.masking(false, true, false)), 0)
	var rows := _rows(scene, [NEAR_PATCH])
	assert_eq((rows[0] as Array).size(), 0,
			"a terrain-disabled light never reaches the terrain pass")
	# The converse: an object-disabled light still pools on the ground.
	scene.clear()
	assert_gt(scene.spawn_glow(GlowSpawn.make(Vector3(0.0, 1.0, 0.0), 8.0, Color.WHITE)
			.masking(false, false, true)), 0)
	rows = _rows(scene, [NEAR_PATCH])
	assert_eq((rows[0] as Array).size(), 1,
			"the object-disable flag does not gate the terrain pass")


func test_an_owned_light_never_reaches_the_terrain() -> void:
	# Both light groups are cleared for the terrain batch, so an owned light
	# (muzzle glow, subobject record) never matches [orig: @0x60967c/@0x609685].
	var scene := LightScene.new()
	assert_gt(scene.spawn_glow(GlowSpawn.make(Vector3(0.0, 1.0, 0.0), 8.0, Color.WHITE)
			.owned_by(77)), 0)
	var rows := _rows(scene, [NEAR_PATCH])
	assert_eq((rows[0] as Array).size(), 0,
			"an owned pool light lights its owner's draws, never the ground")




## The device end to end: a built Terrain handed the pool through
## set_light_context re-draws the patches its own draw list overlaps with the
## pool light (the rows texture row per slot), and a null context retires the
## leg [orig: the per-batch collect @0x609658 over the batch's AABB].
func test_a_built_terrain_collects_rows_for_the_patches_a_light_overlaps() -> void:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(320, 180)
	add_child_autofree(viewport)
	var data := TerrainData.new()
	data.set_trn_path(TestFs.staged_tmap(TMAP_STAGE))
	assert_eq(data.load(), OK, "the Tmap fixture terrain must load")
	var terrain := Terrain.new()
	viewport.add_child(terrain)
	terrain.set_terrain_data(data)
	terrain.build()
	terrain.set_debug_no_frustum(true)
	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.global_position = Vector3(64.0, 27.0, 64.0)
	camera.make_current()

	# A light centred on the camera with a radius that certainly spans the
	# terrain height under it, and certainly not the far sectors.
	var scene := LightScene.new()
	assert_gt(scene.spawn_glow(GlowSpawn.make(Vector3(64.0, 27.0, 64.0), 96.0, Color.WHITE)), 0)
	terrain.set_light_context(scene, 0)
	terrain.render_frame()
	assert_gt(terrain.get_patches_active(), 0, "the frame compiled patches")
	assert_gt(terrain.get_light_rows_total(), 0,
			"the patches under the light re-draw with it")
	assert_gt(terrain.get_light_patches_lit(), 0)
	assert_lt(terrain.get_light_patches_lit(), terrain.get_patches_active(),
			"a far patch draws no pool")
	var material: ShaderMaterial = terrain.get_terrain_material()
	assert_true(bool(material.get_shader_parameter("u_terrain_light_enabled")),
			"the shader gate opens while rows exist")
	assert_not_null(material.get_shader_parameter("u_terrain_light_rows"),
			"the rows texture is bound")
	assert_not_null(material.get_shader_parameter("u_terrain_light_disc"))
	assert_true(material.get_shader_parameter("u_terrain_light_cube") is Cubemap,
			"the cube-normalize map is bound for stage 0")
	# t1: the generated detail coefficient map the surface inputs build from
	# polytrn_detailmap (retail texture slot 8).
	var coefficient: Texture2D = terrain.get_surface_inputs().get_detail_coefficient_texture()
	assert_not_null(coefficient, "the Tmap fixture authors a detailmap")
	assert_same(material.get_shader_parameter("u_terrain_light_normal"), coefficient,
			"the coefficient map is bound for stage 1")

	# No pool: the leg retires and the gate closes. (The pixel end of the leg
	# cannot be pinned here — the headless GUT runner rasterizes nothing, so a
	# SubViewport texture has no image; the capture probe's frames carry it.)
	terrain.set_light_context(null, 0)
	terrain.render_frame()
	assert_eq(terrain.get_light_rows_total(), 0)
	assert_false(bool(material.get_shader_parameter("u_terrain_light_enabled")))

	# Re-arming the same terrain pushes the enable again: the direct write of
	# the null context is the latched value, not a stale open gate.
	terrain.set_light_context(scene, 0)
	terrain.render_frame()
	assert_gt(terrain.get_light_rows_total(), 0)
	assert_true(bool(material.get_shader_parameter("u_terrain_light_enabled")),
			"a re-armed light scene re-opens the shader gate")


func test_the_falloff_texture_has_the_witnessed_shape() -> void:
	# The ps.1.1 pass samples texlight2d on both falloff stages; the
	# texlightspot1d retail also builds sits in a slot no terrain pass reads.
	var size: int = LightScene.terrain_light_texture_size()
	assert_eq(size, 64)
	var disc: PackedByteArray = LightScene.terrain_light_disc_rgba8()
	assert_eq(disc.size(), size * size * 4)
	# The border texels are 0; the centre texel is the exp(-4x^2) peak
	# [orig: Texture_GenerateProceduralFalloffTexture @0x5a92c0 mode 2].
	assert_eq(disc[0], 0, "the disc border is transparent black")
	var centre := (32 * size + 32) * 4
	assert_eq(disc[centre], 254, "the disc centre is the truncated peak")
	assert_eq(disc[centre + 3], 255, "mode 2 texels carry an opaque alpha")
