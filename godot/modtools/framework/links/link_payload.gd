class_name LinkPayload
extends RefCounted
## The drag-data contract for resource links: any widget or pane that drags a
## resource reference packs one of these, and any drop target unpacks it with
## from_drag_data (null when the dragged data is not ours). Keeping the shape in
## one class is what lets future drop targets (browser pane, viewport placement)
## accept links from sources they have never heard of.

const DRAG_TYPE := "opennova/resource_ref"

var kind := ""
var name := ""
var path := ""


static func make(p_kind: String, p_name: String, p_path: String) -> LinkPayload:
	var payload := LinkPayload.new()
	payload.kind = p_kind
	payload.name = p_name
	payload.path = p_path
	return payload


func to_drag_data() -> Dictionary:
	return {"type": DRAG_TYPE, "kind": kind, "name": name, "path": path}


static func from_drag_data(data: Variant) -> LinkPayload:
	if not (data is Dictionary) or String((data as Dictionary).get("type", "")) != DRAG_TYPE:
		return null
	var d := data as Dictionary
	return make(String(d.get("kind", "")), String(d.get("name", "")), String(d.get("path", "")))
