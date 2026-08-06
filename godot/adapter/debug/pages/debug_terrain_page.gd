class_name DebugTerrainPage
extends NovaDebugPage
## Terrain & foliage: what the terrain renderer decided this frame — active /
## visible patch counts, the per-LOD split, the quadtree traversal verdicts —
## and the foliage dispatcher's frame stats, with the render knobs beside
## them. "Hide foliage" rides the NovaDebugOptions registry (the host owns the
## dispatcher node); the draw-mode and terrain-detail knobs poke the terrain
## node through the shared debug catalog. Their bespoke UI is registered with
## the page's control-state bridge so target availability and write policy
## stay identical to the generic controls.

var _patches_label: Label
var _traversal_label: Label
var _foliage_label: Label
var _mode_option: OptionButton
var _detail_slider: HSlider
var _detail_value: Label


func page_id() -> StringName:
	return &"Terrain"


func page_title() -> String:
	return "Terrain & foliage"


func page_category() -> StringName:
	return CATEGORY_WORLD


func _build() -> void:
	add_theme_constant_override("separation", 6)

	_patches_label = Label.new()
	_patches_label.name = "TerrainPatches"
	_patches_label.text = "No terrain."
	_patches_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_patches_label)

	_traversal_label = Label.new()
	_traversal_label.name = "TerrainTraversal"
	_traversal_label.text = ""
	_traversal_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_traversal_label)

	_foliage_label = Label.new()
	_foliage_label.name = "FoliageStats"
	_foliage_label.text = "No foliage."
	_foliage_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_foliage_label)

	var mode_row := HBoxContainer.new()
	mode_row.name = "TerrainModeRow"
	add_child(mode_row)
	var mode_label := Label.new()
	mode_label.text = "Draw mode"
	mode_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	mode_row.add_child(mode_label)
	_mode_option = OptionButton.new()
	_mode_option.name = "TerrainDrawMode"
	_mode_option.focus_mode = Control.FOCUS_NONE
	_mode_option.tooltip_text = "Color the terrain by what the renderer decided — detail levels, sectors, surface angle or raw height — instead of its textures."
	# The NovaTerrain debug_mode enum order, verbatim.
	for mode_name in ["Normal", "Detail levels", "Sector colors", "Surface angle", "Height map"]:
		_mode_option.add_item(mode_name)
	_mode_option.item_selected.connect(_on_mode_selected)
	mode_row.add_child(_mode_option)
	_debug_controls[&"terrain_draw_mode"] = _mode_option

	var detail_row := HBoxContainer.new()
	detail_row.name = "TerrainDetailRow"
	add_child(detail_row)
	var detail_label := Label.new()
	detail_label.text = "Terrain detail"
	detail_row.add_child(detail_label)
	_detail_slider = HSlider.new()
	_detail_slider.name = "TerrainDetail"
	_detail_slider.min_value = 0.1
	_detail_slider.max_value = 4.0
	_detail_slider.step = 0.1
	_detail_slider.value = 1.0
	_detail_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail_slider.tooltip_text = "Scale how aggressively the terrain refines toward the camera (1.0 = normal)."
	_detail_slider.value_changed.connect(_on_detail_changed)
	detail_row.add_child(_detail_slider)
	_debug_controls[&"terrain_lod_quality"] = _detail_slider
	_detail_value = Label.new()
	_detail_value.name = "TerrainDetailValue"
	_detail_value.text = "1.0"
	detail_row.add_child(_detail_value)

	add_option_check(&"hide_foliage")

	var culling_label := Label.new()
	culling_label.text = "Traversal overrides"
	add_child(culling_label)
	for control_id in [
			&"terrain_no_frustum",
			&"terrain_no_nearfar",
			&"terrain_no_sideplanes",
			&"terrain_no_partial_subdiv",
			&"terrain_force_leaves",
			&"terrain_force_lod0",
	]:
		add_debug_control(control_id)


func refresh() -> void:
	var terrain := _terrain()
	if terrain == null:
		_patches_label.text = "No terrain."
		_traversal_label.text = ""
	else:
		var lods: PackedInt32Array = terrain.get_lod_distribution()
		var lod_bits := PackedStringArray()
		for i in range(lods.size()):
			if lods[i] > 0:
				lod_bits.append("L%d %d" % [i, lods[i]])
		_patches_label.text = "Patches: %d active   %d visible\nDetail split: %s" % [
			int(terrain.get_patches_active()), int(terrain.get_visible_patch_count()),
			"  ".join(lod_bits) if not lod_bits.is_empty() else "-"]
		var t: Dictionary = terrain.get_traversal_stats()
		_traversal_label.text = (
				"Traversal: %d nodes   culled near/far %d  L %d  R %d  B %d  T %d\n"
				+ "partial subdiv %d   budget drops %d   emits %d leaf / %d node   LOD fallbacks %d") % [
			int(t.get("nodes_visited", 0)), int(t.get("rej_nearfar", 0)),
			int(t.get("rej_left", 0)), int(t.get("rej_right", 0)),
			int(t.get("rej_bottom", 0)), int(t.get("rej_top", 0)),
			int(t.get("partial_subdiv", 0)), int(t.get("budget_drops", 0)),
			int(t.get("leaf_emits", 0)), int(t.get("nonleaf_emits", 0)),
			int(t.get("lod_fallbacks", 0))]
		# Keep the knobs mirroring the live node (a probe or the editor may
		# have poked it); select()/set_value_no_signal never re-fire.
		_mode_option.select(int(terrain.get_debug_mode()))
		_detail_slider.set_value_no_signal(float(terrain.get_lod_quality()))
		_detail_value.text = "%.1f" % float(terrain.get_lod_quality())

	var dispatcher := _dispatcher()
	if dispatcher == null:
		_foliage_label.text = "No foliage."
		return
	var f: Dictionary = dispatcher.get_frame_stats()
	_foliage_label.text = (
			"Foliage: %d placed   cells %d   near %d + far %d   silhouettes %d   batches %d\n"
			+ "cell cache: %d hits   %d misses   %d resident   %d evicted") % [
		int(dispatcher.get_total_instances()), int(f.get("detail_cells", 0)),
		int(f.get("detail_high_instances", 0)), int(f.get("detail_low_instances", 0)),
		int(f.get("silhouette_instances", 0)), int(f.get("render_batches", 0)),
		int(f.get("detail_cache_hits", 0)), int(f.get("detail_cache_misses", 0)),
		int(f.get("detail_cache_residents", 0)), int(f.get("detail_cache_evictions", 0))]


func _terrain() -> Object:
	var world := _ctx.world()
	if world == null or not world.has_method("get_terrain_node"):
		return null
	var terrain: Variant = world.get_terrain_node()
	if terrain is Object and is_instance_valid(terrain):
		return terrain
	return null


func _dispatcher() -> Object:
	var world := _ctx.world()
	if world == null or not world.has_method("get_foliage_dispatcher"):
		return null
	var dispatcher: Variant = world.get_foliage_dispatcher()
	if dispatcher is Object and is_instance_valid(dispatcher):
		return dispatcher
	return null


func _on_mode_selected(index: int) -> void:
	if _ctx.session != null and _ctx.session.has_control(&"terrain_draw_mode"):
		_ctx.session.set_control_value(&"terrain_draw_mode", index)
		return
	var terrain := _terrain()
	if terrain != null:
		terrain.set_debug_mode(index)


func _on_detail_changed(value: float) -> void:
	_detail_value.text = "%.1f" % value
	if _ctx.session != null and _ctx.session.has_control(&"terrain_lod_quality"):
		_ctx.session.set_control_value(&"terrain_lod_quality", value)
		return
	var terrain := _terrain()
	if terrain != null:
		terrain.set_lod_quality(value)
