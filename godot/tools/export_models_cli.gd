@tool
class_name ExportModelsCli
extends RefCounted
## Headless project command that rebuilds the authored model artifacts:
##   godot --headless --path godot --script res://tools/export_models.gd -- --export-models [name...]
## With no names every manifest under res://authoring is exported; a name is
## the authoring folder (`crate` -> res://authoring/crate/crate.tres). Add
## --verify to compare against the files on disk instead of writing.

const FLAG := "--export-models"
const VERIFY := "--verify"


static func run(user_args: PackedStringArray) -> int:
	var idx := user_args.find(FLAG)
	if idx < 0:
		printerr("usage: --export-models [--verify] [name...]")
		return 1
	var verify := user_args.has(VERIFY)
	var names := PackedStringArray()
	for i in range(idx + 1, user_args.size()):
		if user_args[i] != VERIFY:
			names.append(user_args[i])
	var manifests := PackedStringArray()
	if names.is_empty():
		manifests = ModelExport.list_manifests()
	else:
		for name in names:
			manifests.append(ModelExport.AUTHORING_DIR.path_join(name).path_join(name + ".tres"))
	if manifests.is_empty():
		printerr("export-models: no manifests under %s" % ModelExport.AUTHORING_DIR)
		return 1
	var failures := 0
	for manifest in manifests:
		var result := ModelExport.verify_manifest(manifest) if verify else ModelExport.export_manifest(manifest)
		if result.ok:
			print("export-models: %s -> %s (%s)" % [manifest, ", ".join(result.artifacts),
					"verified" if verify else "written to " + result.output_directory])
		else:
			printerr("export-models: %s: %s" % [manifest, result.error])
			failures += 1
	return 1 if failures > 0 else 0
