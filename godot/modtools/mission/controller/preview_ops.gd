extends "res://modtools/mission/controller/controller_section.gd"

# In-editor PLAYPARTANIM authoring preview. State stays on MissionController
# and is reached through `_c`; this is not a game runtime.

# Index over the placer's construction-time registrations ({model, ref} records — the same
# channel the runtime owner builds its EntityIndex from; never a container scan), rebuilt
# only when the entity set changes. The mission's area triggers ride along for zone resolution.
func _get_preview_registry() -> EntityIndex:
	if _c._preview_registry == null or _c._preview_registry_rev != _c._membership_rev:
		_c._preview_registry = EntityIndex.new()
		_c._preview_registry.build(
				_c._placer.placed_entity_records if _c._placer != null else [],
				_c.get_area_triggers())
		_c._preview_registry_rev = _c._membership_rev
	return _c._preview_registry


# Resolve a PLAYPARTANIM action to one live animatable model: its explicit target (param1) by
# action_type, else an animated current selection, else null. The index resolves only live
# NovaObjectModels, so the first non-null hit is the target.
func _resolve_part_anim_node(action: Dictionary) -> ObjectModel:
	var registry := _get_preview_registry()
	var target := int(action.get("param1", 0))
	var models: Array = []
	match int(action.get("action_type", -1)):
		MissionData.ACTION_CHANGE_SINGLE_AI:
			var hit := registry.resolve_single(target)
			if hit != null:
				models = [hit]
		MissionData.ACTION_CHANGE_GROUP_AI:
			models = registry.resolve_group(target)
		MissionData.ACTION_AREA_AI_RED, MissionData.ACTION_AREA_AI_BLUE:
			models = registry.resolve_zone(target)
	for model: ObjectModel in models:
		if model != null:
			return model
	# Fallback: an animated current selection (e.g. previewing while an object is selected).
	return _c._selected_model


## True when the given scripting action can be previewed (a target model resolves).
func can_preview_part_anim(action: Dictionary) -> bool:
	return not action.is_empty() and _resolve_part_anim_node(action) != null


## Play the action's part animation on its target model (clean restart from rest). Returns false when no
## target resolves. channel = param2, play_type = param3, time = param4 (fixed-seconds raw ->
## seconds via MissionData.fixed_seconds_from_raw; the scale witness lives at the engine home).
func preview_part_anim(action: Dictionary) -> bool:
	var model := _resolve_part_anim_node(action)
	if model == null:
		return false
	stop_preview()
	_c._preview_node = model
	var channel := int(action.get("param2", 0))
	var play_type := int(action.get("param3", 0))
	var time_s := float(MissionData.fixed_seconds_from_raw(int(action.get("param4", 0))))
	model.set_playing(true)
	model.reset_animation_time()
	model.restart_part_anim(channel, play_type, time_s)
	return true


## Stop any running preview and return the previewed model to rest.
## is_instance_valid guards LIVENESS only (a re-bake frees container children).
func stop_preview() -> void:
	if _c._preview_node != null and is_instance_valid(_c._preview_node):
		_c._preview_node.clear_part_anims()
		_c._preview_node.clear_ctrl_values()
		_c._preview_node.reset_animation_time()
	_c._preview_node = null
