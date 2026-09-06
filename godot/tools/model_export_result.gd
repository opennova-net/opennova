@tool
class_name ModelExportResult
extends RefCounted
## The outcome of exporting or verifying one authoring manifest: the model
## name, the artifacts it maps to, what was written or what differs.

var ok := false
var error := ""
var manifest_path := ""
var model_name := ""
var output_directory := ""
## Artifact filenames the manifest produces (the .3di first, then its textures).
var artifacts: PackedStringArray = []
## Verify only: artifacts whose committed bytes differ from a fresh export
## (or are missing).
var mismatches: PackedStringArray = []


static func failure(path: String, reason: String) -> ModelExportResult:
	var result := ModelExportResult.new()
	result.manifest_path = path
	result.error = reason
	return result
