extends GutTest

const Field := WorldField.Id
const Files := preload("res://tools/file_transaction.gd")
const DOCUMENT_EXTENSIONS: Array[String] = ["bms", "trn", "env"]
const DATA := "res://../assets"
var _dirs: PackedStringArray = []
var _worlds: Array[GameWorld] = []


func after_each() -> void:
	for world in _worlds:
		world.unload_preview()
	await get_tree().process_frame
	_worlds.clear()
	for directory in _dirs:
		TestFs.remove_dir_recursive(directory)
	_dirs.clear()


func _source(archive: bool = false) -> WorldSource:
	var directory := OS.get_cache_dir().path_join("opennova_edit_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(directory), OK)
	_dirs.append(directory)
	var source_dir := ProjectSettings.globalize_path(DATA)
	for filename in DirAccess.get_files_at(source_dir):
		assert_eq(DirAccess.copy_absolute(source_dir.path_join(filename), directory.path_join(filename)), OK)
	var source := WorldSource.new()
	source.data_directory = directory
	source.mission_name = "mnml.bms"
	var root := source.open_root()
	var terrain := TerrainData.new()
	assert_eq(terrain.load_from_resource_root(root, "mnml.trn"), OK)
	var definition := TerrainFoliageDef.new()
	definition.graphic = "house.3di"
	definition.match = 17
	definition.attrib_flags = 1 # force_on is independent of shadow
	terrain.set_foliage_defs([definition])
	assert_eq(terrain.save_to_path(directory.path_join("mnml.trn")), OK)
	# Seed real TRN color modes that the Inspector does not expose: the saved
	# foliage block above wrote them at their defaults.
	var trn_path := directory.path_join("mnml.trn")
	var trn_text := FileAccess.get_file_as_string(trn_path).replace("color_lower     0", "color_lower     2").replace("color_upper     0", "color_upper     1")
	var trn_file := FileAccess.open(trn_path, FileAccess.WRITE)
	trn_file.store_string(trn_text)
	trn_file.close()
	if archive:
		var entries: Array = []
		for filename in DirAccess.get_files_at(directory):
			entries.append({"name": filename, "bytes": FileAccess.get_file_as_bytes(directory.path_join(filename))})
		# The synthetic sidecars REPLACE the set's own mnml.pcx / mnml.bin entries: an
		# archive with two entries of one name resolves by index layout, not by intent.
		for extension in WorldEditSession.SIDECAR_EXTENSIONS:
			var synthetic_name: String = "mnml." + String(extension)
			for index in range(entries.size() - 1, -1, -1):
				if String(entries[index]["name"]).to_lower() == synthetic_name:
					entries.remove_at(index)
			entries.append({"name": synthetic_name, "bytes": "synthetic sidecar " + extension})
		assert_eq(TestPff.write(directory.path_join("resource.pff"), entries), OK)
		for filename in DirAccess.get_files_at(directory):
			if filename != "resource.pff":
				assert_eq(DirAccess.remove_absolute(directory.path_join(filename)), OK)
		source.source_kind = WorldSource.RETAIL_INSTALL
	return source


func _session(source: WorldSource) -> WorldEditSession:
	var session := WorldEditSession.new()
	assert_eq(session.open(source), OK, session.get_last_error())
	return session


func _world(source: WorldSource, session: WorldEditSession) -> GameWorld:
	var world := load("res://examples/world_preview.tscn").instantiate() as GameWorld
	world.world_source = source
	add_child_autofree(world)
	_worlds.append(world)
	assert_eq(session.load_preview(world), OK, str(world.get_preview_diagnostics()))
	return world


func test_save_reopens_native_documents_and_keeps_other_mission_and_foliage_fields() -> void:
	var source := _source()
	var session := _session(source)
	var original := source.open_mission(source.open_root())
	var original_count := original.get_entity_count(MissionData.KIND_BUILDING)
	var original_path := original.get_source_path()
	assert_true(original.set_header_int("start_time", 19 * 256))
	assert_eq(original.save_to_path(source.data_directory.path_join("copy.bms")), OK)
	assert_eq(original.get_source_path(), original_path, "a staged write never adopts its path")
	assert_true(original.is_modified(), "a staged write is not an adopted save")
	assert_eq(session.validate_edit(Field.FOLIAGE_GRAPHIC, "house.3di"), "")
	session.apply_value(Field.START_TIME, 18 * 256)
	session.apply_value(Field.SKY_HEIGHT, 243.0)
	session.apply_value(Field.SKY_MAP_1, "wall.tga")
	session.apply_value(Field.FOLIAGE_MATCH, 73)
	session.apply_value(Field.FOLIAGE_SHADOW, true)
	assert_true(session.is_dirty())
	assert_eq(session.get_dirty_files().size(), 3)
	assert_eq(session.save(), OK, session.get_last_error())
	assert_false(session.is_dirty())
	assert_true(DirAccess.get_directories_at(source.data_directory).is_empty())
	var reopened := _session(source)
	assert_eq(reopened.get_value(Field.START_TIME), 18 * 256)
	assert_almost_eq(float(reopened.get_value(Field.SKY_HEIGHT)), 243.0, 0.001)
	assert_eq(reopened.get_value(Field.SKY_MAP_1), "wall.tga")
	assert_eq(reopened.get_value(Field.FOLIAGE_MATCH), 73)
	assert_true(reopened.get_value(Field.FOLIAGE_SHADOW))
	var mission := source.open_mission(source.open_root())
	assert_eq(mission.get_entity_count(MissionData.KIND_BUILDING), original_count)
	assert_eq(mission.get_terrain_ref(), original.get_terrain_ref())
	assert_eq(mission.get_environment_ref(), original.get_environment_ref())
	var terrain := TerrainData.new()
	assert_eq(terrain.load_from_resource_root(source.open_root(), "mnml.trn"), OK)
	assert_ne((terrain.get_foliage_defs()[0] as TerrainFoliageDef).attrib_flags & 1, 0)
	var saved_trn := FileAccess.get_file_as_string(source.data_directory.path_join("mnml.trn"))
	assert_string_contains(saved_trn, "color_lower     2")
	assert_string_contains(saved_trn, "color_upper     1")


func test_undo_across_save_and_reload_tracks_the_saved_values() -> void:
	var source := _source()
	var session := _session(source)
	var original: int = session.get_value(Field.START_TIME)
	var undo := UndoRedo.new()
	autofree(undo)
	undo.create_action("Time")
	undo.add_do_method(session.apply_value.bind(Field.START_TIME, 20 * 256))
	undo.add_undo_method(session.apply_value.bind(Field.START_TIME, original))
	undo.commit_action()
	assert_true(session.is_dirty())
	undo.undo()
	assert_false(session.is_dirty())
	undo.redo()
	assert_true(session.is_dirty())
	assert_eq(session.save(), OK)
	assert_false(session.is_dirty())
	undo.undo()
	assert_true(session.is_dirty(), "undo past the last save is an unsaved change")
	assert_eq(session.reload_from_disk(), OK)
	assert_eq(session.get_value(Field.START_TIME), 20 * 256)
	assert_false(session.is_dirty())
	undo.redo()
	assert_false(session.is_dirty(), "the undo target survives a reload")


func test_archive_copy_preserves_sidecars_and_shares_assets_using_editable_policy() -> void:
	var source := _source(true)
	var session := _session(source)
	assert_false(session.is_editable())
	assert_ne(session.validate_edit(Field.START_TIME, 8 * 256), "")
	var archived_name := source.open_mission(source.open_root()).get_mission_name()
	var archive_hash := FileAccess.get_sha256(source.data_directory.path_join("resource.pff"))
	var copy := session.create_editable_copy("island_edit")
	assert_not_null(copy, session.get_last_error())
	if copy == null:
		return
	assert_eq(copy.source_kind, WorldSource.EDITABLE_GAME_DATA)
	assert_eq(copy.mission_name, "island_edit.bms")
	assert_eq(copy.data_directory, source.data_directory)
	var root := copy.open_root()
	var mission := copy.open_mission(root)
	assert_not_null(mission)
	assert_eq(mission.get_terrain_ref(), "island_edit")
	assert_eq(mission.get_environment_ref(), "island_edit")
	assert_eq(mission.get_mission_name(), archived_name)
	for extension in WorldEditSession.SIDECAR_EXTENSIONS:
		assert_eq(FileAccess.get_file_as_string(source.data_directory.path_join("island_edit." + extension)),
				"synthetic sidecar " + extension)
	assert_true(root.has_file("wall.tga"), "supporting art stays in the archive")
	assert_false(FileAccess.file_exists(source.data_directory.path_join("wall.tga")))
	var editing := _session(copy)
	assert_true(editing.is_editable())
	editing.apply_value(Field.START_TIME, 7 * 256)
	assert_eq(editing.save(), OK, editing.get_last_error())
	assert_eq(copy.open_mission(copy.open_root()).get_info().start_time, 7 * 256)
	assert_eq(FileAccess.get_sha256(source.data_directory.path_join("resource.pff")), archive_hash)
	assert_eq(source.open_mission(source.open_root()).get_mission_name(), archived_name)
	assert_eq(DirAccess.remove_absolute(source.data_directory.path_join("island_edit.bms")), OK)
	assert_null(copy.open_mission(copy.open_root()))
	copy.mission_name = "mnml.bms"
	assert_null(copy.open_mission(copy.open_root()), "even an existing archived BMS cannot hide a missing loose mission")


func test_copy_rejects_collisions_and_path_traversal_without_writes() -> void:
	var source := _source(true)
	var session := _session(source)
	var before := DirAccess.get_files_at(source.data_directory)
	assert_null(session.create_editable_copy("mnml"), "an archive entry is also a collision")
	assert_string_contains(session.get_last_error(), "already exists")
	for name in ["../escape", "too_long_for_pff", "", "dir/file"]:
		assert_null(session.create_editable_copy(name))
	assert_eq(DirAccess.get_files_at(source.data_directory), before)
	assert_true(DirAccess.get_directories_at(source.data_directory).is_empty())


func test_external_change_refuses_save_and_failed_reload_preserves_edits() -> void:
	var source := _source()
	var session := _session(source)
	var mission_before := FileAccess.get_file_as_bytes(source.data_directory.path_join("mnml.bms"))
	session.apply_value(Field.START_TIME, 22 * 256)
	session.apply_value(Field.SKY_HEIGHT, 260.0)
	var environment_path := source.data_directory.path_join("mnml.env")
	assert_eq(DirAccess.remove_absolute(environment_path), OK)
	assert_ne(session.save(), OK)
	assert_string_contains(session.get_last_error(), "mnml.env changed on disk")
	assert_true(session.is_dirty())
	assert_eq(FileAccess.get_file_as_bytes(source.data_directory.path_join("mnml.bms")), mission_before)
	assert_ne(session.reload_from_disk(), OK)
	assert_true(session.is_dirty())
	assert_eq(session.get_value(Field.START_TIME), 22 * 256)
	assert_eq(session.get_value(Field.SKY_HEIGHT), 260.0)


func test_writer_failure_leaves_all_originals_and_removes_staging_files() -> void:
	var source := _source()
	var before := FileAccess.get_file_as_bytes(source.data_directory.path_join("mnml.bms"))
	var session := _session(source)
	var mission := source.open_mission(source.open_root())
	mission.set_header_int("start_time", 19 * 256)
	var reason: String = Files.write(session.get_directory(), {
		"mnml.bms": mission.save_to_path,
		"mnml.env": func(_path: String) -> Error: return ERR_FILE_CANT_WRITE,
	}, func() -> String: return "", true)
	assert_string_contains(reason, "mnml.env")
	assert_eq(FileAccess.get_file_as_bytes(source.data_directory.path_join("mnml.bms")), before)
	assert_true(DirAccess.get_directories_at(source.data_directory).is_empty())


func test_settings_update_shares_documents_keeps_objects_and_never_saves_effective_env() -> void:
	var source := _source()
	var session := _session(source)
	var world := _world(source, session)
	var mission := world.get_loaded_mission()
	var terrain := world.get_terrain_data()
	var population := world.get_static_live_population_count()
	var objects := world.get_runtime().get_node("MissionObjects")
	var first_object := objects.get_child(0)
	var environment := world.get_node("MissionEnvironment") as MissionEnvironment
	var starting_source := environment.environment_data.source_path
	session.apply_value(Field.START_TIME, 21 * 256)
	session.apply_value(Field.SKY_HEIGHT, 211.0)
	session.apply_value(Field.FOLIAGE_MATCH, 35)
	assert_eq(session.update_preview(world), OK)
	assert_same(world.get_loaded_mission(), mission)
	assert_same(world.get_terrain_data(), terrain)
	assert_eq(world.get_static_live_population_count(), population)
	assert_same(world.get_runtime().get_node("MissionObjects"), objects)
	assert_same(objects.get_child(0), first_object)
	assert_eq(mission.get_info().start_time, 21 * 256)
	assert_almost_eq(environment.get_time_of_day(), 2100.0, 0.01)
	assert_almost_eq(environment.environment_data.sky_height, 211.0, 0.001)
	assert_eq(environment.environment_data.source_path, starting_source)
	assert_eq((terrain.get_foliage_defs()[0] as TerrainFoliageDef).match, 35)
	assert_null(world.get_sim())
	world.unload_preview()
	assert_true(session.is_dirty(), "changing scene tabs only releases the preview")
	assert_eq(session.load_preview(world), OK)
	assert_eq(world.get_loaded_mission().get_info().start_time, 21 * 256)
	assert_eq(session.save(), OK)
	var base := EnvFile.new()
	assert_eq(base.load_from_resource_root(source.open_root(), "mnml.env"), OK)
	assert_false(base.has_mission_overrides())
	assert_almost_eq(base.sky_height, 211.0, 0.001)


func test_field_table_covers_every_id_once_and_names_its_document() -> void:
	var table := WorldField.all()
	assert_eq(table.size(), WorldField.Id.size(), "one row per field id")
	var session := _session(_source())
	for i in range(table.size()):
		var spec := table[i]
		assert_eq(spec.id, i, "the table is indexed by id")
		assert_false(spec.label.is_empty())
		assert_false(spec.group.is_empty())
		assert_true(spec.read.is_valid() and spec.write.is_valid(), spec.label)
		assert_eq(session.get_file_name(spec.id).get_extension(), DOCUMENT_EXTENSIONS[spec.document], spec.label)
		if spec.slotted:
			assert_eq(spec.document, WorldField.Document.TERRAIN, "slots address the terrain's foliage")
		match spec.widget:
			WorldField.Widget.INT_SPIN, WorldField.Widget.FLOAT_SPIN, WorldField.Widget.TIME_TEXT:
				assert_lt(spec.min_value, spec.max_value, spec.label)
				assert_false(spec.range_message.is_empty(), spec.label)
			WorldField.Widget.FILENAME_TEXT:
				assert_false(spec.placeholder.is_empty(), spec.label)
		# Every row reads the value it just wrote through the session's documents
		# (the fixture seeds one foliage slot, so slot 0 is valid for slotted rows).
		var before: Variant = session.get_value(spec.id, 0)
		assert_not_null(before, spec.label)
		session.apply_value(spec.id, before, 0)
		assert_eq(session.get_value(spec.id, 0), before, spec.label)
	assert_false(session.is_dirty(), "rewriting the current values changes nothing")
	for document in range(WorldField.Document.size()):
		assert_false(WorldField.for_document(document as WorldField.Document).is_empty(),
				"every document exposes at least one field")


func test_field_validation_rejects_invalid_time_ranges_and_missing_assets() -> void:
	var session := _session(_source())
	assert_ne(session.validate_edit(Field.START_TIME, 24 * 256), "")
	assert_ne(session.validate_edit(Field.SKY_HEIGHT, NAN), "")
	assert_ne(session.validate_edit(Field.FOLIAGE_MATCH, 256), "")
	assert_ne(session.validate_edit(Field.SKY_MAP_1, "../wall.tga"), "")
	assert_ne(session.validate_edit(Field.SKY_MAP_1, "missing.pcx"), "")
	assert_ne(session.validate_edit(Field.FOLIAGE_MATCH, 12, 4), "")
	assert_eq(session.validate_edit(Field.SKY_MAP_1, ""), "")
	assert_false(session.is_dirty())


func test_failed_save_has_native_recovery_without_overwriting_external_edits() -> void:
	var source := _source()
	var session := _session(source)
	session.apply_value(Field.START_TIME, 5 * 256)
	session.apply_value(Field.SKY_HEIGHT, 301.0)
	var mission_path := source.data_directory.path_join("mnml.bms")
	var external := source.open_mission(source.open_root())
	external.set_header_int("start_time", 23 * 256)
	assert_eq(external.save_as(mission_path), OK)
	assert_ne(session.save(), OK)
	var recovery := session.write_recovery(source.data_directory.path_join("recovery"))
	assert_false(recovery.is_empty())
	var recovered := MissionData.new()
	assert_eq(recovered.open_file(recovery.path_join("mnml.bms")), OK)
	assert_eq(recovered.get_info().start_time, 5 * 256)
	var environment := EnvFile.new()
	environment.source_path = recovery.path_join("mnml.env")
	assert_eq(environment.load(), OK)
	assert_almost_eq(environment.sky_height, 301.0, 0.001)
	assert_eq(source.open_mission(source.open_root()).get_info().start_time, 23 * 256)
	assert_true(session.is_dirty())


func test_mission_overrides_remain_effective_while_base_environment_is_saved() -> void:
	var source := _source()
	var mission := source.open_mission(source.open_root())
	assert_true(mission.set_header_int("fog_override", 650))
	assert_true(mission.set_header_flag(2, true)) # native fog-distance override flag
	assert_eq(mission.save_as(source.data_directory.path_join("mnml.bms")), OK)
	var base := EnvFile.new()
	assert_eq(base.load_from_resource_root(source.open_root(), "mnml.env"), OK)
	var base_fog := base.fog_level
	assert_ne(base_fog, 650.0)
	var session := _session(source)
	assert_string_contains(session.get_environment_note(), "fog")
	var world := _world(source, session)
	var environment := world.get_node("MissionEnvironment") as MissionEnvironment
	assert_true(environment.environment_data.has_mission_overrides())
	assert_almost_eq(environment.environment_data.fog_level, 650.0, 0.001)
	session.apply_value(Field.SKY_HEIGHT, 235.0)
	assert_eq(session.update_preview(world), OK)
	assert_almost_eq(environment.environment_data.fog_level, 650.0, 0.001)
	assert_eq(session.save(), OK)
	assert_eq(base.load_from_resource_root(source.open_root(), "mnml.env"), OK)
	assert_almost_eq(base.fog_level, base_fog, 0.001)
	assert_almost_eq(base.sky_height, 235.0, 0.001)
