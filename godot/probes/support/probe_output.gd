class_name ProbeOutput
extends RefCounted

## Where a probe writes the files it produces: the `output_dir` argument
## (absolute, res:// or user:// resolved, or repo-relative), else the run's
## artifact directory. Created on the way out.


static func resolve(ctx: ProbeContext, requested: String) -> String:
	var chosen := requested.strip_edges().replace("\\", "/")
	var path := ctx.artifact_dir
	if not chosen.is_empty():
		if chosen.begins_with("res://") or chosen.begins_with("user://"):
			path = ProjectSettings.globalize_path(chosen)
		elif chosen.is_absolute_path():
			path = chosen
		else:
			path = ProjectSettings.globalize_path("res://../".path_join(chosen))
	path = path.simplify_path()
	DirAccess.make_dir_recursive_absolute(path)
	return path
