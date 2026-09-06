extends GutTest

# The lossless-representation proof for the model tool: every minted
# synthetic model in scope (fixtures/threedi/synth, fixtures/README.md)
# projects into a scene and exports back to the SAME BYTES through the parity
# writer, so every construct in scope has a scene home. The armory family
# carries occlusion records, which have no scene form yet, and is refused by
# name rather than exported with the records dropped (ADR 0003).

const SYNTH := "res://../fixtures/threedi/synth"
const LFS_POINTER := "version https://git-lfs"


func _fixture_names() -> PackedStringArray:
	var names := PackedStringArray()
	for file in DirAccess.get_files_at(ProjectSettings.globalize_path(SYNTH)):
		if file.ends_with(".3di"):
			names.append(file)
	names.sort()
	return names


func _is_pointer(bytes: PackedByteArray) -> bool:
	return bytes.size() >= LFS_POINTER.length() and bytes.slice(0, LFS_POINTER.length()).get_string_from_ascii() == LFS_POINTER


func test_every_synth_model_in_scope_round_trips_byte_for_byte() -> void:
	var names := _fixture_names()
	assert_true(names.size() >= 11, "the synthetic set is present")
	var checked := 0
	var refused := 0
	for name in names:
		var path := SYNTH.path_join(name)
		var original := FileAccess.get_file_as_bytes(path)
		if _is_pointer(original):
			pass_test("%s is an unpulled LFS pointer; skipped" % name)
			continue
		var document := ModelDocument.new()
		assert_eq(document.load_from_path(path), OK, "%s parses" % name)
		var manifest := ModelAuthoringManifest.new()
		var projector := ModelSceneProjector.new()
		var root := projector.project(document, manifest, "")
		if name.begins_with("armory"):
			assert_null(root, "%s carries occlusion records and is refused" % name)
			var named := false
			for refusal in projector.get_refusals():
				if refusal.begins_with("occlusion records"):
					named = true
			assert_true(named, "%s: the refusal names OCCL (%s)" % [name, projector.get_refusals()])
			if root != null:
				root.free()
			refused += 1
			continue
		assert_not_null(root, "%s projects: %s" % [name, projector.get_last_error()])
		if root == null:
			continue
		var exporter := ModelSceneExporter.new()
		var exported := exporter.export_scene(root, manifest)
		assert_not_null(exported, "%s exports: %s" % [name, exporter.get_last_error()])
		if exported != null:
			var bytes := exported.to_bytes()
			assert_eq(bytes.size(), original.size(), "%s: the exported size" % name)
			assert_true(bytes == original, "%s: the exported bytes equal the fixture" % name)
			checked += 1
		root.free()
	assert_true(checked >= 8, "round-tripped %d models" % checked)
	assert_true(refused >= 1, "refused the armory family (%d)" % refused)
