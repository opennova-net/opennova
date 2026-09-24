extends GutTest

## Device-side companion to shader_resource_contract_test.gd and
## shader_provenance_pins_test.gd (the textual contract). Loading every
## checked-in wrapper and include through Godot catches parser/import/include
## failures that a textual graph walk cannot. The windowed swatch lighting mode
## then forces the object wrappers through the actual rasterizer.

const SHADER_ROOT := "res://shaders"
const INSTANCE_UNIFORM_VALUES_PER_GEOMETRY := 16
const RETAINED_OBJECT_GEOMETRY_BUDGET := 16384
const REQUIRED_GLOBAL_SHADER_BUFFER_SIZE := (
		INSTANCE_UNIFORM_VALUES_PER_GEOMETRY * RETAINED_OBJECT_GEOMETRY_BUDGET)


func _collect_sources(directory: String, out: PackedStringArray) -> void:
	var access := DirAccess.open(directory)
	assert_not_null(access, "shader directory must be readable: %s" % directory)
	if access == null:
		return
	access.list_dir_begin()
	var entry := access.get_next()
	while not entry.is_empty():
		if access.current_is_dir():
			_collect_sources(directory.path_join(entry), out)
		elif entry.ends_with(".gdshader") or entry.ends_with(".gdshaderinc"):
			out.append(directory.path_join(entry))
		entry = access.get_next()
	access.list_dir_end()


func test_every_checked_in_shader_resource_loads_through_godot() -> void:
	var paths := PackedStringArray()
	_collect_sources(SHADER_ROOT, paths)
	paths.sort()
	assert_eq(paths.size(), 198, "the runtime inventory must stay closed")
	var wrappers := 0
	var includes := 0
	for path in paths:
		var resource := ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
		assert_not_null(resource, "Godot must parse and load %s" % path)
		if resource == null:
			continue
		if path.ends_with(".gdshader"):
			wrappers += 1
			assert_eq(resource.get_class(), "Shader", "%s must load as Shader" % path)
			assert_false((resource as Shader).code.is_empty(), "%s must contain code" % path)
		else:
			includes += 1
			assert_eq(resource.get_class(), "ShaderInclude",
					"%s must load as ShaderInclude" % path)
	assert_eq(wrappers, 157)
	assert_eq(includes, 41)


func test_global_shader_buffer_covers_retained_object_geometry() -> void:
	# Godot reserves 16 vec4 values for every geometry RID whose shader declares
	# instance uniforms. Retaining all authored object LODs exhausts the default
	# 4,096-instance buffer while loading the retail 00TRa mission, before its
	# first frame. Keep a 16,384-instance / 4 MiB device-side budget.
	assert_gte(int(ProjectSettings.get_setting(
			"rendering/limits/global_shader_variables/buffer_size", 0)),
			REQUIRED_GLOBAL_SHADER_BUFFER_SIZE,
			"global instance-uniform buffer must cover retained retail geometry")
