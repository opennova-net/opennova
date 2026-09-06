@tool
class_name ClipExportResult
extends RefCounted
## The outcome of exporting or verifying one clip set: the .adm name, the
## artifacts it maps to (every .bad, then the .adm), what was written or what
## differs.

var ok := false
var error := ""
var source_path := ""
var adm_name := ""
var output_directory := ""
## Artifact filenames the set produces (the clips' .bad files in spec order,
## then the .adm).
var artifacts: PackedStringArray = []
## Verify only: artifacts whose committed bytes differ from a fresh export
## (or are missing).
var mismatches: PackedStringArray = []


static func failure(path: String, reason: String) -> ClipExportResult:
	var result := ClipExportResult.new()
	result.source_path = path
	result.error = reason
	return result
