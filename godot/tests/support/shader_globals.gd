class_name ShaderGlobals
extends RefCounted

## The project shader globals a raster test overrides, put back afterwards.
## The restore source is the project's registered default
## (ProjectSettings shader_globals/<name>): reading the live value back
## (RenderingServer.global_shader_parameter_get) is an editor-only call, which
## returns null in a headless or game run.


## The registered default of one global; null when the project declares none.
static func project_default(global_name: String) -> Variant:
	var setting: Variant = ProjectSettings.get_setting("shader_globals/" + global_name, {})
	return (setting as Dictionary).get("value") if setting is Dictionary else null


## Sets every named global back to its registered default.
static func restore_defaults(global_names: Array) -> void:
	for global_name in global_names:
		var value: Variant = project_default(String(global_name))
		if value != null:
			RenderingServer.global_shader_parameter_set(StringName(global_name), value)
