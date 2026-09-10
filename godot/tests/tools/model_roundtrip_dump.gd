extends SceneTree
## Headless helper for the model tool: project every synthetic model and write
## the re-exported bytes beside a copy of the fixture, so a byte differ can name
## the chunk that drifts. Usage:
##   godot --headless --path godot -s res://tests/tools/model_roundtrip_dump.gd -- <out_dir>

const SYNTH := "res://../fixtures/threedi/synth"


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	var out_dir := args[0] if args.size() > 0 else OS.get_cache_dir().path_join("model_roundtrip")
	DirAccess.make_dir_recursive_absolute(out_dir)
	for name in DirAccess.get_files_at(ProjectSettings.globalize_path(SYNTH)):
		if not name.ends_with(".3di"):
			continue
		var document := ModelDocument.new()
		if document.load_from_path(SYNTH.path_join(name)) != OK:
			print("%s: cannot parse" % name)
			continue
		var manifest := ModelAuthoringManifest.new()
		var projector := ModelSceneProjector.new()
		var root := projector.project(document, manifest, "")
		if root == null:
			print("%s: refused: %s" % [name, projector.get_last_error()])
			continue
		var exporter := ModelSceneExporter.new()
		var exported := exporter.export_scene(root, manifest)
		root.free()
		if exported == null:
			print("%s: export failed: %s" % [name, exporter.get_last_error()])
			continue
		var err := exported.save_to_path(out_dir.path_join(name))
		print("%s: wrote (%s)" % [name, error_string(err)])
	quit(0)
