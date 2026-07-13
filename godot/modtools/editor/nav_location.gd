class_name EditorNavLocation
extends RefCounted
## One shell location, as Back/Forward history records it: the active
## workspace and its open document ("" when none). Typed record per ADR 0017;
## future context (selection, camera) is additive as new fields.

var workspace_id := -1
var path := ""


static func make(p_workspace_id: int, p_path: String) -> EditorNavLocation:
	var location := EditorNavLocation.new()
	location.workspace_id = p_workspace_id
	location.path = p_path
	return location


static func same(a: EditorNavLocation, b: EditorNavLocation) -> bool:
	if a == null or b == null:
		return a == b
	return a.workspace_id == b.workspace_id and a.path == b.path
