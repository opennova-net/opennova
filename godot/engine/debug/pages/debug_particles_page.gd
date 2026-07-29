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
var _ptl_peak := 0


func page_id() -> StringName:
	return &"Particles"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_ptl_count_label = Label.new()
	_ptl_count_label.name = "ParticleCounts"
	_ptl_count_label.text = "Current Particle Count:  0 / 0"
	add_child(_ptl_count_label)

	_ptl_list = ItemList.new()
	_ptl_list.name = "ParticleGroups"
	_ptl_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_ptl_list.focus_mode = Control.FOCUS_NONE
	add_child(_ptl_list)

	add_option_check(&"hide_particles")
	add_option_check(&"show_effect_boxes")


func refresh() -> void:
	var world := _ctx.effect_world()
	if world == null:
		_ptl_peak = 0
		_ptl_count_label.text = "Current Particle Count:  0 / 0"
		if _ptl_list.item_count > 0:
			_ptl_list.clear()
		return
	var report: Array = world.get_debug_group_report()
	var unresolved: PackedStringArray = (world.get_unresolved_texture_names()
			if world.has_method("get_unresolved_texture_names") else PackedStringArray())
	# Retail's checked facade reports the effect world's active circular-buffer
	# entries, not the particle instances living inside each emitter.
	var current := int(world.active_entry_count())
	# The retail peak latch, quirk included: it resets to zero whenever the
	# current count is zero [orig: Debug_DrawParticleStats @ 0x44c840].
	_ptl_peak = 0 if current == 0 else maxi(_ptl_peak, current)
	_ptl_count_label.text = (
			"Current Particle Count:  %d / %d   (groups %d, effects %d, catalog missing %d)"
			% [current, _ptl_peak, report.size(), world.effect_count(), unresolved.size()])
	_ptl_list.clear()
	for miss in unresolved:
		var miss_idx := _ptl_list.add_item(
				"CATALOG missing texture: %s" % miss, null, false)
		_ptl_list.set_item_custom_fg_color(miss_idx, Color(1.0, 0.35, 0.3))
	for group_v in report:
		var group: Dictionary = group_v
		var source := String(group.get("source", ""))
		var header := "%02d   %s" % [int(group.get("id", 0)), String(group.get("name", ""))]
		if not source.is_empty():
			header += "   [%s]" % source
		_ptl_list.add_item(header, null, false)
		for emitter_v in group.get("emitters", []):
			var emitter: Dictionary = emitter_v
			_ptl_list.add_item("      %s  alive %d  drawn %d" % [
					String(emitter.get("name", "")), int(emitter.get("alive", 0)),
					int(emitter.get("rendered", 0))], null, false)
