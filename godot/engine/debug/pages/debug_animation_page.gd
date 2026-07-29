class_name DebugAnimationPage
extends NovaDebugPage
## Animation & models: the local player's animation scalars plus one row per
## live animatable model (its playing body clip, active draw detail level and
## whether it is skinned), read off the mission entity registry each refresh.
## The bone/user-point world views ride the NovaDebugOptions registry.

const MODEL_ROW_CAP := 96

var _player_label: Label
var _models_header: Label
var _model_list: ItemList


func page_id() -> StringName:
	return &"Animation"


func page_title() -> String:
	return "Animation & models"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_player_label = Label.new()
	_player_label.name = "AnimPlayer"
	_player_label.text = "No local player."
	_player_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_player_label)

	_models_header = Label.new()
	_models_header.name = "AnimModelsHeader"
	_models_header.text = "No live models."
	add_child(_models_header)

	_model_list = ItemList.new()
	_model_list.name = "AnimModels"
	_model_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_model_list.focus_mode = Control.FOCUS_NONE
	add_child(_model_list)

	add_option_check(&"show_skeletons")
	add_option_check(&"show_user_points")


func refresh() -> void:
	var world := _ctx.world()
	if world != null and world.has_method("local_player_anim_key"):
		var key := String(world.local_player_anim_key())
		_player_label.text = "Local player:  %s   body slot %d   phase %d ticks" % [
			key if not key.is_empty() else "-",
			int(world.local_player_body_anim_slot()),
			int(world.local_player_anim_phase_ticks())]
	else:
		_player_label.text = "No local player."

	var nodes := _animatable_nodes()
	if nodes.is_empty():
		_models_header.text = "No live models."
		if _model_list.item_count > 0:
			_model_list.clear()
		return

	var skinned := 0
	var playing := 0
	var shown := mini(nodes.size(), MODEL_ROW_CAP)
	if _model_list.item_count != shown:
		_model_list.clear()
		for i in range(shown):
			_model_list.add_item("", null, false)
	for i in range(shown):
		var model: Node = nodes[i]
		if model == null or not is_instance_valid(model):
			_model_list.set_item_text(i, "(freed)")
			continue
		var clip := String(model.get_active_body_clip()) \
				if model.has_method("get_active_body_clip") else ""
		var lod := int(model.get_active_lod()) if model.has_method("get_active_lod") else -1
		var has_bones: bool = model.has_method("has_skeleton") and model.has_skeleton()
		if has_bones:
			skinned += 1
		if not clip.is_empty():
			playing += 1
		var row := String(model.name)
		if not clip.is_empty():
			row += "   clip %s" % clip
		if lod >= 0:
			row += "   detail L%d" % lod
		if has_bones:
			row += "   skinned"
		_model_list.set_item_text(i, row)
	var suffix := "" if nodes.size() <= MODEL_ROW_CAP \
			else "   (showing %d of %d)" % [shown, nodes.size()]
	_models_header.text = "%d animatable   %d skinned   %d playing%s" % [
		nodes.size(), skinned, playing, suffix]


func _animatable_nodes() -> Array:
	var runtime := _ctx.runtime()
	if runtime == null or not runtime.has_method("get_registry"):
		return []
	var registry: Variant = runtime.get_registry()
	if registry == null or not is_instance_valid(registry) \
			or not (registry as Object).has_method("get_animatable_nodes"):
		return []
	return registry.get_animatable_nodes()
