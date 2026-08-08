class_name DebugParticlesPage
extends NovaDebugPage
## The retail particle debug pages, mimicked (ptl-format-re.md §11 —
## Debug_DrawParticleStats @ 0x44c840 counts + entry list;
## Debug_DrawEffectBrowser @ 0x44c950 name + source file). Fed by the
## context's effect world (render-side, not the sim); the peak latch lives
## here like retail's debug global. The hide/effect-box toggles ride the
## NovaDebugOptions registry ("hide_particles" mimics the retail master
## particle switch [orig: byte_24D261D]).

var _ptl_count_label: Label
var _ptl_list: ItemList
var _ptl_detail: Label
var _catalog_status: Label
var _catalog_issues: ItemList
var _ptl_peak := 0
var _selected_group_id := -1
var _has_selected_group := false
var _groups_by_id: Dictionary = {}


func page_id() -> StringName:
	return &"Particles"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_ptl_count_label = Label.new()
	_ptl_count_label.name = "ParticleCounts"
	_ptl_count_label.text = (
			"Live effects: 0 (peak 0)   Alive particles: 0   Drawn quads: 0")
	_ptl_count_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_ptl_count_label)

	_ptl_list = ItemList.new()
	_ptl_list.name = "ParticleGroups"
	_ptl_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_ptl_list.allow_reselect = true
	_ptl_list.item_selected.connect(_on_group_selected)
	add_child(_ptl_list)

	_ptl_detail = Label.new()
	_ptl_detail.name = "ParticleGroupDetail"
	_ptl_detail.text = "No live particle groups."
	_ptl_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_ptl_detail)

	_catalog_status = Label.new()
	_catalog_status.name = "ParticleCatalogStatus"
	_catalog_status.text = "Catalog unavailable."
	_catalog_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_catalog_status)

	_catalog_issues = ItemList.new()
	_catalog_issues.name = "ParticleCatalogIssues"
	_catalog_issues.custom_minimum_size.y = 54.0
	_catalog_issues.focus_mode = Control.FOCUS_NONE
	_catalog_issues.visible = false
	add_child(_catalog_issues)

	var hide_check := add_option_check(&"hide_particles")
	hide_check.text = "Hide particles (isolate scene)"
	var boxes_check := add_option_check(&"show_effect_boxes")
	boxes_check.text = "Show live effect bounds"


func refresh() -> void:
	var world := _ctx.effect_world()
	if world == null:
		_clear_live_data()
		return
	var unresolved: PackedStringArray = (world.get_unresolved_texture_names()
			if world.has_method("get_unresolved_texture_names") else PackedStringArray())
	if _particles_hidden():
		_show_hidden_report(world.effect_count(), unresolved)
		return
	var report: Array = world.get_debug_group_report()
	var live_effects := int(world.active_entry_count())
	# The retail peak latch, quirk included: it resets to zero whenever the
	# current count is zero [orig: Debug_DrawParticleStats @ 0x44c840].
	_ptl_peak = 0 if live_effects == 0 else maxi(_ptl_peak, live_effects)

	var groups: Array[Dictionary] = []
	var alive_particles := 0
	var drawn_quads := 0
	for group_v in report:
		var group: Dictionary = (group_v as Dictionary).duplicate(true)
		var alive := 0
		var drawn := 0
		var emitters: Array = group.get("emitters", [])
		for emitter_v in emitters:
			var emitter: Dictionary = emitter_v
			alive += maxi(0, int(emitter.get("alive", 0)))
			drawn += maxi(0, int(emitter.get("rendered", 0)))
		alive_particles += alive
		drawn_quads += drawn
		group["_alive"] = alive
		group["_drawn"] = drawn
		group["_attention"] = _attention_score(group, alive, drawn)
		group["_attention_reason"] = _attention_reason(group, alive, drawn)
		groups.append(group)
	groups.sort_custom(_sort_group_attention)

	_ptl_count_label.text = (
			"Live effects: %d (peak %d)   Alive particles: %d   Drawn quads: %d"
			% [live_effects, _ptl_peak, alive_particles, drawn_quads])
	_refresh_groups(groups)
	_refresh_catalog(world.effect_count(), unresolved)


func _particles_hidden() -> bool:
	if _ctx == null:
		return false
	if _ctx.session != null and _ctx.session.has_control(&"hide_particles"):
		return bool(_ctx.session.get_control_state(&"hide_particles").value)
	return _ctx.options != null and bool(_ctx.options.value(&"hide_particles"))


func _show_hidden_report(
		effect_count: int,
		unresolved: PackedStringArray) -> void:
	_ptl_peak = 0
	_selected_group_id = -1
	_has_selected_group = false
	_groups_by_id.clear()
	_ptl_count_label.text = (
			"PARTICLES HIDDEN · live effect diagnostics suppressed")
	_ptl_list.clear()
	_ptl_detail.text = (
			"Particles are hidden, so live effect diagnostics are suppressed. "
			+ "Turn off Hide particles to inspect active emitters.")
	_refresh_catalog(effect_count, unresolved)


func _refresh_groups(groups: Array[Dictionary]) -> void:
	_ptl_list.clear()
	_groups_by_id.clear()
	var selected_index := -1
	for group in groups:
		var group_id := int(group.get("id", 0))
		var attention := int(group.get("_attention", 0))
		var alive := int(group.get("_alive", 0))
		var drawn := int(group.get("_drawn", 0))
		var state := "ATTENTION" if attention > 0 else ("LIVE" if alive > 0 else "idle")
		var row := "%s  %02d  %s  |  %d alive  %d drawn" % [
			state, group_id, String(group.get("name", "(unnamed)")), alive, drawn]
		var idx := _ptl_list.add_item(row)
		_ptl_list.set_item_metadata(idx, group_id)
		_ptl_list.set_item_tooltip(idx, String(group.get("_attention_reason", "")))
		if attention > 0:
			_ptl_list.set_item_custom_fg_color(idx, Color(1.0, 0.55, 0.3))
		elif alive > 0:
			_ptl_list.set_item_custom_fg_color(idx, Color(0.55, 1.0, 0.65))
		else:
			_ptl_list.set_item_custom_fg_color(idx, Color(0.65, 0.65, 0.65))
		_groups_by_id[group_id] = group
		if _has_selected_group and group_id == _selected_group_id:
			selected_index = idx

	if groups.is_empty():
		_has_selected_group = false
		_selected_group_id = -1
		_ptl_detail.text = "No live particle groups."
		return
	if selected_index < 0:
		selected_index = 0
		_selected_group_id = int(_ptl_list.get_item_metadata(0))
		_has_selected_group = true
	_ptl_list.select(selected_index)
	_show_group_detail(_groups_by_id.get(_selected_group_id, {}))


func _refresh_catalog(effect_count: int, unresolved: PackedStringArray) -> void:
	var definition_word := "definition" if effect_count == 1 else "definitions"
	var issue_word := "missing texture" if unresolved.size() == 1 else "missing textures"
	_catalog_status.text = "Catalog: %d %s   %d %s" % [
		effect_count, definition_word, unresolved.size(), issue_word]
	_catalog_issues.clear()
	_catalog_issues.visible = not unresolved.is_empty()
	for missing_name in unresolved:
		var idx := _catalog_issues.add_item(
				"Missing texture: %s" % missing_name, null, false)
		_catalog_issues.set_item_custom_fg_color(idx, Color(1.0, 0.4, 0.35))


func _on_group_selected(index: int) -> void:
	if index < 0 or index >= _ptl_list.item_count:
		return
	_selected_group_id = int(_ptl_list.get_item_metadata(index))
	_has_selected_group = true
	_show_group_detail(_groups_by_id.get(_selected_group_id, {}))


func _show_group_detail(group: Dictionary) -> void:
	if group.is_empty():
		_ptl_detail.text = "No live particle groups."
		return
	var group_name := String(group.get("name", "(unnamed)"))
	var group_id := int(group.get("id", 0))
	var source := String(group.get("source", "")).strip_edges()
	var lifetime := "continuous" if bool(group.get("forever", false)) else "one-shot"
	var lines := PackedStringArray([
		"%s  (group %d)" % [group_name, group_id],
		"Source: %s   Lifetime: %s" % [
			source if not source.is_empty() else "(unknown)", lifetime],
		"Totals: %d alive particles   %d drawn quads" % [
			int(group.get("_alive", 0)), int(group.get("_drawn", 0))],
	])
	var attention_reason := String(group.get("_attention_reason", ""))
	if not attention_reason.is_empty():
		lines.append("Attention: %s" % attention_reason)
	elif int(group.get("_alive", 0)) > 0:
		lines.append("Status: live particles are reaching the renderer.")
	else:
		lines.append("Status: idle; this group has no live particles.")
	var emitters: Array = group.get("emitters", [])
	if emitters.is_empty():
		lines.append("Emitters: none reported.")
	else:
		lines.append("Emitters:")
		for emitter_v in emitters:
			var emitter: Dictionary = emitter_v
			lines.append("  %s: %d alive, %d drawn" % [
				String(emitter.get("name", "(unnamed)")),
				int(emitter.get("alive", 0)),
				int(emitter.get("rendered", 0))])
	_ptl_detail.text = "\n".join(lines)


func _clear_live_data() -> void:
	_ptl_peak = 0
	_selected_group_id = -1
	_has_selected_group = false
	_groups_by_id.clear()
	_ptl_count_label.text = (
			"Live effects: 0 (peak 0)   Alive particles: 0   Drawn quads: 0")
	_ptl_list.clear()
	_ptl_detail.text = "No effect world is available."
	_catalog_status.text = "Catalog unavailable."
	_catalog_issues.clear()
	_catalog_issues.visible = false


func _sort_group_attention(a: Dictionary, b: Dictionary) -> bool:
	var a_attention := int(a.get("_attention", 0))
	var b_attention := int(b.get("_attention", 0))
	if a_attention != b_attention:
		return a_attention > b_attention
	var a_alive := int(a.get("_alive", 0))
	var b_alive := int(b.get("_alive", 0))
	if a_alive != b_alive:
		return a_alive > b_alive
	return int(a.get("id", 0)) < int(b.get("id", 0))


func _attention_score(group: Dictionary, alive: int, drawn: int) -> int:
	if alive > 0 and drawn == 0:
		return 300
	if (group.get("emitters", []) as Array).is_empty():
		return 200
	if String(group.get("source", "")).strip_edges().is_empty():
		return 100
	return 0


func _attention_reason(group: Dictionary, alive: int, drawn: int) -> String:
	if alive > 0 and drawn == 0:
		return "%d particles are alive but no quads are drawn." % alive
	if (group.get("emitters", []) as Array).is_empty():
		return "the group reports no emitters."
	if String(group.get("source", "")).strip_edges().is_empty():
		return "the group has no source file identity."
	return ""
