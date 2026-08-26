extends GutTest

## Device-side companion to tests/test_shader_resources.py. Loading every
## checked-in wrapper and include through Godot catches parser/import/include
## failures that a textual graph walk cannot. The windowed swatch lighting mode
## then forces the object wrappers through the actual rasterizer.

const SHADER_ROOT := "res://shaders"


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
	assert_eq(paths.size(), 195, "the runtime inventory must stay closed")
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
	assert_eq(wrappers, 154)
	assert_eq(includes, 41)
