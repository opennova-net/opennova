extends "res://modtools/terrain/terrain_editor_section.gd"

# Project IO + export for the Terrain workspace (quality slice W4-6d):
# new/open/save of project terrains, texture-slot load/reset, the CDEP
# violation check/fix pair, and the DFX-JO/BHD export path (sync build +
# begin/poll/finish job) with its texture/tileinfo sidecar saves. Moved
# verbatim from terrain_editor.gd; ALL state stays on the editor, reached
# through `_te`.

const TerrainEditorSurfacePaint = preload("res://modtools/terrain/terrain_editor_surface_paint.gd")


func load_texture_slot(slot_id: String, path: String) -> void:
	if _te.is_export_running() or not _te._data:
		return
	if _te._document.load_texture_slot(_te._get_material(), slot_id, path):
		_te.is_dirty = true
		_te._brush_ops._refresh_surface_inputs_for_texture_slot(slot_id)
		if slot_id == "tilestrip":
			_te._mark_tile_overlay_dirty()
		elif slot_id == "foliagemap":
			_te._mark_foliage_preview_dirty()
		_te._mark_ui_state_changed()


func reset_texture_slot(slot_id: String) -> void:
	if _te.is_export_running() or not _te._data:
		return
	_te._document.reset_texture_slot(_te._get_material(), slot_id)
	_te.is_dirty = true
	_te._brush_ops._refresh_surface_inputs_for_texture_slot(slot_id)
	if slot_id == "tilestrip":
		_te._mark_tile_overlay_dirty()
	elif slot_id == "foliagemap":
		_te._mark_foliage_preview_dirty()
	_te._mark_ui_state_changed()


func new_terrain() -> void:
	if _te.is_export_running():
		return
	_te._brush_session.clear_history()
	_te.clear_clone_source()
	_create_default_document("untitled")
	_te._set_heightmap_image(_create_heightmap_image(_te.DEFAULT_HEIGHT))
	_te._sync_sector_layout(true)
	_te._brush_ops._ensure_surface_inputs_rebuilt()
	_te._mark_foliage_preview_dirty()
	_te.is_dirty = false
	_te._mark_ui_state_changed()


func open_trn(trn_path: String, timeline: PerfTimeline = null) -> Error:
	if _te.is_export_running():
		return ERR_BUSY
	var resources: NovaResourceRoot = _te.get_resource_root()
	if not FileAccess.file_exists(trn_path) and resources != null and resources.has_file(trn_path):
		return _open_trn_from_resource_root(resources, trn_path, timeline)
	_te._brush_session.clear_history()
	_te.clear_clone_source()
	PerfTimeline.span_on(timeline, "trn_data")
	_te._data = NovaTerrainData.new()
	_te._data.set_trn_path(trn_path)
	var err: Error = _te._data.load()
	if err != OK:
		return err
	PerfTimeline.end_on(timeline)

	_te.texture_files = {}
	_apply_default_visual_state(false)

	# Prefer a sibling <name>_depth.raw if present (project mode — depth.raw
	# is the authoring source of truth). Fall back to building the heightmap
	# from the CPT (import mode — original game assets have a CPT but no
	# depth.raw). With CPT optional, a project .trn may have neither; in
	# that case _build_heightmap_from_data returns a zero image.
	var dir_path := trn_path.get_base_dir()
	var depth_path: String = dir_path + "/" + _te._data.get_terrain_name() + "_depth.raw"
	var depth_bytes: PackedByteArray
	PerfTimeline.span_on(timeline, "heightmap")
	if FileAccess.file_exists(depth_path):
		var depth_file := FileAccess.open(depth_path, FileAccess.READ)
		if depth_file:
			depth_bytes = depth_file.get_buffer(_te.HM_SIZE * _te.HM_SIZE * 2)
			depth_file.close()
	if depth_bytes.size() == _te.HM_SIZE * _te.HM_SIZE * 2:
		_te._set_heightmap_image(_build_heightmap_from_raw16(depth_bytes))
	else:
		_te._set_heightmap_image(_build_heightmap_from_data())
	PerfTimeline.end_on(timeline)

	PerfTimeline.span_on(timeline, "textures")
	_apply_loaded_textures_from_data()
	PerfTimeline.end_on(timeline)
	PerfTimeline.span_on(timeline, "sectors")
	var normalized: bool = _te._normalize_sector_layout_if_needed()
	_te._sync_sector_layout(true)
	PerfTimeline.end_on(timeline)
	_te._document.capture_trn_resource(_te._data)
	_te._document.load_tileinfo_from_dir(dir_path)
	var normalized_tileinfo: bool = _te._tileinfo_ops._normalize_loaded_tileinfo_if_needed()
	_te._brush_ops._ensure_surface_inputs_rebuilt()
	_te._document.current_trn_path = trn_path
	# When opening a .trn that sits next to its depth.raw + texture assets,
	# treat the directory as the current project dir (Save uses it as the
	# default target). If none of those sidecars exist this is an import of
	# an external .trn; leave project_dir empty so the next Save prompts.
	if depth_bytes.size() > 0:
		_te._document.current_project_dir = dir_path
	else:
		_te._document.current_project_dir = ""
	_te._remember_open_path(trn_path)

	_te.is_dirty = normalized or normalized_tileinfo
	_te._mark_foliage_preview_dirty()
	_te._mark_ui_state_changed()
	_te._mark_ui_state_changed()
	_check_loaded_cdep_violations()
	return OK


func _open_trn_from_resource_root(resources: NovaResourceRoot, trn_name: String, timeline: PerfTimeline = null) -> Error:
	if resources == null:
		return ERR_INVALID_PARAMETER
	_te._brush_session.clear_history()
	_te.clear_clone_source()
	PerfTimeline.span_on(timeline, "trn_data")
	_te._data = NovaTerrainData.new()
	var err: Error = _te._data.load_from_resource_root(resources, trn_name)
	if err != OK:
		return err
	PerfTimeline.end_on(timeline)

	_te.texture_files = {}
	_apply_default_visual_state(false)

	PerfTimeline.span_on(timeline, "heightmap")
	var depth_bytes: PackedByteArray = resources.read_file("%s_depth.raw" % _te._data.get_terrain_name())
	if depth_bytes.size() == _te.HM_SIZE * _te.HM_SIZE * 2:
		_te._set_heightmap_image(_build_heightmap_from_raw16(depth_bytes))
	else:
		_te._set_heightmap_image(_build_heightmap_from_data())
	PerfTimeline.end_on(timeline)

	PerfTimeline.span_on(timeline, "textures")
	_apply_loaded_textures_from_data()
	PerfTimeline.end_on(timeline)
	PerfTimeline.span_on(timeline, "sectors")
	var normalized: bool = _te._normalize_sector_layout_if_needed()
	_te._sync_sector_layout(true)
	PerfTimeline.end_on(timeline)
	_te._document.capture_trn_resource(_te._data)
	var tileinfo: NovaTerrainTileInfo = _te._data.get_tileinfo_resource()
	if tileinfo != null:
		_te._document.tileinfo_resource = tileinfo
		_te._document.tileinfo_source_path = resources.get_root_dir().path_join(_te._data.get_tileinfo_filename())
		_te._document.tileinfo_state = "explicit"
		_te._document.tileinfo_selected_index = -1
	var normalized_tileinfo: bool = _te._tileinfo_ops._normalize_loaded_tileinfo_if_needed()
	_te._brush_ops._ensure_surface_inputs_rebuilt()
	_te._document.current_trn_path = trn_name.get_file()
	_te._document.current_project_dir = ""
	_te._remember_open_path(resources.get_root_dir().path_join(trn_name.get_file()))

	_te.is_dirty = normalized or normalized_tileinfo
	_te._mark_foliage_preview_dirty()
	_te._mark_ui_state_changed()
	_te._mark_ui_state_changed()
	_check_loaded_cdep_violations()
	return OK


func save_project(dir_path: String) -> Error:
	if _te.is_export_running():
		return ERR_BUSY
	DirAccess.make_dir_recursive_absolute(dir_path)
	_te._document.normalize_foliage_state_for_editor()
	# The terrain name is the project directory's basename. Renaming the
	# terrain is done by Save-As'ing into a differently-named directory —
	# the name field in the properties panel is read-only on purpose, so
	# there is no way for the terrain name and the dir name to drift apart.
	var name := dir_path.get_file()
	if name.is_empty():
		name = "untitled"
	if _te._data and String(_te._data.get_terrain_name()) != name:
		_te._data.set_terrain_name(name)

	var raw16: PackedByteArray = _te._data.get_depth_raw16()
	var depth_path := dir_path + "/" + name + "_depth.raw"
	var depth_file: FileAccess = FileAccess.open(depth_path, FileAccess.WRITE)
	if not depth_file:
		return ERR_FILE_CANT_WRITE
	depth_file.store_buffer(raw16)
	depth_file.close()

	var err := _save_texture_assets(dir_path, name)
	if err != OK:
		return err
	err = _te._document.save_tileinfo(dir_path, name)
	if err != OK:
		return err

	# Project save writes the .trn with no polydata — CPT is an export-time
	# bake artifact, not an authoring one. NovaTerrainData::load() tolerates
	# missing CPT since the "make CPT optional" change.
	_te._document.prepare_data_for_trn_save(name, "")
	err = ResourceSaver.save(_te._data, dir_path + "/" + name + ".trn")
	if err != OK:
		return err

	_te._document.current_project_dir = dir_path
	_te._document.current_trn_path = dir_path + "/" + name + ".trn"
	_te._remember_save_dir(dir_path)
	_te.is_dirty = false
	_te._mark_ui_state_changed()
	_te._mark_ui_state_changed()
	return OK


# Heightmaps loaded from project .trn files can predate the editor's live
# CDEP enforcement (or originate from external tools). If any 256-pixel
# horizontal block exceeds the per-block range limit, prompt the user to
# auto-clamp; otherwise the eventual DFX/JO export will fail at the bake
# guard with a less actionable message.
func _check_loaded_cdep_violations() -> void:
	if _te._data == null or not _te._heightmap_image:
		return
	var count: int = _te._data.cdep_count_violations()
	if count == 0:
		return
	if _te.workstation and _te.workstation.has_method("prompt_cdep_violations"):
		_te.workstation.prompt_cdep_violations(count, Callable(self, "_auto_fix_cdep_violations"))
	else:
		_te._notify_status("Heightmap has %d area%s too steep for Joint Operations / DFX export." % [count, "" if count == 1 else "s"])


func _auto_fix_cdep_violations() -> void:
	# The prompt Callable binds this RefCounted section and keeps it alive even
	# if the editor node is freed while the CDEP dialog is pending.
	if not is_instance_valid(_te):
		return
	if _te._data == null or not _te._heightmap_image:
		return
	var clamped: int = _te._data.cdep_clamp_all_violations()
	_te.terrain_mesh.set_heightmap(_te._heightmap_image)
	_te._height_revision += 1
	_te._brush_ops._refresh_surface_input_heightfield()
	_te._mark_foliage_preview_dirty()
	_te._mark_tile_overlay_dirty()
	_te.is_dirty = true
	_te._notify_status("Flattened %d area%s for Joint Operations / DFX export." % [clamped, "" if clamped == 1 else "s"])


# Brush enforcement keeps newly-sculpted heightmaps inside the CDEP envelope,
# but heightmaps loaded from disk (or imported from other tools) can still
# contain blocks the user never touched. Run a global clamp on the export
# path so a JO/DFX bake never fails on legacy data the user hasn't visited
# with the brush yet. BHD/DPTH has no per-block range limit, so skip it.
func _auto_clamp_for_export_if_needed(flavor: int) -> void:
	if flavor != _te.ExportFlavor.DFX_JO:
		return
	if _te._data == null or not _te._heightmap_image:
		return
	var clamped: int = _te._data.cdep_clamp_all_violations()
	if clamped == 0:
		return
	_te.terrain_mesh.set_heightmap(_te._heightmap_image)
	_te._height_revision += 1
	_te._brush_ops._refresh_surface_input_heightfield()
	_te._mark_foliage_preview_dirty()
	_te._mark_tile_overlay_dirty()
	_te.is_dirty = true
	_te._notify_status("Flattened %d area%s before export." % [clamped, "" if clamped == 1 else "s"])


func begin_export_terrain(output_dir: String, flavor: int) -> Error:
	if _te.is_export_running():
		return ERR_BUSY

	DirAccess.make_dir_recursive_absolute(output_dir)
	var name: String = _te._get_terrain_name()
	_auto_clamp_for_export_if_needed(flavor)
	if flavor == _te.ExportFlavor.DFX_JO and not _te._document.cdep_ranges_valid(_te._heightmap_image):
		_te._notify_status("Export blocked: some areas are too steep for Joint Operations / DFX. Flatten them, or export for original Delta Force instead.")
		return ERR_INVALID_DATA
	var raw16: PackedByteArray = _te._data.get_depth_raw16()
	if raw16.is_empty():
		return ERR_INVALID_DATA
	var builder: NovaTerrainBuilder = NovaTerrainBuilder.new()
	var job: NovaTerrainBuildJob = builder.begin_build_from_data(
		raw16, output_dir, name, "", flavor, _te._data.get_quadrant_locks()
	)
	if job == null:
		return ERR_CANT_CREATE

	_te._export_job = job
	_te._export_output_dir = output_dir
	_te.brush_active = false
	_te._brush_session.reset_stroke_tracking()
	_te._remember_export_dir(output_dir)

	if _te.workstation and _te.workstation.has_method("on_export_started"):
		_te.workstation.on_export_started(output_dir)

	return OK


func export_terrain(output_dir: String, flavor: int) -> Error:
	if _te.is_export_running():
		return ERR_BUSY

	DirAccess.make_dir_recursive_absolute(output_dir)
	_auto_clamp_for_export_if_needed(flavor)
	if flavor == _te.ExportFlavor.DFX_JO and not _te._document.cdep_ranges_valid(_te._heightmap_image):
		_te._notify_status("Export blocked: some areas are too steep for Joint Operations / DFX. Flatten them, or export for original Delta Force instead.")
		return ERR_INVALID_DATA
	var raw16: PackedByteArray = _te._data.get_depth_raw16()
	if raw16.is_empty():
		return ERR_INVALID_DATA
	var builder: NovaTerrainBuilder = NovaTerrainBuilder.new()
	var err: Error = builder.build_from_data(
		raw16, output_dir, _te._get_terrain_name(), "", flavor,
		_te._data.get_quadrant_locks()
	)
	if err != OK:
		return err

	var name: String = _te._get_terrain_name()
	err = _save_texture_assets(output_dir, name)
	if err != OK:
		return err
	err = _te._document.save_tileinfo(output_dir, name)
	if err != OK:
		return err

	_te._document.prepare_data_for_trn_save(name, name + ".cpt")
	err = ResourceSaver.save(_te._data, output_dir + "/" + name + ".trn")
	if err != OK:
		return err

	_te._remember_export_dir(output_dir)
	_te._mark_ui_state_changed()
	return OK


func _poll_export_job() -> void:
	if _te._export_job == null:
		return
	if not _te._export_job.is_finished():
		return
	_finish_export_job()


func _finish_export_job() -> void:
	var job: NovaTerrainBuildJob = _te._export_job
	if job == null:
		return

	job.wait_for_completion()
	var err: Error = job.get_result_error()
	var message: String = job.get_result_message()
	var output_dir: String = _te._export_output_dir
	var name: String = _te._get_terrain_name()

	if err == OK:
		err = _save_texture_assets(output_dir, name)
		if err == OK:
			err = _te._document.save_tileinfo(output_dir, name)
		if err == OK:
			_te._document.prepare_data_for_trn_save(name, name + ".cpt")
			err = ResourceSaver.save(_te._data, output_dir + "/" + name + ".trn")
		if err == OK:
			message = "Exported to: %s" % output_dir
		elif message.is_empty():
			message = "Export failed (error %d)" % err
	elif message.is_empty():
		message = "Export failed (error %d)" % err

	_te._export_job = null
	_te._export_output_dir = ""

	if _te.workstation and _te.workstation.has_method("on_export_completed"):
		_te.workstation.on_export_completed(err, message)
	_te._mark_ui_state_changed()


func _create_default_document(terrain_name: String) -> void:
	_te._document.create_default_document(terrain_name, _te._get_material(), _te._build_default_sector_grid())
	_te._active_sector_cell = Vector2i(-1, -1)
	_te.selected_surface_index = TerrainEditorSurfacePaint.DEFAULT_SURFACE_INDEX
	_te._mark_ui_state_changed()


func _apply_default_visual_state(sync_data: bool = true) -> void:
	_te._document.apply_default_visual_state(_te._get_material(), sync_data)


func _apply_loaded_textures_from_data() -> void:
	_te._document.apply_loaded_textures_from_data(_te._get_material())


func _save_texture_assets(output_dir: String, terrain_name: String) -> Error:
	return _te._document.save_texture_assets(_te._get_material(), output_dir, terrain_name)


func _build_heightmap_from_data() -> Image:
	return _te._document.build_heightmap_from_data()


func _build_heightmap_from_raw16(raw_bytes: PackedByteArray) -> Image:
	return _te._document.build_heightmap_from_raw16(raw_bytes)


func _create_heightmap_image(fill_height: float) -> Image:
	var image: Image = Image.create(_te.HM_SIZE, _te.HM_SIZE, false, Image.FORMAT_RF)
	image.fill(Color(fill_height, 0, 0, 1))
	return image
