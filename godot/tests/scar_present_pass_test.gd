extends GutTest

# ScarPresentPass + ScarPresenter on the typed surfaces (ADR 0034): the draw
# list is pure data through the public present_draw_list leg (production
# present() pulls Simulation.get_scar_draw_list), the owner models are real
# ObjectModels built from the armory fixture (their render-part nodes mount the
# entity-ring meshes), the wire resolver is a real WirePresentPass with injected
# nodes, and the textures resolve through a real ResourceRoot over a temp dir.
#
# [orig: Scar_RenderAllCaches @0x5CDF70 -> Scar_RenderCache @0x5CD830; the
#  shared ring draws world-space quads, an entity ring's quads are transformed
#  through the owner's section matrix at draw time — the section-node parent
#  here; docs/world/world-wac-ai-re.md §24.9]

const ScarPresentPass := preload("res://game/world/scar_present_pass.gd")
const MissionPresentation := preload("res://game/world/mission_presentation.gd")

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

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		for file_name in DirAccess.get_files_at(dir_path):
			DirAccess.remove_absolute(dir_path.path_join(file_name))
		DirAccess.remove_absolute(dir_path)
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
func _quad(draw: Dictionary, centre: Vector3) -> void:
	var corners := [
		centre + Vector3(-0.25, 0, -0.25), centre + Vector3(0.25, 0, -0.25),
		centre + Vector3(-0.25, 0, 0.25), centre + Vector3(0.25, 0, 0.25),
	]
	var uv := [Vector2(0, 0), Vector2(1, 0), Vector2(0, 1), Vector2(1, 1)]
	for k in [0, 1, 2, 1, 3, 2]:
		draw["vertices"].append(corners[k])
		draw["uvs"].append(uv[k])
		draw["colors"].append(Color(0.5, 0.5, 0.5, 1.0))


func _draw_list() -> Dictionary:
	return {
		"vertices": PackedVector3Array(),
		"uvs": PackedVector2Array(),
		"colors": PackedColorArray(),
		"batch_owner": PackedInt32Array(),
		"batch_texture": PackedInt32Array(),
		"batch_section": PackedInt32Array(),
		"batch_flags": PackedInt32Array(),
		"batch_first": PackedInt32Array(),
		"batch_count": PackedInt32Array(),
		"batch_bms_id": PackedInt32Array(),
		"batch_spawn_origin": PackedInt64Array(),
		"strip_names": _strip_names(),
		"strip_mode_words": _strip_mode_words(),
		"slots_live": 0,
		"slots_culled": 0,
		"rings_leased": 0,
	}


func _batch(draw: Dictionary, owner: int, texture: int, section: int,
		entity_local: bool, quads: int, bms_id: int = 0,
		spawn_origin: int = SpawnOrigin.NONE) -> void:
	var first: int = draw["vertices"].size()
	for q in range(quads):
		_quad(draw, Vector3(q, 0, 0))
	draw["batch_owner"].append(owner)
	draw["batch_texture"].append(texture)
	draw["batch_section"].append(section)
	draw["batch_flags"].append(1 if entity_local else 0)
	draw["batch_first"].append(first)
	draw["batch_count"].append(quads * 6)
	draw["batch_bms_id"].append(bms_id)
	draw["batch_spawn_origin"].append(spawn_origin)
	draw["slots_live"] = int(draw["slots_live"]) + quads


func _wire_resolver(nodes: Dictionary = {}) -> WirePresentPass:
	var presenter := WirePresentPass.new()
	for handle in nodes:
		presenter.register_wire_node(int(handle), nodes[handle])
	return presenter


func _presenter(root: ResourceRoot = null) -> ScarPresenter:
	var presenter := ScarPresenter.new()
	add_child_autofree(presenter)
	if root != null:
		presenter.set_resource_root(root)
	return presenter


func _world_mesh(presenter: ScarPresenter) -> MeshInstance3D:
	return presenter.get_node_or_null("ScarWorld") as MeshInstance3D


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
	var presenter := _presenter(root)
	var scar_pass := ScarPresentPass.new()
	scar_pass.setup(null, null, null, null, root, Callable(), Callable(), presenter)
	var draw := _draw_list()
	_batch(draw, 0xFFFF, 0, 0, false, 3)            # scorch1 x3
	_batch(draw, 0xFFFF, BHOLE_STRIP, 0, false, 1)  # bhole1 x1

	scar_pass.present_draw_list(draw)

	var world := _world_mesh(presenter)
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
	var stats := scar_pass.get_stats()
	assert_eq(stats.world_surfaces, 2)
	assert_eq(stats.entity_meshes, 0)
	assert_eq(stats.batches, 2)
	assert_eq(stats.textures_missing, 0)
	assert_eq(stats.slots_live, 4)
	assert_eq(presenter.get_stats_record().strips_unsupported, 0,
			"both shipped words decode to a carried drawer state")
	assert_eq(_entity_meshes(self).size(), 0)
	scar_pass.teardown()


func test_the_drawer_states_blend_and_never_alpha_scissor() -> void:
	# The scorch TGAs are black RGB under an alpha falloff: the mark IS the
	# SRCALPHA/INVSRCALPHA blend [orig: mode word 0x120651 — blend nibble 1,
	# no ALPHATESTENABLE bit; the drawer's SetAlphaTestRef(128) @0x5CCDAE is an
	# inert latch there]. Godot's ALPHA_SCISSOR_THRESHOLD moves a material to the
	# opaque pass and drops the blend — the regression that painted every scar
	# as an opaque black blob — so neither drawer state may use it.
	var scorch := load(SHADER_SCORCH) as Shader
	var hole := load(SHADER_HOLE) as Shader
	assert_not_null(scorch)
	assert_not_null(hole)
	if scorch == null or hole == null:
		return
	for shader in [scorch, hole]:
		var code: String = shader.code
		assert_false(code.contains("ALPHA_SCISSOR_THRESHOLD"),
				"%s: no alpha scissor (it would drop the blend)" % shader.resource_path)
		assert_true(code.contains("blend_mix"), "%s: the SRCALPHA/INVSRCALPHA blend" % shader.resource_path)
		assert_true(code.contains("unshaded"), "%s: LIGHTING off" % shader.resource_path)
		assert_false(code.contains("fog_disabled"), "%s: FOGENABLE" % shader.resource_path)
		assert_true(code.contains("source_color"),
				"%s: the scorch art decodes to the linear scene (ADR 0043 linear-scene amendment)" % shader.resource_path)
	assert_true(scorch.code.contains("depth_draw_never"), "scorch: z-write off (0x100000)")
	assert_true(scorch.code.contains("cull_back"), "scorch: the CCW back-face cull")
	# The beauty scar shader needs no pass gate and has no alpha test on its
	# texel.
	assert_eq(scorch.code.count("discard"), 0, "scorch: no camera-pass gate")
	assert_false(scorch.code.contains("tex.a <") or scorch.code.contains("alpha <"),
			"scorch: no alpha test")
	assert_true(hole.code.contains("depth_draw_always"), "bhole: z-write on")
	assert_true(hole.code.contains("cull_disabled"), "bhole: cull none (0x400000)")
	assert_true(hole.code.contains("discard"), "bhole: the GREATER/128 alpha test")
	assert_true(hole.code.contains("128.0"), "bhole: ref 128")


func test_a_strip_whose_tga_arrives_later_binds_it_on_the_next_present() -> void:
	# The material is created on the first present even when the TGA is not
	# resolvable yet (no root); the texture must not stay missing forever once
	# a root is set.
	var presenter := _presenter()
	var scar_pass := ScarPresentPass.new()
	scar_pass.setup(null, null, null, null, null, Callable(), Callable(), presenter)
	var draw := _draw_list()
	_batch(draw, 0xFFFF, 0, 0, false, 1)
	scar_pass.present_draw_list(draw)
	assert_eq(scar_pass.get_stats().textures_missing, 1, "no root: the strip reports its missing TGA")
	var root := _texture_root(["scorch1.tga"])
	presenter.set_resource_root(root)
	scar_pass.present_draw_list(draw)
	assert_eq(scar_pass.get_stats().textures_missing, 0, "the TGA binds once a root resolves it")
	var world := _world_mesh(presenter)
	if world != null and world.mesh != null:
		var material := world.mesh.surface_get_material(0) as ShaderMaterial
		if material != null:
			assert_not_null(material.get_shader_parameter("albedo_tex"))
	scar_pass.teardown()


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
	var presenter := _presenter()
	var scar_pass := ScarPresentPass.new()
	scar_pass.setup(null, null, null, _wire_resolver({OWNER_A: model}), null,
			Callable(), Callable(), presenter)
	var draw := _draw_list()
	_batch(draw, OWNER_A, 0, section, true, 2)            # scorch1 on the section
	_batch(draw, OWNER_A, 2, section, true, 1)            # scorch3 on the same section

	scar_pass.present_draw_list(draw)

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
	var stats := scar_pass.get_stats()
	assert_eq(stats.entity_meshes, 1)
	assert_eq(stats.owners_unresolved, 0)
	assert_eq(stats.textures_missing, 2,
			"no resource root: both strips report their missing TGA rather than hiding")
	# An owner the pass cannot resolve draws nothing this frame, and is counted.
	var unresolved := _draw_list()
	_batch(unresolved, OWNER_B, 0, section, true, 1)
	scar_pass.present_draw_list(unresolved)
	assert_eq(scar_pass.get_stats().owners_unresolved, 1)
	assert_eq(_entity_meshes(self).filter(
			func(m: Node) -> bool: return not m.is_queued_for_deletion()).size(), 0,
			"the previous owner's mesh is retired once it produces no batch")
	scar_pass.teardown()


func test_authored_owners_resolve_through_the_entity_index() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	if model == null:
		return
	var index := EntityIndex.new()
	index.build([{'model': model, 'ref': {
		'bms_id': 41, 'kind': -1, 'index': -1,
		'group': -1, 'team': -1, 'position': Vector3.ZERO}}], [])
	var presenter := _presenter()
	var scar_pass := ScarPresentPass.new()
	scar_pass.setup(null, null, index, null, null, Callable(), Callable(), presenter)
	var section := int(model.get_render_part_nodes().keys()[0])
	var draw := _draw_list()
	_batch(draw, OWNER_A, 0, section, true, 1, 41, SpawnOrigin.NONE)

	scar_pass.present_draw_list(draw)

	assert_eq(scar_pass.get_stats().entity_meshes, 1,
			"a BMS-identified owner resolves through the shared index")
	assert_eq(scar_pass.get_stats().owners_unresolved, 0)
	scar_pass.teardown()


func test_an_empty_list_clears_every_scar_mesh() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	if model == null:
		return
	var section := int(model.get_render_part_nodes().keys()[0])
	var presenter := _presenter()
	var scar_pass := ScarPresentPass.new()
	scar_pass.setup(null, null, null, _wire_resolver({OWNER_A: model}), null,
			Callable(), Callable(), presenter)
	var draw := _draw_list()
	_batch(draw, 0xFFFF, 1, 0, false, 1)
	_batch(draw, OWNER_A, 0, section, true, 1)
	scar_pass.present_draw_list(draw)
	assert_eq(scar_pass.get_stats().world_surfaces, 1)
	assert_eq(scar_pass.get_stats().entity_meshes, 1)

	scar_pass.present_draw_list(_draw_list())

	var world := _world_mesh(presenter)
	assert_not_null(world)
	if world != null:
		assert_false(world.visible, "no shared-ring slot: the world mesh hides")
	assert_eq(scar_pass.get_stats().world_surfaces, 0)
	assert_eq(scar_pass.get_stats().entity_meshes, 0)
	for m in _entity_meshes(self):
		assert_true((m as Node).is_queued_for_deletion())
	scar_pass.reset_runtime_state()
	scar_pass.teardown()


func test_the_pass_creates_its_presenter_under_the_container_and_tears_it_down() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var scar_pass := ScarPresentPass.new()
	scar_pass.setup(null, container, null, null, null)
	var presenter := scar_pass.get_presenter()
	assert_not_null(presenter, "the pass owns a presenter when none is supplied")
	if presenter == null:
		return
	assert_eq(presenter.get_parent(), container)
	scar_pass.teardown()
	assert_true(presenter.is_queued_for_deletion(), "teardown frees the owned presenter")
	assert_null(scar_pass.get_presenter())


func test_a_booted_simulation_publishes_an_empty_typed_list() -> void:
	# The production draw-list seam on a REAL sim: a fresh mission has no scars,
	# but the dictionary carries every typed array plus the 32-strip name table
	# (scorch1..4 at 0..3, bhole1 at 27 [orig: Scar_LoadTextures @0x5CC2E0]).
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var boot_container := Node3D.new()
	add_child_autofree(boot_container)
	var rt := MissionPresentation.new()
	add_child_autofree(rt)
	rt.setup(mission, boot_container)
	var sim := rt.get_sim()
	assert_not_null(sim, "the runtime boots a real simulation over the mission")
	if sim == null:
		return
	var draw: Dictionary = sim.get_scar_draw_list(Vector3.ZERO, 0.0, Color.WHITE)
	assert_true(draw.has("vertices"))
	assert_eq((draw["vertices"] as PackedVector3Array).size(), 0)
	assert_eq((draw["batch_owner"] as PackedInt32Array).size(), 0)
	var names: PackedStringArray = draw["strip_names"]
	assert_eq(names.size(), 32)
	assert_eq(names[0], "scorch1.tga")
	assert_eq(names[3], "scorch4.tga")
	assert_eq(names[BHOLE_STRIP], "bhole1.tga")
	var words: PackedInt32Array = draw["strip_mode_words"]
	assert_eq(words.size(), 32)
	assert_eq(words[0], MODE_WORD_SCORCH, "scorch1 draws in the scorch state")
	assert_eq(words[3], MODE_WORD_SCORCH)
	assert_eq(words[BHOLE_STRIP], MODE_WORD_HOLE, "bhole1 draws in the alpha-tested state")
	assert_eq(int(draw["rings_leased"]), 0)
	var stats := rt.get_scar_present_stats()
	assert_not_null(stats, "the runtime owns the scar presentation pass")
	var fresh := Simulation.new()
	var fresh_draw: Dictionary = fresh.get_scar_draw_list(Vector3.ZERO, 0.0, Color.WHITE)
	assert_eq((fresh_draw.get("batch_owner", PackedInt32Array()) as PackedInt32Array).size(), 0,
			"an unbooted simulation lists no scars, never crashes")
	fresh.free()
