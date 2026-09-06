@tool
class_name ModelExport
extends RefCounted
## The model export workflow (ADR 0045: workflows live under godot/tools):
## an authoring manifest names a scene, the material and LOD words the scene
## cannot spell, and the texture sources; exporting runs the native exporter
## over the instantiated scene, encodes the textures, and installs every
## artifact into the manifest's output directory through one staged
## transaction. Verifying does the same in memory and compares against what
## is on disk, which is how the GUT byte guard reads the authored set.

const AUTHORING_DIR := "res://authoring"
const LFS_POINTER := "version https://git-lfs"


## Every manifest under the authoring tree (`<dir>/<name>/<name>.tres`).
static func list_manifests(authoring_dir: String = AUTHORING_DIR) -> PackedStringArray:
	var manifests := PackedStringArray()
	var root := ProjectSettings.globalize_path(authoring_dir)
	if not DirAccess.dir_exists_absolute(root):
		return manifests
	for folder in DirAccess.get_directories_at(root):
		for file in DirAccess.get_files_at(root.path_join(folder)):
			if not file.ends_with(".tres"):
				continue
			var path := authoring_dir.path_join(folder).path_join(file)
			if load(path) is ModelAuthoringManifest:
				manifests.append(path)
	manifests.sort()
	return manifests


## Export the manifest's scene and textures in memory. On success the result
## lists the artifact names and `bytes` (parallel to `artifacts`) holds each
## artifact's bytes.
static func build(manifest_path: String, bytes: Array[PackedByteArray]) -> ModelExportResult:
	bytes.clear()
	var manifest := load(manifest_path) as ModelAuthoringManifest
	if manifest == null:
		return ModelExportResult.failure(manifest_path, "not a ModelAuthoringManifest")
	var result := ModelExportResult.new()
	result.manifest_path = manifest_path
	result.model_name = manifest.model_name
	result.output_directory = manifest.output_directory
	if manifest.model_name.is_empty():
		result.error = "the manifest has no model_name"
		return result
	if manifest.scene == null:
		result.error = "the manifest names no scene"
		return result
	var root := manifest.scene.instantiate() as Node3D
	if root == null:
		result.error = "the manifest's scene root is not a Node3D"
		return result
	var exporter := ModelSceneExporter.new()
	var document := exporter.export_scene(root, manifest)
	root.free()
	if document == null:
		result.error = exporter.get_last_error()
		return result
	var model_bytes := document.to_bytes()
	if model_bytes.is_empty():
		result.error = document.get_last_error()
		return result
	result.artifacts.append(manifest.model_name + ".3di")
	bytes.append(model_bytes)
	var encoder := ModelTextureEncoder.new()
	for source in manifest.texture_sources:
		var row := source as ModelTextureSource
		if row == null or row.output_name.is_empty() or row.source_path.is_empty():
			result.error = "a texture source row is incomplete"
			return result
		if row.output_name.get_basename().length() > 12 or not row.output_name.to_lower().ends_with(".tga"):
			result.error = "texture '%s': the output must be a .tga whose stem is at most 12 characters" % row.output_name
			return result
		var tga := encoder.encode_file(row.source_path, row.with_alpha)
		if tga.is_empty():
			result.error = "texture '%s': %s" % [row.output_name, encoder.get_last_error()]
			return result
		result.artifacts.append(row.output_name)
		bytes.append(tga)
	result.ok = true
	return result


## Export and install the manifest's artifacts into its output directory
## (replacing existing files atomically).
static func export_manifest(manifest_path: String) -> ModelExportResult:
	var bytes: Array[PackedByteArray] = []
	var result := build(manifest_path, bytes)
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
## names every artifact that differs or is missing. An unpulled LFS pointer
## on disk is reported as such rather than as a mismatch.
static func verify_manifest(manifest_path: String) -> ModelExportResult:
	var bytes: Array[PackedByteArray] = []
	var result := build(manifest_path, bytes)
	if not result.ok:
		return result
	var directory := ProjectSettings.globalize_path(result.output_directory)
	for i in result.artifacts.size():
		var path := directory.path_join(result.artifacts[i])
		if not FileAccess.file_exists(path):
			result.mismatches.append("%s: missing" % result.artifacts[i])
			continue
		var committed := FileAccess.get_file_as_bytes(path)
		if is_lfs_pointer(committed):
			result.mismatches.append("%s: unpulled LFS pointer" % result.artifacts[i])
		elif committed != bytes[i]:
			result.mismatches.append("%s: differs from a fresh export" % result.artifacts[i])
	result.ok = result.mismatches.is_empty()
	if not result.ok:
		result.error = "; ".join(result.mismatches)
	return result


## True for an unpulled Git LFS pointer stub (the artifact's bytes are not local).
static func is_lfs_pointer(data: PackedByteArray) -> bool:
	return data.size() >= LFS_POINTER.length() \
			and data.slice(0, LFS_POINTER.length()).get_string_from_ascii() == LFS_POINTER
