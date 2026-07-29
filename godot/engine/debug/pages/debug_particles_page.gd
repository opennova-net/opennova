class_name DebugParticlesPage
extends VBoxContainer
## The retail particle debug pages, mimicked (ptl-format-re.md §11 —
## Debug_DrawParticleStats @ 0x44c840 counts + entry list;
## Debug_DrawEffectBrowser @ 0x44c950 name + source file). Fed by its own
## effect-world source (the effect world is render-side, not the sim); the
## peak latch lives here like retail's debug global.

## Fired when "Hide particles" is toggled — the retail master particle switch,
## mimicked [orig: byte_24D261D — every effect facade no-ops when set]. Host
## acts on the re-emitted intent.
signal particles_hidden_toggled(hidden: bool)

## Fired when "Show effect boxes" is toggled. The host builds/frees the
## ParticleDebugView (per-emitter wireframe bounds + effect-name labels).
signal particle_boxes_toggled(enabled: bool)

var _effect_world_source := Callable()
var _ptl_count_label: Label
var _ptl_list: ItemList
var _ptl_hide_check: CheckBox
var _ptl_boxes_check: CheckBox
var _ptl_peak := 0


func _init() -> void:
	name = "Particles"
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

	_ptl_hide_check = CheckBox.new()
	_ptl_hide_check.name = "ParticlesHide"
	_ptl_hide_check.text = "Hide particles"
	_ptl_hide_check.tooltip_text = "Hide every particle effect (the retail master particle switch) — flip it to check whether an artifact is particles at all."
	_ptl_hide_check.button_pressed = false
	_ptl_hide_check.toggled.connect(_on_particles_hidden_toggled)
	add_child(_ptl_hide_check)

	_ptl_boxes_check = CheckBox.new()
	_ptl_boxes_check.name = "ParticlesBoxes"
	_ptl_boxes_check.text = "Show effect boxes"
	_ptl_boxes_check.tooltip_text = "Draw a red wireframe box (retail's debug box color) + effect name over every live emitter; effects with missing textures list them on the label."
	_ptl_boxes_check.button_pressed = false
	_ptl_boxes_check.toggled.connect(_on_particle_boxes_toggled)
	add_child(_ptl_boxes_check)


## The effect-world supplier: a Callable returning the live NovaEffectWorld
## (or null). Re-resolved every refresh — mission loads free and rebuild the
## effect world.
func set_effect_world_source(source: Callable) -> void:
	_effect_world_source = source


func refresh() -> void:
	var world = _effect_world_source.call() if _effect_world_source.is_valid() else null
	if world == null or not is_instance_valid(world):
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


func _on_particles_hidden_toggled(pressed: bool) -> void:
	particles_hidden_toggled.emit(pressed)


func _on_particle_boxes_toggled(pressed: bool) -> void:
	particle_boxes_toggled.emit(pressed)
