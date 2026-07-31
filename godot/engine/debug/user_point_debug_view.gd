extends Node3D

# World-wide F3 view for named .3di user points. Live NovaObjectModel sources are
# discovered under the GameWorld and delegated to ObjectUserPointOverlay, which
# follows skeletal/PANM pose. Static mission objects render through MultiMesh and
# therefore have no source nodes; MissionObjectPlacer supplies one grouped snapshot
# of the base entity transforms that actually reached a rendered batch.

const UserPointOverlay := preload("res://engine/object/object_user_point_overlay.gd")

var _root: Node
var _live_overlays: Dictionary = {}  # source instance id -> ObjectUserPointOverlay
var _static_overlay_count := 0


## static_sources rows are { object_data: NovaObjectData,
## transforms: Array[Transform3D] }, grouped by graphic.
func setup(root: Node, static_sources: Array = []) -> void:
	_root = root
	_build_static_overlays(static_sources)
	refresh_now()


## Discover additions/removals deterministically. The process callback delegates
## here so tests and tooling never need to call a private frame hook.
func refresh_now() -> void:
	if _root == null or not is_instance_valid(_root):
		_clear_live_overlays()
		return
	var sources: Array = []
	_collect_live_sources(_root, sources)
	var seen: Dictionary = {}
	for row_v in sources:
		var row: Dictionary = row_v
		var source := row.get("node") as Node3D
		var data := row.get("object_data") as NovaObjectData
		if source == null or data == null:
			continue
		var source_id := source.get_instance_id()
		seen[source_id] = true
		var overlay := _live_overlays.get(source_id) as ObjectUserPointOverlay
		if overlay == null or not is_instance_valid(overlay):
			overlay = UserPointOverlay.new()
			overlay.name = "Live_%d" % source_id
			add_child(overlay)
			overlay.set_source_model(source)
			# This view owns the refresh cadence; disable the overlay's standalone
			# process hook so each marker is not resolved twice per frame.
			overlay.set_process(false)
			_live_overlays[source_id] = overlay
		overlay.set_object_data(data)
		overlay.set_points_visible(true)
		overlay.refresh_now()
	for source_id in _live_overlays.keys():
		if not seen.has(source_id):
			_remove_live_overlay(source_id)


func get_live_overlay_count() -> int:
	return _live_overlays.size()


func get_static_overlay_count() -> int:
	return _static_overlay_count


## Number of named user-point markers currently contributed by live and static
## model overlays.
func get_debug_drawable_count() -> int:
	var count := 0
	for child in get_children():
		if child.has_method("get_user_point_count"):
			count += int(child.call("get_user_point_count"))
	return count


func _process(_delta: float) -> void:
	refresh_now()


func _build_static_overlays(static_sources: Array) -> void:
	_static_overlay_count = 0
	for source_index in range(static_sources.size()):
		var row: Dictionary = static_sources[source_index]
		var data := row.get("object_data") as NovaObjectData
		if data == null or data.get_user_point_count() <= 0:
			continue
		var transforms: Array = row.get("transforms", [])
		for transform_index in range(transforms.size()):
			var xform: Transform3D = transforms[transform_index]
			var overlay := UserPointOverlay.new()
			overlay.name = "Static_%03d_%04d" % [source_index, transform_index]
			add_child(overlay)
			overlay.set_object_data(data)
			overlay.set_entity_transform(xform)
			overlay.set_points_visible(true)
			_static_overlay_count += 1


func _collect_live_sources(node: Node, out: Array) -> void:
	for child in node.get_children():
		if child == self:
			continue
		var source := child as Node3D
		if source != null and source.has_method("get_object_data"):
			var data := source.call("get_object_data") as NovaObjectData
			if data != null and data.get_user_point_count() > 0:
				out.append({ "node": source, "object_data": data })
		if child.get_child_count() > 0:
			_collect_live_sources(child, out)


func _remove_live_overlay(source_id: int) -> void:
	var overlay := _live_overlays.get(source_id) as Node
	_live_overlays.erase(source_id)
	if overlay == null or not is_instance_valid(overlay):
		return
	if overlay.get_parent() == self:
		remove_child(overlay)
	overlay.queue_free()


func _clear_live_overlays() -> void:
	for source_id in _live_overlays.keys():
		_remove_live_overlay(source_id)
