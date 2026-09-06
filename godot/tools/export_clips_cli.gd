@tool
class_name ExportClipsCli
extends RefCounted
## Headless project command that rebuilds the authored clip artifacts:
##   godot --headless --path godot --script res://tools/export_clips.gd -- --export-clips [name...]
## With no names every ClipSetSource under res://authoring is exported; a name
## is `<folder>/<file>` without the extension (`person/person_clips`). Add
## --verify to compare against the files on disk instead of writing.

const FLAG := "--export-clips"
const VERIFY := "--verify"


static func run(user_args: PackedStringArray) -> int:
	var idx := user_args.find(FLAG)
	if idx < 0:
		printerr("usage: --export-clips [--verify] [folder/file...]")
		return 1
	var verify := user_args.has(VERIFY)
	var names := PackedStringArray()
	for i in range(idx + 1, user_args.size()):
		if user_args[i] != VERIFY:
			names.append(user_args[i])
	var sources := PackedStringArray()
	if names.is_empty():
		sources = ClipExport.list_sources()
	else:
		for name in names:
			sources.append(ClipExport.AUTHORING_DIR.path_join(name + ".tres"))
	if sources.is_empty():
		printerr("export-clips: no clip sets under %s" % ClipExport.AUTHORING_DIR)
		return 1
	var failures := 0
	for source in sources:
		var result := ClipExport.verify_source(source) if verify else ClipExport.export_source(source)
		if result.ok:
			print("export-clips: %s -> %s (%s)" % [source, ", ".join(result.artifacts),
					"verified" if verify else "written to " + result.output_directory])
		else:
			printerr("export-clips: %s: %s" % [source, result.error])
			failures += 1
	return 1 if failures > 0 else 0
