class_name ShadowCasterDiagnostic
extends RefCounted

## One authored model admitted to a directional-shadow caster layer.
## The node path is retained only as a deterministic final sort key; the
## scratch diagnostic boundary emits exactly the authored identity requested
## by the attribution profile.

var bms_id := -1
var item_id := 0
var graphic := ""
var attrib2 := 0
var layer := 0
var aabb := AABB()
var sort_path := ""


func _init(
		row_bms_id: int,
		row_item_id: int,
		row_graphic: String,
		row_attrib2: int,
		row_layer: int,
		row_aabb: AABB,
		row_sort_path: String,
		) -> void:
	bms_id = row_bms_id
	item_id = row_item_id
	graphic = row_graphic
	attrib2 = row_attrib2
	layer = row_layer
	aabb = row_aabb
	sort_path = row_sort_path


func to_json_value() -> Dictionary:
	return {
		"bms_id": bms_id,
		"item_id": item_id,
		"graphic": graphic,
		"attrib2": attrib2,
		"layer": layer,
		"aabb": aabb,
	}
