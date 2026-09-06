@tool
class_name ClipExport
extends RefCounted
## The clip export workflow (ADR 0045: workflows live under godot/tools): a
## ClipSetSource names a rig scene, the Animations to sample, the clip names
## they export as and the .adm rows over those names; exporting projects
## every clip through the native ClipProjector, writes the .adm through its
## document, and installs the artifacts into the source's output directory
## through one staged transaction. Verifying does the same in memory and
## compares against what is on disk (the GUT byte guard).

const AUTHORING_DIR := ModelExport.AUTHORING_DIR
const MAX_CLIP_STEM := 12


## Every clip set under the authoring tree (any `.tres` that is a ClipSetSource).
static func list_sources(authoring_dir: String = AUTHORING_DIR) -> PackedStringArray:
	var sources := PackedStringArray()
	var root := ProjectSettings.globalize_path(authoring_dir)
	if not DirAccess.dir_exists_absolute(root):
		return sources
	for folder in DirAccess.get_directories_at(root):
		for file in DirAccess.get_files_at(root.path_join(folder)):
			if not file.ends_with(".tres"):
				continue
			var path := authoring_dir.path_join(folder).path_join(file)
			if load(path) is ClipSetSource:
				sources.append(path)
	sources.sort()
	return sources


## Project every clip and the .adm in memory. On success the result lists the
## artifact names and `bytes` (parallel to `artifacts`) holds each artifact.
static func build(source_path: String, bytes: Array[PackedByteArray]) -> ClipExportResult:
	bytes.clear()
	var source := load(source_path) as ClipSetSource
	if source == null:
		return ClipExportResult.failure(source_path, "not a ClipSetSource")
	var result := ClipExportResult.new()
	result.source_path = source_path
	result.adm_name = source.adm_name
	result.output_directory = source.output_directory
	if source.adm_name.is_empty():
		result.error = "the clip set has no adm_name"
		return result
	if source.scene == null:
		result.error = "the clip set names no scene"
		return result
	if source.clips.is_empty():
		result.error = "the clip set has no clips"
		return result
	var root := source.scene.instantiate() as Node3D
	if root == null:
		result.error = "the clip set's scene root is not a Node3D"
		return result
	var player := _find_player(root)
	var projector := ClipProjector.new()
	var names: Dictionary = {}
	for entry in source.clips:
		var spec := entry as ClipSpec
		if spec == null:
			root.free()
			result.error = "a clip row is empty"
			return result
		var stem := spec.clip_name
		if stem.is_empty() or stem.length() > MAX_CLIP_STEM or stem.contains(" ") or stem.contains("."):
			root.free()
			result.error = "clip '%s': the name is 1 to %d characters, no spaces or dots" % [stem, MAX_CLIP_STEM]
			return result
		if names.has(stem.to_lower()):
			root.free()
			result.error = "clip '%s' is named twice" % stem
			return result
		names[stem.to_lower()] = true
		var animation: Animation = null
		if not spec.animation.is_empty():
			if player == null or not player.has_animation(spec.animation):
				root.free()
				result.error = "clip '%s': the scene has no Animation named '%s'" % [stem, spec.animation]
				return result
			animation = player.get_animation(spec.animation)
		var document := projector.project(root, animation, spec, source.fps, source.ground_bone)
		if document == null:
			root.free()
			result.error = "clip '%s': %s" % [stem, projector.get_last_error()]
			return result
		var clip_bytes := document.to_bytes()
		if clip_bytes.is_empty():
			root.free()
			result.error = "clip '%s': %s" % [stem, document.get_last_error()]
			return result
		result.artifacts.append(stem + ".bad")
		bytes.append(clip_bytes)
	root.free()
	# The .adm: anim_reset first (the rig's skeleton source), every variant a clip of this set.
	var table := AnimDefDocument.new()
	if source.rows.is_empty():
		result.error = "the clip set has no .adm rows"
		return result
	for i in source.rows.size():
		var row := source.rows[i] as AnimSetRow
		if row == null:
			result.error = "an .adm row is empty"
			return result
		if i == 0 and row.key != "anim_reset":
			result.error = "the first .adm row must be anim_reset (the rig's skeleton source)"
			return result
		for variant in row.variants:
			if not names.has(variant.to_lower()):
				result.error = "row '%s' names '%s', which is not a clip of this set" % [row.key, variant]
				return result
		if not table.add_row(row.key, row.variants):
			result.error = table.get_last_error()
			return result
	var adm_bytes := table.to_bytes()
	if adm_bytes.is_empty():
		result.error = table.get_last_error()
		return result
	result.artifacts.append(source.adm_name + ".adm")
	bytes.append(adm_bytes)
	result.ok = true
	return result


## Export and install the set's artifacts into its output directory
## (replacing existing files atomically).
static func export_source(source_path: String) -> ClipExportResult:
	var bytes: Array[PackedByteArray] = []
	var result := build(source_path, bytes)
	if not result.ok:
		return result
	var directory := ProjectSettings.globalize_path(result.output_directory)
	if not DirAccess.dir_exists_absolute(directory):
		result.ok = false
		result.error = "the output directory does not exist: %s" % directory
		return result
	var writers: Dictionary = {}
	for i in result.artifacts.size():
		var payload: PackedByteArray = bytes[i]
		writers[result.artifacts[i]] = func(path: String) -> Error:
			var file := FileAccess.open(path, FileAccess.WRITE)
			if file == null:
				return FileAccess.get_open_error()
			file.store_buffer(payload)
			file.close()
			return OK
	var reason := FileTransaction.write(directory, writers, func() -> String: return "", true)
	if not reason.is_empty():
		result.ok = false
		result.error = reason
	return result


## Export in memory and compare with the artifacts on disk; `mismatches`
## names every artifact that differs or is missing.
static func verify_source(source_path: String) -> ClipExportResult:
	var bytes: Array[PackedByteArray] = []
	var result := build(source_path, bytes)
	if not result.ok:
		return result
	var directory := ProjectSettings.globalize_path(result.output_directory)
	for i in result.artifacts.size():
		var path := directory.path_join(result.artifacts[i])
		if not FileAccess.file_exists(path):
			result.mismatches.append("%s: missing" % result.artifacts[i])
			continue
		var committed := FileAccess.get_file_as_bytes(path)
		if ModelExport.is_lfs_pointer(committed):
			result.mismatches.append("%s: unpulled LFS pointer" % result.artifacts[i])
		elif committed != bytes[i]:
			result.mismatches.append("%s: differs from a fresh export" % result.artifacts[i])
	result.ok = result.mismatches.is_empty()
	if not result.ok:
		result.error = "; ".join(result.mismatches)
	return result


static func _find_player(node: Node) -> AnimationPlayer:
	for child in node.get_children():
		if child is AnimationPlayer:
			return child
		var nested := _find_player(child)
		if nested != null:
			return nested
	return null
