extends GutTest

const ObjectEditorScript = preload("res://modtools/object/object_editor.gd")
const ObjectPreviewScript = preload("res://modtools/object/object_preview.gd")
const ObjectWorkspaceScript = preload("res://modtools/object/object_workspace.gd")
const FlyCameraScript = preload("res://engine/fly_camera.gd")

const BIRD_FIXTURE := "res://../fixtures/3dp/Bird1/Bird1.3di"
const BIRD_PROJECT_FIXTURE := "res://../fixtures/3dp/Bird1/Bird1.3dp"
const BIRD_ASE_FIXTURE := "res://../fixtures/3dp/Bird1/Bird1.ase"
const DVAN_FIXTURE := "res://../fixtures/3dp/dapche2/dapche2.3di"
const ARMRY_FIXTURE := "res://../fixtures/3dp/armry01/Armry01.3di"
const ARMRY_TEXTURE_FIXTURE := "res://../fixtures/3dp/armry01/KArm1_O.TGA"
const US01_PROJECT_FIXTURE := "res://../fixtures/3dp/US01_onimport/US01.3dp"
const OUTPUT_DIR_NAME := "object_editor_export_test"


func before_each() -> void:
	_cleanup_dir(_output_dir())


func after_each() -> void:
	_cleanup_workspace_children()
	_cleanup_dir(_output_dir())


func test_object_data_opens_3di_as_ir_document() -> void:
	var data := NovaObjectData.new()

	var err: Error = data.open_file(ProjectSettings.globalize_path(BIRD_FIXTURE))

	assert_eq(err, OK, "3DI fixtures should open through NovaObjectData.")
	var summary := data.get_summary()
	assert_gt(int(summary.get("lod_count", 0)), 0, "Opened 3DI should expose LODs.")
	assert_gt(int(summary.get("material_count", 0)), 0, "Opened 3DI should expose materials.")
	assert_false(data.get_lod_surfaces(0).is_empty(), "Opened 3DI should expose preview mesh surfaces.")


func test_object_shader_catalog_exposes_oed_slot_flags() -> void:
	var data := NovaObjectData.new()
	var catalog := data.get_shader_catalog()

	assert_false(catalog.is_empty(), "NovaObjectData should expose the known OED shader catalog.")
	var single := _shader_catalog_entry(catalog, "FF_ST_OP")
	var multi := _shader_catalog_entry(catalog, "FF_MT_OP")
	var normal := _shader_catalog_entry(catalog, "VS_DOT3DIFF2")
	var glass := _shader_catalog_entry(catalog, "FFP_GLASS")
	var mirror := _shader_catalog_entry(catalog, "VS_BMTXMIRRT")
	assert_true(bool(single.get("has_diffuse", false)))
	assert_false(bool(single.get("has_secondary", true)))
	assert_true(bool(multi.get("has_secondary", false)))
	assert_true(bool(normal.get("has_normal_a", false)))
	assert_eq(String(normal.get("shader_family", "")), "dot3")
	assert_true(bool(glass.get("is_glass_shader", false)))
	assert_eq(String(glass.get("shader_family", "")), "glass")
	assert_eq(String(glass.get("shader_blend", "")), "additive")
	assert_eq(String(mirror.get("shader_family", "")), "environment")
	assert_eq(String(mirror.get("shader_blend", "")), "opaque")
	assert_true(bool(mirror.get("uses_environment", false)))


func test_object_shader_cache_exposes_renderer_depth_and_cull_modes() -> void:
	var cache := NovaObjectShaderCache.get_singleton()
	assert_not_null(cache, "Object shader cache should be registered with Godot.")

	var opaque_shader: Shader = cache.get_shader_for_key(cache.classify("FF_ST_OP", 0, 0, 0, 128))
	var opaque_code := opaque_shader.code
	assert_string_contains(opaque_code, "depth_draw_opaque", "Opaque object shaders should render in the depth-writing path.")
	assert_string_contains(opaque_code, "cull_back", "Opaque one-sided object shaders should keep backface culling.")
	assert_false(opaque_code.contains("ALPHA ="), "Opaque object shaders should not write ALPHA and enter transparent sorting.")

	var skinned_shader: Shader = cache.get_shader_for_key(cache.classify("VS_SKBUMPDIFFT2", 0, 0, 0, 128))
	assert_string_contains(skinned_shader.code, "depth_draw_opaque", "US01-style skinned bump/detail shaders should render in the opaque path.")
	assert_false(skinned_shader.code.contains("ALPHA ="), "US01-style skinned bump/detail shaders should not use texture alpha as opacity.")

	var alpha_test_shader: Shader = cache.get_shader_for_key(cache.classify("FF_ST_OP", 0x01, 0, 0, 128))
	assert_string_contains(alpha_test_shader.code, "depth_prepass_alpha", "Alpha-test object shaders should use the alpha depth prepass.")

	var two_sided_shader: Shader = cache.get_shader_for_key(cache.classify("FF_ST_OP", 0x04, 0, 0, 128))
	assert_string_contains(two_sided_shader.code, "cull_disabled", "Two-sided object shaders should disable backface culling.")


func test_object_preview_surfaces_apply_strip_vertex_offsets() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(DVAN_FIXTURE)), OK)

	var surfaces := data.get_lod_surfaces(0)
	var surfaces_with_offsets := 0
	var max_edge := 0.0
	for surface in surfaces:
		if int(surface.get("vertex_offset", 0)) > 0:
			surfaces_with_offsets += 1
		max_edge = maxf(max_edge, _max_triangle_edge(surface.get("vertices", PackedVector3Array())))

	assert_gt(surfaces_with_offsets, 0, "The fixture should exercise non-zero strip vertex offsets.")
	assert_lt(max_edge, 32.0, "Preview triangles should stay local instead of connecting unrelated vertex ranges.")


func test_object_preview_surfaces_expose_stable_material_array_indices() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var material_count := data.get_materials().size()

	for surface in data.get_lod_surfaces(0):
		var material_array_index := int(surface.get("material_array_index", -1))
		assert_true(material_array_index >= 0, "Preview surfaces should expose a resolved material array index.")
		assert_lt(material_array_index, material_count, "Preview material array indices should be valid for material lookups.")


func test_object_preview_surfaces_expose_secondary_uvs_and_tangents() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)

	var saw_uv2 := false
	for surface in data.get_lod_surfaces(0):
		var vertices: PackedVector3Array = surface.get("vertices", PackedVector3Array())
		var uvs: PackedVector2Array = surface.get("uvs", PackedVector2Array())
		var uvs2: PackedVector2Array = surface.get("uvs2", PackedVector2Array())
		assert_eq(uvs.size(), vertices.size(), "Preview surfaces should expose one UV0 per vertex.")
		assert_eq(uvs2.size(), vertices.size(), "Preview surfaces should expose one UV1 per vertex.")
		if not uvs2.is_empty():
			saw_uv2 = true
		var tangents: PackedFloat32Array = surface.get("tangents", PackedFloat32Array())
		if not tangents.is_empty():
			assert_eq(tangents.size(), vertices.size() * 4, "Preview tangents should use Godot's four-float per-vertex layout.")

	assert_true(saw_uv2, "The fixture should expose secondary UV arrays.")


func test_object_preview_surfaces_preserve_decoded_winding_for_culling() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(DVAN_FIXTURE)), OK)

	var sampled_triangles := 0
	var normal_disagreements := 0
	for surface in data.get_lod_surfaces(0):
		var result := _normal_alignment_counts(surface.get("vertices", PackedVector3Array()), surface.get("normals", PackedVector3Array()))
		sampled_triangles += int(result.get("sampled", 0))
		normal_disagreements += int(result.get("opposed", 0))

	assert_gt(sampled_triangles, 0, "The fixture should expose preview triangles.")
	assert_gt(normal_disagreements, 0, "Preview winding should preserve decoded indices instead of forcing every triangle to match averaged normals.")


func test_object_data_open_3dp_preserves_material_indices_for_live_preview() -> void:
	var data := _open_us01_project_data()
	var materials := data.get_materials()
	assert_eq(materials.size(), 5, "US01 should expose all project materials.")

	for i in range(materials.size()):
		var material: Dictionary = materials[i]
		assert_eq(int(material.get("index", -1)), i, "Material array index should stay stable.")
		assert_eq(int(material.get("material_index", -1)), i, "Live 3DP material ids should match their material table slots.")

	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)
	var material_defs: Dictionary = preview._material_defs
	assert_eq(_texture_name_for_slot(material_defs.get(0, {}), 1).to_lower(), "aus1_vsg.tga", "Preview material 0 should keep US01's vest material instead of being overwritten by material 04.")
	assert_eq(_texture_name_for_slot(material_defs.get(4, {}), 1).to_lower(), "aus1_hg1.tga", "Preview material 4 should keep US01's final material.")


func test_object_data_open_3dp_render_signature_matches_exported_3di() -> void:
	var live := _open_us01_project_data()
	var export_dir := _output_dir().path_join("us01_export")
	assert_eq(DirAccess.make_dir_recursive_absolute(export_dir), OK)
	assert_eq(live.export_3di_to_dir(export_dir), OK)

	var reopened := NovaObjectData.new()
	assert_eq(reopened.open_file(export_dir.path_join("US01.3di")), OK)

	assert_eq(_object_render_signature(live), _object_render_signature(reopened), "Live 3DP preview data should match the exported/reopened 3DI render data.")


func test_object_data_loads_material_textures_from_source_dir() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)

	var texture_ref := _first_material_texture(data)

	assert_false(texture_ref.is_empty(), "The fixture should expose at least one material texture.")
	var material_index := int(texture_ref.get("material_index", -1))
	var texture_index := int(texture_ref.get("texture_index", -1))
	var resolved_path := data.resolve_material_texture_path(material_index, texture_index)
	assert_false(resolved_path.is_empty(), "Material textures should resolve beside the opened 3DI.")
	assert_true(FileAccess.file_exists(resolved_path), "Resolved material texture should exist on disk.")
	assert_true(data.load_material_texture(material_index, texture_index) is Texture2D, "Resolved material texture should load as a Texture2D.")


func test_object_data_resolves_direct_3di_oed_object_texture_variant() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)

	var resolved_path := data.resolve_material_texture_path(0, 0)

	assert_eq(resolved_path.get_file().to_lower(), "karm1_o.tga", "Direct 3DI texture names should resolve to object-folder OED texture variants.")
	assert_true(data.load_material_texture(0, 0) is Texture2D, "Direct 3DI OED texture variants should load as textures.")


func test_object_data_resolves_dds_texture_variant_from_object_folder() -> void:
	var fixture_dir := _prepare_texture_variant_fixture("KArm1.dds")
	var data := NovaObjectData.new()
	assert_eq(data.open_file(fixture_dir.path_join("Armry01.3di")), OK)

	var resolved_path := data.resolve_material_texture_path(0, 0)

	assert_eq(resolved_path.get_file(), "KArm1.dds", "Texture resolver should accept loose DDS names in the object folder.")
	assert_true(data.load_material_texture(0, 0) is Texture2D, "DDS files with Nova TGA payloads should decode.")


func test_object_data_loads_real_dds_texture_variant_from_object_folder() -> void:
	var fixture_dir := _prepare_real_dds_texture_fixture("KArm1.dds")
	var data := NovaObjectData.new()
	assert_eq(data.open_file(fixture_dir.path_join("Armry01.3di")), OK)

	var resolved_path := data.resolve_material_texture_path(0, 0)

	assert_eq(resolved_path.get_file(), "KArm1.dds", "Direct 3DI texture names should resolve to real DDS variants.")
	assert_true(data.load_material_texture(0, 0) is Texture2D, "Real DDS files should decode through the shared texture loader.")
	assert_true(data.load_texture_name("KArm1.tga") is Texture2D, "Name-based texture loading should share the material texture resolver.")


func test_object_data_skips_unloadable_texture_candidate() -> void:
	var fixture_dir := _prepare_real_dds_texture_fixture("KArm1.dds")
	_write_bytes(fixture_dir.path_join("KArm1.TGA"), PackedByteArray([0, 1, 2, 3]))
	var data := NovaObjectData.new()
	assert_eq(data.open_file(fixture_dir.path_join("Armry01.3di")), OK)

	var resolved_path := data.resolve_material_texture_path(0, 0)

	assert_eq(resolved_path.get_file(), "KArm1.TGA", "The resolver should still report the first existing candidate.")
	assert_true(data.load_material_texture(0, 0) is Texture2D, "Texture loading should keep searching if an earlier candidate cannot decode.")


func test_object_data_resolves_compound_dds_tga_texture_variant_from_object_folder() -> void:
	var fixture_dir := _prepare_texture_variant_fixture("KArm1.dds.tga")
	var data := NovaObjectData.new()
	assert_eq(data.open_file(fixture_dir.path_join("Armry01.3di")), OK)

	var resolved_path := data.resolve_material_texture_path(0, 0)

	assert_eq(resolved_path.get_file(), "KArm1.dds.tga", "Texture resolver should accept compound DDS TGA loose texture names.")
	assert_true(data.load_material_texture(0, 0) is Texture2D, "Compound DDS TGA files should decode through the Nova texture path.")


func test_nova_texture_resource_loader_handles_real_dds_and_renamed_tga() -> void:
	var fixture_dir := _output_dir().path_join("resource_loader_textures")
	assert_eq(DirAccess.make_dir_recursive_absolute(fixture_dir), OK)
	var real_dds_path := fixture_dir.path_join("real.dds")
	var renamed_tga_path := fixture_dir.path_join("renamed.dds")
	_write_test_dds(real_dds_path)
	_copy_file(ProjectSettings.globalize_path(ARMRY_TEXTURE_FIXTURE), renamed_tga_path)

	var real_dds := ResourceLoader.load(real_dds_path, "ImageTexture", ResourceLoader.CACHE_MODE_IGNORE)
	var renamed_tga := ResourceLoader.load(renamed_tga_path, "ImageTexture", ResourceLoader.CACHE_MODE_IGNORE)

	assert_true(real_dds is Texture2D, "ResourceLoader should decode true DDS files.")
	assert_true(renamed_tga is Texture2D, "ResourceLoader should still decode Nova renamed-TGA DDS files.")


func test_object_editor_exports_open_3di() -> void:
	var editor = add_child_autofree(ObjectEditorScript.new())
	var err: Error = editor.open_object(ProjectSettings.globalize_path(BIRD_FIXTURE))
	assert_eq(err, OK, "Object editor should open a 3DI fixture.")
	assert_eq(DirAccess.make_dir_recursive_absolute(_output_dir()), OK, "Export test directory should be creatable.")

	err = editor.export_to_dir(_output_dir())

	assert_eq(err, OK, "Object editor should export a patched 3DI.")
	assert_true(FileAccess.file_exists(_output_dir().path_join("Bird1.3di")), "Export should write the object 3DI.")


func test_object_data_export_failure_includes_native_oed_detail() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(BIRD_PROJECT_FIXTURE)), OK)

	var err := data.export_3di_to_dir(_output_dir().path_join("missing").path_join("nested"))

	assert_eq(err, ERR_FILE_CANT_WRITE, "Exporting to a missing directory should fail.")
	assert_string_contains(data.get_last_error(), "baseline 3DI", "Export failures should expose native OED details.")


func test_object_preview_uses_internal_viewport_without_godot_lights() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(BIRD_FIXTURE)), OK)
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)
	await get_tree().process_frame

	assert_null(_find_node_by_type(preview, "DirectionalLight3D"), "Object preview should not use Godot directional lights.")
	assert_null(_find_node_by_type(preview, "OmniLight3D"), "Object preview should not use Godot omni lights.")
	assert_true(_find_node_by_type(preview, "MeshInstance3D") != null, "Object preview should build visible mesh instances.")
	assert_not_null(_find_node_by_name(preview, "ObjectGrid"), "Object preview should include an authoring grid.")
	var camera := _find_node_by_type(preview, "Camera3D") as Camera3D
	assert_not_null(camera, "Object preview should create a camera.")
	assert_eq(camera.get_script(), FlyCameraScript, "Object preview should use the shared fly camera controls.")


func test_object_preview_bounds_use_transformed_robj_meshes() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)
	await get_tree().process_frame

	var bounds: AABB = preview._compute_transformed_mesh_bounds()
	assert_gt(bounds.size.length(), 0.0, "Object preview should compute transformed mesh bounds.")
	var checked := 0
	for robj_node in preview._robj_nodes.values():
		var node := robj_node as Node3D
		for child in node.get_children():
			if child is MeshInstance3D:
				var instance := child as MeshInstance3D
				if instance.mesh == null:
					continue
				var mesh_aabb := instance.mesh.get_aabb()
				if mesh_aabb.size == Vector3.ZERO:
					continue
				assert_true(_aabb_encloses(bounds, instance.global_transform * mesh_aabb), "Preview bounds should include each transformed robj mesh.")
				checked += 1
	assert_gt(checked, 0, "Fixture should create transformed preview mesh instances.")
	var camera := _find_node_by_type(preview, "Camera3D") as Camera3D
	assert_gt(camera.far, camera.near, "Preview camera clipping should be valid after framing.")


func test_object_preview_applies_diffuse_material_textures() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)
	await get_tree().process_frame

	assert_not_null(_find_textured_shader_material(preview), "Object preview should bind diffuse textures to mesh materials.")
	var material := _find_textured_shader_material(preview)
	assert_true(material.get_shader_parameter("u_diffuse") is Texture2D, "Textured preview materials should bind renderer diffuse uniforms.")
	var shader_code := material.shader.code
	assert_string_contains(shader_code, "obj_transform_uv(UV2)", "Preview shader should carry secondary UVs into renderer materials.")
	assert_false(shader_code.contains("uv_rate"), "Preview shader should not duplicate renderer UV generator evaluation.")
	assert_false(shader_code.contains("rgb_gen_enabled"), "Preview shader should not duplicate renderer RGB generator evaluation.")
	var material_entry := _find_textured_preview_material_entry(preview)
	var material_index := int(material_entry.get("material_index", -1))
	var material_info: Dictionary = data.get_material_info(material_index)
	if bool(material_info.get("two_sided", false)):
		assert_string_contains(shader_code, "cull_disabled", "Two-sided preview materials should disable culling.")
	else:
		assert_string_contains(shader_code, "cull_back", "One-sided preview materials should keep backface culling.")


func test_object_data_builds_each_lod_with_consistent_mesh_arrays() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var lod_count := int(data.get_summary().get("lod_count", 0))
	assert_gt(lod_count, 0, "Fixture should expose render LODs.")

	for lod_index in range(lod_count):
		var submeshes: Array = data.build_lod_submeshes(lod_index)
		assert_false(submeshes.is_empty(), "Each render LOD should build preview submeshes.")
		for submesh in submeshes:
			var entry: Dictionary = submesh
			var mesh := entry.get("mesh") as ArrayMesh
			assert_not_null(mesh, "LOD submeshes should carry ArrayMesh instances.")
			if mesh != null:
				_assert_mesh_arrays_consistent(mesh, lod_index)


func test_object_data_exposes_renderer_runtime_inputs() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)

	assert_gt(data.get_material_count(), 0, "Renderer runtime APIs need material access.")
	var lod_info: Dictionary = data.get_render_lod_info(0)
	assert_gt(int(lod_info.get("render_object_count", 0)), 0, "Render LOD info should expose render object count.")
	var submeshes: Array = data.build_lod_submeshes(0)
	assert_false(submeshes.is_empty(), "Renderer preview should expose submeshes grouped for runtime transforms.")
	var submesh: Dictionary = submeshes[0]
	assert_true(submesh.get("mesh") is ArrayMesh, "Submesh entries should carry ArrayMesh instances.")
	assert_true(submesh.has("robj_index"), "Submesh entries should carry render object indices.")
	var mesh := submesh.get("mesh") as ArrayMesh
	var arrays: Array = mesh.surface_get_arrays(0)
	var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var uvs2: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
	assert_eq(uvs2.size(), vertices.size(), "Runtime preview submeshes should carry secondary UVs.")
	if arrays[Mesh.ARRAY_TANGENT] is PackedFloat32Array:
		var tangents: PackedFloat32Array = arrays[Mesh.ARRAY_TANGENT]
		if not tangents.is_empty():
			assert_eq(tangents.size(), vertices.size() * 4, "Runtime preview submeshes should carry Godot tangents.")

	var material_info: Dictionary = data.get_material_info(0)
	assert_false(String(material_info.get("shader_tag", "")).is_empty(), "Material info should expose shader tags.")
	var runtime: Dictionary = data.eval_material_runtime(0, 250, {})
	assert_true(runtime.get("uv_offset") is Vector2, "Material runtime should expose UV offset.")
	assert_true(runtime.get("uv_scale") is Vector2, "Material runtime should expose UV scale.")
	assert_true(runtime.get("rgb_mod") is Vector3, "Material runtime should expose RGB modulation.")
	assert_eq(typeof(runtime.get("alpha_mod")), TYPE_FLOAT, "Material runtime should expose alpha modulation.")

	var transforms: Dictionary = data.evaluate_panm(0, 250, {})
	assert_gt(transforms.size(), 0, "PANM evaluation should return per-part transforms.")
	var transform_keys: Array = transforms.keys()
	assert_true(transforms[transform_keys[0]] is Transform3D, "PANM results should be Transform3D values.")

	var lights: Array = data.evaluate_lights(250, {})
	if not lights.is_empty():
		var light: Dictionary = lights[0]
		assert_true(light.get("color") is Color, "Runtime lights should expose evaluated colors.")
		assert_true(light.get("position") is Vector3, "Runtime lights should expose positions.")


func test_object_preview_runtime_controls_update_state() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)

	preview.set_playing(false)
	preview.set_wireframe(true)
	preview.set_ctrl_value("door", 70000)
	preview.reset_animation_time()

	var ctrl_values: Dictionary = preview.get_ctrl_values()
	assert_false(preview.is_playing(), "Preview playback should be controllable from the inspector.")
	assert_true(preview.is_wireframe(), "Preview wireframe state should be controllable from the inspector.")
	assert_eq(int(ctrl_values.get("door", -1)), 65535, "Preview control register values should clamp to uint16.")
	assert_eq(preview.get_animation_time_ms(), 0, "Preview reset should rewind animation time.")


func test_object_workspace_preview_inspector_exposes_runtime_controls() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var viewport_host = add_child_autofree(Control.new())
	workspace.mount_viewport(viewport_host)
	var host = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PREVIEW, host)

	var preview := _find_node_by_name(viewport_host, "ObjectPreview") as ObjectPreview
	var play := _find_node_by_name(host, "PreviewPlayButton") as Button
	var reset := _find_node_by_name(host, "PreviewResetButton") as Button
	var wire := _find_node_by_name(host, "PreviewWireCheck") as CheckBox
	assert_not_null(preview)
	assert_not_null(play)
	assert_not_null(reset)
	assert_not_null(wire)

	play.toggled.emit(false)
	wire.toggled.emit(true)
	reset.pressed.emit()

	assert_false(preview.is_playing(), "Preview inspector play toggle should update the preview.")
	assert_true(preview.is_wireframe(), "Preview inspector wire toggle should update the preview.")
	assert_eq(preview.get_animation_time_ms(), 0, "Preview inspector reset should rewind the preview.")


func test_object_workspace_new_creates_empty_saveable_project() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)

	assert_eq(workspace.new_current(), OK)

	var object_data: NovaObjectData = workspace.object_editor.object_data
	assert_not_null(object_data)
	assert_true(object_data.has_document(), "New Object should create a document immediately.")
	assert_eq(object_data.get_source_kind(), "empty", "A new Object document should start as an empty project.")
	assert_true(workspace.can_save_as(), "New Object projects should be saveable as .3dp workspaces.")
	assert_false(workspace.can_export(), "Empty Object projects should not export 3DI until geometry exists.")
	assert_eq(workspace.save_as(_output_dir()), OK)
	assert_true(FileAccess.file_exists(_output_dir().path_join("untitled.3dp")), "Saving a new Object should write an object project file.")


func test_object_workspace_open_ase_saves_reopenable_project() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(BIRD_ASE_FIXTURE)), OK)
	assert_true(workspace.can_export(), "Opening an ASE should create an exportable project-backed object.")

	assert_eq(workspace.save_as(_output_dir()), OK)

	var saved_project := _output_dir().path_join("Bird1.3dp")
	var saved_scene := _output_dir().path_join("Bird1.ase")
	assert_true(FileAccess.file_exists(saved_project), "Saving an ASE-backed object should write a 3DP project.")
	assert_true(FileAccess.file_exists(saved_scene), "Saving an ASE-backed object should keep its scene source with the project.")
	assert_eq(workspace.open_file(saved_project), OK, "Saved ASE-backed projects should reopen from their project directory.")
	assert_eq(workspace.object_editor.object_data.get_source_kind(), "3dp")
	assert_true(workspace.can_export(), "Reopened ASE-backed projects should remain exportable.")
	var export_dir := _output_dir().path_join("exported")
	assert_eq(workspace.begin_export(export_dir, 0), OK)
	assert_true(FileAccess.file_exists(export_dir.path_join("Bird1.3di")), "Reopened ASE-backed projects should export a 3DI.")


func test_object_workspace_new_adds_lod_scene_and_exports_project() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.new_current(), OK)
	assert_true(workspace.has_method("add_lod_scene"), "Object workspace should expose an add LOD scene action.")
	if not workspace.has_method("add_lod_scene"):
		return

	assert_eq(workspace.add_lod_scene(ProjectSettings.globalize_path(BIRD_ASE_FIXTURE)), OK)

	var object_data: NovaObjectData = workspace.object_editor.object_data
	assert_eq(object_data.get_source_kind(), "3dp", "Adding a LOD scene should turn an empty object into a project-backed object.")
	assert_eq(object_data.get_object_name(), "Bird1", "The first added LOD scene should name a new object project.")
	assert_true(workspace.can_export(), "A new project with a LOD scene should export 3DI.")
	var lods: Array = object_data.get_project_lods()
	assert_eq(lods.size(), 1, "The new object project should track one LOD scene.")
	assert_eq(String(lods[0].get("scene_file", "")), "Bird1.ase")
	assert_eq(String(lods[0].get("render_function", "")), "gnrc")

	assert_eq(workspace.save_as(_output_dir()), OK)
	assert_true(FileAccess.file_exists(_output_dir().path_join("Bird1.3dp")))
	assert_true(FileAccess.file_exists(_output_dir().path_join("Bird1.ase")))
	var export_dir := _output_dir().path_join("new_export")
	assert_eq(workspace.begin_export(export_dir, 0), OK)
	assert_true(FileAccess.file_exists(export_dir.path_join("Bird1.3di")))


func test_object_lods_inspector_exposes_scene_and_project_settings() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	var host = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(4, host)

	assert_not_null(_find_node_by_name(host, "ObjectLodsList"), "LOD inspector should list project LOD scene bindings.")
	assert_not_null(_find_node_by_name(host, "ObjectAddLodSceneButton"), "LOD inspector should expose an add scene control.")
	assert_not_null(_find_node_by_name(host, "ObjectReplaceLodSceneButton"), "LOD inspector should expose a replace scene control.")
	assert_not_null(_find_node_by_name(host, "ObjectLodThreshold"), "LOD inspector should expose the selected LOD threshold.")
	assert_not_null(_find_node_by_name(host, "ObjectLodAttributes"), "LOD inspector should expose selected LOD attributes.")
	assert_not_null(_find_node_by_name(host, "ObjectLodRenderFunction"), "LOD inspector should expose selected LOD render function.")
	assert_not_null(_find_node_by_name(host, "ObjectPolyCollisionLod"), "LOD inspector should expose the project collision LOD setting.")


func test_object_lods_inspector_edits_scene_and_project_settings() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.new_current(), OK)
	assert_eq(workspace.add_lod_scene(ProjectSettings.globalize_path(BIRD_ASE_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_true(data.set_lod_field(0, "threshold", 12.5))
	assert_true(data.set_project_field("poly_collision_lod", 3))
	workspace.object_editor.mark_clean()
	var host = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LODS, host)
	var threshold := _find_node_by_name(host, "ObjectLodThreshold") as SpinBox
	var attributes := _find_node_by_name(host, "ObjectLodAttributes") as SpinBox
	var render_function := _find_node_by_name(host, "ObjectLodRenderFunction") as LineEdit
	var poly_lod := _find_node_by_name(host, "ObjectPolyCollisionLod") as SpinBox
	assert_not_null(threshold)
	assert_not_null(attributes)
	assert_not_null(render_function)
	assert_not_null(poly_lod)
	assert_eq(threshold.value, 12.5, "LOD inspector should reflect the selected LOD threshold.")
	assert_eq(poly_lod.value, 3.0, "LOD inspector should reflect the project collision LOD.")

	threshold.value = 25.0
	threshold.value_changed.emit(25.0)
	attributes.value = 7
	attributes.value_changed.emit(7.0)
	render_function.text = "clod"
	render_function.text_submitted.emit("clod")
	poly_lod.value = 0
	poly_lod.value_changed.emit(0.0)

	assert_true(workspace.object_editor.is_dirty, "Editing LOD/project settings should mark the object dirty.")
	var lod: Dictionary = data.get_project_lods()[0]
	assert_eq(float(lod.get("threshold", 0.0)), 25.0)
	assert_eq(int(lod.get("attributes", 0)), 7)
	assert_eq(String(lod.get("render_function", "")), "clod")
	assert_eq(int(data.get_summary().get("poly_collision_lod", -1)), 0)


func test_object_part_anims_inspector_populates_edits_and_exports() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_gt(data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	workspace.object_editor.mark_clean()
	var host = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, host)
	var list := _find_node_by_name(host, "PartAnimList") as ItemList
	var lod_index := _find_node_by_name(host, "PartAnimLodIndex") as SpinBox
	var transform_as := _find_node_by_name(host, "PartAnimTransformAs") as SpinBox
	var parent := _find_node_by_name(host, "PartAnimParent") as SpinBox
	var scale_type := _find_node_by_name(host, "PartAnimScaleType") as SpinBox
	var rotation_type := _find_node_by_name(host, "PartAnimRotationType") as SpinBox
	var translate_type := _find_node_by_name(host, "PartAnimTranslateType") as SpinBox
	var reversed := _find_node_by_name(host, "PartAnimRotationReversed") as CheckBox
	assert_not_null(list, "Part animation inspector should expose a stable list node.")
	assert_not_null(lod_index, "Part animation inspector should expose the editable LOD index.")
	assert_not_null(transform_as)
	assert_not_null(parent)
	assert_not_null(scale_type)
	assert_not_null(rotation_type)
	assert_not_null(translate_type)
	assert_not_null(reversed)
	if list == null or lod_index == null or transform_as == null or parent == null or scale_type == null or rotation_type == null or translate_type == null or reversed == null:
		return

	list.select(0)
	list.item_selected.emit(0)
	var original: Dictionary = data.get_part_anim_info(0, 0)
	assert_eq(int(lod_index.value), 0)
	assert_eq(int(transform_as.value), int(original.get("transform_as", 0)))
	assert_eq(int(parent.value), int(original.get("parent_subobject", 0)))

	transform_as.value = 2
	transform_as.value_changed.emit(2.0)
	parent.value = 1
	parent.value_changed.emit(1.0)
	scale_type.value = 4
	scale_type.value_changed.emit(4.0)
	rotation_type.value = 5
	rotation_type.value_changed.emit(5.0)
	translate_type.value = 6
	translate_type.value_changed.emit(6.0)
	reversed.button_pressed = true
	reversed.toggled.emit(true)

	assert_true(workspace.object_editor.is_dirty, "Editing a part animation should mark the object dirty.")
	var updated: Dictionary = data.get_part_anim_info(0, 0)
	assert_eq(int(updated.get("transform_as", 0)), 2)
	assert_eq(int(updated.get("parent_subobject", 0)), 1)
	assert_eq(int(updated.get("scale_type", 0)), 4)
	assert_eq(int(updated.get("rotation_type", 0)), 5)
	assert_eq(int(updated.get("translate_type", 0)), 6)
	assert_true(bool(updated.get("rotation_reversed", false)))

	var export_dir := _output_dir().path_join("part_anim_export")
	assert_eq(workspace.begin_export(export_dir, 0), OK)
	var reopened := NovaObjectData.new()
	assert_eq(reopened.open_file(export_dir.path_join("Armry01.3di")), OK)
	var reopened_info: Dictionary = reopened.get_part_anim_info(0, 0)
	assert_eq(int(reopened_info.get("transform_as", 0)), 2)
	assert_eq(int(reopened_info.get("parent_subobject", 0)), 1)
	assert_eq(int(reopened_info.get("scale_type", 0)), 4)
	assert_eq(int(reopened_info.get("rotation_type", 0)), 5)
	assert_eq(int(reopened_info.get("translate_type", 0)), 6)
	assert_true(bool(reopened_info.get("rotation_reversed", false)))


func test_object_lights_inspector_populates_edits_and_exports() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_gt(data.get_light_count(), 0, "Fixture should expose object lights.")
	workspace.object_editor.mark_clean()
	var host = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LIGHTS, host)
	var list := _find_node_by_name(host, "ObjectLightsList") as ItemList
	var start_color := _find_node_by_name(host, "LightStartColor") as ColorPickerButton
	var end_color := _find_node_by_name(host, "LightEndColor") as ColorPickerButton
	var attenuation_start := _find_node_by_name(host, "LightAttenuationStart") as SpinBox
	var style := _find_node_by_name(host, "LightStyle") as SpinBox
	var disable_corona := _find_node_by_name(host, "LightDisableCorona") as CheckBox
	var disable_terrain := _find_node_by_name(host, "LightDisableTerrain") as CheckBox
	var disable_objects := _find_node_by_name(host, "LightDisableObjects") as CheckBox
	assert_not_null(list, "Light inspector should expose a stable list node.")
	assert_not_null(start_color)
	assert_not_null(end_color)
	assert_not_null(attenuation_start)
	assert_not_null(style)
	assert_not_null(disable_corona)
	assert_not_null(disable_terrain)
	assert_not_null(disable_objects)
	if list == null or start_color == null or end_color == null or attenuation_start == null or style == null or disable_corona == null or disable_terrain == null or disable_objects == null:
		return

	list.select(0)
	list.item_selected.emit(0)
	var original: Dictionary = data.get_light_info(0)
	assert_true(start_color.color.is_equal_approx(original.get("color_start", Color.WHITE)))
	assert_true(end_color.color.is_equal_approx(original.get("color_end", Color.WHITE)))
	assert_true(is_equal_approx(attenuation_start.value, float(original.get("atten_start", 0.0))))

	var next_color := Color(0.2, 0.4, 0.6, 1.0)
	start_color.color = next_color
	start_color.color_changed.emit(next_color)
	attenuation_start.value = 3.5
	attenuation_start.value_changed.emit(3.5)
	style.value = 7
	style.value_changed.emit(7.0)
	disable_corona.button_pressed = true
	disable_corona.toggled.emit(true)
	disable_terrain.button_pressed = true
	disable_terrain.toggled.emit(true)
	disable_objects.button_pressed = true
	disable_objects.toggled.emit(true)

	assert_true(workspace.object_editor.is_dirty, "Editing a light should mark the object dirty.")
	var updated: Dictionary = data.get_light_info(0)
	assert_true((updated.get("color_start", Color.WHITE) as Color).is_equal_approx(next_color))
	assert_true(is_equal_approx(float(updated.get("atten_start", 0.0)), 3.5))
	assert_eq(int(updated.get("colorgen_style", 0)), 7)
	assert_true(bool(updated.get("disable_corona", false)))
	assert_true(bool(updated.get("disable_lightterrain", false)))
	assert_true(bool(updated.get("disable_lightobjects", false)))

	var export_dir := _output_dir().path_join("light_export")
	assert_eq(workspace.begin_export(export_dir, 0), OK)
	var reopened := NovaObjectData.new()
	assert_eq(reopened.open_file(export_dir.path_join("Armry01.3di")), OK)
	var reopened_info: Dictionary = reopened.get_light_info(0)
	assert_true((reopened_info.get("color_start", Color.WHITE) as Color).is_equal_approx(next_color))
	assert_true(is_equal_approx(float(reopened_info.get("atten_start", 0.0)), 3.5))
	assert_eq(int(reopened_info.get("colorgen_style", 0)), 7)
	assert_true(bool(reopened_info.get("disable_corona", false)))
	assert_true(bool(reopened_info.get("disable_lightterrain", false)))
	assert_true(bool(reopened_info.get("disable_lightobjects", false)))


func test_object_materials_inspector_populates_material_slots() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var host = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, host)

	assert_not_null(_find_node_by_name(host, "MaterialsList"), "Materials inspector should expose the material list.")
	assert_not_null(_find_node_by_name(host, "ShaderTagOption"), "Materials inspector should expose shader tag selection.")
	var slot1_name := _find_node_by_name(host, "TextureSlot1Name") as LineEdit
	var slot1_status := _find_node_by_name(host, "TextureSlot1Status") as Label
	var slot2_name := _find_node_by_name(host, "TextureSlot2Name") as LineEdit
	var slot2_status := _find_node_by_name(host, "TextureSlot2Status") as Label
	assert_not_null(slot1_name)
	assert_not_null(slot1_status)
	assert_not_null(slot2_name)
	assert_not_null(slot2_status)
	assert_eq(slot1_name.text, "KArm1.tga", "Slot 1 should show the diffuse texture name from the 3DI.")
	assert_string_contains(slot1_status.text, "Resolved")
	assert_string_contains(slot1_status.text.to_lower(), "karm1_o.tga")
	assert_eq(slot2_name.text, "KRE_1_O.tga", "Slot 2 should show the detail texture name from the 3DI.")
	assert_string_contains(slot2_status.text, "Resolved")
	assert_not_null(_find_node_by_name(host, "TextureAnimationSection"), "Materials inspector should expose texture animation controls.")


func test_object_materials_inspector_edits_texture_slot_and_marks_dirty() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var host = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, host)
	var slot1_name := _find_node_by_name(host, "TextureSlot1Name") as LineEdit
	var slot1_status := _find_node_by_name(host, "TextureSlot1Status") as Label
	assert_not_null(slot1_name)
	assert_not_null(slot1_status)

	slot1_name.text = "KArm1_O.TGA"
	slot1_name.text_submitted.emit("KArm1_O.TGA")

	assert_true(workspace.object_editor.is_dirty, "Editing a material texture slot should mark the object dirty.")
	var material: Dictionary = workspace.object_editor.object_data.get_materials()[0]
	var texture_info := _texture_for_slot_from_material(material, 1)
	assert_eq(String(texture_info.get("name", "")), "KArm1_O.TGA")
	assert_string_contains(slot1_status.text, "Resolved")


func test_object_materials_inspector_gates_slots_from_shader_flags() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_eq(workspace.object_editor.object_data.set_material_shader(0, "FF_ST_OP"), OK)
	var host = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, host)

	var slot2_name := _find_node_by_name(host, "TextureSlot2Name") as LineEdit
	var slot2_status := _find_node_by_name(host, "TextureSlot2Status") as Label
	assert_not_null(slot2_name)
	assert_not_null(slot2_status)
	assert_false(slot2_name.editable, "Detail slot should be disabled when the shader does not support a secondary texture.")
	assert_string_contains(slot2_status.text, "Unsupported")


func test_object_materials_inspector_edits_uv_generators() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_eq(workspace.object_editor.object_data.set_material_shader(0, "FF_ST_OP#UV"), OK)
	var host = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, host)
	var u_section := _find_node_by_name(host, "UGeneratorSection") as Control
	var u_style := _find_node_by_name(host, "UGeneratorStyle") as SpinBox
	assert_not_null(u_section)
	assert_not_null(u_style)
	assert_true(u_section.visible, "UV generator controls should be visible for #UV shaders.")

	u_style.value = 2
	u_style.value_changed.emit(2.0)

	assert_true(workspace.object_editor.is_dirty, "Editing a generator should mark the object dirty.")
	var material: Dictionary = workspace.object_editor.object_data.get_materials()[0]
	var u_params: Dictionary = material.get("u_params", {})
	assert_eq(int(u_params.get("style", 0)), 2)


func test_object_materials_inspector_copies_and_pastes_settings() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(workspace.object_editor.object_data.get_material_count(), 1, "Copy/paste test needs at least two materials.")
	var host = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, host)

	var list := _find_node_by_name(host, "MaterialsList") as ItemList
	var copy_button := _find_node_by_name(host, "MaterialCopyButton") as Button
	var paste_button := _find_node_by_name(host, "MaterialPasteButton") as Button
	assert_not_null(list)
	assert_not_null(copy_button)
	assert_not_null(paste_button)
	assert_true(paste_button.disabled, "Paste should start disabled before a material is copied.")

	var source_info: Dictionary = workspace.object_editor.object_data.get_material_info(0)
	list.select(0)
	list.item_selected.emit(0)
	copy_button.pressed.emit()
	assert_false(paste_button.disabled, "Copying a material should enable paste.")

	list.select(1)
	list.item_selected.emit(1)
	workspace.object_editor.object_data.set_material_shader(1, "FF_ST_OP")
	paste_button.pressed.emit()

	var target_info: Dictionary = workspace.object_editor.object_data.get_material_info(1)
	assert_eq(String(target_info.get("shader_tag", "")), String(source_info.get("shader_tag", "")), "Pasted material should copy shader tag.")
	assert_eq(String(target_info.get("diffuse_a", "")), String(source_info.get("diffuse_a", "")), "Pasted material should copy diffuse texture.")


func _assert_mesh_arrays_consistent(mesh: ArrayMesh, lod_index: int) -> void:
	assert_gt(mesh.get_surface_count(), 0, "LOD %d submesh should have at least one surface." % lod_index)
	var arrays: Array = mesh.surface_get_arrays(0)
	var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	assert_false(vertices.is_empty(), "LOD %d submesh vertices should not be empty." % lod_index)
	var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
	assert_eq(uvs.size(), vertices.size(), "LOD %d UV0 count should match vertices." % lod_index)
	var uvs2: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV2]
	assert_eq(uvs2.size(), vertices.size(), "LOD %d UV1 count should match vertices." % lod_index)
	if arrays[Mesh.ARRAY_TANGENT] is PackedFloat32Array:
		var tangents: PackedFloat32Array = arrays[Mesh.ARRAY_TANGENT]
		if not tangents.is_empty():
			assert_eq(tangents.size(), vertices.size() * 4, "LOD %d tangents should use four floats per vertex." % lod_index)
	if arrays[Mesh.ARRAY_INDEX] is PackedInt32Array:
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		assert_eq(indices.size() % 3, 0, "LOD %d triangle index count should be divisible by three." % lod_index)


func _aabb_encloses(outer: AABB, inner: AABB) -> bool:
	var epsilon := 0.001
	return (
		inner.position.x >= outer.position.x - epsilon
		and inner.position.y >= outer.position.y - epsilon
		and inner.position.z >= outer.position.z - epsilon
		and inner.end.x <= outer.end.x + epsilon
		and inner.end.y <= outer.end.y + epsilon
		and inner.end.z <= outer.end.z + epsilon
	)


func _find_node_by_type(root: Node, type_name: String) -> Node:
	if root.get_class() == type_name:
		return root
	for child in root.get_children():
		var found := _find_node_by_type(child, type_name)
		if found != null:
			return found
	return null


func _find_textured_preview_material_entry(preview: ObjectPreview) -> Dictionary:
	for i in range(preview._surface_materials.size()):
		var material := preview._surface_materials[i] as ShaderMaterial
		if material != null and material.get_shader_parameter("u_diffuse") is Texture2D:
			return {
				"material": material,
				"material_index": int(preview._surface_material_indices[i]),
			}
	return {}


func _find_textured_shader_material(root: Node) -> ShaderMaterial:
	if root is MeshInstance3D:
		var mesh_instance := root as MeshInstance3D
		if mesh_instance.material_override is ShaderMaterial:
			var material := mesh_instance.material_override as ShaderMaterial
			if material.get_shader_parameter("u_diffuse") is Texture2D:
				return material
	for child in root.get_children():
		var found := _find_textured_shader_material(child)
		if found != null:
			return found
	return null


func _find_node_by_name(root: Node, node_name: String) -> Node:
	if root.name == node_name:
		return root
	for child in root.get_children():
		var found := _find_node_by_name(child, node_name)
		if found != null:
			return found
	return null


func _shader_catalog_entry(catalog: Array, shader_name: String) -> Dictionary:
	for entry in catalog:
		if String(entry.get("name", "")) == shader_name:
			return entry
	return {}


func _max_triangle_edge(vertices: PackedVector3Array) -> float:
	var result := 0.0
	for i in range(0, vertices.size() - 2, 3):
		var a := vertices[i]
		var b := vertices[i + 1]
		var c := vertices[i + 2]
		result = maxf(result, a.distance_to(b))
		result = maxf(result, b.distance_to(c))
		result = maxf(result, c.distance_to(a))
	return result


func _normal_alignment_counts(vertices: PackedVector3Array, normals: PackedVector3Array) -> Dictionary:
	var sampled := 0
	var opposed := 0
	for i in range(0, min(vertices.size(), normals.size()) - 2, 3):
		var a := vertices[i]
		var b := vertices[i + 1]
		var c := vertices[i + 2]
		var face_normal := (b - a).cross(c - a)
		var average_normal := normals[i] + normals[i + 1] + normals[i + 2]
		if face_normal.length_squared() <= 0.000001 or average_normal.length_squared() <= 0.000001:
			continue
		sampled += 1
		if face_normal.dot(average_normal) < -0.0001:
			opposed += 1
	return {
		"sampled": sampled,
		"opposed": opposed,
	}


func _first_material_texture(data: NovaObjectData) -> Dictionary:
	for material in data.get_materials():
		var textures: Array = material.get("textures", [])
		var material_index := int(material.get("index", -1))
		for i in range(textures.size()):
			var texture: Dictionary = textures[i]
			if not String(texture.get("name", "")).is_empty() and not data.resolve_material_texture_path(material_index, i).is_empty():
				return {
					"material_index": material_index,
					"texture_index": i,
				}
	return {}


func _texture_for_slot_from_material(material: Dictionary, slot: int) -> Dictionary:
	var textures: Array = material.get("textures", [])
	for i in range(textures.size()):
		var texture: Dictionary = textures[i]
		if int(texture.get("slot", 0)) == slot:
			return texture
	return {}


func _texture_name_for_slot(material: Dictionary, slot: int) -> String:
	var texture := _texture_for_slot_from_material(material, slot)
	return String(texture.get("name", ""))


func _open_us01_project_data() -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(US01_PROJECT_FIXTURE)), OK)
	return data


func _object_render_signature(data: NovaObjectData) -> Dictionary:
	return {
		"materials": _material_signature(data),
		"surfaces": _surface_signature(data),
	}


func _material_signature(data: NovaObjectData) -> Array:
	var result := []
	for material in data.get_materials():
		var entry: Dictionary = material
		result.append({
			"index": int(entry.get("index", -1)),
			"material_index": int(entry.get("material_index", -1)),
			"shader": String(entry.get("shader", "")),
			"textures": _texture_slot_signature(entry),
		})
	return result


func _texture_slot_signature(material: Dictionary) -> Array:
	var result := []
	var textures: Array = material.get("textures", [])
	for texture in textures:
		var entry: Dictionary = texture
		result.append({
			"name": String(entry.get("name", "")).to_lower(),
			"slot": int(entry.get("slot", 0)),
			"type": int(entry.get("type", 0)),
			"flags": int(entry.get("flags", 0)),
			"frame": int(entry.get("frame", 0)),
		})
	return result


func _surface_signature(data: NovaObjectData) -> Array:
	var result := []
	for surface in data.get_lod_surfaces(0):
		var entry: Dictionary = surface
		var vertices: PackedVector3Array = entry.get("vertices", PackedVector3Array())
		var indices: PackedInt32Array = entry.get("indices", PackedInt32Array())
		var uvs: PackedVector2Array = entry.get("uvs", PackedVector2Array())
		var uvs2: PackedVector2Array = entry.get("uvs2", PackedVector2Array())
		result.append({
			"primitive_index": int(entry.get("primitive_index", -1)),
			"material_index": int(entry.get("material_index", -1)),
			"material_array_index": int(entry.get("material_array_index", -1)),
			"part_index": int(entry.get("part_index", -1)),
			"vertex_count": vertices.size(),
			"index_count": indices.size(),
			"uv0": _uv_bounds_signature(uvs),
			"uv1": _uv_bounds_signature(uvs2),
		})
	return result


func _uv_bounds_signature(uvs: PackedVector2Array) -> Dictionary:
	if uvs.is_empty():
		return {
			"count": 0,
		}
	var min_u := uvs[0].x
	var max_u := uvs[0].x
	var min_v := uvs[0].y
	var max_v := uvs[0].y
	for uv in uvs:
		min_u = minf(min_u, uv.x)
		max_u = maxf(max_u, uv.x)
		min_v = minf(min_v, uv.y)
		max_v = maxf(max_v, uv.y)
	return {
		"count": uvs.size(),
		"min_u": snappedf(min_u, 0.0001),
		"max_u": snappedf(max_u, 0.0001),
		"min_v": snappedf(min_v, 0.0001),
		"max_v": snappedf(max_v, 0.0001),
	}


func _prepare_texture_variant_fixture(texture_filename: String) -> String:
	var fixture_dir := _output_dir().path_join("texture_variant_" + texture_filename.replace(".", "_"))
	assert_eq(DirAccess.make_dir_recursive_absolute(fixture_dir), OK)
	_copy_file(ProjectSettings.globalize_path(ARMRY_FIXTURE), fixture_dir.path_join("Armry01.3di"))
	_copy_file(ProjectSettings.globalize_path(ARMRY_TEXTURE_FIXTURE), fixture_dir.path_join(texture_filename))
	return fixture_dir


func _prepare_real_dds_texture_fixture(texture_filename: String) -> String:
	var fixture_dir := _output_dir().path_join("real_dds_" + texture_filename.replace(".", "_"))
	assert_eq(DirAccess.make_dir_recursive_absolute(fixture_dir), OK)
	_copy_file(ProjectSettings.globalize_path(ARMRY_FIXTURE), fixture_dir.path_join("Armry01.3di"))
	_write_test_dds(fixture_dir.path_join(texture_filename))
	return fixture_dir


func _write_test_dds(path: String) -> void:
	var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.8, 0.2, 0.1, 1.0))
	assert_eq(image.save_dds(path), OK, "Test DDS should be writable: %s" % path)


func _write_bytes(path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Test file destination should open: %s" % path)
	if file == null:
		return
	file.store_buffer(bytes)
	file.close()


func _copy_file(src: String, dst: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(src)
	assert_gt(bytes.size(), 0, "Fixture copy source should contain bytes: %s" % src)
	var file := FileAccess.open(dst, FileAccess.WRITE)
	assert_not_null(file, "Fixture copy destination should open: %s" % dst)
	if file == null:
		return
	file.store_buffer(bytes)
	file.close()


func _output_dir() -> String:
	return OS.get_user_data_dir().path_join(OUTPUT_DIR_NAME)


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for file in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(file))
	for dir in DirAccess.get_directories_at(path):
		_cleanup_dir(path.path_join(dir))
	DirAccess.remove_absolute(path)


func _cleanup_workspace_children() -> void:
	var object_children := []
	for child in get_children():
		if String(child.name).begins_with("ObjectEditor"):
			object_children.append(child)
	for child in object_children:
		if is_instance_valid(child):
			remove_child(child)
			child.free()
