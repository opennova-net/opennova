class_name DebugAnimationPage
extends NovaDebugPage
## Animation & models: explains the local player's current body transition,
## then lets a developer select a live model and inspect its clip/playhead,
## draw detail, skinning, part-animation and CTRL state. Models are read from
## the mission registry each refresh. Selection follows stable entity identity
## rather than list position, so active/picked prioritisation cannot make the
## inspector jump to a different object.

const MODEL_ROW_CAP := 96
const PART_DETAIL_CAP := 4
const CTRL_DETAIL_CAP := 8

var _player_label: Label
var _models_header: Label
var _model_list: ItemList
var _model_detail: Label
var _display_models: Array[NovaObjectModel] = []
var _selected_model_identity := ""


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
	_model_list.focus_mode = Control.FOCUS_ALL
	_model_list.item_selected.connect(_on_model_selected)
	add_child(_model_list)

	_model_detail = Label.new()
	_model_detail.name = "AnimModelDetail"
	_model_detail.text = "Select a live model to inspect it."
	_model_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_model_detail)

	add_option_check(&"show_skeletons")
	add_option_check(&"show_user_points")


func refresh() -> void:
	_refresh_player()
	_refresh_models()


func _refresh_player() -> void:
	var sim := _ctx.sim()
	if sim == null:
		_player_label.text = "No local player."
		return
	var key := String(sim.get_local_player_anim_key())
	var slot := int(sim.get_local_player_body_anim_slot())
	var phase := int(sim.get_local_player_anim_phase_ticks())
	var source_key := String(sim.get_local_player_anim_source_key())
	var source_phase := int(sim.get_local_player_anim_source_phase_ticks())
	var blend := clampf(float(sim.get_local_player_anim_blend_weight()), 0.0, 1.0)
	if not source_key.is_empty() and source_key != key and blend < 0.999:
		_player_label.text = (
				"Player transition\n"
				+ "source  %s @ %d ticks\n" % [source_key, source_phase]
				+ "→ target  %s @ %d ticks   ·   blend %d%%   ·   body slot %d"
				% [key if not key.is_empty() else "-", phase,
						int(roundf(blend * 100.0)), slot])
	else:
		_player_label.text = (
				"Player animation\n"
				+ "%s @ %d ticks   ·   body slot %d   ·   settled"
				% [key if not key.is_empty() else "-", phase, slot])


func _refresh_models() -> void:
	var rows := _model_rows()
	if rows.is_empty():
		_models_header.text = "No live models."
		_model_list.clear()
		_display_models.clear()
		_selected_model_identity = ""
		_model_detail.text = "No live model to inspect."
		return

	var skinned := 0
	var playing := 0
	var picked := 0
	for row in rows:
		var model: NovaObjectModel = row["model"]
		var clip := _model_clip(model)
		var has_bones := _model_has_skeleton(model)
		if has_bones:
			skinned += 1
		if not clip.is_empty():
			playing += 1
		if bool(row["picked"]):
			picked += 1

	var shown := mini(rows.size(), MODEL_ROW_CAP)
	_model_list.clear()
	_display_models.clear()
	var selected_index := -1
	for i in range(shown):
		var row: Dictionary = rows[i]
		var model: NovaObjectModel = row["model"]
		_display_models.append(model)
		_model_list.add_item(_model_row_text(model, bool(row["picked"])), null, true)
		_model_list.set_item_tooltip(i, "Select to inspect this model's live animation state.")
		if String(row["identity"]) == _selected_model_identity:
			selected_index = i

	if selected_index < 0:
		selected_index = 0
		_selected_model_identity = _model_identity(_display_models[0])
	_model_list.select(selected_index)
	_render_model_detail(_display_models[selected_index])

	var picked_text := "   %d picked" % picked if picked > 0 else ""
	var suffix := "" if rows.size() <= MODEL_ROW_CAP \
			else "   (showing %d of %d)" % [shown, rows.size()]
	_models_header.text = "%d animatable   %d playing   %d skinned%s%s" % [
		rows.size(), playing, skinned, picked_text, suffix]


func _model_rows() -> Array:
	var rows: Array = []
	for candidate in _animatable_nodes():
		if not (candidate is NovaObjectModel) or not is_instance_valid(candidate):
			continue
		var model := candidate as NovaObjectModel
		rows.append({
			"model": model,
			"identity": _model_identity(model),
			"picked": _model_is_picked(model),
			"playing": not _model_clip(model).is_empty(),
			"name": String(model.name).to_lower(),
		})
	rows.sort_custom(_model_row_less)
	return rows


func _model_row_less(a: Dictionary, b: Dictionary) -> bool:
	if bool(a["picked"]) != bool(b["picked"]):
		return bool(a["picked"])
	if bool(a["playing"]) != bool(b["playing"]):
		return bool(a["playing"])
	var a_name := String(a["name"])
	var b_name := String(b["name"])
	if a_name != b_name:
		return a_name < b_name
	return String(a["identity"]) < String(b["identity"])


func _model_row_text(model: NovaObjectModel, picked: bool) -> String:
	var text := "PICKED   " if picked else ""
	text += String(model.name)
	var clip := _model_clip(model)
	text += "   clip %s" % (clip if not clip.is_empty() else "stopped")
	var lod := _model_lod(model)
	if lod >= 0:
		text += "   detail L%d" % lod
	if _model_has_skeleton(model):
		text += "   skinned"
	return text


func _on_model_selected(index: int) -> void:
	if index < 0 or index >= _display_models.size():
		return
	var candidate: Variant = _display_models[index]
	if not (candidate is Node) or not is_instance_valid(candidate):
		return
	var model := candidate as NovaObjectModel
	_selected_model_identity = _model_identity(model)
	_render_model_detail(model)


func _render_model_detail(model: NovaObjectModel) -> void:
	if model == null or not is_instance_valid(model):
		_model_detail.text = "The selected model is no longer live."
		return
	var clip := _model_clip(model)
	var lod := _model_lod(model)
	var body_bits := PackedStringArray([
		"%s @ %.3f s" % [clip if not clip.is_empty() else "stopped",
				_model_playhead(model)],
		"detail L%d" % lod if lod >= 0 else "detail unavailable",
		"skinned" if _model_has_skeleton(model) else "rigid",
	])
	var lines := PackedStringArray([
		"%s   ·   %s" % [String(model.name), _model_identity_label(model)],
		"Body: %s" % "   ·   ".join(body_bits),
		"Parts: %s" % _part_state_text(model),
		"CTRL: %s" % _ctrl_state_text(model),
	])
	_model_detail.text = "\n".join(lines)


func _model_clip(model: NovaObjectModel) -> String:
	return String(model.get_active_body_clip())


func _model_lod(model: NovaObjectModel) -> int:
	return int(model.get_active_lod())


func _model_has_skeleton(model: NovaObjectModel) -> bool:
	return bool(model.has_skeleton())


func _model_playhead(model: NovaObjectModel) -> float:
	return float(model.get_animation_time())


func _part_state_text(model: NovaObjectModel) -> String:
	var value: Variant = model.get_active_part_anims()
	if not (value is Dictionary) or (value as Dictionary).is_empty():
		return "no active sweep"
	var states := value as Dictionary
	var keys: Array = states.keys()
	keys.sort_custom(func(a: Variant, b: Variant): return String(a) < String(b))
	var pieces := PackedStringArray()
	for i in range(mini(keys.size(), PART_DETAIL_CAP)):
		var key := String(keys[i])
		var state: Variant = states[keys[i]]
		if state is Dictionary:
			var row := state as Dictionary
			var direction := "forward" if int(row.get("dir", 0)) > 0 else "reverse"
			if int(row.get("dir", 0)) == 0:
				direction = "stopped"
			pieces.append("%s %s, phase %s, step %d" % [
				key, direction, _phase_text(int(row.get("value", 0))),
				int(row.get("rate", 0))])
		else:
			pieces.append("%s = %s" % [key, str(state)])
	if keys.size() > PART_DETAIL_CAP:
		pieces.append("+%d more" % (keys.size() - PART_DETAIL_CAP))
	return "; ".join(pieces)


func _ctrl_state_text(model: NovaObjectModel) -> String:
	var value: Variant = model.get_ctrl_values()
	if not (value is Dictionary) or (value as Dictionary).is_empty():
		return "no live values"
	var controls := value as Dictionary
	var keys: Array = controls.keys()
	keys.sort_custom(func(a: Variant, b: Variant): return String(a) < String(b))
	var pieces := PackedStringArray()
	for i in range(mini(keys.size(), CTRL_DETAIL_CAP)):
		var key := String(keys[i])
		pieces.append("%s=%s" % [key, _phase_text(int(controls[keys[i]]))])
	if keys.size() > CTRL_DETAIL_CAP:
		pieces.append("+%d more" % (keys.size() - CTRL_DETAIL_CAP))
	return "; ".join(pieces)


func _phase_text(value: int) -> String:
	if value >= 0 and value <= 65536:
		return "%d (%.1f%%)" % [value, float(value) * 100.0 / 65536.0]
	return str(value)


func _model_identity(model: NovaObjectModel) -> String:
	var ref := _model_ref(model)
	var bms_id := int(ref.get("bms_id", 0))
	if bms_id != 0:
		return "bms:%d" % bms_id
	var wire_handle := int(ref.get("wire_handle", -1))
	if wire_handle >= 0:
		return "wire:%d" % wire_handle
	var kind := int(ref.get("kind", ref.get("origin_kind", -1)))
	var index := int(ref.get("index", -1))
	if kind >= 0 and index >= 0:
		return "origin:%d:%d" % [kind, index]
	return "instance:%d" % model.get_instance_id()


func _model_identity_label(model: NovaObjectModel) -> String:
	var ref := _model_ref(model)
	var bits := PackedStringArray()
	var bms_id := int(ref.get("bms_id", 0))
	if bms_id != 0:
		bits.append("BMS %d" % bms_id)
	var wire_handle := int(ref.get("wire_handle", -1))
	if wire_handle >= 0:
		bits.append("wire 0x%04X" % wire_handle)
	var kind := int(ref.get("kind", ref.get("origin_kind", -1)))
	var index := int(ref.get("index", -1))
	if kind >= 0 and index >= 0:
		bits.append("origin %d:%d" % [kind, index])
	var item_id := int(ref.get("item_id", -1))
	if item_id >= 0:
		bits.append("item %d" % item_id)
	if bits.is_empty():
		bits.append("instance %d" % model.get_instance_id())
	return "   ·   ".join(bits)


func _model_ref(model: NovaObjectModel) -> Dictionary:
	var value: Variant = model.get_meta("entity_ref", {})
	return value if value is Dictionary else {}


func _model_is_picked(model: NovaObjectModel) -> bool:
	if _ctx.pick_list == null:
		return false
	var ref := _model_ref(model)
	var bms_id := int(ref.get("bms_id", 0))
	var wire_handle := int(ref.get("wire_handle", -1))
	var kind := int(ref.get("kind", ref.get("origin_kind", -1)))
	var index := int(ref.get("index", -1))
	for pick in _ctx.pick_list.get_picks():
		if bms_id != 0 and int(pick.get("bms_id", 0)) == bms_id:
			return true
		if wire_handle >= 0 and int(pick.get("entity_handle", -1)) == wire_handle:
			return true
		if kind >= 0 and index >= 0 \
				and int(pick.get("kind", -1)) == kind \
				and int(pick.get("index", -1)) == index:
			return true
	return false


func _animatable_nodes() -> Array:
	var output: Array = []
	var seen := {}
	var runtime := _ctx.runtime()
	if runtime != null and runtime.has_method("get_registry"):
		var registry: Variant = runtime.get_registry()
		if registry != null and is_instance_valid(registry) \
				and (registry as Object).has_method("get_animatable_nodes"):
			for candidate in registry.get_animatable_nodes():
				_append_animatable(candidate, output, seen)

	# Placed mission models live in the registry. Player avatars and wire-direct
	# dynamic models do not, so inspect the active world subtree as well while
	# this debug page is visible. Instance-id dedupe keeps registry members from
	# appearing twice.
	var world := _ctx.world()
	if world is Node:
		_collect_world_animatables(world as Node, output, seen)
	return output


func _collect_world_animatables(
		node: Node,
		output: Array,
		seen: Dictionary) -> void:
	for child in node.get_children():
		_append_animatable(child, output, seen)
		if child.get_child_count() > 0:
			_collect_world_animatables(child, output, seen)


func _append_animatable(
		candidate: Variant,
		output: Array,
		seen: Dictionary) -> void:
	if not (candidate is Node) or not is_instance_valid(candidate):
		return
	var node := candidate as NovaObjectModel
	if node == null:
		return
	var instance_id := node.get_instance_id()
	if seen.has(instance_id):
		return
	seen[instance_id] = true
	output.append(node)
