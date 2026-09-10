extends GutTest

# The byte guard over the authored models (the Godot analogue of the
# minimal_3di_gen ctest): every manifest under godot/authoring/ re-exports in
# memory and its artifacts in assets/ must equal that export byte for byte.
# Rebuild them with "OpenNova: Export all authored models" or the headless
# --export-models command when a source changes.


func test_every_authoring_manifest_matches_its_tracked_artifacts() -> void:
	var manifests := ModelExport.list_manifests()
	assert_true(manifests.size() >= 1, "the authoring tree carries at least the crate")
	for manifest in manifests:
		var result := ModelExport.verify_manifest(manifest)
		if not result.ok and _only_unpulled(result.mismatches):
			pass_test("%s: artifacts are unpulled LFS pointers; skipped" % manifest)
			continue
		assert_true(result.ok, "%s: %s" % [manifest, result.error])
		assert_true(result.artifacts.size() >= 1, "%s names its artifacts" % manifest)
		assert_true(result.artifacts[0].ends_with(".3di"), "%s: the model comes first" % manifest)


func test_the_crate_manifest_is_the_documented_shape() -> void:
	var manifest := load("res://authoring/crate/crate.tres") as ModelAuthoringManifest
	assert_not_null(manifest)
	if manifest == null:
		return
	assert_eq(manifest.model_name, "crate")
	assert_false(manifest.skinned)
	assert_eq(manifest.output_directory, "res://../assets")
	assert_eq(manifest.materials.size(), 1)
	var material := manifest.materials[0] as ModelMaterialSpec
	assert_eq(material.material_name, "crate", "keyed by the glTF material name")
	assert_eq(material.shader_tag, "FF_ST_OP", "served by the authored _ffp.fx")
	assert_eq(material.get_diffuse_texture(), "crate.tga")
	assert_eq(manifest.texture_sources.size(), 1)
	assert_eq((manifest.texture_sources[0] as ModelTextureSource).output_name, "crate.tga")
	assert_not_null(manifest.scene, "the inherited scene over crate.glb")


func _only_unpulled(mismatches: PackedStringArray) -> bool:
	if mismatches.is_empty():
		return false
	for line in mismatches:
		if not line.ends_with("unpulled LFS pointer"):
			return false
	return true


func test_duplicate_texture_outputs_are_rejected_before_export() -> void:
	var manifest := (load("res://authoring/crate/crate.tres") as ModelAuthoringManifest).duplicate(true) as ModelAuthoringManifest
	var sources := manifest.texture_sources
	sources.append(sources[0])
	manifest.texture_sources = sources
	var path := "user://duplicate_model_outputs_%d.tres" % Time.get_ticks_usec()
	assert_eq(ResourceSaver.save(manifest, path), OK)
	var bytes: Array[PackedByteArray] = []
	var result := ModelExport.build(path, bytes)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	assert_false(result.ok, "two texture rows must not silently overwrite the same output")
	assert_string_contains(result.error.to_lower(), "collid")
