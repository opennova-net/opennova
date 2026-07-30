extends GutTest

const NovaModelResolver := preload("res://engine/mission/nova_model_resolver.gd")
const MODEL_FIXTURE := "res://../fixtures/3dp/CmpFireN/CmpFireN.3di"
const PERSON_ID := 105310
const DYNAMIC_SHADOW_ID := 101291
const ORDINARY_ID := 105004


func before_each() -> void:
	_remove_tree(_fixture_root())


func after_each() -> void:
	_remove_tree(_fixture_root())


func test_make_model_applies_retail_dynamic_shadow_admission() -> void:
	_stage_fixture()
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(_fixture_root()), OK)
	var item_db := NovaItemDatabase.new()
	var load_error := item_db.load(_fixture_root().path_join("items.def"))
	assert_eq(load_error, OK, item_db.get_last_error())
	if load_error != OK:
		return
	var resolver := NovaModelResolver.new()
	resolver.setup(root, item_db)
	var parent := Node3D.new()
	add_child_autofree(parent)

	var person := resolver.make_model(PERSON_ID, parent) as NovaObjectModel
	var dynamic_shadow := resolver.make_model(
			DYNAMIC_SHADOW_ID, parent) as NovaObjectModel
	var ordinary := resolver.make_model(ORDINARY_ID, parent) as NovaObjectModel

	assert_not_null(person)
	assert_not_null(dynamic_shadow)
	assert_not_null(ordinary)
	if person == null or dynamic_shadow == null or ordinary == null:
		return
	assert_true(person.is_shadow_caster_enabled(),
			"retail admits every person to the dynamic silhouette pass")
	assert_true(dynamic_shadow.is_shadow_caster_enabled(),
			"DynamicShadow admits a streamed non-person model")
	assert_false(ordinary.is_shadow_caster_enabled(),
			"ordinary streamed items remain shadow receivers only")


func _stage_fixture() -> void:
	var root := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	var source := ProjectSettings.globalize_path(MODEL_FIXTURE)
	var bytes := FileAccess.get_file_as_bytes(source)
	assert_gt(bytes.size(), 0)
	for basename in ["US01", "Dbuggy1", "StaticCrate1"]:
		var model_file := FileAccess.open(
				root.path_join(basename + ".3di"), FileAccess.WRITE)
		assert_not_null(model_file)
		if model_file != null:
			model_file.store_buffer(bytes)
			model_file.close()
	var def_file := FileAccess.open(root.path_join("items.def"), FileAccess.WRITE)
	assert_not_null(def_file)
	if def_file != null:
		def_file.store_string(
			"begin \"Shadow Person\"\n"
			+ "  id %d\n" % PERSON_ID
			+ "  type person\n"
			+ "  graphic US01\n"
			+ "end\n\n"
			+ "begin \"Dynamic Object\"\n"
			+ "  id %d\n" % DYNAMIC_SHADOW_ID
			+ "  type object\n"
			+ "  graphic Dbuggy1\n"
			+ "  attrib: DynamicShadow\n"
			+ "end\n\n"
			+ "begin \"Ordinary Object\"\n"
			+ "  id %d\n" % ORDINARY_ID
			+ "  type object\n"
			+ "  graphic StaticCrate1\n"
			+ "end\n")
		def_file.close()


func _fixture_root() -> String:
	return OS.get_cache_dir().path_join("opennova_nova_model_resolver_test")


func _remove_tree(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	var dir := DirAccess.open(path)
	if dir == null:
		return
	for filename in dir.get_files():
		DirAccess.remove_absolute(path.path_join(filename))
	for dirname in dir.get_directories():
		_remove_tree(path.path_join(dirname))
	DirAccess.remove_absolute(path)
