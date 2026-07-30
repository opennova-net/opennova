extends "res://modtools/mission/controller/controller_section.gd"

# In-editor PLAYPARTANIM authoring preview. State stays on MissionController
# and is reached through `_c`; this is not a game runtime.

# Registry over the placed (edit-mode) container, rebuilt only when the entity set changes.
func _get_preview_registry():
	if _c._preview_registry == null or _c._preview_registry_rev != _c._membership_rev:
		_c._preview_registry = _c.MissionEntityRegistry.new()
		_c._preview_registry.build(_c._objects_container(), _c._mission)
		_c._preview_registry_rev = _c._membership_rev
	return _c._preview_registry


# Resolve a PLAYPARTANIM action to one live animatable model: its explicit target (param1) by
# action_type, else an animated current selection, else null.
func _resolve_part_anim_node(action: Dictionary) -> Node3D:
	var registry = _get_preview_registry()
	var target := int(action.get("param1", 0))
	var nodes: Array = []
	match int(action.get("action_type", -1)):
		_c._ACT_CHANGE_SINGLE_AI:
			var hit = registry.resolve_single(target)  # registry is untyped here; no := inference
			if hit != null:
				nodes = [hit]
		_c._ACT_CHANGE_GROUP_AI:
			nodes = registry.resolve_group(target)
		_c._ACT_AREA_AI_RED, _c._ACT_AREA_AI_BLUE:
			nodes = registry.resolve_zone(target)
	for n in nodes:
		if n != null and is_instance_valid(n) and n.has_method("play_part_anim"):
			return n
	# Fallback: an animated current selection (e.g. previewing while an object is selected).
	if _c._selected_node != null and is_instance_valid(_c._selected_node) and _c._selected_node.has_method("play_part_anim"):
		return _c._selected_node
	return null


## True when the given scripting action can be previewed (a target model resolves).
func can_preview_part_anim(action: Dictionary) -> bool:
	return not action.is_empty() and _resolve_part_anim_node(action) != null


## Play the action's part animation on its target model (clean restart from rest). Returns false when no
## target resolves. channel = param2, play_type = param3, time = param4 (16.16 seconds -> seconds).
func preview_part_anim(action: Dictionary) -> bool:
	var node := _resolve_part_anim_node(action)
	if node == null:
		return false
	stop_preview()
	_c._preview_node = node
	var channel := int(action.get("param2", 0))
	var play_type := int(action.get("param3", 0))
	var time_s := float(int(action.get("param4", 0))) / 65536.0
	if node.has_method("set_playing"):
		node.set_playing(true)
	if node.has_method("reset_animation_time"):
		node.reset_animation_time()
	if node.has_method("restart_part_anim"):
		node.restart_part_anim(channel, play_type, time_s)
	elif node.has_method("play_part_anim"):
		node.play_part_anim(channel, play_type, time_s)
	return true


## Stop any running preview and return the previewed model to rest.
func stop_preview() -> void:
	if _c._preview_node != null and is_instance_valid(_c._preview_node):
		if _c._preview_node.has_method("clear_part_anims"):
			_c._preview_node.clear_part_anims()
		if _c._preview_node.has_method("clear_ctrl_values"):
			_c._preview_node.clear_ctrl_values()
		if _c._preview_node.has_method("reset_animation_time"):
			_c._preview_node.reset_animation_time()
	_c._preview_node = null
