class_name TerrainEditorDocument
extends RefCounted

## Rejected inputs a user can act on (e.g. a wrong-format texture pick).
## The console line stays at the source (push_error); the owning TerrainEditor
## relays this to the shell toast. Headless holders can ignore it.
signal error_reported(message: String)

const HM_SIZE := NovaTerrainData.ATLAS_SIZE
const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")
const TerrainEditorSurfacePaint = preload("res://modtools/terrain/terrain_editor_surface_paint.gd")
const TILEINFO_STATE_NONE := ""
const TILEINFO_STATE_EXPLICIT := "explicit"
const TILEINFO_STATE_NEW := "new"
const TILEINFO_AUTHORED_FLAG_MASK := (
	NovaTerrainTileInfo.FLAG_FLIP_X
	| NovaTerrainTileInfo.FLAG_FLIP_Y
	| NovaTerrainTileInfo.FLAG_ROTATE_90
	| NovaTerrainTileInfo.FLAG_OUTLINE
)
const TILEINFO_TRANSFORM_FLAG_MASK := (
	NovaTerrainTileInfo.FLAG_FLIP_X
	| NovaTerrainTileInfo.FLAG_FLIP_Y
	| NovaTerrainTileInfo.FLAG_ROTATE_90
)
const TILEINFO_COMPOSABLE_FLAG_CANDIDATES := [
	0,
	NovaTerrainTileInfo.FLAG_FLIP_X,
	NovaTerrainTileInfo.FLAG_FLIP_Y,
	NovaTerrainTileInfo.FLAG_FLIP_X | NovaTerrainTileInfo.FLAG_FLIP_Y,
	NovaTerrainTileInfo.FLAG_ROTATE_90,
	NovaTerrainTileInfo.FLAG_FLIP_X | NovaTerrainTileInfo.FLAG_ROTATE_90,
	NovaTerrainTileInfo.FLAG_FLIP_Y | NovaTerrainTileInfo.FLAG_ROTATE_90,
	NovaTerrainTileInfo.FLAG_FLIP_X | NovaTerrainTileInfo.FLAG_FLIP_Y | NovaTerrainTileInfo.FLAG_ROTATE_90,
]
const TILEINFO_LOCAL_CORNERS := [
	Vector2(0.0, 0.0),
	Vector2(1.0, 0.0),
	Vector2(0.0, 1.0),
	Vector2(1.0, 1.0),
]
const FOLIAGE_CANONICAL_MATCHES := [254, 253, 252, 251]

var data: NovaTerrainData
var texture_files: Dictionary = {}
var heightmap_image: Image
var colormap_image: Image
var colormap_tex: ImageTexture
var blendmap_image: Image
var blendmap_tex: ImageTexture
var is_dirty: bool = false

var current_project_dir: String = ""
var current_trn_path: String = ""

var tileinfo_filename: String = ""
var tileinfo_resource: NovaTerrainTileInfo
var tileinfo_source_path: String = ""
var tileinfo_state: String = TILEINFO_STATE_NONE
var tileinfo_selected_index: int = -1
var tile_stamp_tile_index: int = 0
var tile_stamp_flags: int = 0
var surface_map: NovaTerrainSurfaceMap = null
var foliage_defs: Array[NovaTerrainFoliageDef] = []
var foliage_map: NovaTerrainFoliageMap
var selected_foliage_def_index: int = -1


func reset_paths() -> void:
	current_project_dir = ""
	current_trn_path = ""


func reset_trn_metadata() -> void:
	tileinfo_filename = ""
	tileinfo_resource = null
	tileinfo_source_path = ""
	tileinfo_state = TILEINFO_STATE_NONE
	tileinfo_selected_index = -1
	surface_map = null
	foliage_map = null
	foliage_defs = []
	selected_foliage_def_index = -1


func create_default_document(terrain_name: String, material: ShaderMaterial, sector_grid: PackedInt32Array) -> void:
	data = NovaTerrainData.new()
	data.set_terrain_name(terrain_name)
	data.set_sector_count(8)
	data.set_sector_rows(8)
	data.set_sector_grid(sector_grid)
	data.set_detail_density(128)
	data.set_detail_density2(8)
	data.set_origin_x(-4)
	data.set_origin_y(-4)
	texture_files = {}
	reset_trn_metadata()
	reset_paths()
	apply_default_visual_state(material)
	is_dirty = false


func apply_default_visual_state(material: ShaderMaterial, sync_data: bool = true) -> void:
	set_colormap_image(material, _create_color_image(HM_SIZE, HM_SIZE, Color(0.5, 0.5, 0.5, 1.0)), sync_data)
	set_blendmap_image(material, _create_color_image(HM_SIZE, HM_SIZE, Color(1.0, 0.0, 0.0, 1.0)), sync_data)
	TerrainEditorSlots.apply_default_slots(material, data, sync_data)
	if sync_data and data != null:
		for slot_id in ["charmap", "foliagemap"]:
			data.reset_pcx_slot_default(slot_id, HM_SIZE, HM_SIZE)
			TerrainEditorSlots.apply_slot_texture(material, data, slot_id, TerrainEditorSlots.get_slot_texture(data, slot_id))
		surface_map = _surface_map_from_slot(data.get_pcx_slot_state("charmap"))
		foliage_map = data.get_foliage_map()
	sync_material_from_data(material)


func get_slot_filename(slot_id: String) -> String:
	return String(texture_files.get(slot_id, ""))


func load_tileinfo_from_dir(base_dir: String) -> void:
	_clear_tileinfo_resource()
	if base_dir.is_empty():
		return

	for candidate in _tileinfo_load_candidates(base_dir):
		if not FileAccess.file_exists(candidate):
			continue
		var loaded := ResourceLoader.load(candidate, "NovaTerrainTileInfo", ResourceLoader.CACHE_MODE_IGNORE) as NovaTerrainTileInfo
		if loaded == null:
			continue
		tileinfo_resource = loaded
		tileinfo_source_path = candidate
		tileinfo_state = TILEINFO_STATE_EXPLICIT
		tileinfo_selected_index = -1
		return

	if not tileinfo_filename.is_empty():
		tileinfo_state = TILEINFO_STATE_EXPLICIT
		tileinfo_source_path = base_dir.path_join(tileinfo_filename + ".til")


func load_tileinfo(path: String) -> bool:
	if path.is_empty():
		return false
	var loaded := ResourceLoader.load(path, "NovaTerrainTileInfo", ResourceLoader.CACHE_MODE_IGNORE) as NovaTerrainTileInfo
	if loaded == null:
		return false
	tileinfo_resource = loaded
	tileinfo_source_path = path
	tileinfo_filename = _normalize_tileinfo_reference(path.get_file())
	tileinfo_state = TILEINFO_STATE_EXPLICIT
	tileinfo_selected_index = -1
	is_dirty = true
	return true


func new_tileinfo() -> void:
	tileinfo_resource = NovaTerrainTileInfo.new()
	tileinfo_filename = ""
	tileinfo_source_path = ""
	tileinfo_state = TILEINFO_STATE_NEW
	tileinfo_selected_index = -1
	is_dirty = true


func reset_tileinfo() -> void:
	var had_value := tileinfo_resource != null or not tileinfo_filename.is_empty() or not tileinfo_source_path.is_empty() or tileinfo_state != TILEINFO_STATE_NONE
	tileinfo_filename = ""
	_clear_tileinfo_resource()
	if had_value:
		is_dirty = true


func save_tileinfo(output_dir: String, terrain_name: String) -> Error:
	tileinfo_filename = _normalize_tileinfo_reference(tileinfo_filename)
	if tileinfo_resource == null:
		return OK

	var basename := tileinfo_filename
	if basename.is_empty():
		basename = _normalize_tileinfo_reference(terrain_name)
	if basename.is_empty():
		basename = "untitled"

	var save_path := output_dir.path_join(basename + ".til")
	var err := ResourceSaver.save(tileinfo_resource, save_path)
	if err != OK:
		return err

	tileinfo_filename = basename
	tileinfo_source_path = save_path
	tileinfo_state = TILEINFO_STATE_EXPLICIT
	return OK


func get_tileinfo_summary() -> Dictionary:
	var loaded := tileinfo_resource != null
	var entry_count := tileinfo_resource.get_entry_count() if loaded else 0
	var display_name := "(none)"
	if tileinfo_state == TILEINFO_STATE_NEW:
		display_name = (tileinfo_filename + ".til") if not tileinfo_filename.is_empty() else "(new)"
	elif not tileinfo_source_path.is_empty():
		display_name = tileinfo_source_path.get_file()
	elif not tileinfo_filename.is_empty():
		display_name = tileinfo_filename + ".til"

	return {
		"reference": tileinfo_filename,
		"display_name": display_name,
		"state": tileinfo_state,
		"loaded": loaded,
		"entry_count": entry_count,
		"selected_index": tileinfo_selected_index,
		"stamp_tile_index": tile_stamp_tile_index,
		"stamp_flags": tile_stamp_flags,
		"missing": tileinfo_resource == null and not tileinfo_filename.is_empty(),
	}


func has_tileinfo_resource() -> bool:
	return tileinfo_resource != null


func get_tileinfo_entries() -> Array:
	if tileinfo_resource == null:
		return []
	return tileinfo_resource.get_entries()


func get_tileinfo_entry(index: int) -> NovaTerrainTileEntry:
	if tileinfo_resource == null:
		return null
	return tileinfo_resource.get_entry(index)


func get_tileinfo_selected_index() -> int:
	return tileinfo_selected_index


func set_tileinfo_selected_index(index: int, adopt_stamp: bool = false) -> void:
	tileinfo_selected_index = index
	_clamp_tileinfo_selection()
	if adopt_stamp and tileinfo_selected_index >= 0:
		var entry := get_tileinfo_entry(tileinfo_selected_index)
		if entry != null:
			tile_stamp_tile_index = entry.get_tile_index()
			tile_stamp_flags = entry.get_flags()


func clear_tileinfo_selection() -> void:
	tileinfo_selected_index = -1


func get_tile_stamp_tile_index() -> int:
	return tile_stamp_tile_index


func set_tile_stamp_tile_index(value: int) -> void:
	tile_stamp_tile_index = clampi(value, 0, 255)


func get_tile_stamp_flags() -> int:
	return tile_stamp_flags


func set_tile_stamp_flags(value: int) -> void:
	tile_stamp_flags = _normalize_tileinfo_flags(value)


func find_tileinfo_entry_index_at_cell(cell_x: int, cell_z: int) -> int:
	if tileinfo_resource == null:
		return -1
	return tileinfo_resource.find_entry_index_at_cell(cell_x, cell_z)


func get_tileinfo_entry_indices_at_cell(cell_x: int, cell_z: int) -> PackedInt32Array:
	if tileinfo_resource == null:
		return PackedInt32Array()
	return tileinfo_resource.get_entry_indices_at_cell(cell_x, cell_z)


func stamp_tileinfo_cell(cell_x: int, cell_z: int, force_new: bool = false) -> Dictionary:
	if tileinfo_resource == null:
		return {"changed": false, "index": -1}

	_mark_tileinfo_owned_for_edit()
	var target_index := _resolve_tileinfo_replace_index(cell_x, cell_z, force_new)
	var replacement := _make_tile_stamp_entry(cell_x, cell_z)
	if replacement == null:
		return {"changed": false, "index": -1}

	if target_index >= 0:
		var current := get_tileinfo_entry(target_index)
		if _tile_entries_equal(current, replacement):
			tileinfo_selected_index = target_index
			return {"changed": false, "index": target_index}
		tileinfo_resource.set_entry(target_index, replacement)
	else:
		tileinfo_resource.add_entry(replacement)
		target_index = tileinfo_resource.get_entry_count() - 1

	tileinfo_selected_index = target_index
	is_dirty = true
	return {"changed": true, "index": target_index}


func apply_stamp_to_selected_tileinfo_entry() -> bool:
	if tileinfo_resource == null or tileinfo_selected_index < 0:
		return false

	var current := get_tileinfo_entry(tileinfo_selected_index)
	if current == null:
		return false

	_mark_tileinfo_owned_for_edit()
	var updated := NovaTerrainTileEntry.new()
	updated.set_x_fixed(current.get_x_fixed())
	updated.set_z_fixed(current.get_z_fixed())
	updated.set_tile_index(tile_stamp_tile_index)
	updated.set_flags(tile_stamp_flags)
	if _tile_entries_equal(current, updated):
		return false

	tileinfo_resource.set_entry(tileinfo_selected_index, updated)
	is_dirty = true
	return true


func set_selected_tileinfo_tile_index(value: int) -> bool:
	var current := get_tileinfo_entry(tileinfo_selected_index)
	if current == null:
		return false
	return _update_selected_tileinfo_entry(clampi(value, 0, 255), current.get_flags())


func set_selected_tileinfo_flags(value: int) -> bool:
	var current := get_tileinfo_entry(tileinfo_selected_index)
	if current == null:
		return false
	return _update_selected_tileinfo_entry(current.get_tile_index(), _normalize_tileinfo_flags(value))


func rotate_selected_tileinfo_clockwise() -> bool:
	var current := get_tileinfo_entry(tileinfo_selected_index)
	if current == null:
		return false
	return _update_selected_tileinfo_entry(
		current.get_tile_index(),
		_compose_tileinfo_flags(current.get_flags(), "rotate")
	)


func flip_selected_tileinfo_x() -> bool:
	var current := get_tileinfo_entry(tileinfo_selected_index)
	if current == null:
		return false
	return _update_selected_tileinfo_entry(
		current.get_tile_index(),
		_compose_tileinfo_flags(current.get_flags(), "flip_x")
	)


func flip_selected_tileinfo_y() -> bool:
	var current := get_tileinfo_entry(tileinfo_selected_index)
	if current == null:
		return false
	return _update_selected_tileinfo_entry(
		current.get_tile_index(),
		_compose_tileinfo_flags(current.get_flags(), "flip_y")
	)


func delete_selected_tileinfo_entry() -> bool:
	if tileinfo_resource == null or tileinfo_selected_index < 0:
		return false

	_mark_tileinfo_owned_for_edit()
	tileinfo_resource.remove_entry(tileinfo_selected_index)
	tileinfo_selected_index = -1
	is_dirty = true
	return true


func capture_tileinfo_history_state() -> Dictionary:
	var entries: Array = []
	if tileinfo_resource != null:
		for value in tileinfo_resource.get_entries():
			var entry := value as NovaTerrainTileEntry
			if entry != null:
				entries.append(entry.to_dictionary())
			elif value is Dictionary:
				entries.append((value as Dictionary).duplicate(true))

	return {
		"has_resource": tileinfo_resource != null,
		"entries": entries,
		"reference": tileinfo_filename,
		"source_path": tileinfo_source_path,
		"state": tileinfo_state,
		"selected_index": tileinfo_selected_index,
		"stamp_tile_index": tile_stamp_tile_index,
		"stamp_flags": tile_stamp_flags,
	}


func restore_tileinfo_history_state(state: Dictionary) -> void:
	tileinfo_filename = _normalize_tileinfo_reference(String(state.get("reference", "")))
	tileinfo_source_path = String(state.get("source_path", ""))
	tileinfo_state = String(state.get("state", TILEINFO_STATE_NONE))
	tile_stamp_tile_index = clampi(int(state.get("stamp_tile_index", tile_stamp_tile_index)), 0, 255)
	tile_stamp_flags = _normalize_tileinfo_flags(int(state.get("stamp_flags", tile_stamp_flags)))

	if bool(state.get("has_resource", false)):
		if tileinfo_resource == null:
			tileinfo_resource = NovaTerrainTileInfo.new()
		tileinfo_resource.set_entries(state.get("entries", []))
	else:
		tileinfo_resource = null

	tileinfo_selected_index = int(state.get("selected_index", -1))
	_clamp_tileinfo_selection()


func normalize_tileinfo_for_editor() -> int:
	if tileinfo_resource == null:
		return 0

	var selected := get_tileinfo_entry(tileinfo_selected_index)
	var selected_cell := Vector2i(-1, -1)
	if selected != null:
		selected_cell = Vector2i(selected.get_cell_x(), selected.get_cell_z())

	var normalized_entries: Array = []
	var seen_cells := {}
	var removed_count := 0
	for i in range(tileinfo_resource.get_entry_count() - 1, -1, -1):
		var entry := tileinfo_resource.get_entry(i)
		if entry == null:
			removed_count += 1
			continue
		var key := "%d,%d" % [entry.get_cell_x(), entry.get_cell_z()]
		if seen_cells.has(key):
			removed_count += 1
			continue
		seen_cells[key] = true
		normalized_entries.push_front(entry)

	if removed_count <= 0:
		return 0

	_mark_tileinfo_owned_for_edit()
	tileinfo_resource.set_entries(normalized_entries)
	if selected_cell.x >= 0 and selected_cell.y >= 0:
		tileinfo_selected_index = find_tileinfo_entry_index_at_cell(selected_cell.x, selected_cell.y)
	else:
		tileinfo_selected_index = -1
	_clamp_tileinfo_selection()
	is_dirty = true
	return removed_count


func get_foliage_map_preview_texture() -> Texture2D:
	if foliage_map == null:
		return null
	return foliage_map.get_preview_texture()


func get_surface_palette_bytes() -> PackedByteArray:
	if surface_map == null:
		return PackedByteArray()
	return surface_map.get_palette_bytes()


func get_foliage_def(index: int) -> NovaTerrainFoliageDef:
	if index < 0 or index >= foliage_defs.size():
		return null
	return foliage_defs[index]


func get_selected_foliage_def() -> NovaTerrainFoliageDef:
	return get_foliage_def(selected_foliage_def_index)


func set_selected_foliage_def_index(index: int) -> void:
	selected_foliage_def_index = index
	_clamp_foliage_selection()


func get_canonical_foliage_match(index: int) -> int:
	if index < 0 or index >= FOLIAGE_CANONICAL_MATCHES.size():
		return -1
	return int(FOLIAGE_CANONICAL_MATCHES[index])


func get_selected_foliage_paint_index() -> int:
	return get_canonical_foliage_match(selected_foliage_def_index)


func find_foliage_def_index_by_match(match_index: int) -> int:
	for i in foliage_defs.size():
		if get_canonical_foliage_match(i) == match_index:
			return i
	return -1


func capture_foliage_defs_history_state() -> Array:
	var snapshot: Array = []
	for def in foliage_defs:
		if def != null:
			snapshot.append(def.to_dictionary())
	return snapshot


func restore_foliage_defs_history_state(state: Array) -> void:
	foliage_defs = _clone_foliage_defs(state)
	_clamp_foliage_selection()


func capture_foliage_map_history_state() -> Dictionary:
	if foliage_map == null:
		return {}
	return foliage_map.to_dictionary().duplicate(true)


func restore_foliage_map_history_state(state: Dictionary) -> void:
	if state.is_empty():
		foliage_map = null
		return
	if foliage_map == null:
		foliage_map = NovaTerrainFoliageMap.new()
	foliage_map.load_from_dictionary(state)


func capture_surface_map_history_state() -> Dictionary:
	if surface_map == null:
		return {}
	return surface_map.to_dictionary().duplicate(true)


func restore_surface_map_history_state(material: ShaderMaterial, state: Dictionary) -> void:
	surface_map = _surface_map_from_slot(state)
	sync_surface_map_to_data(material)


func sync_surface_map_to_data(material: ShaderMaterial) -> void:
	if data == null or surface_map == null or surface_map.get_width() <= 0:
		return
	data.set_pcx_slot_state("charmap", surface_map.to_dictionary())
	if material != null:
		material.set_shader_parameter("u_charmap", TerrainEditorSlots.get_slot_texture(data, "charmap"))


func capture_foliage_editor_history_state() -> Dictionary:
	return {
		"defs": capture_foliage_defs_history_state(),
		"map": capture_foliage_map_history_state(),
		"selected_index": selected_foliage_def_index,
	}


func restore_foliage_editor_history_state(state: Dictionary) -> void:
	restore_foliage_defs_history_state(state.get("defs", []))
	restore_foliage_map_history_state(state.get("map", {}))
	selected_foliage_def_index = int(state.get("selected_index", selected_foliage_def_index))
	_clamp_foliage_selection()


func normalize_foliage_state_for_editor() -> void:
	var remap: Dictionary = {}
	var claimed_matches: Dictionary = {}
	for def in foliage_defs:
		if def == null:
			continue
		var old_match := int(def.match)
		if old_match > 0:
			claimed_matches[old_match] = int(claimed_matches.get(old_match, 0)) + 1

	for i in foliage_defs.size():
		var def := foliage_defs[i]
		if def == null:
			continue
		var canonical_match := get_canonical_foliage_match(i)
		var old_match := int(def.match)
		if canonical_match > 0 and old_match > 0 and old_match != canonical_match and int(claimed_matches.get(old_match, 0)) == 1:
			remap[old_match] = canonical_match

	if not remap.is_empty():
		_remap_foliage_map_indices(remap)
	_assign_canonical_foliage_matches()
	_clamp_foliage_selection()


func add_foliage_def() -> bool:
	if foliage_defs.size() >= FOLIAGE_CANONICAL_MATCHES.size():
		return false
	var def := NovaTerrainFoliageDef.new()
	def.color_lower = NovaTerrainFoliageDef.COLOR_MATCH_GROUND
	def.color_upper = NovaTerrainFoliageDef.COLOR_MATCH_GROUND
	foliage_defs.append(def)
	_assign_canonical_foliage_matches()
	selected_foliage_def_index = foliage_defs.size() - 1
	return true


func remove_foliage_def(index: int) -> bool:
	if index < 0 or index >= foliage_defs.size():
		return false

	var remap: Dictionary = {}
	var removed_match := get_canonical_foliage_match(index)
	if removed_match > 0:
		remap[removed_match] = 0
	for slot_index in range(index + 1, foliage_defs.size()):
		var from_match := get_canonical_foliage_match(slot_index)
		var to_match := get_canonical_foliage_match(slot_index - 1)
		if from_match > 0 and to_match > 0:
			remap[from_match] = to_match
	if not remap.is_empty():
		_remap_foliage_map_indices(remap)

	foliage_defs.remove_at(index)
	_assign_canonical_foliage_matches()
	if selected_foliage_def_index == index:
		selected_foliage_def_index = mini(index, foliage_defs.size() - 1)
	elif selected_foliage_def_index > index:
		selected_foliage_def_index -= 1
	_clamp_foliage_selection()
	return true


func capture_trn_resource(resource: NovaTerrainData) -> void:
	if resource == null:
		return
	texture_files = {}
	for slot_id in TerrainEditorSlots.get_slot_ids():
		var slot: Dictionary = TerrainEditorSlots.get_slot(String(slot_id))
		var trn_key := String(slot.get("trn_key", ""))
		if trn_key.is_empty():
			continue
		var filename := String(resource.get_trn_texture_filename(trn_key))
		if not filename.is_empty():
			texture_files[String(slot_id)] = filename
	tileinfo_filename = _normalize_tileinfo_reference(String(resource.get_tileinfo_filename()))
	_clear_tileinfo_resource()
	surface_map = _surface_map_from_slot(resource.get_pcx_slot_state("charmap"))
	foliage_defs = _clone_foliage_defs(resource.get_foliage_defs())
	foliage_map = _clone_foliage_map(resource.get_foliage_map())
	normalize_foliage_state_for_editor()
	_clamp_foliage_selection()


func apply_loaded_textures_from_data(material: ShaderMaterial) -> void:
	if data == null:
		return
	var source_dir := String(data.get_trn_path()).get_base_dir()
	var loaded_colormap := _load_source_image(source_dir, String(data.get_trn_texture_filename("colormap")))
	if loaded_colormap == null and data.get_colormap():
		loaded_colormap = TerrainEditorSlots.texture_to_image(data.get_colormap())
	if loaded_colormap != null:
		set_colormap_image(material, loaded_colormap)

	var loaded_blendmap := _load_source_image(source_dir, String(data.get_trn_texture_filename("detailblendmap")))
	if loaded_blendmap == null and data.get_detailblendmap():
		loaded_blendmap = TerrainEditorSlots.texture_to_image(data.get_detailblendmap())
	if loaded_blendmap != null:
		set_blendmap_image(material, loaded_blendmap)

	for slot_id in TerrainEditorSlots.get_slot_ids():
		var slot: Dictionary = TerrainEditorSlots.get_slot(String(slot_id))
		var trn_key := String(slot.get("trn_key", ""))
		if String(slot.get("format", "")) != "pcx_paletted" and not trn_key.is_empty():
			var loaded_slot_image := _load_source_image(source_dir, String(data.get_trn_texture_filename(trn_key)))
			if loaded_slot_image != null:
				TerrainEditorSlots.apply_slot_image(material, data, String(slot_id), loaded_slot_image)
				continue
		var texture := TerrainEditorSlots.get_slot_texture(data, String(slot_id))
		if texture != null or slot_id == "detailmap" or slot_id == "detailmap2" or slot_id == "detailmapdist2":
			TerrainEditorSlots.apply_slot_texture(material, data, String(slot_id), texture)
	surface_map = _surface_map_from_slot(data.get_pcx_slot_state("charmap"))
	sync_material_from_data(material)


func load_texture_slot(material: ShaderMaterial, slot_id: String, path: String) -> bool:
	if data == null:
		return false
	var slot: Dictionary = TerrainEditorSlots.get_slot(slot_id)
	if String(slot.get("format", "")) == "pcx_paletted":
		if path.get_extension().to_lower() != "pcx":
			var message := "%s slot expects a .pcx file; got %s" % [slot_id, path]
			push_error(message)
			error_reported.emit(message)
			return false
		var err: int = data.import_pcx_slot(slot_id, path)
		if err != OK:
			return false
		material.set_shader_parameter(String(slot["uniform"]), TerrainEditorSlots.get_slot_texture(data, slot_id))
		if slot_id == "charmap":
			surface_map = _surface_map_from_slot(data.get_pcx_slot_state("charmap"))
		if slot_id == "foliagemap":
			foliage_map = _clone_foliage_map(data.get_foliage_map())
		texture_files[slot_id] = path.get_file()
		is_dirty = true
		return true
	var image := TerrainEditorSlots.load_image_from_file(path)
	if image == null:
		return false
	TerrainEditorSlots.apply_slot_image(material, data, slot_id, image)
	texture_files[slot_id] = path.get_file()
	is_dirty = true
	return true


func reset_texture_slot(material: ShaderMaterial, slot_id: String) -> void:
	if data == null:
		return
	var slot: Dictionary = TerrainEditorSlots.get_slot(slot_id)
	if String(slot.get("format", "")) == "pcx_paletted":
		data.reset_pcx_slot_default(slot_id, HM_SIZE, HM_SIZE)
		TerrainEditorSlots.apply_slot_texture(material, data, slot_id, TerrainEditorSlots.get_slot_texture(data, slot_id))
		if slot_id == "charmap":
			surface_map = _surface_map_from_slot(data.get_pcx_slot_state("charmap"))
		if slot_id == "foliagemap":
			foliage_map = _clone_foliage_map(data.get_foliage_map())
	else:
		TerrainEditorSlots.apply_default_slot(material, data, slot_id)
	texture_files.erase(slot_id)
	is_dirty = true


func save_texture_assets(material: ShaderMaterial, output_dir: String, terrain_name: String) -> Error:
	# Source the editable buffers from NovaTerrainData (their owner); fall back to
	# the local refs if data is absent. They are the same Image objects.
	var colormap_source := colormap_image
	var blendmap_source := blendmap_image
	if data != null:
		if data.get_colormap_image() != null:
			colormap_source = data.get_colormap_image()
		if data.get_blendmap_image() != null:
			blendmap_source = data.get_blendmap_image()
	var err := NovaTerrainBuilder.save_image_tga(colormap_source, output_dir + "/" + terrain_name + "_c.tga")
	if err != OK:
		return err
	err = NovaTerrainBuilder.save_image_tga(blendmap_source, output_dir + "/" + terrain_name + "_d1.tga")
	if err != OK:
		return err
	normalize_foliage_state_for_editor()
	if foliage_map != null:
		data.set_foliage_map(foliage_map)

	for slot_id in TerrainEditorSlots.get_slot_ids():
		var slot: Dictionary = TerrainEditorSlots.get_slot(String(slot_id))
		var filename := get_slot_filename(String(slot_id))
		if filename.is_empty():
			filename = TerrainEditorSlots.get_export_filename(String(slot_id), terrain_name)
		var output_path := output_dir + "/" + filename
		if String(slot.get("format", "")) == "pcx_paletted":
			err = data.save_pcx_slot(String(slot_id), output_path)
		else:
			var texture := TerrainEditorSlots.get_slot_texture(data, String(slot_id))
			var image := TerrainEditorSlots.texture_to_image(texture)
			if image == null:
				if TerrainEditorSlots.uses_placeholder_default(String(slot_id)):
					image = TerrainEditorSlots.create_export_placeholder_image()
				else:
					texture_files.erase(String(slot_id))
					continue
			if _tga_bpp_for_export(String(slot_id), filename) == 24:
				err = NovaTerrainBuilder.save_image_tga24(image, output_path)
			else:
				err = NovaTerrainBuilder.save_image_tga(image, output_path)
		if err != OK:
			return err
		texture_files[String(slot_id)] = filename

	return OK


func prepare_data_for_trn_save(terrain_name: String, polydata_filename: String) -> void:
	if data == null:
		return
	normalize_foliage_state_for_editor()
	data.set_terrain_name(terrain_name)
	data.set_polydata_filename(polydata_filename)
	data.set_tileinfo_filename(_normalize_tileinfo_reference(tileinfo_filename))
	if foliage_map != null:
		data.set_foliage_map(foliage_map)
	data.set_foliage_defs(_clone_foliage_defs(foliage_defs))
	data.set_trn_texture_filename("colormap", terrain_name + "_c.tga")
	data.set_trn_texture_filename("detailblendmap", terrain_name + "_d1.tga")
	for slot_id in TerrainEditorSlots.get_slot_ids():
		var slot: Dictionary = TerrainEditorSlots.get_slot(String(slot_id))
		var trn_key := String(slot.get("trn_key", ""))
		if trn_key.is_empty():
			continue
		data.set_trn_texture_filename(trn_key, get_slot_filename(String(slot_id)))


func build_heightmap_from_data() -> Image:
	var raw_bytes: PackedByteArray = data.get_depth_raw16()
	if raw_bytes.size() == HM_SIZE * HM_SIZE * 2:
		return build_heightmap_from_raw16(raw_bytes)
	var image := Image.create(HM_SIZE, HM_SIZE, false, Image.FORMAT_RF)
	for z in HM_SIZE:
		for x in HM_SIZE:
			image.set_pixel(x, z, Color(data.get_height(Vector3(x, 0, z)), 0, 0, 1))
	return image


func build_heightmap_from_raw16(raw_bytes: PackedByteArray) -> Image:
	if data != null:
		var image: Image = data.heightmap_image_from_raw16(raw_bytes)
		if image != null:
			return image
	return Image.create(HM_SIZE, HM_SIZE, false, Image.FORMAT_RF)


func set_heightmap_image(image: Image) -> void:
	heightmap_image = image
	if data != null:
		# NovaTerrainData owns the editable depth: hand it the same Image so brush
		# edits (which mutate this object in place) keep get_depth_raw16 current.
		data.set_heightmap_image(image)


func set_colormap_image(material: ShaderMaterial, image: Image, sync_data: bool = true) -> void:
	colormap_image = image
	colormap_tex = ImageTexture.create_from_image(colormap_image)
	material.set_shader_parameter("u_colormap", colormap_tex)
	if sync_data and data:
		# NovaTerrainData owns the editable buffer (same Image object the brush
		# mutates) plus the derived display/runtime Texture2D.
		data.set_colormap_image(colormap_image)
		data.set_colormap(colormap_tex)


func set_blendmap_image(material: ShaderMaterial, image: Image, sync_data: bool = true) -> void:
	blendmap_image = image
	blendmap_tex = ImageTexture.create_from_image(blendmap_image)
	material.set_shader_parameter("u_blendmap", blendmap_tex)
	if sync_data and data:
		data.set_blendmap_image(blendmap_image)
		data.set_detailblendmap(blendmap_tex)


func sync_material_from_data(material: ShaderMaterial) -> void:
	if data == null:
		return
	material.set_shader_parameter("u_detail_density", float(data.get_detail_density()))


func cdep_ranges_valid(image: Image) -> bool:
	# CDEP's 4-bit bits_per_delta field caps each 256-pixel horizontal block at
	# a 32767-raw-unit range. The brush enforces this live; this bake-time guard
	# makes a corrupt CPT impossible regardless of how the heightmap got into
	# this state. The raw16 range scan lives in C++ (NovaTerrainData ->
	# libs/terrain/cdep_constraint); this stays GDScript as export policy.
	if image == null:
		return false
	if data == null:
		return true
	var violations := data.cdep_count_violations()
	if violations > 0:
		push_error("CDEP per-block range exceeded in %d block(s) (> 32767 raw units)" % violations)
		return false
	return true


func get_terrain_name() -> String:
	if data and not data.get_terrain_name().is_empty():
		return data.get_terrain_name()
	return "untitled"


func _load_source_image(base_dir: String, filename: String) -> Image:
	var path := _resolve_existing_file(base_dir, filename)
	if path.is_empty():
		return null
	return TerrainEditorSlots.load_image_from_file(path)


func _tga_bpp_for_export(slot_id: String, filename: String) -> int:
	if filename.get_extension().to_lower() == "tga" and not current_trn_path.is_empty():
		var source_path := _resolve_existing_file(current_trn_path.get_base_dir(), filename)
		var source_bpp := _read_tga_bpp(source_path)
		if source_bpp == 24 or source_bpp == 32:
			return source_bpp
	return int(TerrainEditorSlots.get_slot(slot_id).get("bpp", 32))


func _read_tga_bpp(path: String) -> int:
	if path.is_empty():
		return 0
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null or file.get_length() < 18:
		return 0
	file.seek(16)
	var bpp := file.get_8()
	file.close()
	return bpp


func _resolve_existing_file(base_dir: String, filename: String) -> String:
	# Shared case-insensitive resolver — same primitive textures/models use.
	return NovaPaths.resolve_file(base_dir, filename)


func _clear_tileinfo_resource() -> void:
	tileinfo_resource = null
	tileinfo_source_path = ""
	tileinfo_state = TILEINFO_STATE_NONE
	tileinfo_selected_index = -1


func _clamp_tileinfo_selection() -> void:
	if tileinfo_resource == null:
		tileinfo_selected_index = -1
		return
	tileinfo_selected_index = clampi(tileinfo_selected_index, -1, tileinfo_resource.get_entry_count() - 1)


func _mark_tileinfo_owned_for_edit() -> void:
	if tileinfo_resource == null:
		return
	if tileinfo_state == TILEINFO_STATE_NONE:
		tileinfo_state = TILEINFO_STATE_NEW


func _resolve_tileinfo_replace_index(cell_x: int, cell_z: int, force_new: bool = false) -> int:
	if tileinfo_resource == null:
		return -1

	if tileinfo_selected_index >= 0:
		var selected := get_tileinfo_entry(tileinfo_selected_index)
		if selected != null and selected.get_cell_x() == cell_x and selected.get_cell_z() == cell_z:
			return tileinfo_selected_index

	var indices := tileinfo_resource.get_entry_indices_at_cell(cell_x, cell_z)
	if indices.is_empty():
		return -1
	return int(indices[indices.size() - 1])


func _make_tile_stamp_entry(cell_x: int, cell_z: int) -> NovaTerrainTileEntry:
	var entry := NovaTerrainTileEntry.new()
	entry.set_cell(cell_x, cell_z)
	entry.set_tile_index(tile_stamp_tile_index)
	entry.set_flags(tile_stamp_flags)
	return entry


func _tile_entries_equal(a: NovaTerrainTileEntry, b: NovaTerrainTileEntry) -> bool:
	if a == null or b == null:
		return false
	return (
		a.get_x_fixed() == b.get_x_fixed()
		and a.get_z_fixed() == b.get_z_fixed()
		and a.get_tile_index() == b.get_tile_index()
		and a.get_flags() == b.get_flags()
	)


func _normalize_tileinfo_flags(value: int) -> int:
	return clampi(value, 0, 255) & TILEINFO_AUTHORED_FLAG_MASK


func _update_selected_tileinfo_entry(tile_index: int, flags: int) -> bool:
	if tileinfo_resource == null or tileinfo_selected_index < 0:
		return false

	var current := get_tileinfo_entry(tileinfo_selected_index)
	if current == null:
		return false

	var normalized_tile_index := clampi(tile_index, 0, 255)
	var normalized_flags := _normalize_tileinfo_flags(flags)
	if current.get_tile_index() == normalized_tile_index and current.get_flags() == normalized_flags:
		return false

	_mark_tileinfo_owned_for_edit()
	var updated := NovaTerrainTileEntry.new()
	updated.set_x_fixed(current.get_x_fixed())
	updated.set_z_fixed(current.get_z_fixed())
	updated.set_tile_index(normalized_tile_index)
	updated.set_flags(normalized_flags)
	tileinfo_resource.set_entry(tileinfo_selected_index, updated)
	is_dirty = true
	return true


func _compose_tileinfo_flags(flags: int, operation: String) -> int:
	var normalized_flags := _normalize_tileinfo_flags(flags)
	var preserved_flags := normalized_flags & ~TILEINFO_TRANSFORM_FLAG_MASK
	var base_flags := normalized_flags & TILEINFO_TRANSFORM_FLAG_MASK
	var target_uvs: Array[Vector2] = []
	for corner in TILEINFO_LOCAL_CORNERS:
		target_uvs.append(NovaTerrainTileInfo.transform_local_uv(_apply_tileinfo_local_operation(corner, operation), base_flags))

	for candidate in TILEINFO_COMPOSABLE_FLAG_CANDIDATES:
		if _tileinfo_uvs_match(target_uvs, candidate):
			return preserved_flags | int(candidate)
	return normalized_flags


func _tileinfo_uvs_match(target_uvs: Array[Vector2], candidate_flags: int) -> bool:
	for i in TILEINFO_LOCAL_CORNERS.size():
		var candidate_uv: Vector2 = NovaTerrainTileInfo.transform_local_uv(TILEINFO_LOCAL_CORNERS[i], candidate_flags)
		if candidate_uv.distance_squared_to(target_uvs[i]) > 0.0001:
			return false
	return true


func _apply_tileinfo_local_operation(value: Vector2, operation: String) -> Vector2:
	match operation:
		"rotate":
			return Vector2(value.y, 1.0 - value.x)
		"flip_x":
			return Vector2(1.0 - value.x, value.y)
		"flip_y":
			return Vector2(value.x, 1.0 - value.y)
		_:
			return value


func _normalize_tileinfo_reference(value: String) -> String:
	var reference := value.strip_edges()
	if reference.is_empty():
		return ""
	reference = reference.get_file()
	if reference.get_extension().to_lower() == "til":
		reference = reference.get_basename().get_file()
	return reference


func _tileinfo_load_candidates(base_dir: String) -> PackedStringArray:
	var candidates := PackedStringArray()
	if tileinfo_filename.is_empty():
		return candidates

	var exact_name := tileinfo_filename.get_file()
	if not exact_name.is_empty():
		candidates.append(base_dir.path_join(exact_name))

	var normalized_path := base_dir.path_join(_normalize_tileinfo_reference(tileinfo_filename) + ".til")
	if candidates.find(normalized_path) == -1:
		candidates.append(normalized_path)
	return candidates


func _clone_foliage_defs(values: Array) -> Array[NovaTerrainFoliageDef]:
	var out: Array[NovaTerrainFoliageDef] = []
	for value in values:
		var def := _clone_foliage_def(value)
		if def != null:
			out.append(def)
	return out


func _clone_foliage_def(value: Variant) -> NovaTerrainFoliageDef:
	if value is NovaTerrainFoliageDef:
		var source := value as NovaTerrainFoliageDef
		var copy := NovaTerrainFoliageDef.new()
		copy.graphic = source.graphic
		copy.color_lower = source.color_lower
		copy.color_upper = source.color_upper
		copy.match = source.match
		copy.attrib_flags = source.attrib_flags
		return copy
	if value is Dictionary:
		var dict := value as Dictionary
		var copy := NovaTerrainFoliageDef.new()
		copy.graphic = String(dict.get("graphic", ""))
		copy.color_lower = int(dict.get("color_lower", NovaTerrainFoliageDef.COLOR_MATCH_GROUND))
		copy.color_upper = int(dict.get("color_upper", NovaTerrainFoliageDef.COLOR_MATCH_GROUND))
		copy.match = int(dict.get("match", -1))
		var attrib_flags := int(dict.get("attrib_flags", 0))
		if bool(dict.get("shadow", false)):
			attrib_flags |= NovaTerrainFoliageDef.ATTRIB_SHADOW
		if bool(dict.get("force_on", false)):
			attrib_flags |= NovaTerrainFoliageDef.ATTRIB_FORCE_ON
		copy.attrib_flags = attrib_flags
		return copy
	return null


func _clone_foliage_map(source: NovaTerrainFoliageMap) -> NovaTerrainFoliageMap:
	if source == null:
		return null
	var copy := NovaTerrainFoliageMap.new()
	copy.load_from_dictionary(source.to_dictionary())
	return copy


func _surface_map_from_slot(state: Dictionary) -> NovaTerrainSurfaceMap:
	if state.is_empty():
		return null
	var map := NovaTerrainSurfaceMap.new()
	map.load_from_dictionary(state)
	return map


func _assign_canonical_foliage_matches() -> void:
	for i in foliage_defs.size():
		var def := foliage_defs[i]
		if def != null:
			def.match = get_canonical_foliage_match(i)


func _remap_foliage_map_indices(remap: Dictionary) -> bool:
	if foliage_map == null or remap.is_empty():
		return false
	return foliage_map.remap_indices(remap) > 0


func _clamp_foliage_selection() -> void:
	selected_foliage_def_index = clampi(selected_foliage_def_index, -1, foliage_defs.size() - 1)


func _create_color_image(width: int, height: int, color: Color) -> Image:
	var image := Image.create(width, height, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image
