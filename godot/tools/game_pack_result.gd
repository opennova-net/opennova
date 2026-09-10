@tool
class_name GamePackResult
extends RefCounted
## Files produced by one pack, stage or release export operation.

var ok := false
var error := ""
var archive := ""
var archived := PackedStringArray()
var loose := PackedStringArray()
var staged := PackedStringArray()
var skipped := PackedStringArray()
var skipped_dirs := PackedStringArray()
var game_dir := ""
var exported := PackedStringArray()


static func failure(message: String) -> GamePackResult:
	var result := GamePackResult.new()
	result.error = message
	return result
