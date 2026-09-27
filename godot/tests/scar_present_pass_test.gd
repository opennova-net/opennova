extends GutTest

# The scar present pass (ScarPresenter's present_frame leg, EntityPresenter's
# owned "Scars" child, ADR 0043 d9) on the typed surfaces (ADR 0034): the draw
# list is pure data through the public present_scar_draw_list leg (production
# present_passes() pulls Simulation.get_scar_draw_list), the owner models are
# real ObjectModels built from the armory fixture (their render-part nodes
# mount the entity-ring meshes), the wire resolver is the presenter's own
# registry with injected nodes, and the textures resolve through a real
# ResourceRoot over a temp dir.
#
# [orig: Scar_RenderAllCaches @0x5CDF70 -> Scar_RenderCache @0x5CD830; the
#  shared ring draws world-space quads, an entity ring's quads are transformed
#  through the owner's section matrix at draw time — the section-node parent
#  here; docs/world/world-wac-ai-re.md §24.9]


const MODEL_GRAPHIC := "armory"
const MODEL_3DI := "res://../fixtures/threedi/synth/armory.3di"
const STRIP_NAMES := ["scorch1.tga", "scorch2.tga", "scorch3.tga", "scorch4.tga"]
const BHOLE_STRIP := 27
# The loader's two GfxShader mode words [orig: Scar_LoadTextures @0x5CC315 /
# @0x5CC321]: every strip but bhole1 draws in the scorch state.
const MODE_WORD_SCORCH := 0x120651
const MODE_WORD_HOLE := 0x460651
const SHADER_SCORCH := "res://shaders/scar_quad.gdshader"
const SHADER_HOLE := "res://shaders/scar_quad_hole.gdshader"
const OWNER_A := 0x1004  # pool 1, slot 4
const OWNER_B := 0x1005
# The pass fog globals the raster legs drive (restored to the project defaults).
const FOG_GLOBALS := ["opennova_fog_enabled", "opennova_fog_color", "opennova_fog_start",
		"opennova_fog_end", "opennova_fog_type"]

var _temp_dirs: Array[String] = []


func after_each() -> void:
	ShaderGlobals.restore_defaults(FOG_GLOBALS)
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()


# A ResourceRoot over a temp dir holding minimal 2x2 BGRA TGAs for the strips.
func _texture_root(names: Array) -> ResourceRoot:
	var dir_path := OS.get_temp_dir().path_join("scar_present_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir_path), OK)
	_temp_dirs.append(dir_path)
	for texture_name in names:
		var bytes := PackedByteArray()
		bytes.resize(18 + 2 * 2 * 4)
		bytes[2] = 2
		bytes[12] = 2
		bytes[14] = 2
		bytes[16] = 32
		bytes[17] = 0x28
		for i in range(18, bytes.size()):
			bytes[i] = 0xff
		var file := FileAccess.open(dir_path.path_join(texture_name), FileAccess.WRITE)
		assert_not_null(file)
		file.store_buffer(bytes)
		file.close()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir_path), OK)
	return root


# A REAL model with render-part nodes, built through the placer's graphic
# registry (the production path — never a hand-assembled node tree).
func _fixture_model(parent: Node) -> ObjectModel:
	var placer := MissionObjectPlacer.new()
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MODEL_3DI)), OK,
			"the armory fixture model loads")
	placer.register_object_data(MODEL_GRAPHIC, data)
	var model: ObjectModel = placer.build_model_from_graphic(MODEL_GRAPHIC, "", parent, "")
	assert_not_null(model, "the fixture model builds")
	return model


func _strip_names() -> PackedStringArray:
	var names := PackedStringArray()
	names.resize(32)
	for i in range(STRIP_NAMES.size()):
		names[i] = STRIP_NAMES[i]
	names[BHOLE_STRIP] = "bhole1.tga"
	return names


func _strip_mode_words() -> PackedInt32Array:
	var words := PackedInt32Array()
	words.resize(32)
	words.fill(MODE_WORD_SCORCH)
	words[BHOLE_STRIP] = MODE_WORD_HOLE
	return words


# One quad = six vertices in the witnessed order around `centre` (half-size 0.25).
# The record's packed arrays are values, so the quad is appended to the local
# arrays and stored back on the record.
func _quad(draw: ScarDrawList, centre: Vector3) -> void:
	var corners := [
		centre + Vector3(-0.25, 0, -0.25), centre + Vector3(0.25, 0, -0.25),
		centre + Vector3(-0.25, 0, 0.25), centre + Vector3(0.25, 0, 0.25),
	]
	var uv := [Vector2(0, 0), Vector2(1, 0), Vector2(0, 1), Vector2(1, 1)]
	var vertices := draw.vertices
	var uvs := draw.uvs
	var colors := draw.colors
	for k in [0, 1, 2, 1, 3, 2]:
		vertices.append(corners[k])
		uvs.append(uv[k])
		colors.append(Color(0.5, 0.5, 0.5, 1.0))
	draw.vertices = vertices
	draw.uvs = uvs
	draw.colors = colors


func _draw_list() -> ScarDrawList:
	var draw := ScarDrawList.new()
	draw.strip_names = _strip_names()
	draw.strip_mode_words = _strip_mode_words()
	return draw


static func _append_i32(values: PackedInt32Array, value: int) -> PackedInt32Array:
	values.append(value)
	return values


func _batch(draw: ScarDrawList, owner: int, texture: int, section: int,
		entity_local: bool, quads: int, bms_id: int = 0,
		spawn_origin: int = Simulation.SPAWN_ORIGIN_NONE) -> void:
	var first := draw.vertices.size()
	for q in range(quads):
		_quad(draw, Vector3(q, 0, 0))
	draw.batch_owner = _append_i32(draw.batch_owner, owner)
	draw.batch_texture = _append_i32(draw.batch_texture, texture)
	draw.batch_section = _append_i32(draw.batch_section, section)
	draw.batch_flags = _append_i32(draw.batch_flags,
			ScarDrawList.FLAG_ENTITY_LOCAL if entity_local else 0)
	draw.batch_first = _append_i32(draw.batch_first, first)
	draw.batch_count = _append_i32(draw.batch_count, quads * 6)
	draw.batch_bms_id = _append_i32(draw.batch_bms_id, bms_id)
	var origins := draw.batch_spawn_origin
	origins.append(spawn_origin)
	draw.batch_spawn_origin = origins
	draw.slots_live += quads


# A REAL EntityPresenter with its scar pass wired: `index` resolves authored
# owners, `wire_nodes` are injected per-handle avatars (its own registry is
# the runtime-only resolver), `root` resolves the strip TGAs. Its "Scars"
# child is the device.
func _make_presenter(index: EntityIndex = null, root: ResourceRoot = null,
		wire_nodes: Dictionary = {}) -> EntityPresenter:
	var entities := EntityPresenter.new()
	add_child_autofree(entities)
	entities.setup(null, index, null)
	for handle in wire_nodes:
		entities.register_wire_node(int(handle), wire_nodes[handle])
	entities.setup_passes(null, null, root, null, null, null, null, null)
	return entities


func _world_mesh(entities: EntityPresenter) -> MeshInstance3D:
	var presenter := entities.scar_presenter()
	return presenter.get_node_or_null("ScarWorld") as MeshInstance3D if presenter != null else null


func _entity_meshes(root: Node) -> Array:
	var out: Array = []
	var stack: Array = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		if n is MeshInstance3D and String(n.name).begins_with("Scars_"):
			out.append(n)
		for child in n.get_children():
			stack.push_back(child)
	return out


func test_shared_ring_batches_become_one_world_mesh_with_a_surface_per_batch() -> void:
	var root := _texture_root(["scorch1.tga", "bhole1.tga"])
	var entities := _make_presenter(null, root)
	var draw := _draw_list()
	_batch(draw, 0xFFFF, 0, 0, false, 3)            # scorch1 x3
	_batch(draw, 0xFFFF, BHOLE_STRIP, 0, false, 1)  # bhole1 x1

	entities.present_scar_draw_list(draw)

	var world := _world_mesh(entities)
	assert_not_null(world, "the shared ring lands on the top-level ScarWorld mesh")
	if world == null:
		return
	assert_true(world.top_level, "shared-ring vertices are world space whatever the parent does")
	assert_true(world.visible)
	assert_not_null(world.mesh)
	assert_eq(world.mesh.get_surface_count(), 2, "one surface per (strip, building) batch")
	assert_eq(world.mesh.surface_get_array_len(0), 18, "three quads = eighteen triangle vertices")
	var material := world.mesh.surface_get_material(0) as ShaderMaterial
	assert_not_null(material, "each strip draws through the scar quad shader")
	if material != null:
		assert_not_null(material.get_shader_parameter("albedo_tex"),
				"scorch1.tga resolved through the resource root")
		assert_eq(material.shader.resource_path, SHADER_SCORCH,
				"the scorch word (0x120651) selects the scorch drawer state")
	var hole_material := world.mesh.surface_get_material(1) as ShaderMaterial
	assert_not_null(hole_material)
	if hole_material != null:
		assert_eq(hole_material.shader.resource_path, SHADER_HOLE,
				"the bhole word (0x460651) selects the alpha-tested drawer state")
	var stats := entities.get_scar_present_stats()
	assert_eq(stats.world_surfaces, 2)
	assert_eq(stats.entity_meshes, 0)
	assert_eq(stats.batches, 2)
	assert_eq(stats.textures_missing, 0)
	assert_eq(stats.slots_live, 4)
	assert_eq(entities.scar_presenter().get_stats_record().strips_unsupported, 0,
			"both shipped words decode to a carried drawer state")
	assert_eq(_entity_meshes(self).size(), 0)
	entities.teardown()


func test_every_strip_draws_on_the_scar_rung() -> void:
	# The scar batches draw in their own frame slot: after the camera-side
	# opaque wave, before detail foliage pass 1 and the camera-side alpha
	# flush [orig: Scar_DrawBatches @0x5C9658 between the BySide flush
	# @0x5C9647 and Foliage_RenderDetailPatchesPass(1) @0x5C9665]. Both drawer
	# states ride the one batch pass.
	var root := _texture_root(["scorch1.tga", "bhole1.tga"])
	var entities := _make_presenter(null, root)
	var draw := _draw_list()
	_batch(draw, 0xFFFF, 0, 0, false, 1)
	_batch(draw, 0xFFFF, BHOLE_STRIP, 0, false, 1)
	entities.present_scar_draw_list(draw)
	var world := _world_mesh(entities)
	assert_not_null(world)
	if world == null or world.mesh == null:
		entities.teardown()
		return
	assert_eq(world.mesh.get_surface_count(), 2)
	for surface in range(world.mesh.get_surface_count()):
		var material := world.mesh.surface_get_material(surface) as ShaderMaterial
		assert_not_null(material)
		if material != null:
			assert_eq(material.render_priority, ObjectShaderCache.RENDER_RUNG_SCARS,
					"surface %d draws on the scar rung" % surface)
	assert_lt(ObjectShaderCache.RENDER_RUNG_WATER_DECALS, ObjectShaderCache.RENDER_RUNG_SCARS,
			"scars follow the water pass")
	assert_lt(ObjectShaderCache.RENDER_RUNG_SCARS, ObjectShaderCache.RENDER_RUNG_ALPHA_CAMERA_SIDE,
			"scars precede the camera-side alpha")
	entities.teardown()


func test_a_strip_whose_tga_arrives_later_binds_it_on_the_next_present() -> void:
	# The material is created on the first present even when the TGA is not
	# resolvable yet (no root); the texture must not stay missing forever once
	# a root is set.
	var entities := _make_presenter()
	var draw := _draw_list()
	_batch(draw, 0xFFFF, 0, 0, false, 1)
	entities.present_scar_draw_list(draw)
	assert_eq(entities.get_scar_present_stats().textures_missing, 1,
			"no root: the strip reports its missing TGA")
	var root := _texture_root(["scorch1.tga"])
	entities.scar_presenter().set_resource_root(root)
	entities.present_scar_draw_list(draw)
	assert_eq(entities.get_scar_present_stats().textures_missing, 0,
			"the TGA binds once a root resolves it")
	var world := _world_mesh(entities)
	if world != null and world.mesh != null:
		var material := world.mesh.surface_get_material(0) as ShaderMaterial
		if material != null:
			assert_not_null(material.get_shader_parameter("albedo_tex"))
	entities.teardown()


func test_entity_ring_batches_parent_under_the_owners_section_node() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	if model == null:
		return
	var parts: Dictionary = model.get_render_part_nodes()
	assert_gt(parts.size(), 0, "the fixture model carries render-part nodes")
	if parts.is_empty():
		return
	var section := int(parts.keys()[0])
	var entities := _make_presenter(null, null, {OWNER_A: model})
	var draw := _draw_list()
	_batch(draw, OWNER_A, 0, section, true, 2)            # scorch1 on the section
	_batch(draw, OWNER_A, 2, section, true, 1)            # scorch3 on the same section

	entities.present_scar_draw_list(draw)

	var meshes := _entity_meshes(self)
	assert_eq(meshes.size(), 1, "one mesh per (owner, section) carries every strip batch")
	if meshes.size() != 1:
		return
	var mesh_instance: MeshInstance3D = meshes[0]
	assert_eq(mesh_instance.get_parent(), parts[section],
			"the section-local quads ride the struck section's render-part node")
	assert_eq(mesh_instance.mesh.get_surface_count(), 2, "one surface per strip")
	assert_eq(mesh_instance.transform, Transform3D.IDENTITY,
			"section-local vertices are uploaded as-is under the section node")
	var stats := entities.get_scar_present_stats()
	assert_eq(stats.entity_meshes, 1)
	assert_eq(stats.owners_unresolved, 0)
	assert_eq(stats.textures_missing, 2,
			"no resource root: both strips report their missing TGA rather than hiding")
	# An owner the pass cannot resolve draws nothing this frame, and is counted.
	var unresolved := _draw_list()
	_batch(unresolved, OWNER_B, 0, section, true, 1)
	entities.present_scar_draw_list(unresolved)
	assert_eq(entities.get_scar_present_stats().owners_unresolved, 1)
	assert_eq(_entity_meshes(self).filter(
			func(m: Node) -> bool: return not m.is_queued_for_deletion()).size(), 0,
			"the previous owner's mesh is retired once it produces no batch")
	entities.teardown()


func test_authored_owners_resolve_through_the_entity_index() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	if model == null:
		return
	var index := EntityIndex.new()
	model.entity_ref = EntityRef.make(-1, -1, 41)
	index.build([model], null)
	var entities := _make_presenter(index)
	var section := int(model.get_render_part_nodes().keys()[0])
	var draw := _draw_list()
	_batch(draw, OWNER_A, 0, section, true, 1, 41, Simulation.SPAWN_ORIGIN_NONE)

	entities.present_scar_draw_list(draw)

	assert_eq(entities.get_scar_present_stats().entity_meshes, 1,
			"a BMS-identified owner resolves through the shared index")
	assert_eq(entities.get_scar_present_stats().owners_unresolved, 0)
	entities.teardown()


func test_an_empty_list_clears_every_scar_mesh() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	if model == null:
		return
	var section := int(model.get_render_part_nodes().keys()[0])
	var entities := _make_presenter(null, null, {OWNER_A: model})
	var draw := _draw_list()
	_batch(draw, 0xFFFF, 1, 0, false, 1)
	_batch(draw, OWNER_A, 0, section, true, 1)
	entities.present_scar_draw_list(draw)
	assert_eq(entities.get_scar_present_stats().world_surfaces, 1)
	assert_eq(entities.get_scar_present_stats().entity_meshes, 1)

	entities.present_scar_draw_list(_draw_list())

	var world := _world_mesh(entities)
	assert_not_null(world)
	if world != null:
		assert_false(world.visible, "no shared-ring slot: the world mesh hides")
	assert_eq(entities.get_scar_present_stats().world_surfaces, 0)
	assert_eq(entities.get_scar_present_stats().entity_meshes, 0)
	for m in _entity_meshes(self):
		assert_true((m as Node).is_queued_for_deletion())
	entities.reset_wire_runtime_state()
	entities.teardown()


func test_the_presenter_owns_its_scars_child_and_reset_clears_every_mesh() -> void:
	# The scar device is the presenter's own "Scars" child (no injected
	# presenter seam); the Stop -> Play boundary reaches it through the
	# presenter's one reset, which drops every scar mesh and the pass census
	# while the child and its texture cache stay.
	var root := _texture_root(["scorch1.tga"])
	var entities := _make_presenter(null, root)
	var presenter := entities.scar_presenter()
	assert_not_null(presenter, "the presenter owns a scar device from birth")
	if presenter == null:
		return
	assert_eq(presenter.get_parent(), entities)
	assert_eq(String(presenter.name), "Scars")
	var draw := _draw_list()
	_batch(draw, 0xFFFF, 0, 0, false, 2)
	entities.present_scar_draw_list(draw)
	assert_eq(entities.get_scar_present_stats().world_surfaces, 1)
	assert_eq(entities.get_scar_present_stats().slots_live, 2)

	entities.reset_wire_runtime_state()

	var stats := entities.get_scar_present_stats()
	assert_eq(stats.world_surfaces, 0, "reset drops the shared-ring mesh")
	assert_eq(stats.batches, 0)
	assert_eq(stats.slots_live, 0, "reset clears the pass census")
	var world := _world_mesh(entities)
	assert_not_null(world)
	if world != null:
		assert_false(world.visible, "the world mesh hides until the next present")
	assert_eq(entities.scar_presenter(), presenter, "the device child survives the reset")
	entities.teardown()
	assert_eq(entities.scar_presenter(), presenter,
			"teardown clears the meshes; the child dies with the presenter node")


func test_a_booted_simulation_publishes_an_empty_typed_list() -> void:
	# The production draw-list seam on a REAL sim: a fresh mission has no scars,
	# but the dictionary carries every typed array plus the 32-strip name table
	# (scorch1..4 at 0..3, bhole1 at 27 [orig: Scar_LoadTextures @0x5CC2E0]).
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var boot_container := Node3D.new()
	add_child_autofree(boot_container)
	var rt := MissionRoot.new()
	add_child_autofree(rt)
	rt.setup(mission, boot_container)
	var sim := rt.get_sim()
	assert_not_null(sim, "the runtime boots a real simulation over the mission")
	if sim == null:
		return
	var draw := sim.get_scar_draw_list(Vector3.ZERO, 0.0, Color.WHITE)
	assert_not_null(draw)
	assert_eq(draw.vertices.size(), 0)
	assert_eq(draw.batch_owner.size(), 0)
	var names := draw.strip_names
	assert_eq(names.size(), 32)
	assert_eq(names[0], "scorch1.tga")
	assert_eq(names[3], "scorch4.tga")
	assert_eq(names[BHOLE_STRIP], "bhole1.tga")
	var words := draw.strip_mode_words
	assert_eq(words.size(), 32)
	assert_eq(words[0], MODE_WORD_SCORCH, "scorch1 draws in the scorch state")
	assert_eq(words[3], MODE_WORD_SCORCH)
	assert_eq(words[BHOLE_STRIP], MODE_WORD_HOLE, "bhole1 draws in the alpha-tested state")
	assert_eq(draw.rings_leased, 0)
	var stats: ScarPresentStats = rt.get_scar_present_stats()
	assert_not_null(stats, "the runtime owns the scar presentation pass")
	var fresh := Simulation.new()
	var fresh_draw := fresh.get_scar_draw_list(Vector3.ZERO, 0.0, Color.WHITE)
	assert_eq(fresh_draw.batch_owner.size(), 0,
			"an unbooted simulation lists no scars, never crashes")


# A 64x64 own-world view over a flat clear, its camera 5 units off the quad.
func _raster_view(clear: Color) -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	viewport.add_child(camera)
	var background := WorldEnvironment.new()
	background.environment = Environment.new()
	background.environment.background_mode = Environment.BG_COLOR
	background.environment.background_color = clear
	viewport.add_child(background)
	return viewport


# The centre pixel of one drawer-state quad (4x4, facing the camera) textured
# with a flat texel.
func _render_quad_centre(viewport: SubViewport, shader_path: String, texel: Color) -> Color:
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(texel)
	var material := ShaderMaterial.new()
	material.shader = load(shader_path)
	material.set_shader_parameter("albedo_tex", ImageTexture.create_from_image(image))
	var quad := QuadMesh.new()
	quad.size = Vector2(4.0, 4.0)
	var instance := MeshInstance3D.new()
	instance.mesh = quad
	instance.material_override = material
	viewport.add_child(instance)
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	var pixel := viewport.get_texture().get_image().get_pixel(32, 32)
	instance.queue_free()
	await get_tree().process_frame
	return pixel


# Both drawer states carry FOGENABLE (the 0x20000 bit of 0x120651 and
# 0x460651) and the drawer selects the scene fog colour
# [orig: Scar_DrawBatches @0x5CCD33 -> CD3DDevice_SetFogAndBlendMode(dev, 0)]:
# a scar at full pass fog renders the fog colour, not its own texel.
func test_scar_quads_fog_toward_the_scene_fog_colour() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var viewport := _raster_view(Color.BLACK)
	# Full linear fog at 5 units: visibility (end - d) / (end - start) = 0.
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", true)
	RenderingServer.global_shader_parameter_set("opennova_fog_color", Vector3(1.0, 0.0, 0.0))
	RenderingServer.global_shader_parameter_set("opennova_fog_start", 0.0)
	RenderingServer.global_shader_parameter_set("opennova_fog_end", 1.0)
	RenderingServer.global_shader_parameter_set("opennova_fog_type", 1)
	for shader_path in [SHADER_SCORCH, SHADER_HOLE]:
		var pixel: Color = await _render_quad_centre(
				viewport, shader_path, Color(0.5, 0.5, 0.5, 1.0))
		assert_gt(pixel.r, 0.9, "%s fogs to the red scene fog colour: %s" % [shader_path, pixel])
		assert_lt(pixel.g, 0.1, "%s: no texel grey survives full fog: %s" % [shader_path, pixel])


# The two drawer states by pixels, black texels over a white clear. The scorch
# TGAs are black RGB under an alpha falloff, so the mark IS the
# SRCALPHA/INVSRCALPHA blend (0x120651: blend nibble 1, no ALPHATESTENABLE, so
# the drawer's SetAlphaTestRef(128) latch is inert); the bhole state
# (0x460651) adds the live GREATER/128 alpha test. An alpha scissor (the
# 2026-08-21 first cut) drew every scorch as an opaque black blob.
func test_scar_quads_blend_and_only_the_hole_state_alpha_tests() -> void:
	if RenderingServer.get_rendering_device() == null:
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	RenderingServer.global_shader_parameter_set("opennova_fog_enabled", false)
	var viewport := _raster_view(Color.WHITE)
	for shader_path in [SHADER_SCORCH, SHADER_HOLE]:
		var dense: Color = await _render_quad_centre(
				viewport, shader_path, Color(0.0, 0.0, 0.0, 0.75))
		assert_between(dense.r, 0.05, 0.95,
				"%s blends a 0.75-alpha texel over the clear: %s" % [shader_path, dense])
	var faint_scorch: Color = await _render_quad_centre(
			viewport, SHADER_SCORCH, Color(0.0, 0.0, 0.0, 0.25))
	assert_between(faint_scorch.r, 0.05, 0.98,
			"scorch has no alpha test: a 0.25-alpha texel still blends: %s" % faint_scorch)
	var faint_hole: Color = await _render_quad_centre(
			viewport, SHADER_HOLE, Color(0.0, 0.0, 0.0, 0.25))
	assert_gt(faint_hole.r, 0.99,
			"bhole's GREATER/128 test discards a 0.25-alpha texel: %s" % faint_hole)
