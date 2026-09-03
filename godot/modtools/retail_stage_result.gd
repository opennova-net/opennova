class_name RetailStageResult
extends RefCounted

## The outcome of staging a retail install beside a resource directory
## (GamePacker.stage_retail, driven by GameRunSession through its platform):
## a typed record, not a dictionary (ADR 0017).

var ok := false
var exe := ""  # the staged retail executable to launch
var packed_dir := ""  # the staged directory the process runs in
var error := ""  # why staging failed (empty when ok)


static func failure(message: String) -> RetailStageResult:
	var result := RetailStageResult.new()
	result.error = message
	return result


static func success(exe_path: String, staged_dir: String) -> RetailStageResult:
	var result := RetailStageResult.new()
	result.ok = true
	result.exe = exe_path
	result.packed_dir = staged_dir
	return result
