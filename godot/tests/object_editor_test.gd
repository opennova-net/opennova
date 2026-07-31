extends GutTest

const ObjectEditorScript = preload("res://modtools/object/object_editor.gd")
const ObjectPreviewScript = preload("res://modtools/object/object_preview.gd")
const ObjectWorkspaceScript = preload("res://modtools/object/object_workspace.gd")
const FlyCameraScript = preload("res://engine/fly_camera.gd")
const NovaObjectModelScript = preload("res://engine/object/nova_object_model.gd")

const BIRD_FIXTURE := "res://../fixtures/3dp/Bird1/Bird1.3di"
const BIRD_PROJECT_FIXTURE := "res://../fixtures/3dp/Bird1/Bird1.3dp"
const BIRD_ASE_FIXTURE := "res://../fixtures/3dp/Bird1/Bird1.ase"
const DVAN_FIXTURE := "res://../fixtures/3dp/dapche2/dapche2.3di"
const ARMRY_FIXTURE := "res://../fixtures/3dp/armry01/Armry01.3di"
const ARMRY_TEXTURE_FIXTURE := "res://../fixtures/3dp/armry01/KArm1_O.TGA"
const US01_PROJECT_FIXTURE := "res://../fixtures/3dp/US01_onimport/US01.3dp"
# 3di3 fixtures for the placement ground-anchor: House ships a lowercase "ground"
# userpoint; CharModel (skinned) has none, exercising the part-0 fallback.
const HOUSE_3DI3_FIXTURE := "res://../fixtures/threedi/3di3/House.3di"
const CHARMODEL_3DI3_FIXTURE := "res://../fixtures/threedi/3di3/CharModel.3di"
const FULL_00_ENV := "res://../fixtures/env/full_00.env"
const OUTPUT_DIR_NAME := "object_editor_export_test"
# Deliberate literal pins of the ModSuperOED ReExport3DI bit layout (libs/oed
# OED_UPDATE_*). Product code aliases NovaObjectData.UPDATE_*; this suite pins
# the values so binding drift fails here (see test_oed_update_mask_binding).
const OED_UPDATE_NONE := 0
const OED_UPDATE_MTRL := 1
const OED_UPDATE_LGHT := 2
const OED_UPDATE_PANM := 4
const OED_UPDATE_ALL := OED_UPDATE_MTRL | OED_UPDATE_LGHT | OED_UPDATE_PANM


class ObjectWorkspaceShellDouble:
	extends Node

	var environment_dialog_requested := false

	func sync_from_editor_state() -> void:
		pass

	func show_environment_dialog() -> void:
		environment_dialog_requested = true


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


func test_ground_anchor_prefers_named_ground_userpoint() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(HOUSE_3DI3_FIXTURE)), OK,
		"House.3di fixture should open.")
	# House.3di ships a lowercase "ground" userpoint. get_ground_anchor must return
	# exactly that position, proving the case-insensitive name match against "ground".
	var expected := Vector3.INF
	for i in range(data.get_user_point_count()):
		var up := data.get_user_point_info(i)
		if String(up.get("name", "")).to_lower() == "ground":
			expected = up.get("position", Vector3.ZERO)
			break
	assert_ne(expected, Vector3.INF, "House.3di should contain a 'ground' userpoint.")
	var anchor: Vector3 = data.get_ground_anchor(0)
	assert_true(anchor.is_equal_approx(expected),
		"Ground anchor should equal the 'ground' userpoint position.")


func test_ground_anchor_falls_back_to_part0_center() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(CHARMODEL_3DI3_FIXTURE)), OK,
		"CharModel.3di fixture should open.")
	# CharModel has no "ground" userpoint, so the anchor falls back to part 0's center.
	var has_ground := false
	for i in range(data.get_user_point_count()):
		if String(data.get_user_point_info(i).get("name", "")).to_lower() == "ground":
			has_ground = true
			break
	assert_false(has_ground, "CharModel.3di is expected to have no 'ground' userpoint.")
	# The fallback must return a finite point (it may legitimately be near origin); the
	# exact value is pinned by the C++ unit test against a synthetic bounding center.
	var anchor: Vector3 = data.get_ground_anchor(0)
	assert_true(anchor.is_finite(), "Fallback ground anchor should be finite.")


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
	var material_defs: Dictionary = preview.get_object_model().get_material_defs()
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


func test_oed_update_mask_binding_matches_modsuperoed_bit_layout() -> void:
	# The single-source contract (ENG-4): product GDScript aliases
	# NovaObjectData.UPDATE_*; these literals pin the ModSuperOED ReExport3DI
	# bit layout carried by libs/oed's OED_UPDATE_* so drift fails loudly.
	assert_eq(NovaObjectData.UPDATE_NONE, OED_UPDATE_NONE)
	assert_eq(NovaObjectData.UPDATE_MTRL, OED_UPDATE_MTRL)
	assert_eq(NovaObjectData.UPDATE_LGHT, OED_UPDATE_LGHT)
	assert_eq(NovaObjectData.UPDATE_PANM, OED_UPDATE_PANM)
	assert_eq(NovaObjectData.UPDATE_ALL, OED_UPDATE_ALL)
	assert_eq(NovaObjectModel.OED_UPDATE_ALL, OED_UPDATE_ALL,
		"The engine object model aliases must resolve to the bound constants.")


func test_object_data_tracks_oed_dirty_mask_for_component_edits() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_NONE, "Opening an object should not mark any OED update chunks dirty.")

	assert_eq(data.set_material_shader(0, "FF_ST_OP"), OK)
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_MTRL, "Material edits should mark only MTRL dirty.")

	assert_true(data.set_light_field(0, "atten_start", 3.5))
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_MTRL | OED_UPDATE_LGHT, "Light edits should OR in LGHT.")

	assert_true(data.set_part_anim_target(0, 0, 2, 0))
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_ALL, "Part animation edits should OR in PANM.")


func test_object_data_exposes_and_edits_semantic_part_anim_channels() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_NONE)
	assert_true(data.has_method("get_part_anim_editor_entries"), "Object data should expose semantic PANM editor entries.")
	assert_true(data.has_method("set_part_anim_channel_enabled"), "Object data should expose semantic PANM channel toggles.")
	assert_true(data.has_method("set_part_anim_channel_mode"), "Object data should expose semantic PANM channel modes.")
	assert_true(data.has_method("set_part_anim_channel_values"), "Object data should expose semantic PANM channel values.")
	if not data.has_method("get_part_anim_editor_entries"):
		return

	var original: Array = data.get_part_anim_editor_entries(0)
	assert_gt(original.size(), 0)
	var entry: Dictionary = original[0]
	assert_true(entry.has("target_part"))
	assert_true(entry.has("parent_part"))
	assert_true(entry.get("rotation") is Dictionary)
	assert_true(entry.get("scale") is Dictionary)
	assert_true(entry.get("translation") is Dictionary)

	assert_true(data.set_part_anim_channel_enabled(0, 0, "rotation", true))
	assert_true(data.set_part_anim_channel_mode(0, 0, "rotation", "x", "slide", -1))
	assert_true(data.set_part_anim_channel_values(0, 0, "rotation", "x", 0.0, 90.0, 1.0))
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_PANM, "Part animation semantic edits should mark only PANM dirty.")

	var updated: Array = data.get_part_anim_editor_entries(0)
	var rotation: Dictionary = (updated[0] as Dictionary).get("rotation", {})
	var x_axis: Dictionary = rotation.get("x", {})
	assert_true(bool(rotation.get("enabled", false)))
	assert_eq(String(x_axis.get("mode", "")), "slide")
	assert_almost_eq(float(x_axis.get("from_value", -1.0)), 0.0, 0.001)
	assert_almost_eq(float(x_axis.get("to_value", -1.0)), 90.0, 0.01)
	assert_almost_eq(float(x_axis.get("speed", -1.0)), 1.0, 0.001)


func test_object_data_adds_duplicates_and_deletes_part_anim_entries() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_true(data.has_method("add_part_anim"), "Object data should expose PANM entry creation.")
	assert_true(data.has_method("duplicate_part_anim"), "Object data should expose PANM entry duplication.")
	assert_true(data.has_method("delete_part_anim"), "Object data should expose PANM entry deletion.")
	if not data.has_method("add_part_anim") or not data.has_method("duplicate_part_anim") or not data.has_method("delete_part_anim"):
		return

	var original_count := data.get_part_anim_count(0)
	var added_index := int(data.call("add_part_anim", 0, 3))
	assert_eq(added_index, original_count)
	assert_eq(data.get_part_anim_count(0), original_count + 1)
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_PANM, "Adding a PANM entry should mark only PANM dirty.")
	var added: Dictionary = data.get_part_anim_editor_entries(0)[added_index]
	assert_eq(int(added.get("target_part", -1)), 3)
	assert_eq(int(added.get("parent_part", -1)), 0)
	assert_false(bool((added.get("rotation", {}) as Dictionary).get("enabled", true)))

	assert_true(data.set_part_anim_channel_enabled(0, added_index, "rotation", true))
	assert_true(data.set_part_anim_channel_mode(0, added_index, "rotation", "x", "slide", -1))
	assert_true(data.set_part_anim_channel_values(0, added_index, "rotation", "x", 0.0, 90.0, 1.0))
	var duplicate_index := int(data.call("duplicate_part_anim", 0, added_index))
	assert_eq(duplicate_index, added_index + 1)
	assert_eq(data.get_part_anim_count(0), original_count + 2)
	var duplicated: Dictionary = data.get_part_anim_editor_entries(0)[duplicate_index]
	var duplicated_rotation: Dictionary = duplicated.get("rotation", {})
	var duplicated_x: Dictionary = duplicated_rotation.get("x", {})
	assert_eq(int(duplicated.get("target_part", -1)), 3)
	assert_eq(String(duplicated_x.get("mode", "")), "slide")
	assert_almost_eq(float(duplicated_x.get("to_value", 0.0)), 90.0, 0.01)

	assert_true(bool(data.call("delete_part_anim", 0, added_index)))
	assert_eq(data.get_part_anim_count(0), original_count + 1)
	var shifted_duplicate: Dictionary = data.get_part_anim_editor_entries(0)[added_index]
	var shifted_x: Dictionary = (shifted_duplicate.get("rotation", {}) as Dictionary).get("x", {})
	assert_eq(int(shifted_duplicate.get("target_part", -1)), 3)
	assert_eq(String(shifted_x.get("mode", "")), "slide")
	assert_true(bool(data.call("delete_part_anim", 0, added_index)))
	assert_eq(data.get_part_anim_count(0), original_count)


func test_object_data_part_anim_target_preserves_matrix_binding() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	var lod_info: Dictionary = data.get_render_lod_info(0)
	var part_count := int(lod_info.get("part_count", lod_info.get("render_object_count", 0)))
	assert_gt(part_count, 1, "Fixture should expose multiple parts for target reassignment.")
	var original: Dictionary = data.get_part_animations(0)[0]
	var original_target := int(original.get("part_index", 0))
	var next_target := (original_target + 1) % part_count
	var original_parent := int(original.get("parent_part", 0))
	var original_matrix := int(original.get("matrix_index", -1))
	var original_bind := int(original.get("bind_matrix_index", -1))

	assert_true(data.set_part_anim_target(0, 0, next_target, original_parent))

	var updated: Dictionary = data.get_part_animations(0)[0]
	assert_eq(int(updated.get("part_index", -1)), next_target, "Target part should change.")
	assert_eq(int(updated.get("parent_part", -1)), original_parent, "Parent part should be preserved when unchanged.")
	assert_eq(int(updated.get("matrix_index", -1)), original_matrix, "Changing the target part should not rebind the PANM matrix index.")
	assert_eq(int(updated.get("bind_matrix_index", -1)), original_bind, "Changing the target part should not rebind the PANM bind matrix index.")


func test_object_data_masked_export_clears_exported_oed_dirty_bits() -> void:
	var data := _open_us01_project_data()
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_NONE)
	assert_eq(data.set_material_shader(0, "FF_ST_OP"), OK)
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_MTRL)

	var export_dir := _output_dir().path_join("masked_us01_export")
	assert_eq(DirAccess.make_dir_recursive_absolute(export_dir), OK)
	assert_eq(data.call("export_3di_to_dir", export_dir, OED_UPDATE_MTRL), OK)

	assert_true(FileAccess.file_exists(export_dir.path_join("US01.3di")), "Masked project export should still write a 3DI.")
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_NONE, "Successful masked export should clear the exported dirty bit.")


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


func test_nova_object_model_builds_runtime_scene_without_editor_viewport() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var model = add_child_autofree(NovaObjectModelScript.new())
	model.set_object_data(data)
	await get_tree().process_frame

	assert_null(_find_node_by_type(model, "Camera3D"), "Object model should not own editor camera nodes.")
	assert_null(_find_node_by_name(model, "ObjectGrid"), "Object model should not own editor grid nodes.")
	assert_true(_find_node_by_type(model, "MeshInstance3D") != null, "Object model should build visible mesh instances.")
	assert_gt(model.get_model_bounds().size.length(), 0.0, "Object model should expose stable local bounds.")
	assert_gt(model.get_surface_materials().size(), 0, "Object model should expose preview shader materials.")
	assert_gt(model.get_surface_material_indices().size(), 0, "Object model should expose material lookup indices.")


func test_object_preview_uses_internal_viewport_without_godot_lights() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(BIRD_FIXTURE)), OK)
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)
	await get_tree().process_frame

	assert_null(_find_node_by_type(preview, "DirectionalLight3D"), "Object preview should not use Godot directional lights.")
	assert_null(_find_node_by_type(preview, "OmniLight3D"), "Object preview should not use Godot omni lights.")
	assert_true(_find_node_by_type(preview, "MeshInstance3D") != null, "Object preview should build visible mesh instances.")
	assert_not_null(preview.get_object_model(), "Object preview should delegate object rendering to NovaObjectModel.")
	assert_not_null(_find_node_by_name(preview, "ObjectGrid"), "Object preview should include an authoring grid.")
	assert_not_null(_find_node_by_name(preview, "ObjectAxisGizmo"), "Object preview should include an authoring axis gizmo.")
	assert_null(_find_node_by_name(preview, "ObjectEnvironmentButton"), "Object preview should not duplicate the global environment button.")
	var camera := _find_node_by_type(preview, "Camera3D") as Camera3D
	assert_not_null(camera, "Object preview should create a camera.")
	assert_eq(camera.get_script(), FlyCameraScript, "Object preview should use the shared fly camera controls.")


func test_object_preview_ignores_rebinding_same_object_data() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)
	await get_tree().process_frame
	var model = preview.get_object_model()
	var mesh_instance := _find_node_by_type(model, "MeshInstance3D") as MeshInstance3D
	assert_not_null(mesh_instance, "Object preview should build a mesh instance before the rebind check.")
	if mesh_instance == null:
		return

	preview.set_object_data(data)
	await get_tree().process_frame

	assert_true(is_instance_valid(mesh_instance), "Rebinding the same object data should not free preview nodes.")
	if is_instance_valid(mesh_instance):
		assert_not_null(mesh_instance.get_parent(), "Rebinding the same object data should not rebuild the model.")


func test_object_preview_bounds_use_transformed_robj_meshes() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)
	await get_tree().process_frame

	var model = preview.get_object_model()
	var bounds: AABB = model.get_model_bounds()
	assert_gt(bounds.size.length(), 0.0, "Object preview should compute transformed mesh bounds.")
	var checked := 0
	for robj_node in model.get_render_part_nodes().values():
		var node := robj_node as Node3D
		for child in node.get_children():
			if child is MeshInstance3D:
				var instance := child as MeshInstance3D
				if instance.mesh == null:
					continue
				var mesh_aabb := instance.mesh.get_aabb()
				if mesh_aabb.size == Vector3.ZERO:
					continue
				var local_aabb: AABB = model.global_transform.affine_inverse() * (instance.global_transform * mesh_aabb)
				assert_true(_aabb_encloses(bounds, local_aabb), "Preview bounds should include each transformed robj mesh.")
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
	assert_true(material.get_shader_parameter("u_uv_transform_u") is Vector3,
			"Preview materials should bind the affine U coefficients.")
	assert_true(material.get_shader_parameter("u_uv_transform_v") is Vector3,
			"Preview materials should bind the affine V coefficients.")
	var shader_code := material.shader.code
	assert_string_contains(shader_code, "obj_transform_uv(UV2)", "Preview shader should carry secondary UVs into renderer materials.")
	assert_string_contains(shader_code, "dot(uv1, u_uv_transform_u)",
			"Preview shader should preserve the full affine UV transform.")
	assert_false(shader_code.contains("uv_rate"), "Preview shader should not duplicate renderer UV generator evaluation.")
	assert_false(shader_code.contains("rgb_gen_enabled"), "Preview shader should not duplicate renderer RGB generator evaluation.")
	var material_entry := _find_textured_preview_material_entry(preview)
	var material_index := int(material_entry.get("material_index", -1))
	var material_info: Dictionary = data.get_material_info(material_index)
	if bool(material_info.get("two_sided", false)):
		assert_string_contains(shader_code, "cull_disabled", "Two-sided preview materials should disable culling.")
	else:
		assert_string_contains(shader_code, "cull_back", "One-sided preview materials should keep backface culling.")


func test_object_preview_applies_environment_lighting_and_fog_uniforms() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var env := EnvFile.new()
	env.set_source_path(ProjectSettings.globalize_path(FULL_00_ENV))
	env.load()
	assert_true(env.is_loaded(), "Environment fixture should load from the fixtures dir.")
	if env == null:
		return
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_environment(env, 1200.0)
	preview.set_object_data(data)
	await get_tree().process_frame

	var material := _find_textured_shader_material(preview.get_object_model())
	assert_not_null(material, "Environment uniforms should be written to object shader materials.")
	if material == null:
		return
	var tod := env.interpolate_time_of_day(1200.0)
	_assert_vector3_close(material.get_shader_parameter("u_dir_light_color"), tod.get("sun", Vector3.ZERO), 0.01, "Preview should use environment sun lighting.")
	# The witnessed hemisphere pair: ground -> HemiGroundColor, sky ->
	# HemiSkyColor (docs/render/render-lighting-re.md, REN-5).
	_assert_vector3_close(material.get_shader_parameter("u_hemi_ground_color"), tod.get("ground", Vector3.ZERO), 0.01, "Preview should use environment ground lighting.")
	_assert_vector3_close(material.get_shader_parameter("u_hemi_sky_color"), tod.get("sky", Vector3.ZERO), 0.01, "Preview should use environment sky lighting.")
	# Fog render color is the keyframe color doubled-and-saturated, matching the
	# engine [orig: Environment_UpdateWeatherTick @ 0x57f17c] — .env fog is
	# authored at half intensity. See docs/env/env-tod-re.md.
	var fog_raw: Vector3 = tod.get("fog", Vector3.ZERO)
	var fog_doubled := EnvFile.double_saturate_color(Color(fog_raw.x, fog_raw.y, fog_raw.z))
	_assert_vector3_close(material.get_shader_parameter("u_fog_color"), Vector3(fog_doubled.r, fog_doubled.g, fog_doubled.b), 0.01, "Preview should use the engine-doubled environment fog color.")
	_assert_vector3_close(material.get_shader_parameter("u_dir_light_dir"), -env.compute_sun_direction(1200.0).normalized(), 0.01, "Preview should use environment sun direction.")
	assert_true(bool(material.get_shader_parameter("u_fog_enabled")), "Loaded environments should enable object fog uniforms.")
	assert_eq(int(material.get_shader_parameter("u_fog_type")), env.get_fog_type())
	assert_true(float(material.get_shader_parameter("u_fog_end")) > 0.0, "Object fog should carry a positive fog end distance.")


func test_object_workspace_viewport_uses_global_environment_button_only() -> void:
	var shell := ObjectWorkspaceShellDouble.new()
	add_child_autofree(shell)
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(shell)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var viewport_mount = add_child_autofree(Control.new())
	workspace.mount_viewport(viewport_mount)
	await get_tree().process_frame

	assert_null(_find_node_by_name(viewport_mount, "ObjectEnvironmentButton"), "Object workspace should leave environment controls to the global viewport rail.")
	workspace.release_viewport()


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
	assert_true(runtime.get("uv_transform_u") is Vector3, "Material runtime should expose the affine U coefficients.")
	assert_true(runtime.get("uv_transform_v") is Vector3, "Material runtime should expose the affine V coefficients.")
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


func test_global_control_register_catalog_and_local_rename_surface() -> void:
	var catalog: Array = NovaObjectData.get_global_control_register_catalog()
	assert_eq(catalog.size(), 96)
	assert_eq(catalog[0], {"ordinal": 0, "name": "LOD_FRAC"})
	assert_eq(catalog[95], {"ordinal": 95, "name": "TEX_CAMO3"})
	assert_eq(NovaObjectData.canonical_control_register_name("eWeAp_GuNyAw"),
			"EWEAP_GUNYAW")
	assert_eq(NovaObjectData.canonical_control_register_name("not_retail"), "")

	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_true(data.set_control_register_name(0, "VEHICLE_SPECIAL1"))
	var registers: Array = data.get_control_registers()
	assert_eq(String((registers[0] as Dictionary).get("name", "")),
			"VEHICLE_SPECIAL1")
	assert_eq(int((registers[0] as Dictionary).get("runtime_ordinal", -1)), 71)
	assert_eq(String((registers[0] as Dictionary).get("runtime_name", "")),
			"VEHICLE_SPECIAL1")

	assert_true(data.set_control_register_name(0, "NOT_A_RETAIL_REGISTER"))
	registers = data.get_control_registers()
	assert_eq(String((registers[0] as Dictionary).get("name", "")),
			"NOT_A_RETAIL_REGISTER",
			"Metadata must preserve the model-authored spelling.")
	assert_eq(int((registers[0] as Dictionary).get("runtime_ordinal", -1)), 0,
			"Retail loader misses alias global ordinal zero.")
	assert_eq(String((registers[0] as Dictionary).get("runtime_name", "")),
			"LOD_FRAC")

	assert_true(data.set_control_register_name(0, ""))
	registers = data.get_control_registers()
	assert_eq(String((registers[0] as Dictionary).get("name", "sentinel")), "")
	assert_eq(int((registers[0] as Dictionary).get("runtime_ordinal", -1)), 0)
	assert_eq(String((registers[0] as Dictionary).get("runtime_name", "")),
			"LOD_FRAC",
			"An empty local CTRL name follows retail's ordinal-zero alias.")
	assert_false(data.set_control_register_name(0, "X".repeat(25)),
			"a 24-byte CTRL record cannot silently truncate an authored name")


func test_object_light_control_register_colors_resolve_local_slot_name() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(data.get_light_count(), 0, "Fixture should expose a light for runtime evaluation.")
	var registers: Array = data.get_control_registers()
	assert_gt(registers.size(), 0, "Fixture should expose its FLICKER control register.")
	var register_name := String((registers[0] as Dictionary).get("name", ""))
	assert_eq(register_name, "FLICKER")

	assert_true(data.set_light_field(0, "disable_lightobjects", false))
	assert_true(data.set_light_field(0, "colorgen_phase", 0))
	assert_true(data.set_light_field(0, "color_start", Color.BLACK))
	assert_true(data.set_light_field(0, "color_end", Color.WHITE))
	for style in [113, 114]:
		assert_true(data.set_light_field(0, "colorgen_style", style))
		var at_zero: Array = data.evaluate_lights(0, {register_name: 0})
		var at_half: Array = data.evaluate_lights(0, {register_name: 32768})
		var at_end: Array = data.evaluate_lights(0, {register_name: 65536})
		var below_start: Array = data.evaluate_lights(0, {register_name: -32768})
		assert_gt(at_zero.size(), 0)
		assert_gt(at_half.size(), 0)
		assert_gt(at_end.size(), 0)
		assert_gt(below_start.size(), 0)
		var zero_color: Color = (at_zero[0] as Dictionary).get("color", Color.WHITE)
		var half_color: Color = (at_half[0] as Dictionary).get("color", Color.BLACK)
		var end_color: Color = (at_end[0] as Dictionary).get("color", Color.BLACK)
		var negative_color: Color = (below_start[0] as Dictionary).get("color", Color.BLACK)
		assert_true(zero_color.is_equal_approx(Color.BLACK),
				"Controlled light style %d should sample the register's zero endpoint." % style)
		assert_almost_eq(half_color.r, 127.0 / 255.0, 0.00001,
				"Controlled light style %d should use byte-exact half interpolation." % style)
		assert_true(end_color.is_equal_approx(Color.WHITE),
				"Controlled light style %d should preserve the 0x10000 endpoint." % style)
		assert_almost_eq(negative_color.r, -128.0 / 255.0, 0.00001,
				"Controlled light style %d should preserve negative signed values." % style)

	# Other codes keep the packed byte as waveform phase and do not consume CTRL.
	assert_true(data.set_light_field(0, "colorgen_style", 115))
	var wave_low: Color = (data.evaluate_lights(733, {register_name: 0})[0] as Dictionary).get("color")
	var wave_high: Color = (data.evaluate_lights(733, {register_name: 65535})[0] as Dictionary).get("color")
	assert_true(wave_low.is_equal_approx(wave_high),
			"Light style 115 should remain a waveform when a same-index CTRL value changes.")


func test_object_preview_runtime_controls_update_state() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)

	preview.set_playing(false)
	preview.set_wireframe(true)
	preview.set_ctrl_value("DOOR_00", 65536)
	preview.set_ctrl_value("EWEAP_GUNYAW", -32768)
	preview.set_ctrl_value("LOD_FRAC", -2147483648)
	preview.set_ctrl_value("TEX_CAMO3", 2147483647)
	preview.reset_animation_time()

	var ctrl_values: Dictionary = preview.get_ctrl_values()
	assert_false(preview.is_playing(), "Preview playback should be controllable from the inspector.")
	assert_true(preview.is_wireframe(), "Preview wireframe state should be controllable from the inspector.")
	assert_eq(int(ctrl_values.get("DOOR_00", -1)), 65536,
			"Preview controls preserve retail's exact 0x10000 endpoint.")
	assert_eq(int(ctrl_values.get("EWEAP_GUNYAW", 0)), -32768,
			"Preview controls preserve signed retail dwords.")
	assert_eq(int(ctrl_values.get("LOD_FRAC", 0)), -2147483648,
			"Preview controls preserve INT32_MIN.")
	assert_eq(int(ctrl_values.get("TEX_CAMO3", 0)), 2147483647,
			"Preview controls preserve INT32_MAX.")
	assert_eq(preview.get_animation_time_ms(), 0, "Preview reset should rewind animation time.")


func test_object_workspace_preview_inspector_exposes_runtime_controls() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var viewport_mount = add_child_autofree(Control.new())
	workspace.mount_viewport(viewport_mount)
	var mount = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PREVIEW, mount)

	var preview := _find_node_by_name(viewport_mount, "ObjectPreview") as ObjectPreview
	var play := _find_node_by_name(mount, "PreviewPlayButton") as Button
	var reset := _find_node_by_name(mount, "PreviewResetButton") as Button
	var wire := _find_node_by_name(mount, "PreviewWireCheck") as CheckBox
	var ctrl_value := _find_node_by_name(mount, "ControlRegisterValue_0") as SpinBox
	assert_not_null(preview)
	assert_not_null(play)
	assert_not_null(reset)
	assert_not_null(wire)
	assert_not_null(ctrl_value)
	if ctrl_value != null:
		assert_eq(int(ctrl_value.min_value), -2147483648)
		assert_eq(int(ctrl_value.max_value), 2147483647)

	play.toggled.emit(false)
	wire.toggled.emit(true)
	reset.pressed.emit()

	assert_false(preview.is_playing(), "Preview inspector play toggle should update the preview.")
	assert_true(preview.is_wireframe(), "Preview inspector wire toggle should update the preview.")
	assert_eq(preview.get_animation_time_ms(), 0, "Preview inspector reset should rewind the preview.")


func test_object_preview_inspector_aliases_unknown_and_empty_ctrl_names_like_retail() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_true(workspace.object_editor.object_data.set_control_register_name(
			0, "NOT_A_RETAIL_REGISTER"))

	var viewport_mount = add_child_autofree(Control.new())
	workspace.mount_viewport(viewport_mount)
	var preview := _find_node_by_name(viewport_mount, "ObjectPreview") as ObjectPreview
	assert_not_null(preview)

	var unknown_mount = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(
			ObjectEditorWorkspace.Workflow.PREVIEW, unknown_mount)
	var unknown_label := _find_node_by_name(
			unknown_mount, "ControlRegisterLabel_0") as Label
	var unknown_value := _find_node_by_name(
			unknown_mount, "ControlRegisterValue_0") as SpinBox
	assert_not_null(unknown_label)
	assert_not_null(unknown_value)
	if unknown_label != null:
		assert_eq(unknown_label.text, "NOT_A_RETAIL_REGISTER",
				"The inspector preserves the authored name for display.")
	if unknown_value != null and preview != null:
		unknown_value.value_changed.emit(123)
		assert_eq(preview.get_ctrl_values(), {"LOD_FRAC": 123},
				"An unknown authored name drives retail's ordinal-zero alias.")

	assert_true(workspace.object_editor.object_data.set_control_register_name(0, ""))
	if preview != null:
		preview.clear_ctrl_values()
	var empty_mount = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(
			ObjectEditorWorkspace.Workflow.PREVIEW, empty_mount)
	var empty_label := _find_node_by_name(
			empty_mount, "ControlRegisterLabel_0") as Label
	var empty_value := _find_node_by_name(
			empty_mount, "ControlRegisterValue_0") as SpinBox
	assert_not_null(empty_label,
			"Empty CTRL records remain visible rather than being dropped.")
	assert_not_null(empty_value)
	if empty_label != null:
		assert_eq(empty_label.text, "<empty>")
	if empty_value != null and preview != null:
		empty_value.value_changed.emit(-456)
		assert_eq(preview.get_ctrl_values(), {"LOD_FRAC": -456},
				"An empty authored name drives retail's ordinal-zero alias.")


func test_object_preview_shows_labeled_userpoints_by_default() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(HOUSE_3DI3_FIXTURE)), OK)
	assert_gt(data.get_user_point_count(), 0, "House fixture should carry userpoints.")
	var preview = add_child_autofree(ObjectPreviewScript.new())
	preview.set_object_data(data)
	await get_tree().process_frame

	var overlay := _find_node_by_name(preview, "ObjectUserPoints")
	assert_not_null(overlay, "Object preview should mount the reusable userpoint overlay.")
	if overlay == null:
		return
	assert_true(preview.has_user_points(), "Preview reports userpoints when the model has them.")
	assert_true(preview.is_user_points_visible(), "Userpoints are visible by default in the object preview.")
	var expected_name := String(data.get_user_point_info(0).get("name", ""))
	if expected_name.strip_edges().is_empty():
		expected_name = "userpoint_00"
	var label := _find_node_by_type(overlay, "Label3D") as Label3D
	assert_not_null(label, "Userpoints should render a 3D label.")
	if label != null:
		assert_eq(label.text, expected_name, "The label text should come from the userpoint name.")
		assert_eq(label.font_size, 8, "Userpoint labels should use the compact preview font size.")


func test_object_workspace_preview_userpoint_toggle_controls_overlay() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(HOUSE_3DI3_FIXTURE)), OK)
	var viewport_mount = add_child_autofree(Control.new())
	workspace.mount_viewport(viewport_mount)
	var mount = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PREVIEW, mount)

	var preview := _find_node_by_name(viewport_mount, "ObjectPreview") as ObjectPreview
	var check := _find_node_by_name(mount, "PreviewUserPointsCheck") as CheckBox
	assert_not_null(preview)
	assert_not_null(check)
	if preview == null or check == null:
		return
	assert_false(check.disabled, "The userpoint checkbox should be enabled when the preview has userpoints.")
	assert_true(check.button_pressed, "The userpoint checkbox should mirror the default-visible overlay.")

	check.toggled.emit(false)

	assert_false(preview.is_user_points_visible(), "The inspector toggle should hide userpoint labels.")


func test_object_workspace_preview_userpoint_toggle_disables_without_points() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.new_current(), OK)
	var viewport_mount = add_child_autofree(Control.new())
	workspace.mount_viewport(viewport_mount)
	var mount = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PREVIEW, mount)

	var preview := _find_node_by_name(viewport_mount, "ObjectPreview") as ObjectPreview
	var check := _find_node_by_name(mount, "PreviewUserPointsCheck") as CheckBox
	assert_not_null(preview)
	assert_not_null(check)
	if preview == null or check == null:
		return
	assert_false(preview.has_user_points(), "An empty object document should not expose userpoints.")
	assert_true(check.disabled, "The userpoint checkbox should disable when no labels can be shown.")


func test_object_workspace_preview_and_lods_do_not_mount_empty_detail_dock() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)

	var preview_mount = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PREVIEW, preview_mount)

	assert_null(_find_node_by_name(detail_mount, "ObjectDetailDock"), "Preview should not reserve a right pane.")
	assert_eq(detail_mount.get_child_count(), 0, "Preview should leave the right-pane mount empty.")

	var lods_mount = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LODS, lods_mount)

	assert_null(_find_node_by_name(detail_mount, "ObjectDetailDock"), "LODs should keep their editor in the left pane until a real detail editor exists.")


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


func test_object_workspace_export_mask_controls_follow_oed_dirty_mask() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(US01_PROJECT_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_eq(data.set_material_shader(0, "FF_ST_OP"), OK)

	var mount = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PREVIEW, mount)
	var mtrl := _find_node_by_name(mount, "ObjectExportMtrlCheck") as CheckBox
	var lght := _find_node_by_name(mount, "ObjectExportLghtCheck") as CheckBox
	var panm := _find_node_by_name(mount, "ObjectExportPanmCheck") as CheckBox
	assert_not_null(mtrl)
	assert_not_null(lght)
	assert_not_null(panm)
	if mtrl == null or lght == null or panm == null:
		return

	assert_true(mtrl.button_pressed, "MTRL export toggle should default from the dirty mask.")
	assert_false(lght.button_pressed, "LGHT export toggle should stay off when only materials are dirty.")
	assert_false(panm.button_pressed, "PANM export toggle should stay off when only materials are dirty.")

	var export_dir := _output_dir().path_join("workspace_masked_export")
	assert_eq(workspace.begin_export(export_dir, 0), OK)
	assert_true(FileAccess.file_exists(export_dir.path_join("US01.3di")))
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_NONE, "Workspace export should clear the selected dirty mask.")


func test_object_lods_inspector_exposes_scene_and_project_settings() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	var mount = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(4, mount)

	assert_not_null(_find_node_by_name(mount, "ObjectLodsList"), "LOD inspector should list project LOD scene bindings.")
	assert_not_null(_find_node_by_name(mount, "ObjectAddLodSceneButton"), "LOD inspector should expose an add scene control.")
	assert_not_null(_find_node_by_name(mount, "ObjectReplaceLodSceneButton"), "LOD inspector should expose a replace scene control.")
	assert_not_null(_find_node_by_name(mount, "ObjectLodThreshold"), "LOD inspector should expose the selected LOD threshold.")
	assert_not_null(_find_node_by_name(mount, "ObjectLodAttributes"), "LOD inspector should expose selected LOD attributes.")
	assert_not_null(_find_node_by_name(mount, "ObjectLodRenderFunction"), "LOD inspector should expose selected LOD render function.")
	assert_not_null(_find_node_by_name(mount, "ObjectPolyCollisionLod"), "LOD inspector should expose the project collision LOD setting.")


func test_object_lods_inspector_edits_scene_and_project_settings() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.new_current(), OK)
	assert_eq(workspace.add_lod_scene(ProjectSettings.globalize_path(BIRD_ASE_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_true(data.set_lod_field(0, "threshold", 12.5))
	assert_true(data.set_project_field("poly_collision_lod", 3))
	workspace.object_editor.mark_clean()
	var mount = add_child_autofree(Control.new())

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LODS, mount)
	var threshold := _find_node_by_name(mount, "ObjectLodThreshold") as SpinBox
	var attributes := _find_node_by_name(mount, "ObjectLodAttributes") as SpinBox
	var render_function := _find_node_by_name(mount, "ObjectLodRenderFunction") as LineEdit
	var poly_lod := _find_node_by_name(mount, "ObjectPolyCollisionLod") as SpinBox
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
	await get_tree().process_frame

	assert_true(workspace.object_editor.is_dirty, "Editing LOD/project settings should mark the object dirty.")
	var lod: Dictionary = data.get_project_lods()[0]
	assert_eq(float(lod.get("threshold", 0.0)), 25.0)
	assert_eq(int(lod.get("attributes", 0)), 7)
	assert_eq(String(lod.get("render_function", "")), "clod")
	assert_eq(int(data.get_summary().get("poly_collision_lod", -1)), 0)


func test_object_part_anims_use_left_list_and_right_detail_dock() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_gt(data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	workspace.object_editor.mark_clean()
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	assert_false(workspace.uses_asset_dock(), "Object Preview should not reserve the terrain-style right dock.")
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, list_mount)
	assert_true(workspace.uses_asset_dock(), "Object part animations should use the terrain-style right dock for detail editing.")

	var list_pane := _find_node_by_name(list_mount, "PartAnimListPane")
	var list := _find_node_by_name(list_mount, "PartAnimList") as ItemList
	var lod_index := _find_node_by_name(list_mount, "PartAnimLodIndex") as SpinBox
	var add_button := _find_node_by_name(list_mount, "PartAnimAddButton") as Button
	var duplicate_button := _find_node_by_name(list_mount, "PartAnimDuplicateButton") as Button
	var delete_button := _find_node_by_name(list_mount, "PartAnimDeleteButton") as Button
	var detail_dock := _find_node_by_name(detail_mount, "ObjectDetailDock")
	var target_part := _find_node_by_name(detail_mount, "PartAnimTargetPart") as OptionButton
	var parent_part := _find_node_by_name(detail_mount, "PartAnimParentPart") as OptionButton
	assert_not_null(list, "Part animation inspector should expose a stable list node.")
	assert_not_null(lod_index, "Part animation inspector should expose the editable LOD index.")
	assert_not_null(list_pane, "Part animation workflow should use the left inspector as a list pane.")
	assert_not_null(add_button, "Part animation list pane should expose Add.")
	assert_not_null(duplicate_button, "Part animation list pane should expose Duplicate.")
	assert_not_null(delete_button, "Part animation list pane should expose Delete.")
	assert_not_null(detail_dock, "Part animation workflow should mount details in the right dock.")
	assert_not_null(target_part, "Right detail dock should expose semantic animated-part selection.")
	assert_not_null(parent_part, "Right detail dock should expose semantic parent selection.")
	assert_null(_find_node_by_name(detail_mount, "PartAnimTransformAs"), "PANM detail UI should not expose raw transform_as.")
	assert_null(_find_node_by_name(detail_mount, "PartAnimScaleType"), "PANM detail UI should not expose raw scale type values.")
	assert_null(_find_node_by_name(detail_mount, "PartAnimTrack_rotation_xFunction"), "PANM detail UI should not expose raw track function fields.")
	if list == null or lod_index == null or target_part == null or parent_part == null:
		return

	assert_true(target_part.disabled, "Animated part dropdown should be read-only; the left list conveys the target.")
	assert_true(parent_part.disabled, "Moves-relative-to dropdown should be read-only.")

	list.select(0)
	list.item_selected.emit(0)
	var original: Dictionary = data.get_part_anim_editor_entries(0)[0]
	assert_eq(int(lod_index.value), 0)
	assert_eq(target_part.get_selected_id(), int(original.get("target_part", 0)))
	assert_eq(parent_part.get_selected_id(), int(original.get("parent_part", 0)))

	var target_id := 1 if target_part.get_item_count() > 1 else 0
	var parent_id := 0
	var target_index := _option_index_by_id(target_part, target_id)
	var parent_index := _option_index_by_id(parent_part, parent_id)
	assert_true(target_index >= 0)
	assert_true(parent_index >= 0)
	target_part.select(target_index)
	target_part.item_selected.emit(target_index)
	parent_part.select(parent_index)
	parent_part.item_selected.emit(parent_index)
	await get_tree().process_frame

	assert_true(workspace.object_editor.is_dirty, "Editing a part animation should mark the object dirty.")
	var updated: Dictionary = data.get_part_anim_editor_entries(0)[0]
	assert_eq(int(updated.get("target_part", -1)), target_id)
	assert_eq(int(updated.get("parent_part", -1)), parent_id)

	var export_dir := _output_dir().path_join("part_anim_export")
	assert_eq(workspace.begin_export(export_dir, 0), OK)
	var reopened := NovaObjectData.new()
	assert_eq(reopened.open_file(export_dir.path_join("Armry01.3di")), OK)
	var reopened_info: Dictionary = reopened.get_part_anim_editor_entries(0)[0]
	assert_eq(int(reopened_info.get("target_part", -1)), target_id)
	assert_eq(int(reopened_info.get("parent_part", -1)), parent_id)


func test_object_part_anims_left_actions_add_duplicate_and_delete_entries() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_gt(data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	workspace.object_editor.mark_clean()
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, list_mount)
	var list := _find_node_by_name(list_mount, "PartAnimList") as ItemList
	var add_button := _find_node_by_name(list_mount, "PartAnimAddButton") as Button
	var duplicate_button := _find_node_by_name(list_mount, "PartAnimDuplicateButton") as Button
	var delete_button := _find_node_by_name(list_mount, "PartAnimDeleteButton") as Button
	assert_not_null(list)
	assert_not_null(add_button)
	assert_not_null(duplicate_button)
	assert_not_null(delete_button)
	if list == null or add_button == null or duplicate_button == null or delete_button == null:
		return

	var original_count := data.get_part_anim_count(0)
	list.select(0)
	list.item_selected.emit(0)
	add_button.pressed.emit()
	assert_eq(data.get_part_anim_count(0), original_count + 1, "Add should create a PANM entry in the selected LOD.")
	assert_true(list.is_selected(original_count), "Add should select the new PANM entry.")

	duplicate_button.pressed.emit()
	assert_eq(data.get_part_anim_count(0), original_count + 2, "Duplicate should append a copy of the selected PANM entry.")
	assert_true(list.is_selected(original_count + 1), "Duplicate should select the copied PANM entry.")

	delete_button.pressed.emit()
	assert_eq(data.get_part_anim_count(0), original_count + 1, "Delete should remove the selected PANM entry.")
	delete_button.pressed.emit()
	assert_eq(data.get_part_anim_count(0), original_count, "Delete should be able to remove the added entry too.")
	await get_tree().process_frame

	assert_true(workspace.object_editor.is_dirty, "PANM list actions should mark the object dirty.")
	assert_eq(_oed_dirty_mask(data), OED_UPDATE_PANM, "PANM list actions should mark only PANM dirty.")


func test_object_part_anim_detail_dock_reflows_within_right_pane() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(workspace.object_editor.object_data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(PanelContainer.new())
	detail_mount.custom_minimum_size = Vector2(360, 640)
	detail_mount.size = Vector2(360, 640)

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, list_mount)
	var list := _find_node_by_name(list_mount, "PartAnimList") as ItemList
	assert_not_null(list)
	if list == null:
		return
	list.select(0)
	list.item_selected.emit(0)
	await get_tree().process_frame

	var detail_box := _find_node_by_name(detail_mount, "ObjectDetailDockBox") as Control
	assert_not_null(detail_box)
	if detail_box == null:
		return
	var max_content_width := 340.0
	assert_lt(detail_box.get_combined_minimum_size().x, max_content_width + 0.01, "PANM right pane content should not require horizontal clipping at the shared dock width.")

	for control_name in [
		"PartAnimTargetPart",
		"PartAnimParentPart",
		"PartAnimRotationMode",
		"PartAnimRotationXFrom",
		"PartAnimRotationXTo",
		"PartAnimScaleMode",
		"PartAnimTranslationAxis",
		"PartAnimTranslationMode",
	]:
		var control := _find_node_by_name(detail_mount, control_name) as Control
		assert_not_null(control, "%s should exist in the PANM detail dock." % control_name)
		if control == null:
			continue
		assert_lt(control.get_combined_minimum_size().x, max_content_width + 0.01, "%s should be allowed to fit inside the right pane." % control_name)
		var label := _field_label_for_control(control)
		assert_not_null(label, "%s should have an associated field label." % control_name)
		if label != null:
			assert_false(label.clip_text, "%s label should wrap instead of clipping." % control_name)
			assert_true(label.autowrap_mode != TextServer.AUTOWRAP_OFF, "%s label should be configured to wrap." % control_name)


func test_object_part_anim_value_edits_keep_preview_signal_safe() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(workspace.object_editor.object_data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	var viewport_mount = add_child_autofree(Control.new())
	viewport_mount.size = Vector2(640, 480)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(PanelContainer.new())

	workspace.mount_viewport(viewport_mount)
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, list_mount)
	await get_tree().process_frame
	var preview = _find_node_by_name(viewport_mount, "ObjectPreview")
	assert_not_null(preview, "Object workspace should mount a live object preview.")
	if preview == null:
		return
	var model = preview.call("get_object_model")
	assert_not_null(model, "Object preview should expose the runtime model.")
	var before_bounds: AABB = model.get_model_bounds()
	assert_true(_aabb_is_finite(before_bounds), "Preview bounds should be finite before PANM edits.")

	var list := _find_node_by_name(list_mount, "PartAnimList") as ItemList
	var rotation_x_to := _find_node_by_name(detail_mount, "PartAnimRotationXTo") as SpinBox
	assert_not_null(list)
	assert_not_null(rotation_x_to)
	if list == null or rotation_x_to == null:
		return
	list.select(0)
	list.item_selected.emit(0)

	rotation_x_to.value += 5.0
	rotation_x_to.value_changed.emit(rotation_x_to.value)
	await get_tree().process_frame

	assert_true(is_instance_valid(rotation_x_to), "Editing PANM values should not free the active editor control during its signal.")
	var after_bounds: AABB = model.get_model_bounds()
	assert_true(_aabb_is_finite(after_bounds), "Preview bounds should remain finite after PANM value edits.")
	assert_lt(after_bounds.size.length(), 10000.0, "PANM value edits should not explode preview bounds.")


func test_object_part_anim_sine_translation_end_edit_does_not_rebuild_preview_nodes() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(workspace.object_editor.object_data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	var viewport_mount = add_child_autofree(Control.new())
	viewport_mount.size = Vector2(640, 480)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(PanelContainer.new())

	workspace.mount_viewport(viewport_mount)
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, list_mount)
	await get_tree().process_frame
	var preview = _find_node_by_name(viewport_mount, "ObjectPreview")
	assert_not_null(preview, "Object workspace should mount a live object preview.")
	if preview == null:
		return
	var model = preview.call("get_object_model")
	assert_not_null(model, "Object preview should expose the runtime model.")
	var list := _find_node_by_name(list_mount, "PartAnimList") as ItemList
	var translation_enabled := _find_node_by_name(detail_mount, "PartAnimTranslationEnabled") as CheckBox
	var translation_mode := _find_node_by_name(detail_mount, "PartAnimTranslationMode") as OptionButton
	var translation_to := _find_node_by_name(detail_mount, "PartAnimTranslationTo") as SpinBox
	assert_not_null(list)
	assert_not_null(translation_enabled)
	assert_not_null(translation_mode)
	assert_not_null(translation_to)
	if list == null or translation_enabled == null or translation_mode == null or translation_to == null:
		return
	list.select(0)
	list.item_selected.emit(0)
	translation_enabled.button_pressed = true
	translation_enabled.toggled.emit(true)
	var sine_index := _option_index_by_id(translation_mode, 50)
	assert_true(sine_index >= 0, "Translation driver should expose Sine wave.")
	if sine_index < 0:
		return
	translation_mode.select(sine_index)
	translation_mode.item_selected.emit(sine_index)
	await get_tree().process_frame

	var before_node_ids := _render_node_instance_ids(model)
	assert_gt(before_node_ids.size(), 0, "Preview should have render nodes before editing translation end.")
	var before_bounds: AABB = model.get_model_bounds()
	assert_true(_aabb_is_finite(before_bounds), "Preview bounds should be finite before sine translation edits.")

	translation_to.value = clampf(translation_to.value + 1.0, translation_to.min_value, translation_to.max_value)
	translation_to.value_changed.emit(translation_to.value)
	await get_tree().process_frame

	assert_true(is_instance_valid(translation_to), "Editing a sine translation end value should not free the active spinbox during its signal.")
	assert_eq(_render_node_instance_ids(model), before_node_ids, "PANM value edits should update preview transforms without rebuilding render nodes.")
	var after_bounds: AABB = model.get_model_bounds()
	assert_true(_aabb_is_finite(after_bounds), "Preview bounds should remain finite after sine translation end edits.")
	assert_lt(after_bounds.size.length(), 10000.0, "Sine translation end edits should not explode preview bounds.")
	var updated: Dictionary = workspace.object_editor.object_data.get_part_anim_editor_entries(0)[0]
	var translation: Dictionary = updated.get("translation", {})
	var track: Dictionary = translation.get("track", {})
	assert_eq(String(track.get("mode", "")), "sine_wave", "Translation driver should remain semantic sine wave after editing.")
	assert_almost_eq(float(track.get("to_value", 0.0)), float(translation_to.value), 0.01)


func test_object_part_anim_target_dropdown_preserves_preview_binding() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_gt(data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	var lod_info: Dictionary = data.get_render_lod_info(0)
	var part_count := int(lod_info.get("part_count", lod_info.get("render_object_count", 0)))
	assert_gt(part_count, 1, "Fixture should expose multiple parts for target reassignment.")
	var viewport_mount = add_child_autofree(Control.new())
	viewport_mount.size = Vector2(640, 480)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(PanelContainer.new())

	workspace.mount_viewport(viewport_mount)
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, list_mount)
	await get_tree().process_frame
	var preview = _find_node_by_name(viewport_mount, "ObjectPreview")
	assert_not_null(preview)
	if preview == null:
		return
	var model = preview.call("get_object_model")
	assert_not_null(model)
	var before_bounds: AABB = model.get_model_bounds()
	assert_true(_aabb_is_finite(before_bounds), "Preview bounds should be finite before target edits.")

	var list := _find_node_by_name(list_mount, "PartAnimList") as ItemList
	var target_part := _find_node_by_name(detail_mount, "PartAnimTargetPart") as OptionButton
	assert_not_null(list)
	assert_not_null(target_part)
	if list == null or target_part == null:
		return
	list.select(0)
	list.item_selected.emit(0)
	var original: Dictionary = data.get_part_animations(0)[0]
	var next_target := (int(original.get("part_index", 0)) + 1) % part_count
	var original_matrix := int(original.get("matrix_index", -1))
	var original_bind := int(original.get("bind_matrix_index", -1))
	var target_index := _option_index_by_id(target_part, next_target)
	assert_true(target_index >= 0, "Target dropdown should expose the next part.")
	if target_index < 0:
		return

	target_part.select(target_index)
	target_part.item_selected.emit(target_index)
	await get_tree().process_frame

	var updated: Dictionary = data.get_part_animations(0)[0]
	assert_eq(int(updated.get("part_index", -1)), next_target, "Target dropdown should update the semantic target part.")
	assert_eq(int(updated.get("matrix_index", -1)), original_matrix, "Target dropdown should preserve the PANM matrix binding.")
	assert_eq(int(updated.get("bind_matrix_index", -1)), original_bind, "Target dropdown should preserve the PANM bind matrix binding.")
	var after_bounds: AABB = model.get_model_bounds()
	assert_true(_aabb_is_finite(after_bounds), "Preview bounds should remain finite after target edits.")
	assert_lt(after_bounds.size.length(), 10000.0, "Target edits should not explode preview bounds.")


func test_object_lights_inspector_populates_edits_and_exports() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_gt(data.get_light_count(), 0, "Fixture should expose object lights.")
	workspace.object_editor.mark_clean()
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LIGHTS, list_mount)
	var list := _find_node_by_name(list_mount, "ObjectLightsList") as ItemList
	var start_color := _find_node_by_name(detail_mount, "LightStartColor") as ColorPickerButton
	var end_color := _find_node_by_name(detail_mount, "LightEndColor") as ColorPickerButton
	var attenuation_start := _find_node_by_name(detail_mount, "LightAttenuationStart") as SpinBox
	var style := _find_node_by_name(detail_mount, "LightStyle") as OptionButton
	var disable_corona := _find_node_by_name(detail_mount, "LightDisableCorona") as CheckBox
	var disable_terrain := _find_node_by_name(detail_mount, "LightDisableTerrain") as CheckBox
	var disable_objects := _find_node_by_name(detail_mount, "LightDisableObjects") as CheckBox
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

	var original: Dictionary = data.get_light_info(0)
	assert_true(start_color.color.is_equal_approx(original.get("color_start", Color.WHITE)))
	assert_true(end_color.color.is_equal_approx(original.get("color_end", Color.WHITE)))
	assert_true(is_equal_approx(attenuation_start.value, float(original.get("atten_start", 0.0))))

	var next_color := Color(0.2, 0.4, 0.6, 1.0)
	start_color.color = next_color
	start_color.color_changed.emit(next_color)
	attenuation_start.value = 3.5
	attenuation_start.value_changed.emit(3.5)
	var style_set_index := _option_index_by_id(style, 24)
	assert_true(style_set_index >= 0, "Light style dropdown should offer the 'Set' generator style (id 24).")
	style.select(style_set_index)
	style.item_selected.emit(style_set_index)
	disable_corona.button_pressed = true
	disable_corona.toggled.emit(true)
	disable_terrain.button_pressed = true
	disable_terrain.toggled.emit(true)
	disable_objects.button_pressed = true
	disable_objects.toggled.emit(true)
	await get_tree().process_frame

	assert_true(workspace.object_editor.is_dirty, "Editing a light should mark the object dirty.")
	var updated: Dictionary = data.get_light_info(0)
	assert_true((updated.get("color_start", Color.WHITE) as Color).is_equal_approx(next_color))
	assert_true(is_equal_approx(float(updated.get("atten_start", 0.0)), 3.5))
	assert_eq(int(updated.get("colorgen_style", 0)), 24)
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
	assert_eq(int(reopened_info.get("colorgen_style", 0)), 24)
	assert_true(bool(reopened_info.get("disable_corona", false)))
	assert_true(bool(reopened_info.get("disable_lightterrain", false)))
	assert_true(bool(reopened_info.get("disable_lightobjects", false)))


func test_object_light_style_dropdown_names_every_style() -> void:
	# Every light's colorgen_style must resolve to a real name in the Style dropdown,
	# never the "Custom N" fallback (Armry01 uses style 55 = Set wave: smooth random).
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	var light_count := data.get_light_count()
	assert_gt(light_count, 0, "Fixture should expose object lights.")
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LIGHTS, list_mount)

	var list := _find_node_by_name(list_mount, "ObjectLightsList") as ItemList
	assert_not_null(list, "Light inspector should expose the light list.")
	if list == null:
		return
	var first_style := _find_node_by_name(detail_mount, "LightStyle") as OptionButton
	assert_not_null(first_style, "Light inspector should expose its consumer-specific style options.")
	if first_style != null:
		assert_eq(
				_option_text_by_id(first_style, 114),
				"Add (control register)",
				"Light style 114 should retain its witnessed CTRL dispatch.")
		assert_eq(
				_option_text_by_id(first_style, 115),
				"Wave: triangle",
				"Light style 115 should be labeled as its waveform fallback.")
		var phase := _find_node_by_name(detail_mount, "LightPhase") as SpinBox
		var control_reference := _find_node_by_name(
				detail_mount, "LightControlRegister") as OptionButton
		assert_not_null(phase)
		assert_not_null(control_reference)
		var wave_index := _option_index_by_id(first_style, 115)
		assert_gte(wave_index, 0)
		first_style.select(wave_index)
		first_style.item_selected.emit(wave_index)
		await get_tree().process_frame
		assert_false((phase.get_parent() as Control).visible,
				"Light style 115 phase comes from the loader-resolved reference.")
		assert_true((control_reference.get_parent() as Control).visible,
				"Light style 115 should expose its model-local CTRL reference.")
	for i in range(light_count):
		list.select(i)
		list.item_selected.emit(i)
		var style := _find_node_by_name(detail_mount, "LightStyle") as OptionButton
		assert_not_null(style, "Light %d should expose a Style dropdown." % i)
		if style == null:
			continue
		var label := style.get_item_text(style.selected) if style.selected >= 0 else ""
		var raw_style := int(data.get_light_info(i).get("colorgen_style", 0))
		assert_false(label.begins_with("Custom"), "Light %d style %d should have a real name, not '%s'." % [i, raw_style, label])


func test_object_lights_editor_uses_left_list_and_right_detail_dock() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(workspace.object_editor.object_data.get_light_count(), 0, "Fixture should expose object lights.")
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LIGHTS, list_mount)
	var list := _find_node_by_name(list_mount, "ObjectLightsList") as ItemList
	var detail := _find_node_by_name(detail_mount, "LightDetailPanel") as Control
	assert_not_null(list, "Light left pane should expose the light list.")
	assert_not_null(detail, "Light right dock should expose the selected light detail editor.")
	assert_null(_find_node_by_name(list_mount, "LightStartColor"), "Light color editing should move out of the left pane.")
	assert_null(_find_node_by_name(list_mount, "LightAttenuationStart"), "Light attenuation editing should move out of the left pane.")
	assert_not_null(_find_node_by_name(detail_mount, "LightStartColor"), "Light color editing should live in the right dock.")
	assert_not_null(_find_node_by_name(detail_mount, "LightAttenuationStart"), "Light attenuation editing should live in the right dock.")
	assert_not_null(_find_node_by_name(detail_mount, "LightPositiveFlags"), "Light output toggles should live in the right dock.")


func test_object_lights_inspector_uses_positive_oed_toggles() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	workspace.object_editor.mark_clean()
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LIGHTS, list_mount)
	var list := _find_node_by_name(list_mount, "ObjectLightsList") as ItemList
	var flags := _find_node_by_name(detail_mount, "LightPositiveFlags") as VBoxContainer
	var draw_corona := _find_node_by_name(detail_mount, "LightDrawCorona") as CheckBox
	var light_terrain := _find_node_by_name(detail_mount, "LightTerrain") as CheckBox
	var light_objects := _find_node_by_name(detail_mount, "LightObjects") as CheckBox
	assert_not_null(list)
	assert_not_null(flags, "Positive OED light toggles should be stacked for the side panel.")
	assert_not_null(draw_corona)
	assert_not_null(light_terrain)
	assert_not_null(light_objects)
	if list == null or draw_corona == null or light_terrain == null or light_objects == null:
		return

	assert_eq(draw_corona.text, "Draw corona")
	assert_eq(light_terrain.text, "Light terrain")
	assert_eq(light_objects.text, "Light objects")

	draw_corona.button_pressed = false
	draw_corona.toggled.emit(false)
	light_terrain.button_pressed = false
	light_terrain.toggled.emit(false)
	light_objects.button_pressed = false
	light_objects.toggled.emit(false)

	var disabled: Dictionary = data.get_light_info(0)
	assert_true(bool(disabled.get("disable_corona", false)))
	assert_true(bool(disabled.get("disable_lightterrain", false)))
	assert_true(bool(disabled.get("disable_lightobjects", false)))

	draw_corona.button_pressed = true
	draw_corona.toggled.emit(true)
	light_terrain.button_pressed = true
	light_terrain.toggled.emit(true)
	light_objects.button_pressed = true
	light_objects.toggled.emit(true)
	await get_tree().process_frame

	var enabled: Dictionary = data.get_light_info(0)
	assert_false(bool(enabled.get("disable_corona", true)))
	assert_false(bool(enabled.get("disable_lightterrain", true)))
	assert_false(bool(enabled.get("disable_lightobjects", true)))
	assert_true(workspace.object_editor.is_dirty, "Editing positive light toggles should mark the object dirty.")


func test_object_materials_inspector_populates_material_slots() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)

	assert_not_null(_find_node_by_name(list_mount, "MaterialsList"), "Materials inspector should expose the material list.")
	assert_not_null(_find_node_by_name(detail_mount, "ShaderTagOption"), "Materials inspector should expose shader tag selection.")
	var slot1_name := _find_node_by_name(detail_mount, "TextureSlot1Name") as LineEdit
	var slot1_status := _find_node_by_name(detail_mount, "TextureSlot1Status") as Label
	var slot2_name := _find_node_by_name(detail_mount, "TextureSlot2Name") as LineEdit
	var slot2_status := _find_node_by_name(detail_mount, "TextureSlot2Status") as Label
	assert_not_null(slot1_name)
	assert_not_null(slot1_status)
	assert_not_null(slot2_name)
	assert_not_null(slot2_status)
	assert_eq(slot1_name.text, "KArm1.tga", "Slot 1 should show the diffuse texture name from the 3DI.")
	assert_string_contains(slot1_status.text, "Resolved")
	assert_string_contains(slot1_status.text.to_lower(), "karm1_o.tga")
	assert_eq(slot2_name.text, "KRE_1_O.tga", "Slot 2 should show the detail texture name from the 3DI.")
	assert_string_contains(slot2_status.text, "Resolved")
	assert_not_null(_find_node_by_name(detail_mount, "TextureAnimationSection"), "Materials inspector should expose texture animation controls.")


func test_object_materials_editor_uses_left_list_and_right_detail_dock() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)
	var list := _find_node_by_name(list_mount, "MaterialsList") as ItemList
	var copy_button := _find_node_by_name(list_mount, "MaterialCopyButton") as Button
	var paste_button := _find_node_by_name(list_mount, "MaterialPasteButton") as Button
	var detail := _find_node_by_name(detail_mount, "MaterialDetailPanel") as Control
	assert_not_null(list, "Materials left pane should expose the material list.")
	assert_not_null(copy_button, "Materials left pane should expose Copy.")
	assert_not_null(paste_button, "Materials left pane should expose Paste.")
	assert_not_null(detail, "Materials right dock should expose the selected material detail editor.")
	assert_null(_find_node_by_name(list_mount, "ShaderTagPicker"), "Shader editing should move out of the left pane.")
	assert_null(_find_node_by_name(list_mount, "TextureSlot1Widget"), "Texture slot editing should move out of the left pane.")
	assert_not_null(_find_node_by_name(detail_mount, "ShaderTagPicker"), "Shader editing should live in the right dock.")
	assert_not_null(_find_node_by_name(detail_mount, "TextureSlot1Widget"), "Texture slot editing should live in the right dock.")
	assert_not_null(_find_node_by_name(detail_mount, "TextureAnimationSection"), "Texture animation controls should live in the right dock.")


func test_object_materials_inspector_edits_texture_slot_and_marks_dirty() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)
	var slot1_name := _find_node_by_name(detail_mount, "TextureSlot1Name") as LineEdit
	var slot1_status := _find_node_by_name(detail_mount, "TextureSlot1Status") as Label
	assert_not_null(slot1_name)
	assert_not_null(slot1_status)

	slot1_name.text = "KArm1_O.TGA"
	slot1_name.text_submitted.emit("KArm1_O.TGA")
	await get_tree().process_frame

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
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)

	var slot2_name := _find_node_by_name(detail_mount, "TextureSlot2Name") as LineEdit
	var slot2_status := _find_node_by_name(detail_mount, "TextureSlot2Status") as Label
	assert_not_null(slot2_name)
	assert_not_null(slot2_status)
	assert_false(slot2_name.editable, "Detail slot should be disabled when the shader does not support a secondary texture.")
	assert_string_contains(slot2_status.text, "Unsupported")


func test_object_materials_inspector_generator_rows_follow_style() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_eq(workspace.object_editor.object_data.set_material_shader(0, "FF_ST_OP#UV"), OK)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)
	var u_section := _find_node_by_name(detail_mount, "UGeneratorSection") as Control
	var u_style := _find_node_by_name(detail_mount, "UGeneratorStyleOption") as OptionButton
	var u_phase := _find_node_by_name(detail_mount, "UGeneratorPhase") as SpinBox
	var u_rate := _find_node_by_name(detail_mount, "UGeneratorRate") as SpinBox
	var u_reg := _find_node_by_name(detail_mount, "UGeneratorControlReg") as OptionButton
	var rgb_style := _find_node_by_name(detail_mount, "RgbGeneratorStyleOption") as OptionButton
	var rgb_phase := _find_node_by_name(detail_mount, "RgbGeneratorPhase") as SpinBox
	var rgb_reg := _find_node_by_name(detail_mount, "RgbGeneratorControlReg") as OptionButton
	var alpha_style := _find_node_by_name(detail_mount, "AlphaGeneratorStyleOption") as OptionButton
	var alpha_phase := _find_node_by_name(detail_mount, "AlphaGeneratorPhase") as SpinBox
	var alpha_reg := _find_node_by_name(detail_mount, "AlphaGeneratorControlReg") as OptionButton
	assert_not_null(u_section)
	assert_not_null(u_style)
	assert_not_null(u_phase)
	assert_not_null(u_rate)
	assert_not_null(u_reg)
	assert_not_null(rgb_style)
	assert_not_null(rgb_phase)
	assert_not_null(rgb_reg)
	assert_not_null(alpha_style)
	assert_not_null(alpha_phase)
	assert_not_null(alpha_reg)
	assert_true(u_section.visible, "UV generator controls should be visible for #UV shaders.")
	if u_style != null and rgb_style != null and alpha_style != null:
		assert_eq(
				_option_text_by_id(u_style, 115),
				"Skew (control register)",
				"UV style 115 should retain its controlled skew meaning.")
		assert_eq(
				_option_text_by_id(rgb_style, 115),
				"Wave: triangle",
				"RGB style 115 should be labeled as its waveform fallback.")
		assert_eq(
				_option_text_by_id(alpha_style, 114),
				"Wave: sine",
				"Alpha style 114 should be labeled as its waveform fallback.")

	# The raw numeric twins are internal and must not be user-facing.
	assert_null(_find_node_by_name(detail_mount, "UGeneratorStyle"), "Raw generator style number should be gone from the UI.")
	assert_null(_find_node_by_name(detail_mount, "UGeneratorReg"), "Raw control-register number should be gone from the UI.")

	# Style None -> the generator's parameter rows collapse away.
	var none_index := _option_index_by_id(u_style, 0)
	assert_true(none_index >= 0)
	u_style.select(none_index)
	u_style.item_selected.emit(none_index)
	await get_tree().process_frame
	assert_false((u_rate.get_parent() as Control).visible, "Param rows should hide when the generator style is None.")
	assert_false((u_reg.get_parent() as Control).visible, "Control reg row should hide when the generator style is None.")

	# A real style (Slide = 16) shows the params, but not the control-register row.
	var slide_index := _option_index_by_id(u_style, 16)
	assert_true(slide_index >= 0)
	u_style.select(slide_index)
	u_style.item_selected.emit(slide_index)
	await get_tree().process_frame
	assert_true((u_rate.get_parent() as Control).visible, "Param rows should show once a generator style is set.")
	assert_false((u_reg.get_parent() as Control).visible, "Control reg row should stay hidden for non-register styles.")
	assert_true(workspace.object_editor.is_dirty, "Editing a generator should mark the object dirty.")
	var material: Dictionary = workspace.object_editor.object_data.get_materials()[0]
	var u_params: Dictionary = material.get("u_params", {})
	assert_eq(int(u_params.get("style", 0)), 16)

	# A control-register style (113) reveals the control-register row.
	var reg_index := _option_index_by_id(u_style, 113)
	assert_true(reg_index >= 0)
	u_style.select(reg_index)
	u_style.item_selected.emit(reg_index)
	await get_tree().process_frame
	assert_true((u_reg.get_parent() as Control).visible, "Control reg row should appear for register-driven styles.")
	assert_false((u_phase.get_parent() as Control).visible,
			"The loader-reference byte replaces authored phase for UV style 113.")

	# RGB/alpha waveform fallbacks do not read the CTRL value, but their packed
	# parameter is still a model-local CTRL reference which the loader resolves
	# to the waveform phase ordinal.
	var rgb_wave_index := _option_index_by_id(rgb_style, 115)
	assert_gte(rgb_wave_index, 0)
	rgb_style.select(rgb_wave_index)
	rgb_style.item_selected.emit(rgb_wave_index)
	await get_tree().process_frame
	assert_true((rgb_reg.get_parent() as Control).visible,
			"RGB style 115 should author the loader's CTRL reference.")
	assert_false((rgb_phase.get_parent() as Control).visible,
			"RGB style 115 should not expose the phase value ignored by the loader.")

	var alpha_wave_index := _option_index_by_id(alpha_style, 114)
	assert_gte(alpha_wave_index, 0)
	alpha_style.select(alpha_wave_index)
	alpha_style.item_selected.emit(alpha_wave_index)
	await get_tree().process_frame
	assert_true((alpha_reg.get_parent() as Control).visible,
			"Alpha style 114 should author the loader's CTRL reference.")
	assert_false((alpha_phase.get_parent() as Control).visible,
			"Alpha style 114 should not expose the phase value ignored by the loader.")


func test_object_materials_inspector_uses_compact_layout_and_named_generator_controls() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_eq(workspace.object_editor.object_data.set_material_shader(0, "FF_ST_OP#UV"), OK)
	workspace.object_editor.mark_clean()
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)
	var list_panel := _find_node_by_name(list_mount, "MaterialListPanel") as Control
	var detail := _find_node_by_name(detail_mount, "MaterialDetailPanel") as Control
	var shader_picker := _find_node_by_name(detail_mount, "ShaderTagPicker") as OptionButton
	var slot1_clear := _find_node_by_name(detail_mount, "TextureSlot1Clear") as Button
	var slot1_options := _find_node_by_name(detail_mount, "TextureSlot1OptionsRow") as Control
	var u_style := _find_node_by_name(detail_mount, "UGeneratorStyleOption") as OptionButton
	var u_reg := _find_node_by_name(detail_mount, "UGeneratorControlReg") as OptionButton
	assert_null(_find_node_by_name(list_mount, "MaterialInspectorSplit"), "Materials inspector should not use a horizontal split inside the side panel.")
	assert_null(_find_node_by_name(detail_mount, "MaterialDetailScroll"), "Materials inspector should rely on the side panel's existing scroll area.")
	assert_not_null(list_panel, "Materials inspector should expose a compact list panel.")
	assert_not_null(detail, "Materials inspector should expose a stable detail panel.")
	assert_null(_find_node_by_name(list_mount, "ShaderTagPicker"), "Shader editing should not live in the left list pane.")
	assert_null(_find_node_by_name(list_mount, "TextureSlot1Widget"), "Texture slots should not live in the left list pane.")
	assert_not_null(shader_picker, "Materials inspector should use the shader tag picker.")
	assert_not_null(slot1_clear, "Texture slot controls should expose a clear button.")
	assert_not_null(slot1_options, "Texture slot options should be on a second compact row.")
	assert_not_null(u_style, "UV generator style should be a named option control.")
	assert_not_null(u_reg, "UV generator register should be a named option control.")
	if list_panel != null:
		assert_eq(list_panel.custom_minimum_size.x, 0.0, "Material list panel should not force a wide side panel.")
	if u_style == null:
		return

	var slide_index := _option_index_by_id(u_style, 16)
	assert_true(slide_index >= 0, "Generator style picker should expose OED style 16 as a named option.")
	u_style.select(slide_index)
	u_style.item_selected.emit(slide_index)
	await get_tree().process_frame

	var material: Dictionary = workspace.object_editor.object_data.get_materials()[0]
	var u_params: Dictionary = material.get("u_params", {})
	assert_eq(int(u_params.get("style", 0)), 16)
	assert_true(workspace.object_editor.is_dirty, "Editing a named generator option should mark the object dirty.")


func test_object_materials_dock_survives_editor_state_sync_without_rebuild() -> void:
	# Dragging time-of-day fans out an editor-state sync to the active workspace's
	# asset dock. That transient sync must not tear down and rebuild the heavy
	# Materials detail dock (the source of the TOD lag).
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)

	var detail := _find_node_by_name(detail_mount, "MaterialDetailPanel")
	assert_not_null(detail, "Materials dock should build the detail panel.")
	if detail == null:
		return
	var id := detail.get_instance_id()

	# The workstation re-calls BOTH set_asset_dock (same mount) and sync_asset_dock
	# on every editor-state sync (e.g. each time-of-day drag step). Neither may
	# rebuild the dock while the same object is open.
	workspace.set_asset_dock(detail_mount)
	workspace.sync_asset_dock()
	workspace.set_asset_dock(detail_mount)

	var after := _find_node_by_name(detail_mount, "MaterialDetailPanel")
	assert_not_null(after, "Materials detail dock should still exist after an editor-state sync.")
	if after == null:
		return
	assert_eq(after.get_instance_id(), id, "Editor-state syncs (time-of-day drags) must not rebuild the Materials detail dock.")


func test_object_materials_detail_rebuilds_when_object_changes() -> void:
	# Opening/creating a different object must rebuild the detail so it stops
	# showing the previous object's data (the re-mount no-rebuild guard must key on
	# the object identity, not blindly skip).
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)

	var panel_before := _find_node_by_name(detail_mount, "MaterialDetailPanel")
	assert_not_null(panel_before, "Materials dock should build the detail panel.")
	if panel_before == null:
		return
	var id_before := panel_before.get_instance_id()

	workspace.new_current()
	workspace.sync_asset_dock()

	var panel_after := _find_node_by_name(detail_mount, "MaterialDetailPanel")
	assert_not_null(panel_after, "Materials dock should still expose a detail panel for the new object.")
	if panel_after == null:
		return
	assert_ne(panel_after.get_instance_id(), id_before, "Switching objects should rebuild the detail dock, not keep the previous object's controls.")


func test_object_materials_selection_resyncs_without_rebuilding_detail_nodes() -> void:
	# Selecting a different material must re-sync the existing detail controls in
	# place, not free and rebuild the whole dock (the source of the selection lag).
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(workspace.object_editor.object_data.get_material_count(), 1, "Selection test needs at least two materials.")
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)

	var list := _find_node_by_name(list_mount, "MaterialsList") as ItemList
	var picker_before := _find_node_by_name(detail_mount, "ShaderTagPicker")
	assert_not_null(list, "Materials left pane should expose the material list.")
	assert_not_null(picker_before, "Materials dock should build the shader tag picker.")
	if list == null or picker_before == null:
		return

	list.select(1)
	list.item_selected.emit(1)
	await get_tree().process_frame

	assert_true(is_instance_valid(picker_before), "Selecting a material should not free the existing detail controls.")
	var picker_after := _find_node_by_name(detail_mount, "ShaderTagPicker")
	assert_eq(picker_after, picker_before, "Selecting a material should re-sync existing controls, not rebuild the detail dock.")


func test_object_material_rgb_gen_colors_are_opaque_rgb_only() -> void:
	# RGB-gen start/end colors are RGB-only; the .3di leaves their alpha byte 0, so the
	# pickers must force opaque display (like lights) and disable alpha editing, else the
	# swatches render transparent/checkerboarded and look wrong.
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(workspace.object_editor.object_data.get_material_count(), 0, "Fixture should expose materials.")
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)

	var start_color := _find_node_by_name(detail_mount, "RgbGeneratorStartColor") as ColorPickerButton
	var end_color := _find_node_by_name(detail_mount, "RgbGeneratorEndColor") as ColorPickerButton
	assert_not_null(start_color, "Materials dock should expose the RGB-gen start color picker.")
	assert_not_null(end_color, "Materials dock should expose the RGB-gen end color picker.")
	if start_color == null or end_color == null:
		return
	assert_eq(start_color.color.a, 1.0, "RGB-gen start color should display opaque, not transparent.")
	assert_eq(end_color.color.a, 1.0, "RGB-gen end color should display opaque, not transparent.")
	assert_false(start_color.edit_alpha, "RGB-gen start color is RGB-only; alpha editing should be off.")
	assert_false(end_color.edit_alpha, "RGB-gen end color is RGB-only; alpha editing should be off.")


func test_object_materials_inspector_copies_and_pastes_settings() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	assert_gt(workspace.object_editor.object_data.get_material_count(), 1, "Copy/paste test needs at least two materials.")
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.MATERIALS, list_mount)

	var list := _find_node_by_name(list_mount, "MaterialsList") as ItemList
	var copy_button := _find_node_by_name(list_mount, "MaterialCopyButton") as Button
	var paste_button := _find_node_by_name(list_mount, "MaterialPasteButton") as Button
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


func test_object_part_anim_detail_dock_edits_semantic_channels() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var data: NovaObjectData = workspace.object_editor.object_data
	assert_gt(data.get_part_anim_count(0), 0, "Fixture should expose part animations.")
	workspace.object_editor.mark_clean()
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())

	workspace.set_asset_dock(detail_mount)
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, list_mount)
	var list := _find_node_by_name(list_mount, "PartAnimList") as ItemList
	var rotation_enabled := _find_node_by_name(detail_mount, "PartAnimRotationEnabled") as CheckBox
	var rotation_mode := _find_node_by_name(detail_mount, "PartAnimRotationMode") as OptionButton
	var rotation_x_to := _find_node_by_name(detail_mount, "PartAnimRotationXTo") as SpinBox
	var rotation_speed := _find_node_by_name(detail_mount, "PartAnimRotationSpeed") as SpinBox
	var scale_enabled := _find_node_by_name(detail_mount, "PartAnimScaleEnabled") as CheckBox
	var scale_mode := _find_node_by_name(detail_mount, "PartAnimScaleMode") as OptionButton
	var translation_enabled := _find_node_by_name(detail_mount, "PartAnimTranslationEnabled") as CheckBox
	var translation_axis := _find_node_by_name(detail_mount, "PartAnimTranslationAxis") as OptionButton
	assert_not_null(list)
	assert_not_null(rotation_enabled)
	assert_not_null(rotation_mode)
	assert_not_null(rotation_x_to)
	assert_not_null(rotation_speed)
	assert_not_null(scale_enabled)
	assert_not_null(scale_mode)
	assert_not_null(translation_enabled)
	assert_not_null(translation_axis)
	assert_null(_find_node_by_name(detail_mount, "PartAnimRotationType"), "Semantic PANM UI should not expose raw rotation type values.")
	assert_null(_find_node_by_name(detail_mount, "PartAnimTranslateType"), "Semantic PANM UI should not expose raw translation type values.")
	if list == null or rotation_enabled == null or rotation_mode == null or rotation_x_to == null or rotation_speed == null or scale_enabled == null or scale_mode == null or translation_enabled == null or translation_axis == null:
		return

	list.select(0)
	list.item_selected.emit(0)
	rotation_enabled.button_pressed = true
	rotation_enabled.toggled.emit(true)
	var slide_index := _option_index_by_id(rotation_mode, 16)
	assert_true(slide_index >= 0, "Rotation driver should expose Slide.")
	rotation_mode.select(slide_index)
	rotation_mode.item_selected.emit(slide_index)
	rotation_x_to.value = 90.0
	rotation_x_to.value_changed.emit(90.0)
	rotation_speed.value = 1.0
	rotation_speed.value_changed.emit(1.0)

	scale_enabled.button_pressed = true
	scale_enabled.toggled.emit(true)
	var per_axis_index := _option_index_by_id(scale_mode, 2)
	assert_true(per_axis_index >= 0, "Scale style should expose per-axis.")
	scale_mode.select(per_axis_index)
	scale_mode.item_selected.emit(per_axis_index)

	translation_enabled.button_pressed = true
	translation_enabled.toggled.emit(true)
	var z_index := _option_index_by_id(translation_axis, 3)
	assert_true(z_index >= 0, "Translation direction should expose Z.")
	translation_axis.select(z_index)
	translation_axis.item_selected.emit(z_index)
	await get_tree().process_frame

	var updated: Dictionary = data.get_part_anim_editor_entries(0)[0]
	var rotation: Dictionary = updated.get("rotation", {})
	var rotation_x: Dictionary = rotation.get("x", {})
	var scale: Dictionary = updated.get("scale", {})
	var translation: Dictionary = updated.get("translation", {})
	assert_true(bool(rotation.get("enabled", false)))
	assert_eq(String(rotation_x.get("mode", "")), "slide")
	assert_almost_eq(float(rotation_x.get("to_value", 0.0)), 90.0, 0.01)
	assert_almost_eq(float(rotation_x.get("speed", 0.0)), 1.0, 0.001)
	assert_true(bool(scale.get("enabled", false)))
	assert_eq(String(scale.get("style", "")), "per_axis")
	assert_true(bool(translation.get("enabled", false)))
	assert_eq(String(translation.get("axis", "")), "z")
	assert_true(workspace.object_editor.is_dirty, "Editing semantic part animation controls should mark the object dirty.")


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


func _aabb_is_finite(bounds: AABB) -> bool:
	return (
		is_finite(bounds.position.x)
		and is_finite(bounds.position.y)
		and is_finite(bounds.position.z)
		and is_finite(bounds.size.x)
		and is_finite(bounds.size.y)
		and is_finite(bounds.size.z)
	)


func _render_node_instance_ids(model) -> Dictionary:
	var ids := {}
	if model == null:
		return ids
	var nodes: Dictionary = model.get_render_part_nodes()
	for key in nodes.keys():
		var node := nodes[key] as Object
		ids[key] = node.get_instance_id() if node != null else 0
	return ids


func _assert_vector3_close(actual: Vector3, expected: Vector3, epsilon: float, message: String) -> void:
	assert_true(actual.distance_to(expected) <= epsilon, "%s expected %s got %s" % [message, str(expected), str(actual)])


func _find_node_by_type(root: Node, type_name: String) -> Node:
	if root.get_class() == type_name:
		return root
	for child in root.get_children():
		var found := _find_node_by_type(child, type_name)
		if found != null:
			return found
	return null


func _find_textured_preview_material_entry(preview) -> Dictionary:
	var model = preview.call("get_object_model") if preview != null and preview.has_method("get_object_model") else null
	var materials: Array = model.get_surface_materials() if model != null else preview._surface_materials
	var material_indices: PackedInt32Array = model.get_surface_material_indices() if model != null else preview._surface_material_indices
	for i in range(materials.size()):
		var material := materials[i] as ShaderMaterial
		if material != null and material.get_shader_parameter("u_diffuse") is Texture2D:
			return {
				"material": material,
				"material_index": int(material_indices[i]),
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


func _option_index_by_id(option: OptionButton, item_id: int) -> int:
	if option == null:
		return -1
	for i in range(option.get_item_count()):
		if option.get_item_id(i) == item_id:
			return i
	return -1


func _option_text_by_id(option: OptionButton, item_id: int) -> String:
	var index := _option_index_by_id(option, item_id)
	return option.get_item_text(index) if index >= 0 else ""


func _field_label_for_control(control: Control) -> Label:
	if control == null or control.get_parent() == null:
		return null
	for sibling in control.get_parent().get_children():
		if sibling is Label:
			return sibling as Label
	return null


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


func _oed_dirty_mask(data: NovaObjectData) -> int:
	if data == null or not data.has_method("get_oed_dirty_mask"):
		return -1
	return int(data.call("get_oed_dirty_mask"))


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
	# Unique per call so the texture resolver's per-directory cache is never reused
	# across tests that recreate a same-named dir with different contents.
	var fixture_dir := _output_dir().path_join("texture_variant_%s_%d" % [texture_filename.replace(".", "_"), Time.get_ticks_usec()])
	assert_eq(DirAccess.make_dir_recursive_absolute(fixture_dir), OK)
	_copy_file(ProjectSettings.globalize_path(ARMRY_FIXTURE), fixture_dir.path_join("Armry01.3di"))
	_copy_file(ProjectSettings.globalize_path(ARMRY_TEXTURE_FIXTURE), fixture_dir.path_join(texture_filename))
	return fixture_dir


func _prepare_real_dds_texture_fixture(texture_filename: String) -> String:
	# Unique per call (see _prepare_texture_variant_fixture): isolates the resolver's
	# per-directory cache between tests that reuse a same-named fixture dir.
	var fixture_dir := _output_dir().path_join("real_dds_%s_%d" % [texture_filename.replace(".", "_"), Time.get_ticks_usec()])
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


# --- Phase 1: responsive list + scroll floors ---

func test_object_lists_use_compact_list_floor() -> void:
	var workspace = ObjectWorkspaceScript.new()
	workspace.set_editor_shell(self)
	assert_eq(workspace.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)
	var list_mount = add_child_autofree(Control.new())
	var detail_mount = add_child_autofree(Control.new())
	workspace.set_asset_dock(detail_mount)

	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.LIGHTS, list_mount)
	var lights_list := _find_node_by_name(list_mount, "ObjectLightsList") as ItemList
	assert_not_null(lights_list, "Lights inspector should expose its list.")
	if lights_list != null:
		assert_eq(lights_list.custom_minimum_size.y, 200.0, "Object lists should use a compact 200px floor so short windows aren't dominated by the list.")

	var parts_mount = add_child_autofree(Control.new())
	workspace.build_workflow_inspector(ObjectEditorWorkspace.Workflow.PARTS, parts_mount)
	var part_list := _find_node_by_name(parts_mount, "PartAnimList") as ItemList
	assert_not_null(part_list, "Part anims inspector should expose its list.")
	if part_list != null:
		assert_eq(part_list.custom_minimum_size.y, 200.0, "Part anim list should use the compact 200px floor.")


func test_inspector_box_disables_horizontal_scroll() -> void:
	var mount = add_child_autofree(Control.new())
	var box = InspectorForms.make_inspector_box(mount)
	var scroll := box.get_parent() as ScrollContainer
	assert_not_null(scroll, "make_inspector_box should wrap content in a ScrollContainer.")
	if scroll != null:
		assert_eq(scroll.horizontal_scroll_mode, ScrollContainer.SCROLL_MODE_DISABLED, "Inspector content should reflow vertically, never scroll horizontally.")


# --- B3: native edit-state snapshots ------------------------------------------

func test_edit_state_snapshot_byte_roundtrip() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(BIRD_FIXTURE)), OK)
	var baseline: PackedByteArray = data.snapshot_edit_state()
	assert_gt(baseline.size(), 0, "A loaded document snapshots to a non-empty blob.")

	var original_alpha := int(data.get_material_info(0).get("alpha_test", -1))
	assert_eq(data.set_material_alpha_threshold(0, 0.73), OK)
	assert_ne(int(data.get_material_info(0).get("alpha_test", -1)), original_alpha,
		"The mutation should be observable before the restore.")
	var added := data.add_part_anim(0, 0)
	assert_gte(added, 0, "add_part_anim should append an animation.")
	var mutated_count := data.get_part_anim_count(0)

	assert_eq(data.apply_edit_state(baseline), OK, "Applying the baseline restores it.")
	assert_eq(int(data.get_material_info(0).get("alpha_test", -1)), original_alpha,
		"Material scalar restored.")
	assert_eq(data.get_part_anim_count(0), mutated_count - 1,
		"Part-anim realloc restored the original count.")
	assert_eq(data.snapshot_edit_state(), baseline,
		"Snapshot after apply is byte-identical (exact roundtrip).")
	assert_eq(data.get_last_oed_update_mask(), OED_UPDATE_ALL,
		"apply_edit_state notifies consumers with UPDATE_ALL.")


func test_edit_state_rejects_mismatched_geometry() -> void:
	var bird := NovaObjectData.new()
	assert_eq(bird.open_file(ProjectSettings.globalize_path(BIRD_FIXTURE)), OK)
	var armry := NovaObjectData.new()
	assert_eq(armry.open_file(ProjectSettings.globalize_path(ARMRY_FIXTURE)), OK)

	var bird_snapshot: PackedByteArray = bird.snapshot_edit_state()
	var armry_before: PackedByteArray = armry.snapshot_edit_state()
	assert_eq(armry.apply_edit_state(bird_snapshot), ERR_INVALID_DATA,
		"A blob from a different model must be rejected (count validation).")
	assert_eq(armry.snapshot_edit_state(), armry_before,
		"A rejected apply must leave the document untouched.")
	assert_eq(armry.apply_edit_state(PackedByteArray()), ERR_INVALID_DATA,
		"An empty blob is invalid data.")


func test_edit_state_without_document_is_inert() -> void:
	var data := NovaObjectData.new()
	assert_eq(data.snapshot_edit_state().size(), 0,
		"No document -> empty snapshot (the undo session stays inert).")
	assert_eq(data.apply_edit_state(PackedByteArray()), ERR_UNCONFIGURED,
		"No document -> apply is unconfigured.")
